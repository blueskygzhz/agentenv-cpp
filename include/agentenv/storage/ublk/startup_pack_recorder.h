// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/impls/startup_pack_recorder.rs
//
// First-touch page recorder for the startup memory pack.
//
// Attached to an `OverlaybdTarget` while a throwaway VM re-resumes a freshly
// captured snapshot. Every read submitted to the device marks its 4 KiB pages
// in an atomic bitmap; the thread that actually flips a bit appends the page
// to the ordered first-touch log. That ordering *is* the product: position in
// the log is the page's first-touch rank, which is the only record of what the
// boot needed first, and it is what the publisher plans the pack layout from.
//
// Three properties shape the whole design:
//
//   * **Exactly-once append.** A page must appear in the log once even when
//     eight queue threads read it at the same instant. The bitmap's
//     `fetch_or` decides the winner: only the thread that observed the bit as
//     clear appends. Without that the log would hold duplicates, and a
//     duplicate is not harmless -- it would inflate the rank of every page
//     after it.
//
//   * **The hot path stays lock-free except for the append.** This runs
//     inside `HandleIoRequest`, before the read that the guest is waiting on.
//     The activity clocks are atomics; the only lock is a short log append,
//     and it is taken only by reads that marked new pages -- so a re-read of
//     already-recorded pages never contends.
//
//   * **A latency heuristic, not cache attribution.** Completion latency
//     feeds an estimate of how much of the recording was served remotely:
//     local pread and cache hits are ~0.1 ms while an OSS refill is ~20 ms,
//     so a slow read that touched new pages is treated as remote-served.
//     This is a quality signal for the recording, deliberately bought without
//     plumbing cache attribution through the entire layer stack.
#ifndef AGENTENV_STORAGE_UBLK_STARTUP_PACK_RECORDER_H_
#define AGENTENV_STORAGE_UBLK_STARTUP_PACK_RECORDER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace ublk {

/// Rust `BITMAP_MAX_PAGES` — 16 GiB of device space, 512 KiB of bitmap words.
///
/// Pages above it are counted in `skipped_high_pages` but never recorded. A
/// cap is needed because the bitmap is allocated from the device's virtual
/// size, which the guest chooses; counting the overflow rather than silently
/// ignoring it is what lets the caller see that the trace is incomplete.
const uint64_t kRecorderBitmapMaxPages = 4ull * 1024 * 1024;

/// Rust `REMOTE_LATENCY_THRESHOLD` — 10 ms.
const uint64_t kRemoteLatencyThresholdNanos = 10ull * 1000 * 1000;

/// Rust `RecordingVerdict` — the window state machine's answer per tick.
enum class RecordingVerdict {
    Continue,
    /// Window finished; finalize the pack.
    Finish,
    /// Abort the pack (low-quality recording).
    ///
    /// `Verdict` never returns this today, upstream included: no quality
    /// guard currently trips. It is kept because the window task handles it,
    /// and dropping it would make adding a guard a protocol change rather
    /// than a one-line change.
    Fail,
};

/// Rust `FinalizeOutcome`.
struct FinalizeOutcome {
    uint32_t pages = 0;
    /// Memory bytes covered by the trace (pages x 4 KiB).
    uint64_t bytes = 0;
    uint64_t remote_bytes = 0;
    /// Pages above the bitmap coverage cap, which were never recorded.
    uint64_t skipped_high_pages = 0;
};

class StartupPackReadGuard;

/// Rust `StartupPackRecorder`.
class StartupPackRecorder {
 public:
    /// `max_pages` caps the *log*, not the bitmap: marking continues past the
    /// cap so `total_marked` stays an honest count of what the boot touched,
    /// while the trace stays bounded.
    StartupPackRecorder(uint64_t mem_virtual_size, std::size_t max_pages);

    /// Record the pages covered by a read at submission time -- the order the
    /// daemon sees requests, which is approximately the guest's fault order.
    ///
    /// At submission and not at completion: completions finish out of order,
    /// so recording there would scramble the first-touch ranking that is the
    /// entire point of the log.
    ///
    /// Returns true when this read marked at least one previously-unseen
    /// page, so the completion path can attribute remote latency to new pages
    /// only.
    bool ObserveSubmit(uint64_t offset, std::size_t len);

    /// Rust `verdict` — polled by the daemon's window tick.
    ///
    /// Durations are nanoseconds. The protocol carries milliseconds, but the
    /// quiet window is compared against an idle clock sampled on the read
    /// path, and rounding that to milliseconds would make a sub-millisecond
    /// quiet period unexpressible.
    RecordingVerdict Verdict(uint64_t min_window_nanos, uint64_t quiet_nanos,
                             uint64_t max_window_nanos) const;

    /// Write the recorded first-touch page list as a trace file.
    ///
    /// No page data is read back: the publisher plans the pages onto the final
    /// layer objects and packages the selected physical blocks itself, so this
    /// never issues a read against the image -- remote or local. That is what
    /// makes finalizing safe to run after the recording device is detached.
    ///
    /// `cancelled` is checked immediately before the rename. The rename is
    /// what publishes the trace, so a recording cancelled at any point before
    /// it leaves nothing at `output` -- and the tmp file is left behind for
    /// the caller's cleanup rather than removed here, because the abort path
    /// owns both files and removing one from under it would race.
    core::Expected<FinalizeOutcome, std::string> Finalize(
        const std::string& output, const std::atomic<bool>& cancelled) const;

