// SPDX-License-Identifier: MIT
// Tests for LSMTReadOnlyFile — the merged read view over an LSMT index.
// Rust: storage/overlaybd/src/lsmt/file/readonly.rs
#include "microtest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "agentenv/storage/overlaybd/lsmt_readonly.h"

using namespace agentenv::storage::overlaybd;
namespace core = agentenv::core;

namespace {

const uint64_t kBlk = 512;
const uint64_t kHdr = HeaderTrailer::kSpace;  // 4096

/// An in-memory VirtualFile, so a test can state an LSMT image as bytes
/// rather than going through the filesystem.
class MemFile : public VirtualFile {
 public:
    explicit MemFile(const std::vector<uint8_t>& d) : data_(d), reads_(0) {}

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override {
        ++reads_;
        if (offset >= data_.size()) return ByteBuffer();
        const size_t avail = data_.size() - static_cast<size_t>(offset);
        const size_t take = avail < len ? avail : len;
        return ByteBuffer(data_.begin() + static_cast<size_t>(offset),
                          data_.begin() + static_cast<size_t>(offset) + take);
    }
    core::Expected<size_t, std::string> WriteAt(uint64_t, const uint8_t*, size_t) override {
        return core::make_unexpected(std::string("read-only fixture"));
    }
    core::Expected<uint64_t, std::string> Size() override {
        return static_cast<uint64_t>(data_.size());
    }
    int Reads() const { return reads_; }

 private:
    std::vector<uint8_t> data_;
    int reads_;
};

/// Builds `[header 4096][data][index][trailer 4096]`, the layout
/// `LSMTReadOnlyFile::Open` expects.
struct ImageBuilder {
    std::vector<SegmentMapping> mappings;
    std::vector<uint8_t> data;       ///< the data region, starting at 4096
    uint64_t virtual_size;
    bool seal_trailer;
    bool data_file_trailer;
    bool corrupt_header_magic;
    uint64_t index_size_override;    ///< 0 = use mappings.size()
    bool use_index_size_override;
    std::string uuid;                ///< written into the trailer

    ImageBuilder()
        : virtual_size(0), seal_trailer(true), data_file_trailer(true),
          corrupt_header_magic(false), index_size_override(0),
          use_index_size_override(false) {}

    std::vector<uint8_t> Build() const {
        std::vector<uint8_t> image(static_cast<size_t>(kHdr), 0);

        HeaderTrailer h = HeaderTrailer::New();
        h.SetHeader();
        h.SetDataFile();
        h.SetSealed();
        h.virtual_size = virtual_size;
        uint8_t hb[HeaderTrailer::kOnDiskSize];
        h.Serialize(hb);
        for (size_t i = 0; i < sizeof(hb); ++i) image[i] = hb[i];
        if (corrupt_header_magic) image[0] ^= 0xFF;

        image.insert(image.end(), data.begin(), data.end());

        const uint64_t index_off = image.size();
        for (size_t i = 0; i < mappings.size(); ++i) {
            DiskSegmentMapping d = DiskSegmentMapping::FromMemory(mappings[i]);
            uint8_t rec[16];
            for (int k = 0; k < 8; ++k) {
                rec[k] = static_cast<uint8_t>((d.data_low >> (8 * k)) & 0xFF);
            }
            for (int k = 0; k < 8; ++k) {
                rec[8 + k] = static_cast<uint8_t>((d.data_high >> (8 * k)) & 0xFF);
            }
            image.insert(image.end(), rec, rec + 16);
        }

        const uint64_t trailer_off = image.size();
        HeaderTrailer t = HeaderTrailer::New();
        t.SetHeader();
        if (data_file_trailer) t.SetDataFile();
        if (seal_trailer) t.SetSealed();
        t.SetTrailer();
        t.index_offset = index_off;
        t.index_size = use_index_size_override ? index_size_override : mappings.size();
        t.virtual_size = virtual_size;
        for (size_t i = 0; i < uuid.size() && i + 1 < sizeof(t.uuid); ++i) {
            t.uuid[i] = static_cast<uint8_t>(uuid[i]);
        }
        uint8_t tb[HeaderTrailer::kOnDiskSize];
        t.Serialize(tb);
        image.resize(static_cast<size_t>(trailer_off + kHdr), 0);
        for (size_t i = 0; i < sizeof(tb); ++i) {
            image[static_cast<size_t>(trailer_off) + i] = tb[i];
        }
        return image;
    }
};

/// A data region of `blocks` blocks, block i filled with byte `first + i`.
std::vector<uint8_t> DataBlocks(size_t blocks, uint8_t first) {
    std::vector<uint8_t> d(blocks * static_cast<size_t>(kBlk), 0);
    for (size_t b = 0; b < blocks; ++b) {
        std::memset(&d[b * static_cast<size_t>(kBlk)], static_cast<int>(first + b),
                    static_cast<size_t>(kBlk));
    }
    return d;
}

std::shared_ptr<LsmtReadOnlyFile> OpenImage(const ImageBuilder& b, std::string* err) {
    std::shared_ptr<VirtualFile> mem(new MemFile(b.Build()));
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f =
        LsmtReadOnlyFile::Open(mem);
    if (!f.ok()) {
        if (err) *err = f.error();
        return std::shared_ptr<LsmtReadOnlyFile>();
    }
    return f.value();
}

}  // namespace

