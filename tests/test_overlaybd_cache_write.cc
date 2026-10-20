// SPDX-License-Identifier: MIT
// Tests for the file cache's write path: publishing caller-supplied bytes as
// cached blocks.
//
// The interesting property is that a write smaller than a cache block has to
// be merged into that block rather than dropped -- the block is the unit of
// both the bitmap and the mapping, so publishing a partially written block
// would serve its untouched remainder as if it were data.
//
// Rust: storage/overlaybd/src/backend/cache/full_file_cache/cache_store.rs
//       (`cache_refill_with_data_generic`, `write_at_with_flags`)
#include "microtest.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "agentenv/storage/overlaybd/cache_pool.h"

using namespace agentenv::storage::overlaybd;
namespace core = agentenv::core;

namespace {

const uint64_t kBlock = 64u * 1024u;
const uint64_t kPage = 4096u;

/// A source with a position-dependent pattern, so a test can tell source
/// bytes from written bytes and from zero-fill.
class PatternSource : public VirtualFile {
 public:
    explicit PatternSource(uint64_t size) : size_(size) {}

    static uint8_t ByteAt(uint64_t off) {
        // Never 0x00, and never either of the two values the tests write, so
        // a wrong byte cannot be mistaken for a right one.
        return static_cast<uint8_t>((off % 251) + 1);
    }

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override {
        if (offset >= size_) return ByteBuffer();
        const uint64_t remain = size_ - offset;
        const size_t take = remain < len ? static_cast<size_t>(remain) : len;
        ByteBuffer out(take, 0);
        for (size_t i = 0; i < take; ++i) out[i] = ByteAt(offset + i);
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
    char tmpl[] = "/tmp/agentenv_cw_XXXXXX";
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

FileCacheBackendOptions TestOptions(const std::string& dir, uint64_t capacity) {
    FileCacheBackendOptions o;
    o.cache_dir = dir;
    o.capacity_bytes = capacity;
    o.block_size = kBlock;
    return o;
}

/// One open file over a fresh backend, with the plumbing every case repeats.
struct Fixture {
    std::string dir;
    std::shared_ptr<FileCacheBackend> backend;
    std::shared_ptr<PatternSource> source;
    std::shared_ptr<CachedFile> file;
    uint64_t size;

    Fixture() : size(0) {}

    bool Open(uint64_t source_size, uint64_t capacity) {
        dir = MakeTempDir();
        size = source_size;
        core::Expected<std::shared_ptr<FileCacheBackend>, std::string> made =
            FileCacheBackend::WithOptions(TestOptions(dir, capacity));
        if (!made.ok()) return false;
        backend = made.value();
        source.reset(new PatternSource(source_size));
        core::Expected<std::shared_ptr<CachedFile>, std::string> opened =
            backend->OpenFile("cw://blob", source, source_size, 0);
        if (!opened.ok()) return false;
        file = opened.value();
        return true;
    }

    /// Drops the source so a later read can only be answered from the cache.
    /// Without this a plain read would silently refill, and every assertion
    /// about cached content would hold whether or not the write landed.
    void DropSource() { file->SetSource(std::shared_ptr<VirtualFile>()); }

    ~Fixture() {
        // The handle first: `~CachedFile` dereferences its raw backend
        // pointer to release the entry's open count.
        file.reset();
        backend.reset();
        if (!dir.empty()) RemoveTreeBestEffort(dir);
    }
};

/// Reports the first differing index, or -1. Used instead of a bare bool so a
/// failure says *where* the block went wrong.
long long FirstMismatch(const uint8_t* got, const uint8_t* want, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (got[i] != want[i]) return static_cast<long long>(i);
    }
    return -1;
}

}  // namespace

// ============================================================
// a write smaller than a block
// ============================================================

MT_TEST(a_sub_block_write_is_merged_into_its_block) {
    // The write covers one page of a 64 KiB block. For the block to be
    // publishable the other 60 KiB have to come from somewhere, and the only
    // correct answer is the source -- which is what makes this a
    // read-modify-write rather than a write.
    Fixture fx;
    MT_EXPECT_TRUE(fx.Open(4 * kBlock, 64u * 1024u * 1024u));
    if (!fx.file) return;

    std::vector<uint8_t> payload(static_cast<size_t>(kPage), 0xA5);
    const core::Expected<size_t, std::string> wrote =
        fx.file->WriteAt(0, &payload[0], payload.size());
    MT_EXPECT_TRUE(wrote.ok());
    if (!wrote.ok()) return;
    MT_EXPECT_EQ(static_cast<long long>(wrote.value()),
                 static_cast<long long>(kPage));

    // The whole block must now be cached, or the bitmap is claiming a block
    // it cannot serve.
    const core::Expected<uint64_t, std::string> cached =
        fx.file->Query(0, static_cast<size_t>(kBlock));
    MT_EXPECT_TRUE(cached.ok());
    if (cached.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(cached.value()),
                     static_cast<long long>(kBlock));
    }

