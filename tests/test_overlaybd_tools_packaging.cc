// SPDX-License-Identifier: MIT
// Tests for `tools::CreateMappingsFromSparse` / `tools::PackageRawAsOverlaybd`
// — the volume backing path's last step.
//
// The mapping semantics are asserted against an in-memory file whose extent
// layout is dictated by the test, because the real question is what the
// scanner does with a given SEEK_DATA/SEEK_HOLE answer, and a filesystem is
// free to report extents at whatever granularity it likes. The packaging
// round trip then runs on a real file, and checks the bytes read back rather
// than the mapping count, so it holds whichever way the filesystem splits it.
//
// Rust: storage/overlaybd/src/tools/packaging.rs,
//       storage/overlaybd/src/lsmt/file/helper.rs :: create_mappings_from_sparse
#include "microtest.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/lsmt_readonly.h"
#include "agentenv/storage/overlaybd/tools_packaging.h"

using namespace agentenv::storage::overlaybd;
namespace core = agentenv::core;

namespace {

const uint64_t kBlk = 512;

/// A file whose allocated extents are whatever the test says they are.
///
/// Only `Size`/`SeekData`/`SeekHole` matter to the scanner; the reads exist so
/// the same object can also be handed to `CompactTo`. Content is derived from
/// the offset so a misplaced copy is visible in the value, not just the length.
class SparseMemFile : public VirtualFile {
 public:
    typedef std::pair<uint64_t, uint64_t> Extent;  // [begin, end)

    SparseMemFile(uint64_t size, const std::vector<Extent>& extents)
        : size_(size), extents_(extents) {}

    static uint8_t ByteAt(uint64_t offset) {
        return static_cast<uint8_t>((offset / kBlk) & 0xff);
    }

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override {
        if (offset >= size_) return ByteBuffer();
        const uint64_t avail = size_ - offset;
        const size_t take = avail < len ? static_cast<size_t>(avail) : len;
        ByteBuffer out(take, 0);
        for (size_t i = 0; i < take; ++i) {
            if (InData(offset + i)) out[i] = ByteAt(offset + i);
        }
        return out;
    }

    core::Expected<size_t, std::string> WriteAt(uint64_t, const uint8_t*, size_t) override {
        return core::make_unexpected(std::string("read-only"));
    }

    core::Expected<uint64_t, std::string> Size() override { return size_; }

    /// POSIX `SEEK_DATA`: the first data byte at or after `offset`, or absent
    /// when nothing follows.
    core::Expected<core::Optional<uint64_t>, std::string> SeekData(uint64_t offset) override {
        for (size_t i = 0; i < extents_.size(); ++i) {
            if (extents_[i].second <= offset) continue;
            const uint64_t begin = extents_[i].first;
            return core::Optional<uint64_t>(begin > offset ? begin : offset);
        }
        return core::Optional<uint64_t>();
    }

    /// POSIX `SEEK_HOLE`: the first hole at or after `offset`. An offset
    /// already inside a hole answers itself; end-of-file always counts as one.
    core::Expected<core::Optional<uint64_t>, std::string> SeekHole(uint64_t offset) override {
        for (size_t i = 0; i < extents_.size(); ++i) {
            if (extents_[i].first <= offset && offset < extents_[i].second) {
                return core::Optional<uint64_t>(extents_[i].second);
            }
            if (extents_[i].first > offset) break;
        }
        return core::Optional<uint64_t>(offset);
    }

 private:
    bool InData(uint64_t offset) const {
        for (size_t i = 0; i < extents_.size(); ++i) {
            if (extents_[i].first <= offset && offset < extents_[i].second) return true;
        }
        return false;
    }

    uint64_t size_;
    std::vector<SparseMemFile::Extent> extents_;
};

std::vector<SparseMemFile::Extent> Extents(uint64_t b0, uint64_t e0) {
    std::vector<SparseMemFile::Extent> out;
    out.push_back(std::make_pair(b0, e0));
    return out;
}

std::string TempPath(const char* stem) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/aenv-pack-%s-%d", stem, static_cast<int>(::getpid()));
    return std::string(buf);
}

}  // namespace

// ============================================================
// CreateMappingsFromSparse
// ============================================================