// ============================================================
// alignment
// ============================================================

MT_TEST(reads_must_be_block_aligned) {
    MT_EXPECT_TRUE(CheckAlignment(0, 512).ok());
    MT_EXPECT_TRUE(CheckAlignment(1024, 4096).ok());
    MT_EXPECT_TRUE(CheckAlignment(0, 0).ok());

    // The index addresses blocks, not bytes, so an unaligned request has no
    // exact answer -- it would have to be widened, and the caller would get
    // back more than it asked for with no way to tell.
    MT_EXPECT_TRUE(!CheckAlignment(1, 512).ok());
    MT_EXPECT_TRUE(!CheckAlignment(0, 100).ok());
    core::Expected<core::Unit, std::string> r = CheckAlignment(7, 512);
    if (!r.ok()) MT_EXPECT_TRUE(r.error().find("aligned to 512") != std::string::npos);
}

// ============================================================
// open / validation
// ============================================================

MT_TEST(open_accepts_a_well_formed_sealed_image) {
    ImageBuilder b;
    b.data = DataBlocks(2, 0xA0);
    b.virtual_size = 8 * kBlk;
    // Blocks 0-1 map to the data region (which starts at 4096 == block 8).
    b.mappings.push_back(SegmentMapping(0, 2, 8, false, 0));

    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    MT_EXPECT_TRUE(f.get() != 0);
    if (!f) return;
    core::Expected<uint64_t, std::string> sz = f->Size();
    MT_EXPECT_TRUE(sz.ok());
    // The *virtual* size, not the file size. Everything above this layer
    // addresses the un-sparsed space; the bytes on disk are this layer's
    // business.
    if (sz.ok()) MT_EXPECT_EQ(static_cast<long long>(sz.value()), static_cast<long long>(8 * kBlk));
    MT_EXPECT_EQ(static_cast<int>(f->FileType()), static_cast<int>(kLsmtFileReadOnly));
}

MT_TEST(open_rejects_a_bad_header_even_when_the_trailer_is_fine) {
    ImageBuilder b;
    b.data = DataBlocks(1, 0xA0);
    b.virtual_size = 8 * kBlk;
    b.corrupt_header_magic = true;

    std::string err;
    MT_EXPECT_TRUE(OpenImage(b, &err).get() == 0);
    // A file whose header is wrong but whose trailer parses is not an LSMT
    // file that lost its header -- it is something else with 390 plausible
    // bytes at the end. Trusting it would build mappings into nothing.
    MT_EXPECT_TRUE(err.find("header") != std::string::npos);
}

MT_TEST(open_rejects_an_unsealed_trailer) {
    ImageBuilder b;
    b.data = DataBlocks(1, 0xA0);
    b.virtual_size = 8 * kBlk;
    b.seal_trailer = false;

    std::string err;
    MT_EXPECT_TRUE(OpenImage(b, &err).get() == 0);
    // Stricter than the header check on purpose: the trailer carries the
    // index offset, and that offset only means anything once the file is
    // sealed. In a file still being written it may point at a region that is
    // not an index yet, which would parse as plausible garbage.
    MT_EXPECT_TRUE(err.find("sealedness") != std::string::npos);
}

MT_TEST(open_rejects_a_trailer_that_is_not_a_data_file) {
    ImageBuilder b;
    b.data = DataBlocks(1, 0xA0);
    b.virtual_size = 8 * kBlk;
    b.data_file_trailer = false;

    std::string err;
    MT_EXPECT_TRUE(OpenImage(b, &err).get() == 0);
    MT_EXPECT_TRUE(err.find("file type") != std::string::npos);
}

MT_TEST(open_rejects_a_file_too_small_to_hold_a_trailer) {
    std::vector<uint8_t> tiny(100, 0);
    std::shared_ptr<VirtualFile> mem(new MemFile(tiny));
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f =
        LsmtReadOnlyFile::Open(mem);
    MT_EXPECT_TRUE(!f.ok());
}

MT_TEST(open_rejects_an_index_region_outside_the_file) {
    ImageBuilder b;
    b.data = DataBlocks(1, 0xA0);
    b.virtual_size = 8 * kBlk;
    b.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));
    // A count far larger than the file can hold.
    b.use_index_size_override = true;
    b.index_size_override = 1000000;

    std::string err;
    MT_EXPECT_TRUE(OpenImage(b, &err).get() == 0);
    // Bounded before allocating: the count comes out of the file, so a
    // corrupt trailer would otherwise be a request to allocate an arbitrary
    // amount of memory.
    MT_EXPECT_TRUE(err.find("out of range") != std::string::npos);
}

MT_TEST(open_accepts_an_image_with_an_empty_index) {
    ImageBuilder b;
    b.virtual_size = 16 * kBlk;
    // No mappings at all: a fully sparse image. Legal, and reads as zeros.
    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    MT_EXPECT_TRUE(f.get() != 0);
    if (!f) return;

    std::vector<uint8_t> dst(static_cast<size_t>(4 * kBlk), 0xFF);
    core::Expected<size_t, std::string> n = f->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(n.value(), dst.size());
    bool all_zero = true;
    for (size_t i = 0; i < dst.size(); ++i) if (dst[i] != 0) all_zero = false;
    MT_EXPECT_TRUE(all_zero);
}

