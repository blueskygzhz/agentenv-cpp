// SPDX-License-Identifier: MIT
// Tests for the open side of the file cache.
//
// The property under test is that opening a handle counts as a use of the
// entry. It matters because the second eviction tier reclaims the blocks of
// entries that are *currently open* but idle, ordered by last access: an
// entry reopened without its timestamp being refreshed is still wearing the
// age it had before, and so is the first thing thrown away -- before the
// handle that just opened it has read a single byte.
//
// Rust: storage/overlaybd/src/backend/cache/full_file_cache/cache_pool.rs
//       (`open_file_with_flags_and_size`, `open_cache_only_with_flags`,
//        `eviction_inner`)
#include "microtest.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentenv/storage/overlaybd/cache_pool.h"

using namespace agentenv::storage::overlaybd;
namespace core = agentenv::core;

namespace {

const uint64_t kBlock = 64u * 1024u;

/// Two blocks per file, so one evicted file releases more than the target and
/// the eviction loop stops after exactly one victim.
const uint64_t kFileSize = 2 * kBlock;

/// Chosen so that with two fully cached files the cache is over its watermark
/// by less than one file's worth:
///
///   water_mark   = capacity / 100 * 90 = 2185 * 90 = 196650
///   current      = 2 * 131072          = 262144
///   evict target = 262144 - 196650     = 65494  (< 131072)
///
/// `kCacheMaxFreeSpaceBytes` does not apply below 50 GiB, and `RiskMark`'s
/// midpoint (207575) sits above one file but below two, so the first file
/// caches without the backend declaring itself full.
const uint64_t kCapacity = 218500;

class PatternSource : public VirtualFile {
 public:
    explicit PatternSource(uint64_t size) : size_(size) {}

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override {
        if (offset >= size_) return ByteBuffer();
        const uint64_t remain = size_ - offset;
        const size_t take = remain < len ? static_cast<size_t>(remain) : len;
        ByteBuffer out(take, 0);
        for (size_t i = 0; i < take; ++i) {
            out[i] = static_cast<uint8_t>(((offset + i) % 251) + 1);
        }
        return out;
    }

    core::Expected<size_t, std::string> WriteAt(uint64_t, const uint8_t*, size_t) override {
        return core::make_unexpected(std::string("read-only source"));
    }

    core::Expected<uint64_t, std::string> Size() override { return size_; }
    core::Expected<core::Unit, std::string> Sync() override { return core::Unit(); }

 private:
    uint64_t size_;
};

std::string MakeTempDir() {
    char tmpl[] = "/tmp/agentenv_co_XXXXXX";
    char* made = ::mkdtemp(tmpl);
    if (made == 0) {
        std::fprintf(stderr, "  FAIL: mkdtemp\n");
        std::exit(1);
    }
    return std::string(made);
}

void RemoveTreeBestEffort(const std::string& dir) {
    const std::string cmd = "rm -rf '" + dir + "'";
    if (::system(cmd.c_str()) != 0) { /* ignored on purpose */ }
}

FileCacheBackendOptions TestOptions(const std::string& dir, uint64_t idle_secs) {
    FileCacheBackendOptions o;
    o.cache_dir = dir;
    o.capacity_bytes = kCapacity;
    o.block_size = kBlock;
    o.open_evict_idle_secs = idle_secs;
    return o;
}

uint64_t CachedBytes(const std::shared_ptr<FileCacheBackend>& backend, const std::string& name) {
    CachedFileStats stats;
    if (!backend->FileStatsBySrcName(name, &stats)) return 0;
    return stats.bytes_used;
}

}  // namespace

// ============================================================
// opening counts as a use
// ============================================================

