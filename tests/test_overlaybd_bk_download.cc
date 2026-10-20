// SPDX-License-Identifier: MIT
// Tests for the cache backend's ownership of the background-download
// scheduler, and for the submission entry points `image_service` drives.
//
// Rust: storage/overlaybd/src/backend/cache/{bk_download.rs,
// full_file_cache/cache_pool.rs}
#include "microtest.h"

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "agentenv/storage/overlaybd/bk_download.h"
#include "agentenv/storage/overlaybd/cache_pool.h"

using namespace agentenv::storage::overlaybd;
namespace core = agentenv::core;

namespace {

/// Deterministic source bytes plus a served-byte counter, so a test can tell
/// whether a read reached the source or was answered from the cache.
class CountingSource : public VirtualFile {
 public:
    explicit CountingSource(uint64_t size, uint32_t read_delay_us = 0)
        : size_(size), served_(0), read_delay_us_(read_delay_us) {}

    static uint8_t ByteAt(uint64_t off) { return static_cast<uint8_t>((off / 512) * 7 + 3); }

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override {
        // A download worker is otherwise far faster than the test can get to
        // its teardown, so without this the "mid-flight" cases would silently
        // be testing an already-finished download.
        if (read_delay_us_ > 0) ::usleep(static_cast<useconds_t>(read_delay_us_));
        if (offset >= size_) return ByteBuffer();
        const uint64_t remain = size_ - offset;
        const size_t take = remain < len ? static_cast<size_t>(remain) : len;
        ByteBuffer out(take, 0);
        for (size_t i = 0; i < take; ++i) out[i] = ByteAt(offset + i);
        // Read by the test thread while a scheduler thread writes it.
        served_.fetch_add(take);
        return out;
    }

    core::Expected<size_t, std::string> WriteAt(uint64_t, const uint8_t*, size_t) override {
        return core::make_unexpected(std::string("read-only source"));
    }

    core::Expected<uint64_t, std::string> Size() override { return size_; }
    core::Expected<core::Unit, std::string> Sync() override { return core::Unit(); }

    uint64_t Served() const { return served_.load(); }

 private:
    uint64_t size_;
    std::atomic<uint64_t> served_;
    uint32_t read_delay_us_;
};

std::string MakeTempDir() {
    char tmpl[] = "/tmp/agentenv_bk_XXXXXX";
    char* made = ::mkdtemp(tmpl);
    if (made == 0) {
        std::fprintf(stderr, "  FAIL: mkdtemp\n");
        std::exit(1);
    }
    return std::string(made);
}

void RemoveTreeBestEffort(const std::string& dir) {
    // Test hygiene only; a leftover directory under /tmp is not worth its own
    // recursive-delete implementation here.
    const std::string cmd = "rm -rf '" + dir + "'";
    if (::system(cmd.c_str()) != 0) { /* ignored on purpose */ }
}

const uint64_t kCacheBlock = 64u * 1024u;

FileCacheBackendOptions TestOptions(const std::string& dir) {
    FileCacheBackendOptions o;
    o.cache_dir = dir;
    o.capacity_bytes = 64u * 1024u * 1024u;
    o.block_size = kCacheBlock;
    o.bk_download_block_size = static_cast<uint32_t>(kCacheBlock);
    return o;
}

/// A config that starts immediately: the shipped default delay is 300
/// *seconds*, which would make every one of these tests time out.
DownloadConfig EagerConfig() {
    DownloadConfig c;
    c.enable = true;
    c.delay = 0;
    c.delay_extra = 1;
    c.max_mbps = 0;   // no throttle
    c.block_size = static_cast<uint32_t>(kCacheBlock);
    return c;
}

/// Polls instead of sleeping a fixed amount: the scheduler is threaded, so a
/// fixed sleep would be either flaky or slow.
bool WaitUntilFullyCached(const std::shared_ptr<CachedFile>& file, uint64_t size,
                          uint64_t timeout_ms) {
    const uint64_t step_ms = 20;
    for (uint64_t waited = 0; waited <= timeout_ms; waited += step_ms) {
        const core::Expected<uint64_t, std::string> cached =
            file->Query(0, static_cast<size_t>(size));
        if (cached.ok() && cached.value() == size) return true;
        ::usleep(static_cast<useconds_t>(step_ms * 1000));
    }
    return false;
}

}  // namespace

// ============================================================
// ownership
// ============================================================