MT_TEST(open_refuses_a_null_file) {
    MT_EXPECT_TRUE(!LsmtReadOnlyFile::Open(std::shared_ptr<VirtualFile>()).ok());
}

// ============================================================
// the resolver
// ============================================================
//
// Every virtual block is one of three things, and all three are legal: mapped
// to a physical range in a layer, recorded as zeroed, or absent from the
// index. The last two both read back as zeros but mean different things, and
// the difference is load-bearing once there is more than one layer.

MT_TEST(a_mapped_range_reads_the_layer_bytes) {
    ImageBuilder b;
    b.data = DataBlocks(3, 0xA0);     // data at file block 8, 9, 10
    b.virtual_size = 16 * kBlk;
    b.mappings.push_back(SegmentMapping(0, 3, 8, false, 0));

    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    MT_EXPECT_TRUE(f.get() != 0);
    if (!f) return;

    std::vector<uint8_t> dst(static_cast<size_t>(3 * kBlk), 0);
    core::Expected<size_t, std::string> n = f->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(n.value(), dst.size());
    MT_EXPECT_EQ(static_cast<int>(dst[0]), 0xA0);
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(kBlk)]), 0xA1);
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(2 * kBlk)]), 0xA2);
}

MT_TEST(an_absent_range_reads_as_zeros) {
    ImageBuilder b;
    b.data = DataBlocks(1, 0xA0);
    b.virtual_size = 16 * kBlk;
    // Block 0 mapped; blocks 1-3 absent from the index entirely.
    b.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    MT_EXPECT_TRUE(f.get() != 0);
    if (!f) return;

    std::vector<uint8_t> dst(static_cast<size_t>(4 * kBlk), 0xFF);
    core::Expected<size_t, std::string> n = f->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(n.value(), dst.size());
    MT_EXPECT_EQ(static_cast<int>(dst[0]), 0xA0);
    // Zeros are the *defined* contents of an unmapped range, not a fallback.
    // This is what lets an image declare a virtual size far larger than the
    // bytes it stores -- the whole basis of a sparse memory snapshot.
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(kBlk)]), 0);
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(4 * kBlk) - 1]), 0);
}

MT_TEST(a_hole_between_two_mappings_reads_as_zeros) {
    ImageBuilder b;
    b.data = DataBlocks(2, 0xB0);
    b.virtual_size = 16 * kBlk;
    // Block 0 -> data block 0; block 3 -> data block 1. Blocks 1-2 are a gap.
    b.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));
    b.mappings.push_back(SegmentMapping(3, 1, 9, false, 0));

    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    MT_EXPECT_TRUE(f.get() != 0);
    if (!f) return;

    std::vector<uint8_t> dst(static_cast<size_t>(4 * kBlk), 0xFF);
    core::Expected<size_t, std::string> n = f->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(dst[0]), 0xB0);
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(kBlk)]), 0);
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(2 * kBlk)]), 0);
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(3 * kBlk)]), 0xB1);
}

MT_TEST(a_zeroed_mapping_reads_as_zeros_without_touching_a_layer) {
    ImageBuilder b;
    b.data = DataBlocks(1, 0xC0);
    b.virtual_size = 16 * kBlk;
    // A zeroed mapping with no physical backing. Distinct from an absent
    // range: this range *was* written, and what was written was zeros. The
    // index records it so a lower layer cannot be seen through it.
    b.mappings.push_back(SegmentMapping(0, 2, kNoPhysicalOffset, true, 0));
    b.mappings.push_back(SegmentMapping(2, 1, 8, false, 0));

    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    MT_EXPECT_TRUE(f.get() != 0);
    if (!f) return;

    std::vector<uint8_t> dst(static_cast<size_t>(3 * kBlk), 0xFF);
    core::Expected<size_t, std::string> n = f->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(dst[0]), 0);
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(kBlk)]), 0);
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(2 * kBlk)]), 0xC0);
}

MT_TEST(a_read_is_clamped_to_the_virtual_size) {
    ImageBuilder b;
    b.data = DataBlocks(1, 0xD0);
    b.virtual_size = 2 * kBlk;   // only two blocks of address space
    b.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    MT_EXPECT_TRUE(f.get() != 0);
    if (!f) return;

    std::vector<uint8_t> dst(static_cast<size_t>(8 * kBlk), 0xFF);
    core::Expected<size_t, std::string> n = f->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    // Two blocks, not eight. Resolving past the declared address space would
    // produce zeros indistinguishable from a real hole, so the caller could
    // not tell the file ended.
    MT_EXPECT_EQ(n.value(), static_cast<size_t>(2 * kBlk));
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(2 * kBlk)]), 0xFF);  // untouched
}

MT_TEST(a_read_starting_past_the_virtual_size_is_empty) {
    ImageBuilder b;
    b.data = DataBlocks(1, 0xD0);
    b.virtual_size = 2 * kBlk;
    b.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    if (!f) return;

    std::vector<uint8_t> dst(static_cast<size_t>(kBlk), 0xFF);
    core::Expected<size_t, std::string> n = f->ReadAtInto(16 * kBlk, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (n.ok()) MT_EXPECT_EQ(n.value(), static_cast<size_t>(0));

    core::Expected<ByteBuffer, std::string> r = f->ReadAt(16 * kBlk, static_cast<size_t>(kBlk));
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) MT_EXPECT_TRUE(r.value().empty());
}