MT_TEST(opening_a_handle_refreshes_the_entrys_lru_position) {
    // Both files are read, then left idle past `open_evict_idle_secs` with
    // their handles still open -- which is what makes them second-tier
    // candidates rather than first-tier ones.
    //
    // `a` is read *first*, so by read time it is the older of the two. It is
    // then reopened last. Read order and reopen order are deliberately
    // opposite: without the touch at open the victim is decided by the reads
    // (`a` goes), with it by the reopen (`b` goes). Nothing else in the setup
    // distinguishes the two files.
    const std::string dir = MakeTempDir();
    core::Expected<std::shared_ptr<FileCacheBackend>, std::string> backend =
        FileCacheBackend::WithOptions(TestOptions(dir, 1));
    MT_EXPECT_TRUE(backend.ok());
    if (!backend.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    std::shared_ptr<PatternSource> src_a(new PatternSource(kFileSize));
    std::shared_ptr<PatternSource> src_b(new PatternSource(kFileSize));

    core::Expected<std::shared_ptr<CachedFile>, std::string> a =
        backend.value()->OpenFile("open://a", src_a, kFileSize, 0);
    core::Expected<std::shared_ptr<CachedFile>, std::string> b =
        backend.value()->OpenFile("open://b", src_b, kFileSize, 0);
    MT_EXPECT_TRUE(a.ok() && b.ok());
    if (!a.ok() || !b.ok()) {
        if (a.ok()) a.value().reset();
        if (b.ok()) b.value().reset();
        backend.value().reset();
        RemoveTreeBestEffort(dir);
        return;
    }

    MT_EXPECT_TRUE(a.value()->ReadAt(0, static_cast<size_t>(kFileSize)).ok());
    ::usleep(150 * 1000);
    MT_EXPECT_TRUE(b.value()->ReadAt(0, static_cast<size_t>(kFileSize)).ok());

    // Both have to be fully cached, or the eviction below has nothing to
    // choose between and the assertion would hold for the wrong reason.
    MT_EXPECT_EQ(static_cast<long long>(CachedBytes(backend.value(), "open://a")),
                 static_cast<long long>(kFileSize));
    MT_EXPECT_EQ(static_cast<long long>(CachedBytes(backend.value(), "open://b")),
                 static_cast<long long>(kFileSize));

    // Past `open_evict_idle_secs = 1`, so both are now idle-open candidates.
    ::usleep(1200 * 1000);

    // The reopen under test. The first handle stays open, so this is a second
    // handle on an entry that is already busy -- exactly the case the first
    // eviction tier skips.
    core::Expected<std::shared_ptr<CachedFile>, std::string> a_again =
        backend.value()->OpenFile("open://a", src_a, kFileSize, 0);
    MT_EXPECT_TRUE(a_again.ok());

    backend.value()->EvictionInner();

    const uint64_t left_a = CachedBytes(backend.value(), "open://a");
    const uint64_t left_b = CachedBytes(backend.value(), "open://b");

    // Exactly one victim: the target is smaller than one file, so the loop
    // stops after the first. Asserted so that a run where disk pressure
    // evicted everything fails here instead of appearing to agree.
    MT_EXPECT_EQ(static_cast<long long>(left_a), static_cast<long long>(kFileSize));
    MT_EXPECT_EQ(static_cast<long long>(left_b), static_cast<long long>(0));

    if (a_again.ok()) a_again.value().reset();
    a.value().reset();
    b.value().reset();
    backend.value().reset();
    RemoveTreeBestEffort(dir);
}

MT_TEST(an_idle_open_entry_is_still_reclaimed_when_nothing_was_reopened) {
    // The control for the case above. Same setup, no reopen: the older file
    // is the victim. Without this, a broken `EvictionInner` that simply never
    // reached the second tier would let the previous test pass -- `a` would
    // survive because nothing was evicted at all.
    const std::string dir = MakeTempDir();
    core::Expected<std::shared_ptr<FileCacheBackend>, std::string> backend =
        FileCacheBackend::WithOptions(TestOptions(dir, 1));
    MT_EXPECT_TRUE(backend.ok());
    if (!backend.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    std::shared_ptr<PatternSource> src_a(new PatternSource(kFileSize));
    std::shared_ptr<PatternSource> src_b(new PatternSource(kFileSize));

    core::Expected<std::shared_ptr<CachedFile>, std::string> a =
        backend.value()->OpenFile("open://a", src_a, kFileSize, 0);
    core::Expected<std::shared_ptr<CachedFile>, std::string> b =
        backend.value()->OpenFile("open://b", src_b, kFileSize, 0);
    MT_EXPECT_TRUE(a.ok() && b.ok());
    if (!a.ok() || !b.ok()) {
        if (a.ok()) a.value().reset();
        if (b.ok()) b.value().reset();
        backend.value().reset();
        RemoveTreeBestEffort(dir);
        return;
    }

    MT_EXPECT_TRUE(a.value()->ReadAt(0, static_cast<size_t>(kFileSize)).ok());
    ::usleep(150 * 1000);
    MT_EXPECT_TRUE(b.value()->ReadAt(0, static_cast<size_t>(kFileSize)).ok());
    ::usleep(1200 * 1000);

    backend.value()->EvictionInner();

    MT_EXPECT_EQ(static_cast<long long>(CachedBytes(backend.value(), "open://a")),
                 static_cast<long long>(0));
    MT_EXPECT_EQ(static_cast<long long>(CachedBytes(backend.value(), "open://b")),
                 static_cast<long long>(kFileSize));

    a.value().reset();
    b.value().reset();
    backend.value().reset();
    RemoveTreeBestEffort(dir);
}

MT_TEST(a_busy_entry_is_not_removed_outright) {
    // The boundary between the two tiers: an open entry gives up its blocks
    // but survives as an entry, so the handle keeps working and refills on
    // its next read. Removing it would invalidate a mapping still in use.
    const std::string dir = MakeTempDir();
    core::Expected<std::shared_ptr<FileCacheBackend>, std::string> backend =
        FileCacheBackend::WithOptions(TestOptions(dir, 1));
    MT_EXPECT_TRUE(backend.ok());
    if (!backend.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    std::shared_ptr<PatternSource> src(new PatternSource(kFileSize));
    core::Expected<std::shared_ptr<CachedFile>, std::string> file =
        backend.value()->OpenFile("open://solo", src, kFileSize, 0);
    MT_EXPECT_TRUE(file.ok());
    if (!file.ok()) {
        backend.value().reset();
        RemoveTreeBestEffort(dir);
        return;
    }

    const core::Expected<ByteBuffer, std::string> first =
        file.value()->ReadAt(0, static_cast<size_t>(kFileSize));
    MT_EXPECT_TRUE(first.ok());

    // Tier 1 asked directly. The entry is open, so it must be left alone.
    const core::Expected<uint64_t, std::string> freed =
        backend.value()->EvictBySize(kFileSize);
    MT_EXPECT_TRUE(freed.ok());
    if (freed.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(freed.value()), static_cast<long long>(0));
    }
    MT_EXPECT_EQ(static_cast<long long>(CachedBytes(backend.value(), "open://solo")),
                 static_cast<long long>(kFileSize));

    // ...and the handle still reads the same bytes.
    const core::Expected<ByteBuffer, std::string> again =
        file.value()->ReadAt(0, static_cast<size_t>(kFileSize));
    MT_EXPECT_TRUE(again.ok());
    if (first.ok() && again.ok()) {
        MT_EXPECT_TRUE(first.value() == again.value());
    }

    file.value().reset();
    backend.value().reset();
    RemoveTreeBestEffort(dir);
}

MT_MAIN