    fx.DropSource();
    const core::Expected<ByteBuffer, std::string> read =
        fx.file->ReadAt(0, static_cast<size_t>(kBlock));
    MT_EXPECT_TRUE(read.ok());
    if (!read.ok()) return;
    MT_EXPECT_EQ(static_cast<long long>(read.value().size()),
                 static_cast<long long>(kBlock));

    std::vector<uint8_t> want(static_cast<size_t>(kBlock), 0);
    for (size_t i = 0; i < want.size(); ++i) want[i] = PatternSource::ByteAt(i);
    for (size_t i = 0; i < payload.size(); ++i) want[i] = 0xA5;

    MT_EXPECT_EQ(FirstMismatch(&read.value()[0], &want[0], want.size()),
                 static_cast<long long>(-1));
}

MT_TEST(a_write_inside_a_block_leaves_the_bytes_around_it_alone) {
    // Offset 4096 is page-aligned -- which is all `WriteAt` requires -- but
    // not block-aligned. The merge has to place the payload at the right
    // offset *within* the block, so this is the case that catches a
    // read-modify-write that writes the payload to the block's start.
    Fixture fx;
    MT_EXPECT_TRUE(fx.Open(4 * kBlock, 64u * 1024u * 1024u));
    if (!fx.file) return;

    std::vector<uint8_t> payload(static_cast<size_t>(kPage), 0x5A);
    const core::Expected<size_t, std::string> wrote =
        fx.file->WriteAt(kPage, &payload[0], payload.size());
    MT_EXPECT_TRUE(wrote.ok());
    if (!wrote.ok()) return;
    MT_EXPECT_EQ(static_cast<long long>(wrote.value()),
                 static_cast<long long>(kPage));

    fx.DropSource();
    const core::Expected<ByteBuffer, std::string> read =
        fx.file->ReadAt(0, static_cast<size_t>(kBlock));
    MT_EXPECT_TRUE(read.ok());
    if (!read.ok()) return;

    std::vector<uint8_t> want(static_cast<size_t>(kBlock), 0);
    for (size_t i = 0; i < want.size(); ++i) want[i] = PatternSource::ByteAt(i);
    for (size_t i = 0; i < payload.size(); ++i) {
        want[static_cast<size_t>(kPage) + i] = 0x5A;
    }

    MT_EXPECT_EQ(FirstMismatch(&read.value()[0], &want[0], want.size()),
                 static_cast<long long>(-1));
}