MT_TEST(an_out_of_range_layer_tag_is_reported_at_read_time) {
    ImageBuilder b;
    b.data = DataBlocks(1, 0xE0);
    b.virtual_size = 16 * kBlk;
    b.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    MT_EXPECT_TRUE(f.get() != 0);
    if (!f) return;

    // A single-file open resets every tag to 0, so this cannot be reached
    // through `Open`. Built directly instead, with a tag naming a layer that
    // is not there.
    std::vector<SegmentMapping> bad;
    bad.push_back(SegmentMapping(0, 1, 8, false, 3));
    std::shared_ptr<LogIndex> idx(new ReadOnlyIndex(bad));
    std::vector<std::shared_ptr<VirtualFile> > one;
    one.push_back(std::shared_ptr<VirtualFile>(new MemFile(DataBlocks(16, 0))));

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> st =
        LsmtReadOnlyFile::FromLayers(idx, one, 16 * kBlk);
    MT_EXPECT_TRUE(st.ok());
    if (!st.ok()) return;

    std::vector<uint8_t> dst(static_cast<size_t>(kBlk), 0);
    core::Expected<size_t, std::string> n = st.value()->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(!n.ok());
    if (!n.ok()) {
        // Reported at read time rather than at open: the index may
        // legitimately cover more layers than any one read touches, so this
        // is only wrong once it is actually reached.
        MT_EXPECT_TRUE(n.error().find("Layer index") != std::string::npos);
    }
}

// ============================================================
// layer stacking and stepping
// ============================================================

MT_TEST(the_tag_selects_which_layer_a_mapping_reads_from) {
    // Two layers with distinguishable contents, and an index that interleaves
    // them. This is what the merged view exists for: one address space
    // assembled from several files.
    std::vector<std::shared_ptr<VirtualFile> > layers;
    layers.push_back(std::shared_ptr<VirtualFile>(new MemFile(DataBlocks(4, 0x10))));
    layers.push_back(std::shared_ptr<VirtualFile>(new MemFile(DataBlocks(4, 0x20))));

    std::vector<SegmentMapping> m;
    m.push_back(SegmentMapping(0, 1, 0, false, 0));   // layer 0 block 0 -> 0x10
    m.push_back(SegmentMapping(1, 1, 1, false, 1));   // layer 1 block 1 -> 0x21
    m.push_back(SegmentMapping(2, 1, 2, false, 0));   // layer 0 block 2 -> 0x12
    std::shared_ptr<LogIndex> idx(new ReadOnlyIndex(m));

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f =
        LsmtReadOnlyFile::FromLayers(idx, layers, 4 * kBlk);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    std::vector<uint8_t> dst(static_cast<size_t>(3 * kBlk), 0);
    core::Expected<size_t, std::string> n = f.value()->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(dst[0]), 0x10);
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(kBlk)]), 0x21);
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(2 * kBlk)]), 0x12);
}

MT_TEST(from_layers_refuses_a_null_index_or_an_empty_layer_set) {
    std::vector<std::shared_ptr<VirtualFile> > layers;
    layers.push_back(std::shared_ptr<VirtualFile>(new MemFile(DataBlocks(1, 0))));
    std::shared_ptr<LogIndex> idx(new ReadOnlyIndex(std::vector<SegmentMapping>()));

    MT_EXPECT_TRUE(!LsmtReadOnlyFile::FromLayers(std::shared_ptr<LogIndex>(), layers, 512).ok());
    MT_EXPECT_TRUE(
        !LsmtReadOnlyFile::FromLayers(idx, std::vector<std::shared_ptr<VirtualFile> >(), 512).ok());

    // A null entry in an otherwise populated set: named by index, because a
    // failure that just said "a layer is null" would not say which.
    std::vector<std::shared_ptr<VirtualFile> > with_hole;
    with_hole.push_back(layers[0]);
    with_hole.push_back(std::shared_ptr<VirtualFile>());
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> r =
        LsmtReadOnlyFile::FromLayers(idx, with_hole, 512);
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) MT_EXPECT_TRUE(r.error().find("layer 1") != std::string::npos);
}

MT_TEST(a_read_longer_than_the_step_is_split_and_still_contiguous) {
    // 40 blocks read in one call with the step forced down to 4 blocks, so
    // the loop runs ten times. The result must be identical to a single pass
    // -- the split is an implementation detail and must not show as a seam.
    std::vector<std::shared_ptr<VirtualFile> > layers;
    layers.push_back(std::shared_ptr<VirtualFile>(new MemFile(DataBlocks(40, 0x40))));

    std::vector<SegmentMapping> m;
    m.push_back(SegmentMapping(0, 40, 0, false, 0));
    std::shared_ptr<LogIndex> idx(new ReadOnlyIndex(m));

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f =
        LsmtReadOnlyFile::FromLayers(idx, layers, 40 * kBlk);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    MT_EXPECT_TRUE(f.value()->SetMaxIoSize(static_cast<size_t>(4 * kBlk)).ok());

    std::vector<uint8_t> dst(static_cast<size_t>(40 * kBlk), 0);
    core::Expected<size_t, std::string> n = f.value()->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(n.value(), dst.size());
    bool ok = true;
    for (size_t blk = 0; blk < 40; ++blk) {
        if (dst[blk * static_cast<size_t>(kBlk)] != static_cast<uint8_t>(0x40 + blk)) ok = false;
    }
    MT_EXPECT_TRUE(ok);
}

