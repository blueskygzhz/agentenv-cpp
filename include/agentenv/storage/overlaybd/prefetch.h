// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/prefetch.rs — trace-driven prefetch.
//
// Two phases, one file. On the first boot of an image the trace file exists
// but is empty, so every layer read is *recorded*; on later boots the file has
// content, so the recorded reads are *replayed* ahead of the guest. The point
// is that a container's startup reads are highly repeatable, so the second
// boot can fetch them in bulk instead of one fault at a time.
//
// The mode is therefore derived from the file, never configured: whoever
// provisions the image decides by creating an empty trace, and a boot cannot
// disagree with the artifact it was given.
//
// This replaces an earlier placeholder in this file that invented a `Backend`
// and a `PrefetchEntry`; neither exists in Rust.
#ifndef AGENTENV_STORAGE_OVERLAYBD_PREFETCH_H_
#define AGENTENV_STORAGE_OVERLAYBD_PREFETCH_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `TRACE_MAGIC`.
const uint32_t kPrefetchTraceMagic = 3270449184u;
/// Rust `TRACE_HEADER_SIZE` / `TRACE_RECORD_SIZE`.
///
/// Both are 24 bytes but for unrelated reasons, so they are separate
/// constants: the header is magic + size + checksum with padding, the record
/// is op + layer + count + offset.
const size_t kPrefetchTraceHeaderSize = 24;
const size_t kPrefetchTraceRecordSize = 24;
/// Rust `POLL_INTERVAL` — how often the recorder checks whether its lock file
/// has been removed.
const uint64_t kPrefetchPollIntervalMs = 1000;

/// Rust `PrefetchMode`.
enum class PrefetchMode {
    /// No trace file at all. `NewPrefetcher` refuses rather than silently
    /// doing nothing, so a misconfigured path is reported instead of costing
    /// a boot its prefetch.
    Disabled,
    /// An empty trace file: record this boot's reads.
    Record,
    /// A populated trace file: replay it.
    Replay
};

/// Shared state. Defined in the implementation: a `PrefetchFile` outlives the
/// `Prefetcher` handle in principle, so the two share this by pointer rather
/// than one owning the other.
struct PrefetcherInner;

/// Rust `Prefetcher`.
///
/// Not copyable and held by `shared_ptr`: the destructor is what finalises a
/// recorded trace, so a second copy going out of scope would try to dump
/// again.
class Prefetcher {
 public:
    /// Rust `detect_mode` — absent file means disabled, empty means record,
    /// anything else means replay.
    ///
    /// Note that an *empty* file is meaningful. Provisioning creates one to
    /// request recording, so "exists but zero length" cannot be treated as
    /// "no trace".
    static PrefetchMode DetectMode(const std::string& trace_file_path);

    ~Prefetcher();

    PrefetchMode Mode() const;

    /// Rust `new_prefetch_file` — wrap a layer's source file.
    ///
    /// In record mode the wrapper notes each complete read; in replay mode it
    /// also registers the source under `layer_index`, because the trace
    /// identifies layers by index and the replay workers have to map them back
    /// to a file.
    std::shared_ptr<VirtualFile> NewPrefetchFile(const std::shared_ptr<VirtualFile>& src_file,
                                                    uint32_t layer_index);

    /// Rust `replay` — start the replay workers.
    ///
    /// Idempotent, and a no-op unless the mode is replay *and* both a trace
    /// and at least one registered source exist. Must be called after the
    /// layers have been wrapped, since that is what registers them.
    core::Expected<core::Unit, std::string> Replay();

    /// Records accumulated so far. Rust keeps this private; it is exposed
    /// here because the dump is otherwise only observable through the file,
    /// and a test should be able to tell "recorded nothing" from "dumped
    /// nothing".
    size_t RecordedCount() const;
    /// Trace entries not yet replayed.
    size_t ReplayQueueSize() const;

 private:
    friend core::Expected<std::shared_ptr<Prefetcher>, std::string> NewPrefetcher(
        const std::string& trace_file_path, int32_t concurrency);

    Prefetcher() {}
    Prefetcher(const Prefetcher&);
    Prefetcher& operator=(const Prefetcher&);

    std::shared_ptr<PrefetcherInner> inner_;
    /// Rust `detect_thread` — only in record mode.
    std::shared_ptr<std::thread> detect_thread_;
    /// Rust spawns one thread hosting `concurrency` tokio tasks. See the
    /// implementation for why this is a vector of threads instead.
    std::vector<std::shared_ptr<std::thread> > replay_threads_;
};

/// Rust `new_prefetcher`.
///
/// `concurrency` is clamped up to 1: zero workers would make replay mode load
/// a trace and then ignore it, which is harder to diagnose than a slow replay.
core::Expected<std::shared_ptr<Prefetcher>, std::string> NewPrefetcher(
    const std::string& trace_file_path, int32_t concurrency);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_PREFETCH_H_