MT_TEST(a_fully_allocated_file_maps_to_one_contiguous_segment) {
    SparseMemFile file(4 * kBlk, Extents(0, 4 * kBlk));
    const core::Expected<std::vector<lsmt::SegmentMapping>, std::string> mappings =
        tools::CreateMappingsFromSparse(&file, 0);
    MT_EXPECT_TRUE(mappings.ok());
    if (!mappings.ok()) return;
    MT_EXPECT_EQ(static_cast<long long>(mappings.value().size()), 1LL);
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[0].Offset()), 0LL);
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[0].Length()), 4LL);
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[0].moffset), 0LL);
    // Real data, not a recorded hole: `zeroed` is what decides read visibility.
    MT_EXPECT_TRUE(!mappings.value()[0].zeroed);
    MT_EXPECT_EQ(static_cast<int>(mappings.value()[0].tag), 0);
}

MT_TEST(holes_are_left_unmapped_so_they_cost_no_space) {
    // Data in blocks 0-1 and 3-4; blocks 2 and 5-7 are holes.
    std::vector<SparseMemFile::Extent> extents;
    extents.push_back(std::make_pair(0, 2 * kBlk));
    extents.push_back(std::make_pair(3 * kBlk, 5 * kBlk));
    SparseMemFile file(8 * kBlk, extents);

    const core::Expected<std::vector<lsmt::SegmentMapping>, std::string> mappings =
        tools::CreateMappingsFromSparse(&file, 0);
    MT_EXPECT_TRUE(mappings.ok());
    if (!mappings.ok()) return;
    MT_EXPECT_EQ(static_cast<long long>(mappings.value().size()), 2LL);

    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[0].Offset()), 0LL);
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[0].Length()), 2LL);
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[1].Offset()), 3LL);
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[1].Length()), 2LL);
    // The hole is not described at all -- not even as a zeroed mapping.
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[1].moffset), 3LL);
}

MT_TEST(a_file_with_no_allocated_extent_yields_no_mappings) {
    SparseMemFile file(4 * kBlk, std::vector<SparseMemFile::Extent>());
    const core::Expected<std::vector<lsmt::SegmentMapping>, std::string> mappings =
        tools::CreateMappingsFromSparse(&file, 0);
    MT_EXPECT_TRUE(mappings.ok());
    if (!mappings.ok()) return;
    MT_EXPECT_TRUE(mappings.value().empty());
}

MT_TEST(a_file_no_longer_than_the_window_yields_no_mappings) {
    // Rust returns an empty vector rather than an error here.
    SparseMemFile file(kBlk, Extents(0, kBlk));
    const core::Expected<std::vector<lsmt::SegmentMapping>, std::string> mappings =
        tools::CreateMappingsFromSparse(&file, 2 * kBlk);
    MT_EXPECT_TRUE(mappings.ok());
    if (!mappings.ok()) return;
    MT_EXPECT_TRUE(mappings.value().empty());
}

MT_TEST(an_unaligned_start_offset_is_refused) {
    SparseMemFile file(4 * kBlk, Extents(0, 4 * kBlk));
    const core::Expected<std::vector<lsmt::SegmentMapping>, std::string> mappings =
        tools::CreateMappingsFromSparse(&file, 100);
    MT_EXPECT_TRUE(!mappings.ok());
    if (mappings.ok()) return;
    MT_EXPECT_TRUE(mappings.error().find("aligned") != std::string::npos);
}

MT_TEST(the_window_shifts_logical_offsets_but_not_physical_ones) {
    // This is the distinction the layer format turns on: `offset` is where the
    // guest sees the block, `moffset` is where it is read from in the source.
    // Collapsing the two would produce a layer that reads the wrong blocks.
    SparseMemFile file(4 * kBlk, Extents(kBlk, 4 * kBlk));
    const core::Expected<std::vector<lsmt::SegmentMapping>, std::string> mappings =
        tools::CreateMappingsFromSparse(&file, kBlk);
    MT_EXPECT_TRUE(mappings.ok());
    if (!mappings.ok()) return;
    MT_EXPECT_EQ(static_cast<long long>(mappings.value().size()), 1LL);
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[0].Offset()), 0LL);
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[0].moffset), 1LL);
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[0].Length()), 3LL);
}