MT_TEST(max_io_size_must_stay_a_nonzero_multiple_of_the_block) {
    std::vector<std::shared_ptr<VirtualFile> > layers;
    layers.push_back(std::shared_ptr<VirtualFile>(new MemFile(DataBlocks(1, 0))));
    std::shared_ptr<LogIndex> idx(new ReadOnlyIndex(std::vector<SegmentMapping>()));
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f =
        LsmtReadOnlyFile::FromLayers(idx, layers, kBlk);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    // Zero would make the read loop consume nothing per iteration and never
    // terminate; an unaligned step would put the next iteration's offset off
    // a block boundary, so the index lookup would start mid-block.
    MT_EXPECT_TRUE(!f.value()->SetMaxIoSize(0).ok());
    MT_EXPECT_TRUE(!f.value()->SetMaxIoSize(1000).ok());
    MT_EXPECT_TRUE(f.value()->SetMaxIoSize(static_cast<size_t>(kBlk)).ok());
    MT_EXPECT_EQ(f.value()->GetMaxIoSize(), static_cast<size_t>(kBlk));
    // The default is Rust's 4 MiB.
    MT_EXPECT_EQ(kLsmtMaxIoSize, static_cast<size_t>(4 * 1024 * 1024));
}

MT_TEST(a_short_layer_file_is_an_error_not_a_hole) {
    // The index claims a block the layer file does not actually contain.
    std::vector<std::shared_ptr<VirtualFile> > layers;
    layers.push_back(std::shared_ptr<VirtualFile>(new MemFile(DataBlocks(1, 0x50))));

    std::vector<SegmentMapping> m;
    m.push_back(SegmentMapping(0, 4, 0, false, 0));  // four blocks, file has one
    std::shared_ptr<LogIndex> idx(new ReadOnlyIndex(m));

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f =
        LsmtReadOnlyFile::FromLayers(idx, layers, 4 * kBlk);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    std::vector<uint8_t> dst(static_cast<size_t>(4 * kBlk), 0);
    core::Expected<size_t, std::string> n = f.value()->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(!n.ok());
    if (!n.ok()) {
        // A short read is a success at the VirtualFile boundary but a failure
        // here: the index said these bytes are data, so handing back the
        // zeros already in the buffer would substitute a hole for whatever
        // the layer was supposed to hold.
        MT_EXPECT_TRUE(n.error().find("unexpected EOF") != std::string::npos);
    }
}

// ============================================================
// writes and introspection
// ============================================================

MT_TEST(writes_are_refused_with_a_reason_about_this_handle) {
    ImageBuilder b;
    b.data = DataBlocks(1, 0xA0);
    b.virtual_size = 8 * kBlk;
    b.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));
    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    MT_EXPECT_TRUE(f.get() != 0);
    if (!f) return;

    const uint8_t byte = 1;
    core::Expected<size_t, std::string> w = f->WriteAt(0, &byte, 1);
    MT_EXPECT_TRUE(!w.ok());
    // Not the base class's "write_at is not supported": LSMT *does* have a
    // write path, so the reason is that this handle is read-only, not that
    // the format cannot be written.
    if (!w.ok()) MT_EXPECT_TRUE(w.error().find("read-only") != std::string::npos);
}

MT_TEST(data_stat_counts_only_real_extents) {
    ImageBuilder b;
    b.data = DataBlocks(3, 0xA0);
    b.virtual_size = 32 * kBlk;
    b.mappings.push_back(SegmentMapping(0, 3, 8, false, 0));                  // 3 data blocks
    b.mappings.push_back(SegmentMapping(10, 5, kNoPhysicalOffset, true, 0));  // zeroed

    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    MT_EXPECT_TRUE(f.get() != 0);
    if (!f) return;

    DataStat s = f->Stat();
    // Three blocks, not eight. Zeroed mappings occupy index space but no data
    // space, which is the entire reason for recording them apart from real
    // extents: they let a layer say "this range is zeros" without storing any.
    MT_EXPECT_EQ(static_cast<long long>(s.total_data_size), static_cast<long long>(3 * kBlk));
    MT_EXPECT_EQ(static_cast<long long>(s.valid_data_size), static_cast<long long>(3 * kBlk));
}

MT_TEST(seek_data_reports_mapped_spans_and_skips_zeroed_ones) {
    ImageBuilder b;
    b.data = DataBlocks(4, 0xA0);
    b.virtual_size = 32 * kBlk;
    b.mappings.push_back(SegmentMapping(0, 2, 8, false, 0));
    b.mappings.push_back(SegmentMapping(4, 3, kNoPhysicalOffset, true, 0));
    b.mappings.push_back(SegmentMapping(8, 2, 10, false, 0));

    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    MT_EXPECT_TRUE(f.get() != 0);
    if (!f) return;

    std::vector<Segment> segs;
    core::Expected<size_t, std::string> n = f->SeekDataSegments(0, 32 * kBlk, &segs);
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    // Two spans, not three: a zeroed range is not data. Reporting it would
    // make a caller copying "the data" write out zeros it could have skipped,
    // which for a sparse export defeats the point.
    MT_EXPECT_EQ(static_cast<int>(segs.size()), 2);
    if (segs.size() == 2) {
        MT_EXPECT_EQ(static_cast<long long>(segs[0].offset), 0LL);
        MT_EXPECT_EQ(static_cast<long long>(segs[1].offset), 8LL);
    }
}