MT_TEST(a_write_spanning_two_blocks_lands_in_both) {
    // Starts one page before a block boundary and ends one page after it, so
    // neither block is written whole and both need merging.
    Fixture fx;
    MT_EXPECT_TRUE(fx.Open(4 * kBlock, 64u * 1024u * 1024u));
    if (!fx.file) return;

    const uint64_t at = kBlock - kPage;
    std::vector<uint8_t> payload(static_cast<size_t>(2 * kPage), 0x3C);
    const core::Expected<size_t, std::string> wrote =
        fx.file->WriteAt(at, &payload[0], payload.size());
    MT_EXPECT_TRUE(wrote.ok());
    if (!wrote.ok()) return;
    MT_EXPECT_EQ(static_cast<long long>(wrote.value()),
                 static_cast<long long>(payload.size()));

    const core::Expected<uint64_t, std::string> cached =
        fx.file->Query(0, static_cast<size_t>(2 * kBlock));
    MT_EXPECT_TRUE(cached.ok());
    if (cached.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(cached.value()),
                     static_cast<long long>(2 * kBlock));
    }

    fx.DropSource();
    const core::Expected<ByteBuffer, std::string> read =
        fx.file->ReadAt(0, static_cast<size_t>(2 * kBlock));
    MT_EXPECT_TRUE(read.ok());
    if (!read.ok()) return;

    std::vector<uint8_t> want(static_cast<size_t>(2 * kBlock), 0);
    for (size_t i = 0; i < want.size(); ++i) want[i] = PatternSource::ByteAt(i);
    for (size_t i = 0; i < payload.size(); ++i) {
        want[static_cast<size_t>(at) + i] = 0x3C;
    }

    MT_EXPECT_EQ(FirstMismatch(&read.value()[0], &want[0], want.size()),
                 static_cast<long long>(-1));
}

// ============================================================
// accounting
// ============================================================

MT_TEST(rewriting_a_cached_block_does_not_inflate_the_accounting) {
    // The byte counter drives the eviction watermark, so counting a rewrite
    // as new bytes does not merely misreport: it walks the cache toward a
    // permanent "full" verdict and evicts blocks it did not need to.
    Fixture fx;
    MT_EXPECT_TRUE(fx.Open(4 * kBlock, 64u * 1024u * 1024u));
    if (!fx.file) return;

    std::vector<uint8_t> payload(static_cast<size_t>(kBlock), 0x11);
    MT_EXPECT_TRUE(fx.file->WriteAt(0, &payload[0], payload.size()).ok());

    const uint64_t after_first = fx.backend->Stats().bytes_used;
    MT_EXPECT_EQ(static_cast<long long>(after_first), static_cast<long long>(kBlock));

    // Same block, different contents.
    std::vector<uint8_t> again(static_cast<size_t>(kBlock), 0x22);
    MT_EXPECT_TRUE(fx.file->WriteAt(0, &again[0], again.size()).ok());

    MT_EXPECT_EQ(static_cast<long long>(fx.backend->Stats().bytes_used),
                 static_cast<long long>(after_first));

    // ...and the rewrite is what a read sees, so the "no new bytes" result
    // above cannot be because the second write was skipped.
    fx.DropSource();
    const core::Expected<ByteBuffer, std::string> read =
        fx.file->ReadAt(0, static_cast<size_t>(kPage));
    MT_EXPECT_TRUE(read.ok());
    if (read.ok() && read.value().size() == static_cast<size_t>(kPage)) {
        std::vector<uint8_t> want(static_cast<size_t>(kPage), 0x22);
        MT_EXPECT_EQ(FirstMismatch(&read.value()[0], &want[0], want.size()),
                     static_cast<long long>(-1));
    }
}

// ============================================================
// capacity
// ============================================================

MT_TEST(a_write_is_refused_when_the_cache_has_no_capacity) {
    // Refused rather than reported as a short write: the caller asked for
    // bytes to become cached, and answering "0 bytes, no error" reads as
    // "nothing needed doing".
    Fixture fx;
    MT_EXPECT_TRUE(fx.Open(4 * kBlock, 0));
    if (!fx.file) return;

    std::vector<uint8_t> payload(static_cast<size_t>(kPage), 0x77);
    const core::Expected<size_t, std::string> wrote =
        fx.file->WriteAt(0, &payload[0], payload.size());
    MT_EXPECT_TRUE(!wrote.ok());
}

MT_MAIN