MT_TEST(a_window_opening_inside_an_extent_maps_only_the_part_inside_it) {
    // `SEEK_DATA(cursor)` answers at or after `cursor`, so an extent that
    // began earlier is reported as starting at the window edge rather than at
    // its own start. That is why Rust's `begin < start_offset` branch is
    // unreachable under correct seek semantics, and why nothing before the
    // window leaks into the layer.
    SparseMemFile file(4 * kBlk, Extents(0, 4 * kBlk));
    const core::Expected<std::vector<lsmt::SegmentMapping>, std::string> mappings =
        tools::CreateMappingsFromSparse(&file, 2 * kBlk);
    MT_EXPECT_TRUE(mappings.ok());
    if (!mappings.ok()) return;
    MT_EXPECT_EQ(static_cast<long long>(mappings.value().size()), 1LL);
    // Logical 0 (the window's first block) read from physical block 2.
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[0].Offset()), 0LL);
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[0].moffset), 2LL);
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[0].Length()), 2LL);
}

MT_TEST(an_extent_longer_than_the_length_field_is_split) {
    // A segment's length is 14 bits, so a single extent cannot always be one
    // mapping. Getting this wrong truncates the tail of a large volume.
    const uint64_t max_len = static_cast<uint64_t>(lsmt::Segment::kMaxLength);
    const uint64_t blocks = max_len + 10;
    SparseMemFile file(blocks * kBlk, Extents(0, blocks * kBlk));

    const core::Expected<std::vector<lsmt::SegmentMapping>, std::string> mappings =
        tools::CreateMappingsFromSparse(&file, 0);
    MT_EXPECT_TRUE(mappings.ok());
    if (!mappings.ok()) return;
    MT_EXPECT_EQ(static_cast<long long>(mappings.value().size()), 2LL);
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[0].Length()),
                 static_cast<long long>(max_len));
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[1].Length()), 10LL);
    // The split must stay contiguous in both address spaces.
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[1].Offset()),
                 static_cast<long long>(max_len));
    MT_EXPECT_EQ(static_cast<long long>(mappings.value()[1].moffset),
                 static_cast<long long>(max_len));
}

MT_TEST(an_unaligned_extent_boundary_is_refused_rather_than_truncated) {
    // The block arithmetic truncates, so a trailing partial block would read
    // back as zeros with nothing reporting it.
    SparseMemFile file(3 * kBlk + 100, Extents(0, 3 * kBlk + 100));
    const core::Expected<std::vector<lsmt::SegmentMapping>, std::string> mappings =
        tools::CreateMappingsFromSparse(&file, 0);
    MT_EXPECT_TRUE(!mappings.ok());
    if (mappings.ok()) return;
    MT_EXPECT_TRUE(mappings.error().find("unaligned data extent") != std::string::npos);
}

MT_TEST(a_null_source_is_refused) {
    const core::Expected<std::vector<lsmt::SegmentMapping>, std::string> mappings =
        tools::CreateMappingsFromSparse(NULL, 0);
    MT_EXPECT_TRUE(!mappings.ok());
}

// ============================================================
// PackageRawAsOverlaybd
// ============================================================