MT_TEST(seek_data_on_an_empty_or_inverted_range_is_zero_segments) {
    ImageBuilder b;
    b.data = DataBlocks(1, 0xA0);
    b.virtual_size = 8 * kBlk;
    b.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));
    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    if (!f) return;

    std::vector<Segment> segs;
    // Not an error: a caller walking a file hits this naturally at the end.
    MT_EXPECT_TRUE(f->SeekDataSegments(100, 100, &segs).ok());
    MT_EXPECT_TRUE(f->SeekDataSegments(200, 100, &segs).ok());
    MT_EXPECT_TRUE(segs.empty());
    MT_EXPECT_TRUE(!f->SeekDataSegments(0, 100, 0).ok());
}

MT_TEST(read_at_returns_an_owned_buffer_sized_to_what_it_resolved) {
    ImageBuilder b;
    b.data = DataBlocks(2, 0x70);
    b.virtual_size = 4 * kBlk;
    b.mappings.push_back(SegmentMapping(0, 2, 8, false, 0));
    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    MT_EXPECT_TRUE(f.get() != 0);
    if (!f) return;

    core::Expected<ByteBuffer, std::string> r = f->ReadAt(0, static_cast<size_t>(2 * kBlk));
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_EQ(r.value().size(), static_cast<size_t>(2 * kBlk));
    MT_EXPECT_EQ(static_cast<int>(r.value()[0]), 0x70);
    MT_EXPECT_EQ(static_cast<int>(r.value()[static_cast<size_t>(kBlk)]), 0x71);

    // Zero length is empty, and resolves nothing.
    core::Expected<ByteBuffer, std::string> z = f->ReadAt(0, 0);
    MT_EXPECT_TRUE(z.ok());
    if (z.ok()) MT_EXPECT_TRUE(z.value().empty());
}

MT_TEST(an_unaligned_read_is_refused_rather_than_widened) {
    ImageBuilder b;
    b.data = DataBlocks(2, 0x70);
    b.virtual_size = 4 * kBlk;
    b.mappings.push_back(SegmentMapping(0, 2, 8, false, 0));
    std::string err;
    std::shared_ptr<LsmtReadOnlyFile> f = OpenImage(b, &err);
    if (!f) return;

    std::vector<uint8_t> dst(static_cast<size_t>(kBlk), 0);
    MT_EXPECT_TRUE(!f->ReadAtInto(0, &dst[0], 100).ok());
    MT_EXPECT_TRUE(!f->ReadAtInto(7, &dst[0], static_cast<size_t>(kBlk)).ok());
    MT_EXPECT_TRUE(!f->ReadAtInto(0, 0, static_cast<size_t>(kBlk)).ok());
}


// ============================================================
// OpenFilesRo — stacking sealed layers
// ============================================================
//
// The input order is bottom-to-top (oldest base first), while the merged
// index tags layers the other way round (tag 0 is newest). Getting that
// backwards is the most dangerous failure in this file: both orders produce
// well-formed reads, so a reversed stack returns some other version of the
// image with no error anywhere.

MT_TEST(a_newer_layer_shadows_an_older_one_at_the_same_block) {
    // Two sealed single-layer images, both mapping virtual block 0.
    ImageBuilder lo;
    lo.data = DataBlocks(1, 0x11);
    lo.virtual_size = 8 * kBlk;
    lo.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    ImageBuilder hi;
    hi.data = DataBlocks(1, 0x99);
    hi.virtual_size = 8 * kBlk;
    hi.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(lo.Build())));   // oldest
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(hi.Build())));   // newest

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f = OpenFilesRo(files);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    std::vector<uint8_t> dst(static_cast<size_t>(kBlk), 0);
    core::Expected<size_t, std::string> n = f.value()->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    // 0x99, the last layer in the input. If the reversal were missing the
    // read would return 0x11 -- a complete, plausible, wrong answer.
    MT_EXPECT_EQ(static_cast<int>(dst[0]), 0x99);
}

MT_TEST(blocks_only_an_older_layer_has_still_resolve) {
    // The older layer covers blocks 0-1; the newer one only block 0. Block 1
    // has to fall through to the older layer, which is the entire point of
    // stacking rather than just using the newest layer.
    ImageBuilder lo;
    lo.data = DataBlocks(2, 0x20);
    lo.virtual_size = 8 * kBlk;
    lo.mappings.push_back(SegmentMapping(0, 2, 8, false, 0));

    ImageBuilder hi;
    hi.data = DataBlocks(1, 0x80);
    hi.virtual_size = 8 * kBlk;
    hi.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(lo.Build())));
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(hi.Build())));

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f = OpenFilesRo(files);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    std::vector<uint8_t> dst(static_cast<size_t>(2 * kBlk), 0);
    core::Expected<size_t, std::string> n = f.value()->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(dst[0]), 0x80);                              // newer wins
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(kBlk)]), 0x21);      // older shows through
}