MT_TEST(backend_owns_a_background_download_scheduler) {
    // The regression guard for the gap this file was added to close: the
    // scheduler was fully ported but `BkDownloadScheduler::New` had no
    // caller, so every background download silently had nowhere to go.
    const std::string dir = MakeTempDir();
    core::Expected<std::shared_ptr<FileCacheBackend>, std::string> backend =
        FileCacheBackend::WithOptions(TestOptions(dir));
    MT_EXPECT_TRUE(backend.ok());
    if (!backend.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }
    MT_EXPECT_TRUE(static_cast<bool>(backend.value()->BkScheduler()));
    MT_EXPECT_TRUE(!backend.value()->BkScheduler()->IsClosed());
    backend.value().reset();
    RemoveTreeBestEffort(dir);
}

MT_TEST(scheduler_takes_its_caps_from_the_options) {
    // The caps are node-level and come from the *global* download config at
    // backend creation; a per-image override must never resize them, so the
    // value the scheduler holds has to be the one the options carried.
    const std::string dir = MakeTempDir();
    FileCacheBackendOptions o = TestOptions(dir);
    o.bk_download_max_inflight_blocks = 3;
    o.bk_download_max_concurrent_files = 2;
    core::Expected<std::shared_ptr<FileCacheBackend>, std::string> backend =
        FileCacheBackend::WithOptions(o);
    MT_EXPECT_TRUE(backend.ok());
    if (!backend.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }
    MT_EXPECT_EQ(static_cast<long long>(backend.value()->BkScheduler()->MaxInflightBlocks()),
                 static_cast<long long>(3));
    backend.value().reset();
    RemoveTreeBestEffort(dir);
}

MT_TEST(default_options_carry_the_download_defaults) {
    // Rust seeds these from `DownloadConfig::default()`; drifting from it
    // would change the node's download behaviour without any config saying so.
    const FileCacheBackendOptions o;
    const DownloadConfig d;
    MT_EXPECT_EQ(static_cast<long long>(o.bk_download_max_inflight_blocks),
                 static_cast<long long>(d.max_inflight_blocks));
    MT_EXPECT_EQ(static_cast<long long>(o.bk_download_max_concurrent_files),
                 static_cast<long long>(d.max_concurrent_files));
    MT_EXPECT_EQ(static_cast<long long>(o.bk_download_block_size),
                 static_cast<long long>(d.block_size));
    MT_EXPECT_EQ(static_cast<long long>(o.bk_download_hedge_timeout_ms),
                 static_cast<long long>(kBkDefaultHedgeTimeoutMs));
}

// ============================================================
// submission
// ============================================================

MT_TEST(a_submitted_download_fills_the_cache_without_foreground_reads) {
    const std::string dir = MakeTempDir();
    core::Expected<std::shared_ptr<FileCacheBackend>, std::string> backend =
        FileCacheBackend::WithOptions(TestOptions(dir));
    MT_EXPECT_TRUE(backend.ok());
    if (!backend.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    const uint64_t size = 4 * kCacheBlock;
    std::shared_ptr<CountingSource> source(new CountingSource(size));
    core::Expected<std::shared_ptr<CachedFile>, std::string> file =
        backend.value()->OpenFile("bk://layer-a", source, size, 0);
    MT_EXPECT_TRUE(file.ok());
    if (!file.ok()) {
        backend.value().reset();
        RemoveTreeBestEffort(dir);
        return;
    }

    // Nothing cached before the download: otherwise "fully cached" afterwards
    // would prove nothing.
    core::Expected<uint64_t, std::string> before = file.value()->Query(0, static_cast<size_t>(size));
    MT_EXPECT_TRUE(before.ok());
    if (before.ok()) MT_EXPECT_EQ(static_cast<long long>(before.value()), static_cast<long long>(0));

    const core::Expected<core::Unit, std::string> submitted = SubmitBkDownload(
        backend.value().get(), file.value(), EagerConfig(), core::Optional<std::string>());
    MT_EXPECT_TRUE(submitted.ok());
    if (!submitted.ok()) {
        std::fprintf(stderr, "  submit: %s\n", submitted.error().c_str());
        backend.value().reset();
        RemoveTreeBestEffort(dir);
        return;
    }

    MT_EXPECT_TRUE(WaitUntilFullyCached(file.value(), size, 15000));

    // And the bytes are the source's, read back without touching it again:
    // `kRwV2CacheOnly` refuses to refill, so a wrong answer here cannot be
    // masked by an on-demand read.
    const uint64_t served_after_download = source->Served();
    const core::Expected<ByteBuffer, std::string> got =
        file.value()->ReadAtWithFlags(kCacheBlock, static_cast<size_t>(kCacheBlock),
                                      kRwV2CacheOnly);
    MT_EXPECT_TRUE(got.ok());
    if (got.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(got.value().size()),
                     static_cast<long long>(kCacheBlock));
        bool same = got.value().size() == kCacheBlock;
        for (size_t i = 0; same && i < got.value().size(); ++i) {
            if (got.value()[i] != CountingSource::ByteAt(kCacheBlock + i)) same = false;
        }
        MT_EXPECT_TRUE(same);
    }
    MT_EXPECT_EQ(static_cast<long long>(source->Served()),
                 static_cast<long long>(served_after_download));

    // `~CachedFile` dereferences its raw `FileCacheBackend*` to decrement the
    // entry's open count, so every handle has to go first. Letting `file` die
    // at end of scope would destroy it *after* the backend.
    file.value().reset();
    backend.value().reset();
    RemoveTreeBestEffort(dir);
}