    uint64_t RemoteBytes() const { return remote_bytes_.load(); }
    uint64_t TotalMarked() const { return total_marked_.load(); }
    uint64_t SkippedHighPages() const { return skipped_high_pages_.load(); }

    /// Log length. Rust gates this on `cfg(test)`; here it is public so the
    /// tests can read it without a friend declaration.
    std::size_t PageCount() const;
    bool IsMarked(uint32_t page) const;
    std::vector<uint32_t> OrderedPages() const;

 private:
    friend class StartupPackReadGuard;

    StartupPackRecorder(const StartupPackRecorder&);
    StartupPackRecorder& operator=(const StartupPackRecorder&);

    /// Nanoseconds since this recorder was armed.
    uint64_t ElapsedNanos() const;
    /// Stamp the activity clock without a lock.
    void TouchActivity();
    /// Nanoseconds since the last observed activity.
    uint64_t IdleNanos() const;
    void ObserveCompletion(uint64_t latency_nanos, uint64_t bytes, bool touched_new);

    uint64_t mem_virtual_size_;

    /// A heap array, not a `std::vector<std::atomic<uint64_t> >`: in C++11 an
    /// atomic is neither copyable nor movable, so a vector of them cannot be
    /// sized after construction. The elements also need an explicit zeroing
    /// pass -- `std::atomic`'s default constructor leaves the value
    /// indeterminate, which would seed the bitmap with pages marked as
    /// already seen.
    std::unique_ptr<std::atomic<uint64_t>[]> bitmap_;
    std::size_t bitmap_words_;

    mutable std::mutex log_mutex_;
    std::vector<uint32_t> log_;
    std::size_t max_pages_;

    /// Pages ever marked in the bitmap. The log may be capped below this.
    std::atomic<uint64_t> total_marked_;
    std::atomic<uint64_t> skipped_high_pages_;
    /// Signed: a completion may be observed by a different thread than the
    /// submission, and a transient negative value is preferable to an
    /// unsigned wrap that would read as billions of in-flight reads and pin
    /// the window open forever.
    std::atomic<int64_t> in_flight_;
    std::atomic<uint64_t> remote_bytes_;

    uint64_t armed_at_nanos_;
    /// Rust's `OnceLock<Instant>`, as nanoseconds since arm with a sentinel
    /// for "no read yet". Set once, by compare-exchange: the *first* read is
    /// what starts the window, so a later read must not move it.
    std::atomic<uint64_t> first_read_nanos_;
    std::atomic<uint64_t> last_activity_nanos_;
};

/// Rust `StartupPackReadGuard` — settles one in-flight read on destruction.
///
/// Holds a `shared_ptr` so the recorder cannot be freed while a read it is
/// counting is still outstanding; a detach (`SetRecorder(nullptr)`) while
/// reads are in flight is the normal end of a recording, not an error.
///
/// The constructor takes that pointer rather than deriving it from
/// `shared_from_this`: the read path already holds one from loading the
/// recorder, so requiring it costs nothing and keeps the recorder usable as a
/// plain stack object in tests.
class StartupPackReadGuard {
 public:
    /// Disarmed: destroying one settles nothing. This is the state the read
    /// path leaves it in when no recording is armed, which is the common case.
    StartupPackReadGuard() : bytes_(0), touched_new_(false), started_nanos_(0) {}
    StartupPackReadGuard(std::shared_ptr<StartupPackRecorder> recorder, uint64_t bytes,
                         bool touched_new);
    ~StartupPackReadGuard();

    /// Start counting a read on an already-declared guard.
    ///
    /// Exists because the read path has to declare the guard in the scope that
    /// spans the read but can only decide whether to arm it after loading the
    /// recorder. C++11 has no `Optional<T>` for a non-movable type here, and a
    /// guard that is move-assigned would settle the moved-from read on the
    /// temporary's destruction.
    ///
    /// Arming an already-armed guard would lose the first read's completion,
    /// so it is refused rather than overwritten.
    void Arm(std::shared_ptr<StartupPackRecorder> recorder, uint64_t bytes,
             bool touched_new);

 private:
    StartupPackReadGuard(const StartupPackReadGuard&);
    StartupPackReadGuard& operator=(const StartupPackReadGuard&);

    std::shared_ptr<StartupPackRecorder> recorder_;
    uint64_t bytes_;
    bool touched_new_;
    uint64_t started_nanos_;
};

/// Rust `tmp_pack_path` — the staging path for one pack output.
///
/// Derived from the output rather than placed in a temp dir so both files
/// share a directory, which is what makes the publish a rename rather than a
/// cross-filesystem copy. The daemon's abort cleanup removes it.
std::string TmpPackPath(const std::string& output);

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_STARTUP_PACK_RECORDER_H_
