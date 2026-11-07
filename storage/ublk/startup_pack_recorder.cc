// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/impls/startup_pack_recorder.rs
#include "agentenv/storage/ublk/startup_pack_recorder.h"

#include <stdio.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>

#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/startup_pack.h"

namespace agentenv {
namespace storage {
namespace ublk {

namespace {

/// "No read observed yet". `UINT64_MAX` rather than 0 because 0 is a valid
/// reading -- a read submitted in the same nanosecond the recorder was armed.
const uint64_t kNoFirstRead = ~static_cast<uint64_t>(0);

uint64_t NowNanos() {
    // Steady, not system: the window is a duration measurement, and a wall
    // clock step during a recording would either end it instantly or hold it
    // open past its cap.
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

std::string TmpPackPath(const std::string& output) { return output + ".tmp"; }

StartupPackRecorder::StartupPackRecorder(uint64_t mem_virtual_size, std::size_t max_pages)
    : mem_virtual_size_(mem_virtual_size),
      bitmap_words_(0),
      max_pages_(max_pages),
      total_marked_(0),
      skipped_high_pages_(0),
      in_flight_(0),
      remote_bytes_(0),
      armed_at_nanos_(NowNanos()),
      first_read_nanos_(kNoFirstRead),
      last_activity_nanos_(0) {
    uint64_t bitmap_pages = mem_virtual_size / overlaybd::startup::kPackPageBytes;
    if (bitmap_pages > kRecorderBitmapMaxPages) bitmap_pages = kRecorderBitmapMaxPages;
    bitmap_words_ = static_cast<std::size_t>((bitmap_pages + 63) / 64);

    if (bitmap_words_ > 0) {
        bitmap_.reset(new std::atomic<uint64_t>[bitmap_words_]);
        // Explicit, because `std::atomic`'s default constructor does not
        // initialize the value. Skipping this would start the recording with
        // arbitrary pages already marked, and those pages would then never be
        // recorded -- a silently truncated trace rather than a crash.
        for (std::size_t i = 0; i < bitmap_words_; ++i) {
            bitmap_[i].store(0, std::memory_order_relaxed);
        }
    }
}

uint64_t StartupPackRecorder::ElapsedNanos() const {
    const uint64_t now = NowNanos();
    return now > armed_at_nanos_ ? now - armed_at_nanos_ : 0;
}

void StartupPackRecorder::TouchActivity() {
    const uint64_t nanos = ElapsedNanos();
    // Rust's `fetch_max`, which C++11 does not have. The loop keeps the clock
    // monotonic under concurrent stamps: two threads whose reads completed out
    // of order must not let the earlier one walk the clock backwards and
    // extend the quiet window.
    uint64_t observed = last_activity_nanos_.load(std::memory_order_relaxed);
    while (observed < nanos) {
        if (last_activity_nanos_.compare_exchange_weak(observed, nanos,
                                                       std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
            return;
        }
    }
}

uint64_t StartupPackRecorder::IdleNanos() const {
    const uint64_t now = ElapsedNanos();
    const uint64_t last = last_activity_nanos_.load(std::memory_order_relaxed);
    return now > last ? now - last : 0;
}

bool StartupPackRecorder::ObserveSubmit(uint64_t offset, std::size_t len) {
    if (len == 0) return false;

    // Set-once, so the window is anchored to the *first* read. A plain store
    // would let a later read push the anchor forward and keep extending the
    // minimum window.
    uint64_t expected = kNoFirstRead;
    first_read_nanos_.compare_exchange_strong(expected, ElapsedNanos(),
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed);

    const uint64_t first_page = offset / overlaybd::startup::kPackPageBytes;
    const uint64_t end_page =
        (offset + static_cast<uint64_t>(len) + overlaybd::startup::kPackPageBytes - 1) /
        overlaybd::startup::kPackPageBytes;
    // Words times 64, not the page count the words were sized from: rounding
    // up to a whole word leaves spare bits, and upstream records into them
    // rather than leaving a partial word unused.
    const uint64_t bitmap_pages = static_cast<uint64_t>(bitmap_words_) * 64;

    std::vector<uint32_t> new_pages;
    for (uint64_t page = first_page; page < end_page; ++page) {
        if (page >= bitmap_pages) {
            skipped_high_pages_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        std::atomic<uint64_t>& word = bitmap_[static_cast<std::size_t>(page / 64)];
        const uint64_t mask = static_cast<uint64_t>(1) << (page % 64);
        // The exactly-once decision. Whoever observes the bit clear owns the
        // append; every other thread reading the same page sees it set and
        // appends nothing.
        if ((word.fetch_or(mask, std::memory_order_relaxed) & mask) == 0) {
            new_pages.push_back(static_cast<uint32_t>(page));
        }
    }
    if (new_pages.empty()) return false;

    total_marked_.fetch_add(new_pages.size(), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> guard(log_mutex_);
        // The cap truncates the log but not the marking: a page past the cap
        // stays marked, so it is never re-appended if the log later had room.
        const std::size_t remaining =
            max_pages_ > log_.size() ? max_pages_ - log_.size() : 0;
        const std::size_t take =
            remaining < new_pages.size() ? remaining : new_pages.size();
        log_.insert(log_.end(), new_pages.begin(), new_pages.begin() + take);
    }
    TouchActivity();
    return true;
}

void StartupPackRecorder::ObserveCompletion(uint64_t latency_nanos, uint64_t bytes,
                                            bool touched_new) {
    in_flight_.fetch_sub(1, std::memory_order_relaxed);
    // Gated on `touched_new`, which is what keeps the estimate meaningful: a
    // slow re-read of an already-recorded page says nothing about how the
    // recording was served, and counting it would inflate remote_bytes with
    // traffic the pack would never have to cover.
    if (touched_new && latency_nanos >= kRemoteLatencyThresholdNanos) {
        remote_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }
    TouchActivity();
}

RecordingVerdict StartupPackRecorder::Verdict(uint64_t min_window_nanos,
                                              uint64_t quiet_nanos,
                                              uint64_t max_window_nanos) const {
    const uint64_t first_read = first_read_nanos_.load(std::memory_order_relaxed);
    if (first_read == kNoFirstRead) {
        // No read at all. The guest may never touch the device, so the
        // no-show case is bounded by the same hard cap rather than waiting
        // forever for a window that cannot start.
        return ElapsedNanos() >= max_window_nanos ? RecordingVerdict::Finish
                                                  : RecordingVerdict::Continue;
    }

    const uint64_t now = ElapsedNanos();
    const uint64_t elapsed = now > first_read ? now - first_read : 0;
    if (elapsed < min_window_nanos) return RecordingVerdict::Continue;
    if (elapsed >= max_window_nanos) return RecordingVerdict::Finish;

    // Both conditions, not either. An idle clock alone would end the window
    // during one slow remote read -- exactly the read whose pages matter most
    // -- because nothing stamps activity between submission and completion.
    if (in_flight_.load(std::memory_order_relaxed) == 0 && IdleNanos() >= quiet_nanos) {
        return RecordingVerdict::Finish;
    }
    return RecordingVerdict::Continue;
}

std::size_t StartupPackRecorder::PageCount() const {
    std::lock_guard<std::mutex> guard(log_mutex_);
    return log_.size();
}

bool StartupPackRecorder::IsMarked(uint32_t page) const {
    const std::size_t word_index = static_cast<std::size_t>(page) / 64;
    if (word_index >= bitmap_words_) return false;
    const uint64_t mask = static_cast<uint64_t>(1) << (page % 64);
    return (bitmap_[word_index].load(std::memory_order_relaxed) & mask) != 0;
}

std::vector<uint32_t> StartupPackRecorder::OrderedPages() const {
    std::lock_guard<std::mutex> guard(log_mutex_);
    return log_;
}

core::Expected<FinalizeOutcome, std::string> StartupPackRecorder::Finalize(
    const std::string& output, const std::atomic<bool>& cancelled) const {
    std::vector<uint32_t> pages;
    {
        std::lock_guard<std::mutex> guard(log_mutex_);
        pages = log_;
    }
    if (pages.empty()) {
        // A trace with no pages describes no prefetch, so publishing one
        // would only make consumers do work to learn there is nothing to do.
        return core::make_unexpected(std::string("no pages recorded"));
    }

    std::vector<uint64_t> offsets;
    offsets.reserve(pages.size());
    for (std::size_t i = 0; i < pages.size(); ++i) {
        offsets.push_back(static_cast<uint64_t>(pages[i]) * overlaybd::startup::kPackPageBytes);
    }

    const core::Expected<std::vector<uint8_t>, std::string> trace =
        overlaybd::startup::EncodeTrace(mem_virtual_size_, offsets);
    if (!trace.ok()) {
        return core::make_unexpected("encode first-touch trace: " + trace.error());
    }

    const std::string tmp_path = TmpPackPath(output);
    const std::string bytes(reinterpret_cast<const char*>(trace.value().data()),
                            trace.value().size());
    const core::Expected<core::Unit, std::string> written = core::fs::Write(tmp_path, bytes);
    if (!written.ok()) {
        return core::make_unexpected("write trace tmp file " + tmp_path + ": " +
                                     written.error());
    }

    // Checked here, immediately before the rename, and not at the top: the
    // rename is the publish, so this is the last moment a cancel can still
    // prevent the trace from appearing. The tmp file is left for the abort
    // path, which owns both files.
    if (cancelled.load(std::memory_order_relaxed)) {
        return core::make_unexpected(
            std::string("startup trace finalize cancelled before rename"));
    }

    if (::rename(tmp_path.c_str(), output.c_str()) != 0) {
        std::ostringstream why;
        why << "move trace into place " << tmp_path << " -> " << output << ": "
            << std::strerror(errno) << " (errno " << errno << ")";
        return core::make_unexpected(why.str());
    }

    FinalizeOutcome outcome;
    outcome.pages = static_cast<uint32_t>(pages.size());
    outcome.bytes = static_cast<uint64_t>(pages.size()) * overlaybd::startup::kPackPageBytes;
    outcome.remote_bytes = RemoteBytes();
    outcome.skipped_high_pages = SkippedHighPages();
    return outcome;
}

// ---------------------------------------------------------------------------
// StartupPackReadGuard
// ---------------------------------------------------------------------------

StartupPackReadGuard::StartupPackReadGuard(std::shared_ptr<StartupPackRecorder> recorder,
                                           uint64_t bytes, bool touched_new)
    : bytes_(0), touched_new_(false), started_nanos_(0) {
    Arm(recorder, bytes, touched_new);
}

void StartupPackReadGuard::Arm(std::shared_ptr<StartupPackRecorder> recorder, uint64_t bytes,
                               bool touched_new) {
    // Refused rather than overwritten: replacing the recorder would drop the
    // first read's in-flight count, and the window's quiet check would then
    // see zero in-flight reads while one is still outstanding.
    if (recorder_ || !recorder) return;
    recorder_ = recorder;
    bytes_ = bytes;
    touched_new_ = touched_new;
    // Stamped after the increment is decided but before the caller's read, so
    // the measured latency covers the read and nothing else.
    started_nanos_ = NowNanos();
    recorder_->in_flight_.fetch_add(1, std::memory_order_relaxed);
}

StartupPackReadGuard::~StartupPackReadGuard() {
    if (!recorder_) return;
    const uint64_t now = NowNanos();
    recorder_->ObserveCompletion(now > started_nanos_ ? now - started_nanos_ : 0, bytes_,
                                 touched_new_);
}

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