MT_TEST(a_shut_down_scheduler_refuses_submission) {
    // `image_service` keys its "fall back to foreground-only" warning off
    // exactly this condition, so the message has to be the closed one and not
    // a generic failure.
    const std::string dir = MakeTempDir();
    core::Expected<std::shared_ptr<FileCacheBackend>, std::string> backend =
        FileCacheBackend::WithOptions(TestOptions(dir));
    MT_EXPECT_TRUE(backend.ok());
    if (!backend.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    const uint64_t size = 2 * kCacheBlock;
    std::shared_ptr<CountingSource> source(new CountingSource(size));
    core::Expected<std::shared_ptr<CachedFile>, std::string> file =
        backend.value()->OpenFile("bk://layer-b", source, size, 0);
    MT_EXPECT_TRUE(file.ok());
    if (!file.ok()) {
        backend.value().reset();
        RemoveTreeBestEffort(dir);
        return;
    }

    backend.value()->ShutdownBkDownloads();
    MT_EXPECT_TRUE(backend.value()->BkScheduler()->IsClosed());

    const core::Expected<core::Unit, std::string> submitted = SubmitBkDownload(
        backend.value().get(), file.value(), EagerConfig(), core::Optional<std::string>());
    MT_EXPECT_TRUE(!submitted.ok());
    if (!submitted.ok()) {
        MT_EXPECT_TRUE(submitted.error() == std::string(kBkSubmitClosedMessage));
    }

    file.value().reset();
    backend.value().reset();
    RemoveTreeBestEffort(dir);
}

MT_TEST(shutdown_leaves_no_task_registered) {
    // The drain is what makes `~FileCacheBackend` safe: a task still in the
    // registry means a worker may yet pick it up and dereference the raw
    // backend pointer it carries.
    const std::string dir = MakeTempDir();
    core::Expected<std::shared_ptr<FileCacheBackend>, std::string> backend =
        FileCacheBackend::WithOptions(TestOptions(dir));
    MT_EXPECT_TRUE(backend.ok());
    if (!backend.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    const uint64_t size = 8 * kCacheBlock;
    std::shared_ptr<CountingSource> source(new CountingSource(size));
    core::Expected<std::shared_ptr<CachedFile>, std::string> file =
        backend.value()->OpenFile("bk://layer-c", source, size, 0);
    MT_EXPECT_TRUE(file.ok());
    if (!file.ok()) {
        backend.value().reset();
        RemoveTreeBestEffort(dir);
        return;
    }

    MT_EXPECT_TRUE(SubmitBkDownload(backend.value().get(), file.value(), EagerConfig(),
                                    core::Optional<std::string>()).ok());
    backend.value()->ShutdownBkDownloads();
    MT_EXPECT_EQ(static_cast<long long>(backend.value()->BkScheduler()->RegisteredCount()),
                 static_cast<long long>(0));

    file.value().reset();
    backend.value().reset();
    RemoveTreeBestEffort(dir);
}

MT_TEST(destroying_the_backend_stops_the_source_being_read) {
    // The lifetime property stated in `~FileCacheBackend`: once it returns,
    // no worker is inside a task. Observed through the source, which the test
    // keeps alive on purpose -- if a worker were still running it would keep
    // serving bytes after the backend is gone.
    const std::string dir = MakeTempDir();
    core::Expected<std::shared_ptr<FileCacheBackend>, std::string> backend =
        FileCacheBackend::WithOptions(TestOptions(dir));
    MT_EXPECT_TRUE(backend.ok());
    if (!backend.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    // Big enough, and slow enough per read, that the download cannot possibly
    // finish before teardown. Mutation-checked: with a fast in-memory source
    // the download completed first, and removing the drain from
    // `~FileCacheBackend` left this test still passing -- i.e. it was
    // asserting nothing.
    const uint64_t size = 256 * kCacheBlock;
    std::shared_ptr<CountingSource> source(new CountingSource(size, 2000));
    core::Expected<std::shared_ptr<CachedFile>, std::string> file =
        backend.value()->OpenFile("bk://layer-d", source, size, 0);
    MT_EXPECT_TRUE(file.ok());
    if (!file.ok()) {
        backend.value().reset();
        RemoveTreeBestEffort(dir);
        return;
    }

    MT_EXPECT_TRUE(SubmitBkDownload(backend.value().get(), file.value(), EagerConfig(),
                                    core::Optional<std::string>()).ok());

    // Wait until a worker has demonstrably started, so the teardown below
    // really does happen mid-flight.
    bool started = false;
    for (int i = 0; i < 500 && !started; ++i) {
        if (source->Served() > 0) started = true;
        else ::usleep(10 * 1000);
    }
    MT_EXPECT_TRUE(started);
    // ...and has demonstrably not finished, or there would be nothing left to
    // stop and the assertion below would hold for the wrong reason.
    MT_EXPECT_TRUE(source->Served() < size);

    file.value().reset();
    backend.value().reset();

    const uint64_t served_at_teardown = source->Served();
    ::usleep(300 * 1000);
    MT_EXPECT_EQ(static_cast<long long>(source->Served()),
                 static_cast<long long>(served_at_teardown));
    RemoveTreeBestEffort(dir);
}

// ============================================================
// closing a handle counts as a use
// ============================================================

MT_TEST(closing_a_handle_refreshes_its_lru_position) {
    // Rust `remove_open_file` touches the entry as well as decrementing the
    // open count. Observable because tier-1 eviction only ever considers
    // *closed* entries (`EntryIsBusy` excludes open ones), so an entry enters
    // the candidate list at the moment it is closed -- carrying a stale read
    // timestamp if the touch is missing, and going first.
    //
    // Read order and close order are deliberately opposite: that is what
    // separates the two behaviours. Without the touch the order is decided by
    // the reads (a before b, so `a` is evicted); with it, by the closes (b
    // before a, so `b` is evicted).
    const std::string dir = MakeTempDir();
    core::Expected<std::shared_ptr<FileCacheBackend>, std::string> backend =
        FileCacheBackend::WithOptions(TestOptions(dir));
    MT_EXPECT_TRUE(backend.ok());
    if (!backend.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    const uint64_t size = 2 * kCacheBlock;
    std::shared_ptr<CountingSource> source_a(new CountingSource(size));
    std::shared_ptr<CountingSource> source_b(new CountingSource(size));
    core::Expected<std::shared_ptr<CachedFile>, std::string> a =
        backend.value()->OpenFile("lru://a", source_a, size, 0);
    core::Expected<std::shared_ptr<CachedFile>, std::string> b =
        backend.value()->OpenFile("lru://b", source_b, size, 0);
    MT_EXPECT_TRUE(a.ok() && b.ok());
    if (!a.ok() || !b.ok()) {
        if (a.ok()) a.value().reset();
        if (b.ok()) b.value().reset();
        backend.value().reset();
        RemoveTreeBestEffort(dir);
        return;
    }

    // `a` is read first, so it is the older of the two by read time.
    MT_EXPECT_TRUE(a.value()->ReadAt(0, static_cast<size_t>(size)).ok());
    ::usleep(20 * 1000);
    MT_EXPECT_TRUE(b.value()->ReadAt(0, static_cast<size_t>(size)).ok());

    // Both must actually hold bytes, or neither is an eviction candidate and
    // the assertion below would pass for the wrong reason.
    CachedFileStats stats_a;
    CachedFileStats stats_b;
    MT_EXPECT_TRUE(backend.value()->FileStatsBySrcName("lru://a", &stats_a));
    MT_EXPECT_TRUE(backend.value()->FileStatsBySrcName("lru://b", &stats_b));
    MT_EXPECT_TRUE(stats_a.bytes_used > 0);
    MT_EXPECT_TRUE(stats_b.bytes_used > 0);

    // Closed in the opposite order.
    ::usleep(20 * 1000);
    b.value().reset();
    ::usleep(20 * 1000);
    a.value().reset();

    // Enough to reclaim one file but not both.
    const core::Expected<uint64_t, std::string> freed =
        backend.value()->EvictBySize(stats_b.bytes_used);
    MT_EXPECT_TRUE(freed.ok());

    CachedFileStats after_a;
    CachedFileStats after_b;
    const bool a_alive =
        backend.value()->FileStatsBySrcName("lru://a", &after_a) && after_a.bytes_used > 0;
    const bool b_alive =
        backend.value()->FileStatsBySrcName("lru://b", &after_b) && after_b.bytes_used > 0;

    // `b` was closed first, so `b` is the colder one and goes.
    MT_EXPECT_TRUE(a_alive);
    MT_EXPECT_TRUE(!b_alive);

    backend.value().reset();
    RemoveTreeBestEffort(dir);
}

MT_MAIN
