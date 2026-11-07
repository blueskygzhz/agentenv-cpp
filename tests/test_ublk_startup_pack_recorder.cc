// SPDX-License-Identifier: MIT
// Tests for storage/ublk's first-touch recorder.
// Rust: storage/ublk/src/impls/startup_pack_recorder.rs (mod tests)
//
// None of this needs a ublk kernel: the recorder is fed offsets and lengths,
// and everything it produces is either an in-memory log or a trace file. What
// it guards is subtle enough to be worth covering directly -- an exactly-once
// append under contention, and a first-touch *order* that no later pass can
// reconstruct if it is lost.
#include "agentenv/storage/ublk/startup_pack_recorder.h"

#include <unistd.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/startup_pack.h"
#include "microtest.h"

namespace ublk = agentenv::storage::ublk;
namespace startup = agentenv::storage::overlaybd::startup;
namespace core = agentenv::core;

namespace {

const uint64_t kPage = startup::kPackPageBytes;

struct Scratch {
    std::string dir;

    Scratch() {
        const core::Expected<std::string, std::string> made =
            core::fs::CreateTempDir("agentenv-pack-recorder-test");
        if (made.ok()) dir = made.value();
    }
    ~Scratch() {
        if (!dir.empty()) core::fs::RemoveDirAll(dir);
    }
    std::string Path(const char* leaf) const { return core::fs::Join(dir, leaf); }
};

bool SamePages(const std::vector<uint32_t>& got, const uint32_t* want, std::size_t n) {
    if (got.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (got[i] != want[i]) return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Marking and ordering
// ---------------------------------------------------------------------------

MT_TEST(recorder_appends_each_page_once_in_first_touch_order) {
    ublk::StartupPackRecorder rec(1u << 30, 1024);
    rec.ObserveSubmit(0x4000, 0x2000);  // pages 4, 5
    rec.ObserveSubmit(0x1000, 0x1000);  // page 1
    rec.ObserveSubmit(0x4000, 0x1000);  // already seen: zero appends

    // Order is the product. Page 1 comes *after* 4 and 5 because that is the
    // order the device saw them, and that ranking is the only record of what
    // the boot needed first -- sorting it would destroy the information the
    // pack layout is planned from.
    const uint32_t want[3] = {4, 5, 1};
    MT_EXPECT_TRUE(SamePages(rec.OrderedPages(), want, 3));
    MT_EXPECT_EQ(rec.PageCount(), static_cast<std::size_t>(3));
}

MT_TEST(recorder_returns_whether_a_read_touched_new_pages) {
    ublk::StartupPackRecorder rec(1u << 30, 1024);
    // The return value gates the remote-bytes estimate, so it has to mean
    // "this read marked something new", not "this read was recorded".
    MT_EXPECT_TRUE(rec.ObserveSubmit(0, kPage));
    MT_EXPECT_TRUE(!rec.ObserveSubmit(0, kPage));
    MT_EXPECT_TRUE(rec.ObserveSubmit(0, 2 * kPage));  // page 1 is new
    MT_EXPECT_TRUE(!rec.ObserveSubmit(0, 2 * kPage));
}

MT_TEST(recorder_covers_unaligned_reads_page_granular) {
    ublk::StartupPackRecorder rec(1u << 30, 1024);
    // Spans pages 1 and 2: a read is recorded by the pages it *touches*, not
    // by the page its offset lands in. Rounding down instead would leave the
    // tail page out of the pack and fault on it at resume.
    rec.ObserveSubmit(0x1800, 0x1000);
    MT_EXPECT_TRUE(rec.IsMarked(1));
    MT_EXPECT_TRUE(rec.IsMarked(2));
    MT_EXPECT_TRUE(!rec.IsMarked(3));
    const uint32_t want[2] = {1, 2};
    MT_EXPECT_TRUE(SamePages(rec.OrderedPages(), want, 2));
}

MT_TEST(zero_length_reads_are_ignored) {
    ublk::StartupPackRecorder rec(1u << 30, 1024);
    MT_EXPECT_TRUE(!rec.ObserveSubmit(0x1000, 0));
    MT_EXPECT_EQ(rec.PageCount(), static_cast<std::size_t>(0));
    // And it must not start the window either: a zero-length read is not a
    // fault, so anchoring the minimum window to it would cut the recording
    // short.
    MT_EXPECT_TRUE(rec.Verdict(60ull * 1000000000ull, 1000000, 120ull * 1000000000ull) ==
                   ublk::RecordingVerdict::Continue);
}

MT_TEST(the_log_is_capped_but_marking_continues) {
    ublk::StartupPackRecorder rec(1u << 30, 2);
    rec.ObserveSubmit(0, 3 * kPage);  // 3 pages, log capacity 2

    MT_EXPECT_EQ(rec.PageCount(), static_cast<std::size_t>(2));
    // Still marked, which is what keeps the cap from corrupting the order: a
    // page dropped from the log must never be appended later, or it would
    // appear with a first-touch rank it did not earn.
    MT_EXPECT_TRUE(rec.IsMarked(2));
    MT_EXPECT_EQ(rec.TotalMarked(), static_cast<uint64_t>(3));

    rec.ObserveSubmit(2 * kPage, kPage);
    MT_EXPECT_EQ(rec.PageCount(), static_cast<std::size_t>(2));
}

MT_TEST(pages_above_the_bitmap_cap_are_counted_not_recorded) {
    // A 32 KiB image is 8 pages, so the bitmap is one word wide: 64 pages of
    // coverage after rounding up.
    ublk::StartupPackRecorder rec(32 * 1024, 1024);

    MT_EXPECT_TRUE(rec.ObserveSubmit(0, kPage));
    MT_EXPECT_EQ(rec.SkippedHighPages(), static_cast<uint64_t>(0));

    // Page 64 is past the bitmap. Counted rather than silently dropped: the
    // count is the only signal that the trace is incomplete, and a trace that
    // looks complete but is not would have the pack miss pages at resume.
    MT_EXPECT_TRUE(!rec.ObserveSubmit(64 * kPage, kPage));
    MT_EXPECT_EQ(rec.SkippedHighPages(), static_cast<uint64_t>(1));
    MT_EXPECT_EQ(rec.PageCount(), static_cast<std::size_t>(1));
}

MT_TEST(concurrent_submits_append_no_duplicate_pages) {
    // The exactly-once property, which is the one thing a single-threaded
    // test cannot show. A duplicate would not merely waste a slot: it would
    // shift the first-touch rank of every page after it.
    const std::shared_ptr<ublk::StartupPackRecorder> rec(
        new ublk::StartupPackRecorder(1u << 30, 1u << 20));

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.push_back(std::thread([rec]() {
            for (uint64_t i = 0; i < 4096; ++i) {
                rec->ObserveSubmit((i % 4096) * kPage, static_cast<std::size_t>(kPage));
            }
        }));
    }
    for (std::size_t i = 0; i < threads.size(); ++i) threads[i].join();

    std::vector<uint32_t> pages = rec->OrderedPages();
    MT_EXPECT_EQ(pages.size(), static_cast<std::size_t>(4096));
    std::vector<bool> seen(4096, false);
    bool duplicate = false;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (pages[i] < 4096 && seen[pages[i]]) duplicate = true;
        if (pages[i] < 4096) seen[pages[i]] = true;
    }
    MT_EXPECT_TRUE(!duplicate);
    MT_EXPECT_EQ(rec->TotalMarked(), static_cast<uint64_t>(4096));
}

// ---------------------------------------------------------------------------
// Window state machine
// ---------------------------------------------------------------------------

MT_TEST(window_waits_for_the_minimum_and_honours_the_hard_cap) {
    ublk::StartupPackRecorder rec(1u << 30, 1024);
    rec.ObserveSubmit(0, kPage);

    // Minimum not reached: a boot that happens to pause early must not end
    // the recording.
    MT_EXPECT_TRUE(rec.Verdict(60ull * 1000000000ull, 0, 120ull * 1000000000ull) ==
                   ublk::RecordingVerdict::Continue);
    // Hard cap always wins, so a guest that never goes quiet still terminates.
    MT_EXPECT_TRUE(rec.Verdict(0, 0, 0) == ublk::RecordingVerdict::Finish);
}

MT_TEST(a_recorder_that_saw_no_read_is_bounded_by_the_same_cap) {
    ublk::StartupPackRecorder rec(1u << 30, 1024);
    // The guest may never touch the device. Without this the window would
    // wait forever for a start that never comes, and the throwaway VM would
    // never be reclaimed.
    MT_EXPECT_TRUE(rec.Verdict(0, 1000000, 0) == ublk::RecordingVerdict::Finish);
    MT_EXPECT_TRUE(rec.Verdict(0, 1000000, 60ull * 1000000000ull) ==
                   ublk::RecordingVerdict::Continue);
}

MT_TEST(quiet_requires_both_no_new_pages_and_no_in_flight_read) {
    const std::shared_ptr<ublk::StartupPackRecorder> rec(
        new ublk::StartupPackRecorder(1u << 30, 1024));
    rec->ObserveSubmit(0, kPage);
    ::usleep(5000);

    const uint64_t quiet = 1000000;  // 1 ms
    const uint64_t cap = 60ull * 1000000000ull;
    {
        ublk::StartupPackReadGuard guard(rec, kPage, true);
        // An in-flight read must suppress quiet. Nothing stamps the activity
        // clock between a submission and its completion, so an idle clock
        // alone would end the window in the middle of one slow remote read --
        // precisely the read whose pages matter most.
        MT_EXPECT_TRUE(rec->Verdict(0, quiet, cap) == ublk::RecordingVerdict::Continue);
    }
    ::usleep(5000);
    MT_EXPECT_TRUE(rec->Verdict(0, quiet, cap) == ublk::RecordingVerdict::Finish);
}

MT_TEST(a_slow_read_on_new_pages_feeds_the_remote_estimate) {
    const std::shared_ptr<ublk::StartupPackRecorder> rec(
        new ublk::StartupPackRecorder(1u << 30, 1024));

    // Fast: below the 10 ms threshold, so it reads as locally served.
    {
        const bool touched = rec->ObserveSubmit(0, static_cast<std::size_t>(kPage));
        ublk::StartupPackReadGuard guard(rec, kPage, touched);
    }
    MT_EXPECT_EQ(rec->RemoteBytes(), static_cast<uint64_t>(0));

    // Slow, and on a page already recorded. Must *not* count: a slow re-read
    // says nothing about how the recording was served, and counting it would
    // inflate the estimate with traffic the pack would never have to cover.
    {
        const bool touched = rec->ObserveSubmit(0, static_cast<std::size_t>(kPage));
        MT_EXPECT_TRUE(!touched);
        ublk::StartupPackReadGuard guard(rec, kPage, touched);
        ::usleep(12000);
    }
    MT_EXPECT_EQ(rec->RemoteBytes(), static_cast<uint64_t>(0));

    // Slow and on a new page: counted.
    {
        const bool touched = rec->ObserveSubmit(kPage, static_cast<std::size_t>(kPage));
        MT_EXPECT_TRUE(touched);
        ublk::StartupPackReadGuard guard(rec, kPage, touched);
        ::usleep(12000);
    }
    MT_EXPECT_EQ(rec->RemoteBytes(), kPage);
}

MT_TEST(a_disarmed_guard_settles_nothing) {
    const std::shared_ptr<ublk::StartupPackRecorder> rec(
        new ublk::StartupPackRecorder(1u << 30, 1024));
    rec->ObserveSubmit(0, kPage);
    ::usleep(5000);

    {
        // What the read path leaves behind when no recording is armed.
        ublk::StartupPackReadGuard idle;
        (void)idle;
    }
    // In particular it must not have decremented the in-flight count, which
    // would make the quiet check fire while a real read was outstanding.
    MT_EXPECT_TRUE(rec->Verdict(0, 1000000, 60ull * 1000000000ull) ==
                   ublk::RecordingVerdict::Finish);
}

// ---------------------------------------------------------------------------
// Finalize
// ---------------------------------------------------------------------------

MT_TEST(finalize_writes_a_decodable_first_touch_trace) {
    Scratch scratch;
    const std::string output = scratch.Path("memory-startup.trace");
    ublk::StartupPackRecorder rec(1u << 30, 1024);
    rec.ObserveSubmit(0x4000, 0x2000);  // pages 4, 5
    rec.ObserveSubmit(0x1000, 0x1000);  // page 1

    const std::atomic<bool> cancelled(false);
    const core::Expected<ublk::FinalizeOutcome, std::string> outcome =
        rec.Finalize(output, cancelled);
    MT_EXPECT_TRUE(outcome.ok());
    MT_EXPECT_EQ(outcome.value().pages, static_cast<uint32_t>(3));
    MT_EXPECT_EQ(outcome.value().bytes, 3 * kPage);

    const core::Expected<std::string, std::string> raw = core::fs::ReadToString(output);
    MT_EXPECT_TRUE(raw.ok());
    const core::Expected<std::pair<uint64_t, std::vector<uint64_t> >, std::string> decoded =
        startup::DecodeTrace(reinterpret_cast<const uint8_t*>(raw.value().data()),
                             raw.value().size());
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_EQ(decoded.value().first, static_cast<uint64_t>(1u << 30));
    // Round-tripped in first-touch order, not sorted.
    MT_EXPECT_EQ(decoded.value().second.size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(decoded.value().second[0], static_cast<uint64_t>(0x4000));
    MT_EXPECT_EQ(decoded.value().second[1], static_cast<uint64_t>(0x5000));
    MT_EXPECT_EQ(decoded.value().second[2], static_cast<uint64_t>(0x1000));

    // And the staging file is gone, because the publish was a rename.
    MT_EXPECT_TRUE(!core::fs::Exists(ublk::TmpPackPath(output)));
}

MT_TEST(a_cancelled_finalize_never_publishes) {
    Scratch scratch;
    const std::string output = scratch.Path("memory-startup.trace");
    ublk::StartupPackRecorder rec(1u << 30, 1024);
    rec.ObserveSubmit(0, kPage);

    const std::atomic<bool> cancelled(true);
    MT_EXPECT_TRUE(!rec.Finalize(output, cancelled).ok());
    // The rename is the publish, so a cancel before it must leave nothing at
    // the output path for a consumer to pick up.
    MT_EXPECT_TRUE(!core::fs::Exists(output));
    // The tmp file is left for the abort path, which owns both names; removing
    // it here would race that cleanup.
    MT_EXPECT_TRUE(core::fs::Exists(ublk::TmpPackPath(output)));
}

MT_TEST(finalize_without_pages_fails) {
    Scratch scratch;
    const std::string output = scratch.Path("memory-startup.trace");
    ublk::StartupPackRecorder rec(1u << 30, 1024);

    const std::atomic<bool> cancelled(false);
    // A trace with no pages describes no prefetch, so publishing one would
    // only make consumers do work to learn there is nothing to do.
    MT_EXPECT_TRUE(!rec.Finalize(output, cancelled).ok());
    MT_EXPECT_TRUE(!core::fs::Exists(output));
}

MT_TEST(finalize_rejects_offsets_past_the_image) {
    // A 32 KiB image with a one-word bitmap covers 64 pages, so pages well
    // past the image are recordable -- but not encodable. The failure has to
    // come from the encoder rather than producing a trace whose offsets
    // address memory the image does not have.
    Scratch scratch;
    const std::string output = scratch.Path("memory-startup.trace");
    ublk::StartupPackRecorder rec(32 * 1024, 1024);
    rec.ObserveSubmit(40 * kPage, static_cast<std::size_t>(kPage));
    MT_EXPECT_EQ(rec.PageCount(), static_cast<std::size_t>(1));

    const std::atomic<bool> cancelled(false);
    MT_EXPECT_TRUE(!rec.Finalize(output, cancelled).ok());
    MT_EXPECT_TRUE(!core::fs::Exists(output));
}

MT_TEST(tmp_pack_path_stays_in_the_output_directory) {
    // Both names must share a directory: that is what makes the publish a
    // rename rather than a cross-filesystem copy, and a rename is the only
    // step that is atomic enough to be the publish.
    MT_EXPECT_TRUE(ublk::TmpPackPath("/a/b/memory-startup.pack") ==
                   "/a/b/memory-startup.pack.tmp");
}

MT_MAIN