MT_TEST(a_zeroed_mapping_in_a_newer_layer_hides_older_data) {
    // This is why `zeroed` is recorded at all rather than simply omitted: an
    // omitted range would fall through to the older layer, which for an
    // overwritten-with-zeros block is the wrong answer.
    ImageBuilder lo;
    lo.data = DataBlocks(1, 0x33);
    lo.virtual_size = 8 * kBlk;
    lo.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    ImageBuilder hi;
    hi.virtual_size = 8 * kBlk;
    hi.mappings.push_back(SegmentMapping(0, 1, kNoPhysicalOffset, true, 0));

    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(lo.Build())));
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(hi.Build())));

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f = OpenFilesRo(files);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    std::vector<uint8_t> dst(static_cast<size_t>(kBlk), 0xFF);
    core::Expected<size_t, std::string> n = f.value()->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(dst[0]), 0);
}

MT_TEST(uuids_follow_the_merged_layer_order_not_the_input_order) {
    ImageBuilder lo;
    lo.data = DataBlocks(1, 0x11);
    lo.virtual_size = 8 * kBlk;
    lo.uuid = "oldest-layer";
    lo.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    ImageBuilder hi;
    hi.data = DataBlocks(1, 0x99);
    hi.virtual_size = 8 * kBlk;
    hi.uuid = "newest-layer";
    hi.mappings.push_back(SegmentMapping(1, 1, 8, false, 0));

    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(lo.Build())));
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(hi.Build())));

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f = OpenFilesRo(files);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    // Layer 0 is the newest, matching tag 0. The uuids were reversed
    // alongside the layers, so asking layer 0 for its identity names the
    // same file a tag-0 mapping reads from. Leave the uuids in input order
    // and every mapping would be attributed to the wrong layer -- which is
    // exactly what a caller uses this for when deciding which layer to
    // re-fetch or invalidate.
    core::Expected<std::string, std::string> u0 = f.value()->GetUuid(0);
    core::Expected<std::string, std::string> u1 = f.value()->GetUuid(1);
    MT_EXPECT_TRUE(u0.ok());
    MT_EXPECT_TRUE(u1.ok());
    if (u0.ok()) MT_EXPECT_TRUE(u0.value() == "newest-layer");
    if (u1.ok()) MT_EXPECT_TRUE(u1.value() == "oldest-layer");
    MT_EXPECT_TRUE(!f.value()->GetUuid(2).ok());
    MT_EXPECT_EQ(static_cast<int>(f.value()->Layers().size()), 2);
}

MT_TEST(a_layer_recorded_tag_is_discarded_when_it_is_restacked) {
    // A layer carries tags relative to the stack it was *built* in. Opened
    // into a different stack those numbers are meaningless, so they are
    // reset before the merge reassigns them by position. This layer claims
    // tag 7 -- a layer that does not exist here.
    ImageBuilder lo;
    lo.data = DataBlocks(1, 0x11);
    lo.virtual_size = 8 * kBlk;
    lo.mappings.push_back(SegmentMapping(0, 1, 8, false, 7));

    ImageBuilder hi;
    hi.data = DataBlocks(1, 0x99);
    hi.virtual_size = 8 * kBlk;
    hi.mappings.push_back(SegmentMapping(1, 1, 8, false, 5));

    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(lo.Build())));
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(hi.Build())));

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f = OpenFilesRo(files);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    std::vector<uint8_t> dst(static_cast<size_t>(2 * kBlk), 0);
    core::Expected<size_t, std::string> n = f.value()->ReadAtInto(0, &dst[0], dst.size());
    // Kept the stale tags and this would be "Layer index 7 out of range",
    // or worse -- a tag that happens to be in range and reads the wrong file.
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(dst[0]), 0x11);
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(kBlk)]), 0x99);
}

MT_TEST(the_virtual_size_is_the_newest_layer_that_declares_one) {
    ImageBuilder lo;
    lo.data = DataBlocks(1, 0x11);
    lo.virtual_size = 64 * kBlk;      // older layer: large
    lo.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    ImageBuilder hi;
    hi.data = DataBlocks(1, 0x99);
    hi.virtual_size = 16 * kBlk;      // newer layer: smaller
    hi.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(lo.Build())));
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(hi.Build())));

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f = OpenFilesRo(files);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    core::Expected<uint64_t, std::string> sz = f.value()->Size();
    MT_EXPECT_TRUE(sz.ok());
    // The newest declaration, not the largest. An image that shrank is
    // reporting its current size, and taking the maximum would address
    // blocks the current version no longer has.
    if (sz.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(sz.value()), static_cast<long long>(16 * kBlk));
    }
}

MT_TEST(a_single_layer_stack_behaves_like_a_direct_open) {
    ImageBuilder b;
    b.data = DataBlocks(2, 0x55);
    b.virtual_size = 8 * kBlk;
    b.mappings.push_back(SegmentMapping(0, 2, 8, false, 0));

    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(b.Build())));

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f = OpenFilesRo(files);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    std::vector<uint8_t> dst(static_cast<size_t>(2 * kBlk), 0);
    core::Expected<size_t, std::string> n = f.value()->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    MT_EXPECT_EQ(static_cast<int>(dst[0]), 0x55);
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(kBlk)]), 0x56);
}