MT_TEST(packaging_a_raw_file_produces_a_layer_that_reads_back_identically) {
    const std::string raw = TempPath("roundtrip-raw");
    const std::string out = TempPath("roundtrip-out.commit");
    ::unlink(raw.c_str());
    ::unlink(out.c_str());

    // 8 blocks of known content. Written in full rather than sparsely so the
    // expectation does not depend on how the filesystem reports extents.
    const uint64_t blocks = 8;
    std::vector<uint8_t> expected(static_cast<size_t>(blocks * kBlk), 0);
    for (uint64_t b = 0; b < blocks; ++b) {
        for (uint64_t i = 0; i < kBlk; ++i) {
            expected[static_cast<size_t>(b * kBlk + i)] = static_cast<uint8_t>(0xA0 + b);
        }
    }
    {
        core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(raw);
        MT_EXPECT_TRUE(f.ok());
        if (!f.ok()) return;
        const core::Expected<size_t, std::string> put =
            f.value()->WriteAt(0, &expected[0], expected.size());
        MT_EXPECT_TRUE(put.ok());
        MT_EXPECT_TRUE(f.value()->Sync().ok());
    }

    const core::Expected<core::Unit, std::string> packaged =
        tools::PackageRawAsOverlaybd(raw, out);
    MT_EXPECT_TRUE(packaged.ok());
    if (!packaged.ok()) {
        ::unlink(raw.c_str());
        return;
    }

    // The temporary must not survive a success: a leftover `.tmp` next to a
    // real layer is indistinguishable from an interrupted run.
    MT_EXPECT_TRUE(::access((out + ".tmp").c_str(), F_OK) != 0);

    core::Expected<std::shared_ptr<LocalFile>, std::string> layer = LocalFile::OpenRo(out);
    MT_EXPECT_TRUE(layer.ok());
    if (layer.ok()) {
        core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> ro =
            LsmtReadOnlyFile::Open(layer.value());
        MT_EXPECT_TRUE(ro.ok());
        if (ro.ok()) {
            // The layer presents the source's whole address space.
            MT_EXPECT_EQ(static_cast<long long>(ro.value()->VirtualSize()),
                         static_cast<long long>(blocks * kBlk));
            const core::Expected<ByteBuffer, std::string> got =
                ro.value()->ReadAt(0, static_cast<size_t>(blocks * kBlk));
            MT_EXPECT_TRUE(got.ok());
            if (got.ok()) {
                MT_EXPECT_EQ(static_cast<long long>(got.value().size()),
                             static_cast<long long>(expected.size()));
                MT_EXPECT_TRUE(got.value().size() == expected.size() &&
                               std::memcmp(&got.value()[0], &expected[0],
                                           expected.size()) == 0);
            }
        }
    }

    ::unlink(raw.c_str());
    ::unlink(out.c_str());
}

MT_TEST(a_missing_source_leaves_no_output_behind) {
    const std::string raw = TempPath("missing-raw");
    const std::string out = TempPath("missing-out.commit");
    ::unlink(raw.c_str());
    ::unlink(out.c_str());

    const core::Expected<core::Unit, std::string> packaged =
        tools::PackageRawAsOverlaybd(raw, out);
    MT_EXPECT_TRUE(!packaged.ok());
    if (packaged.ok()) return;
    MT_EXPECT_TRUE(packaged.error().find("open source raw file failed") != std::string::npos);
    // Neither the final name nor the temporary may exist: the volume path
    // would otherwise write an image.json naming an unusable layer.
    MT_EXPECT_TRUE(::access(out.c_str(), F_OK) != 0);
    MT_EXPECT_TRUE(::access((out + ".tmp").c_str(), F_OK) != 0);
}

MT_TEST(packaging_an_empty_source_produces_a_zero_sized_layer) {
    // `mkfs` never yields this, but a zero-length source must not be turned
    // into a layer claiming space it does not have.
    const std::string raw = TempPath("empty-raw");
    const std::string out = TempPath("empty-out.commit");
    ::unlink(raw.c_str());
    ::unlink(out.c_str());
    {
        core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(raw);
        MT_EXPECT_TRUE(f.ok());
        if (!f.ok()) return;
    }

    const core::Expected<core::Unit, std::string> packaged =
        tools::PackageRawAsOverlaybd(raw, out);
    MT_EXPECT_TRUE(packaged.ok());
    if (packaged.ok()) {
        core::Expected<std::shared_ptr<LocalFile>, std::string> layer = LocalFile::OpenRo(out);
        MT_EXPECT_TRUE(layer.ok());
        if (layer.ok()) {
            core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> ro =
                LsmtReadOnlyFile::Open(layer.value());
            MT_EXPECT_TRUE(ro.ok());
            if (ro.ok()) {
                MT_EXPECT_EQ(static_cast<long long>(ro.value()->VirtualSize()), 0LL);
            }
        }
    }

    ::unlink(raw.c_str());
    ::unlink(out.c_str());
}

MT_TEST(an_output_path_without_a_parent_is_refused) {
    const core::Expected<core::Unit, std::string> packaged =
        tools::PackageRawAsOverlaybd(TempPath("noparent-raw"), std::string("layer.commit"));
    MT_EXPECT_TRUE(!packaged.ok());
    if (packaged.ok()) return;
    MT_EXPECT_TRUE(packaged.error().find("parent directory") != std::string::npos);
}

MT_MAIN