MT_TEST(open_files_ro_refuses_an_empty_or_oversized_stack) {
    MT_EXPECT_TRUE(!OpenFilesRo(std::vector<std::shared_ptr<VirtualFile> >()).ok());

    ImageBuilder b;
    b.virtual_size = kBlk;
    std::vector<std::shared_ptr<VirtualFile> > many;
    for (int i = 0; i < 256; ++i) {
        many.push_back(std::shared_ptr<VirtualFile>(new MemFile(b.Build())));
    }
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> r = OpenFilesRo(many);
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) {
        // The tag is a single byte, so a 256th layer has no number left to
        // name it and would wrap onto an existing one.
        MT_EXPECT_TRUE(r.error().find("too many layers") != std::string::npos);
    }
}

MT_TEST(a_corrupt_layer_names_which_one_failed) {
    ImageBuilder ok;
    ok.data = DataBlocks(1, 0x11);
    ok.virtual_size = 8 * kBlk;
    ok.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    ImageBuilder bad;
    bad.data = DataBlocks(1, 0x22);
    bad.virtual_size = 8 * kBlk;
    bad.seal_trailer = false;   // not sealed
    bad.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(ok.Build())));
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(bad.Build())));

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f = OpenFilesRo(files);
    MT_EXPECT_TRUE(!f.ok());
    if (!f.ok()) {
        // Named by position: a stack can be dozens of layers deep, and
        // "a layer is corrupt" would leave an operator to find it by hand.
        MT_EXPECT_TRUE(f.error().find("layer 1") != std::string::npos);
    }
}

MT_TEST(a_null_layer_in_the_stack_is_refused_by_position) {
    ImageBuilder b;
    b.data = DataBlocks(1, 0x11);
    b.virtual_size = 8 * kBlk;
    b.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(b.Build())));
    files.push_back(std::shared_ptr<VirtualFile>());

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f = OpenFilesRo(files);
    MT_EXPECT_TRUE(!f.ok());
    if (!f.ok()) MT_EXPECT_TRUE(f.error().find("layer 1") != std::string::npos);
}

MT_TEST(from_merged_layers_refuses_a_uuid_list_that_does_not_line_up) {
    std::vector<std::shared_ptr<VirtualFile> > layers;
    layers.push_back(std::shared_ptr<VirtualFile>(new MemFile(DataBlocks(1, 0))));
    layers.push_back(std::shared_ptr<VirtualFile>(new MemFile(DataBlocks(1, 0))));
    std::shared_ptr<LogIndex> idx(new ReadOnlyIndex(std::vector<SegmentMapping>()));

    std::vector<std::string> one_uuid;
    one_uuid.push_back("a");
    // Refused rather than padded: `GetUuid` would otherwise answer with
    // another layer's identity, which is worse than not answering.
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> r =
        LsmtReadOnlyFile::FromMergedLayers(idx, layers, kBlk, one_uuid);
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) MT_EXPECT_TRUE(r.error().find("uuid count") != std::string::npos);
}

MT_TEST(three_layers_resolve_in_the_right_priority_order) {
    // Block 0 in all three, block 1 in the bottom two, block 2 only in the
    // bottom. A single read must pick the newest holder of each.
    ImageBuilder l0;  // oldest
    l0.data = DataBlocks(3, 0x10);
    l0.virtual_size = 8 * kBlk;
    l0.mappings.push_back(SegmentMapping(0, 3, 8, false, 0));

    ImageBuilder l1;
    l1.data = DataBlocks(2, 0x20);
    l1.virtual_size = 8 * kBlk;
    l1.mappings.push_back(SegmentMapping(0, 2, 8, false, 0));

    ImageBuilder l2;  // newest
    l2.data = DataBlocks(1, 0x30);
    l2.virtual_size = 8 * kBlk;
    l2.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(l0.Build())));
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(l1.Build())));
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(l2.Build())));

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f = OpenFilesRo(files);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    std::vector<uint8_t> dst(static_cast<size_t>(3 * kBlk), 0);
    core::Expected<size_t, std::string> n = f.value()->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(dst[0]), 0x30);                           // newest
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(kBlk)]), 0x21);   // middle
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(2 * kBlk)]), 0x12);  // oldest
}

MT_TEST(layer_metadata_is_read_in_input_order) {
    ImageBuilder a;
    a.data = DataBlocks(1, 0x11);
    a.virtual_size = 8 * kBlk;
    a.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    ImageBuilder b;
    b.data = DataBlocks(1, 0x22);
    b.virtual_size = 32 * kBlk;
    b.mappings.push_back(SegmentMapping(0, 1, 8, false, 0));

    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(a.Build())));
    files.push_back(std::shared_ptr<VirtualFile>(new MemFile(b.Build())));

    core::Expected<std::vector<ReadOnlyLayerMetadata>, std::string> md =
        LoadReadonlyLayersMetadata(files);
    MT_EXPECT_TRUE(md.ok());
    if (!md.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(md.value().size()), 2);
    // Still input order here -- the reversal happens later, when the layers
    // are lined up against the merged index's tags.
    MT_EXPECT_EQ(static_cast<long long>(md.value()[0].virtual_size),
                 static_cast<long long>(8 * kBlk));
    MT_EXPECT_EQ(static_cast<long long>(md.value()[1].virtual_size),
                 static_cast<long long>(32 * kBlk));
    MT_EXPECT_EQ(static_cast<int>(md.value()[0].index_size), 1);
}

MT_MAIN
