// SPDX-License-Identifier: MIT
// Tests for overlaybd::lsmt —ported 1:1 from storage/overlaybd/src/lsmt/*.rs
// #[cfg(test)] modules.
#include "microtest.h"

#include <cstdio>

#include "agentenv/storage/overlaybd/lsmt.h"

using namespace agentenv::storage::overlaybd::lsmt;

static SegmentMapping SM(uint64_t off, uint32_t len, uint64_t moff) {
    return SegmentMapping(off, len, moff, false, 0);
}
static SegmentMapping SMT(uint64_t off, uint32_t len, uint64_t moff, uint8_t tag) {
    return SegmentMapping(off, len, moff, false, tag);
}

static bool eq_vec(const std::vector<SegmentMapping>& a,
                   const std::vector<SegmentMapping>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (!(a[i] == b[i])) return false;
    return true;
}

// Rust: test_readonly_lookup
MT_TEST(readonly_lookup) {
    std::vector<SegmentMapping> m;
    m.push_back(SM(0, 10, 0));
    m.push_back(SM(10, 10, 50));
    m.push_back(SM(100, 10, 20));
    ReadOnlyIndex idx(m);

    std::vector<SegmentMapping> out;
    idx.Lookup(Segment(5, 10), &out);
    std::vector<SegmentMapping> exp1;
    exp1.push_back(SM(5, 5, 5));
    exp1.push_back(SM(10, 5, 50));
    MT_EXPECT_TRUE(eq_vec(out, exp1));

    out.clear();
    idx.Lookup(Segment(16, 10), &out);
    std::vector<SegmentMapping> exp2;
    exp2.push_back(SM(16, 4, 56));
    MT_EXPECT_TRUE(eq_vec(out, exp2));

    out.clear();
    idx.Lookup(Segment(26, 10), &out);
    MT_EXPECT_TRUE(out.empty());

    out.clear();
    idx.Lookup(Segment(6, 100), &out);
    std::vector<SegmentMapping> exp4;
    exp4.push_back(SM(6, 4, 6));
    exp4.push_back(SM(10, 10, 50));
    exp4.push_back(SM(100, 6, 20));
    MT_EXPECT_TRUE(eq_vec(out, exp4));
}

// Rust: test_mutable_insert
MT_TEST(mutable_insert) {
    MutableIndex idx;
    idx.Insert(SM(0, 20, 0));
    idx.Insert(SM(10, 15, 50));
    idx.Insert(SM(30, 100, 20));
    idx.Insert(SM(5, 10, 3));
    idx.Insert(SM(40, 10, 123));
    idx.Insert(SM(200, 10, 2133));
    idx.Insert(SM(150, 100, 21));

    std::vector<SegmentMapping> exp;
    exp.push_back(SM(0, 5, 0));
    exp.push_back(SM(5, 10, 3));
    exp.push_back(SM(15, 10, 55));
    exp.push_back(SM(30, 10, 20));
    exp.push_back(SM(40, 10, 123));
    exp.push_back(SM(50, 80, 40));
    exp.push_back(SM(150, 100, 21));
    MT_EXPECT_TRUE(eq_vec(idx.Dump(), exp));
}

// Rust: test_index_compress (subset of the cases)
MT_TEST(index_compress) {
    {
        std::vector<SegmentMapping> src;
        src.push_back(SM(5, 5, 0)); src.push_back(SM(10, 10, 5)); src.push_back(SM(100, 10, 20));
        MT_EXPECT_EQ(static_cast<int>(CompressRawIndexPredict(src)), 2);
        size_t n = CompressRawIndex(&src);
        MT_EXPECT_EQ(static_cast<int>(n), 2);
        std::vector<SegmentMapping> exp;
        exp.push_back(SM(5, 15, 0)); exp.push_back(SM(100, 10, 20));
        MT_EXPECT_TRUE(eq_vec(src, exp));
    }
    {
        // broken by tag
        std::vector<SegmentMapping> src;
        src.push_back(SM(5, 5, 0)); src.push_back(SM(10, 10, 5));
        src.push_back(SMT(20, 10, 15, 1)); src.push_back(SM(100, 10, 20));
        size_t n = CompressRawIndex(&src);
        MT_EXPECT_EQ(static_cast<int>(n), 3);
    }
    {
        // broken by physical discontinuity
        std::vector<SegmentMapping> src;
        src.push_back(SM(0, 10, 100)); src.push_back(SM(10, 10, 200));
        MT_EXPECT_EQ(static_cast<int>(CompressRawIndex(&src)), 2);
    }
}

// Rust: test_compress_with_zeroed_segments
MT_TEST(compress_zeroed) {
    std::vector<SegmentMapping> m;
    m.push_back(SegmentMapping(0, 10, 0, true, 0));
    m.push_back(SegmentMapping(10, 10, 0, true, 0));
    MT_EXPECT_EQ(static_cast<int>(CompressRawIndex(&m)), 1);
    MT_EXPECT_TRUE(m[0] == SegmentMapping(0, 20, 0, true, 0));
}

// Rust: test_index_merge (Case 1: merge idx0 over idx1)
MT_TEST(index_merge_two_layers) {
    ReadOnlyIndex idx0(std::vector<SegmentMapping>{SM(5, 5, 0), SM(10, 10, 50), SM(100, 10, 20)});
    ReadOnlyIndex idx1(std::vector<SegmentMapping>{
        SM(0, 1, 7), SM(2, 4, 5), SM(15, 10, 22), SM(30, 15, 89), SM(87, 50, 32), SM(150, 10, 84)});

    // Upper = idx0 inserted into a MutableIndex; lower = merge(idx1..).
    MutableIndex upper;
    const std::vector<SegmentMapping>& m0 = idx0.Mappings();
    for (size_t i = 0; i < m0.size(); ++i) upper.Insert(m0[i]);

    std::vector<const ReadOnlyIndex*> lowers;
    lowers.push_back(&idx1);
    ReadOnlyIndex merged = ReadOnlyIndex::Merge(lowers);
    std::shared_ptr<ReadOnlyIndex> lower(new ReadOnlyIndex(merged));

    ComboIndex ci(upper, lower, 1);
    std::vector<SegmentMapping> out;
    size_t count = ci.Lookup(Segment(0, 10000), &out);

    std::vector<SegmentMapping> exp;
    exp.push_back(SMT(0, 1, 7, 0));
    exp.push_back(SMT(2, 3, 5, 0));
    exp.push_back(SMT(5, 5, 0, 1));
    exp.push_back(SMT(10, 10, 50, 1));
    exp.push_back(SMT(20, 5, 27, 0));
    exp.push_back(SMT(30, 15, 89, 0));
    exp.push_back(SMT(87, 13, 32, 0));
    exp.push_back(SMT(100, 10, 20, 1));
    exp.push_back(SMT(110, 27, 55, 0));
    exp.push_back(SMT(150, 10, 84, 0));
    MT_EXPECT_EQ(static_cast<int>(count), static_cast<int>(exp.size()));
    MT_EXPECT_TRUE(eq_vec(out, exp));
}

// Rust: test_segment_mapping_boundary
MT_TEST(segment_mapping_boundary) {
    SegmentMapping m(10, 20, 100, false, 0);
    MT_EXPECT_EQ(static_cast<int>(m.End()), 30);
    MT_EXPECT_EQ(static_cast<int>(m.MEnd()), 120);
    m.ForwardOffsetTo(15);
    MT_EXPECT_EQ(static_cast<int>(m.Offset()), 15);
    MT_EXPECT_EQ(static_cast<int>(m.Length()), 15);
    MT_EXPECT_EQ(static_cast<int>(m.moffset), 105);

    SegmentMapping m2(10, 20, 100, true, 0);
    MT_EXPECT_EQ(static_cast<int>(m2.MEnd()), 100);
    m2.ForwardOffsetTo(15);
    MT_EXPECT_EQ(static_cast<int>(m2.moffset), 100);
}

// format.rs: magic + bit packing round-trip
MT_TEST(disk_mapping_roundtrip) {
    SegmentMapping m(12345, 678, 90123, true, 7);
    DiskSegmentMapping d = DiskSegmentMapping::FromMemory(m);
    SegmentMapping back = d.ToMemory();
    MT_EXPECT_TRUE(back == m);
}

MT_TEST(header_magic_and_flags) {
    HeaderTrailer h = HeaderTrailer::New();
    MT_EXPECT_TRUE(h.VerifyMagic());
    MT_EXPECT_TRUE(static_cast<unsigned long long>(h.magic0) == 0x00020100544d534cULL);
 MT_EXPECT_TRUE(h.IsTrailer());
    h.SetHeader();
    MT_EXPECT_TRUE(h.IsHeader());
    h.SetSealed();
    MT_EXPECT_TRUE(h.IsSealed());
    h.SetDataFile();
    MT_EXPECT_TRUE(h.IsDataFile());
    h.SetIndexFile();
    MT_EXPECT_TRUE(!h.IsDataFile());
}

// format.rs: HeaderTrailer serialize/deserialize round-trip (390-byte layout).
MT_TEST(header_trailer_serialize_roundtrip) {
    HeaderTrailer h = HeaderTrailer::New();
    h.SetHeader();
    h.SetDataFile();
    h.SetSealed();
    h.index_offset = 8192;
    h.index_size = 5;
h.virtual_size = 1ULL << 30;
    h.version = 1;
    h.sub_version = 1;

    uint8_t buf[HeaderTrailer::kOnDiskSize];
    h.Serialize(buf);

    HeaderTrailer back;
 MT_EXPECT_TRUE(HeaderTrailer::Deserialize(buf, sizeof(buf), &back));
    MT_EXPECT_TRUE(back.VerifyMagic());
  MT_EXPECT_TRUE(back.IsHeader());
    MT_EXPECT_TRUE(back.IsDataFile());
    MT_EXPECT_TRUE(back.IsSealed());
    MT_EXPECT_EQ(static_cast<int>(back.index_offset), 8192);
    MT_EXPECT_EQ(static_cast<int>(back.index_size), 5);
    MT_EXPECT_TRUE(static_cast<unsigned long long>(back.virtual_size) == (1ULL << 30));
    // size field must equal the on-disk struct size (390).
    MT_EXPECT_EQ(static_cast<int>(back.size), 390);
}

// file/readonly.rs: build a real sealed LSMT file on disk, reopen via
// OpenIndexFile and verify the index looks up correctly.
MT_TEST(open_index_file_roundtrip) {
  const uint64_t HDR = HeaderTrailer::kSpace;  // 4096
    // Mappings to persist (already sorted, non-overlapping).
    std::vector<SegmentMapping> mappings;
    mappings.push_back(SM(0, 10, 2));
    mappings.push_back(SM(20, 5, 100));
    mappings.push_back(SegmentMapping(30, 8, 0, true, 0));  // zeroed segment

    // Build the file image: [header 4096][data 4096][index N*16][trailer 4096].
    const uint64_t data_off = HDR;
    const uint64_t data_len = HDR;          // arbitrary 1 block of "data"
    const uint64_t index_off = data_off + data_len;

    std::vector<uint8_t> image;
  image.resize(static_cast<size_t>(index_off), 0);

 // header @0
    {
        HeaderTrailer h = HeaderTrailer::New();
        h.SetHeader();
        h.SetDataFile();
        h.SetSealed();
        h.virtual_size = 38ULL * 4096;  // covers up to block 38
        uint8_t hb[HeaderTrailer::kOnDiskSize];
        h.Serialize(hb);
   for (size_t i = 0; i < sizeof(hb); ++i) image[i] = hb[i];
    }

    // index region: DiskSegmentMapping[] little-endian (low@+0, high@+8).
    for (size_t i = 0; i < mappings.size(); ++i) {
        DiskSegmentMapping d = DiskSegmentMapping::FromMemory(mappings[i]);
uint8_t rec[16];
        for (int k = 0; k < 8; ++k) rec[k] = static_cast<uint8_t>((d.data_low >> (8 * k)) & 0xFF);
    for (int k = 0; k < 8; ++k) rec[8 + k] = static_cast<uint8_t>((d.data_high >> (8 * k)) & 0xFF);
   image.insert(image.end(), rec, rec + 16);
    }

    // trailer block (4096), first 390 bytes are the HeaderTrailer.
  uint64_t trailer_off = image.size();
    {
        HeaderTrailer t = HeaderTrailer::New();
   t.SetHeader();     // start from header then flip to trailer
        t.SetDataFile();
        t.SetSealed();
        t.SetTrailer();
        t.index_offset = index_off;
        t.index_size = mappings.size();
    t.virtual_size = 38ULL * 4096;
        uint8_t tb[HeaderTrailer::kOnDiskSize];
     t.Serialize(tb);
        image.resize(static_cast<size_t>(trailer_off + HDR), 0);
        for (size_t i = 0; i < sizeof(tb); ++i) image[static_cast<size_t>(trailer_off) + i] = tb[i];
    }

    // Write to a temp file.
 std::string path = "/tmp/agentenv_lsmt_test.lsmt";
    FILE* f = std::fopen(path.c_str(), "wb");
    MT_EXPECT_TRUE(f != nullptr);
    if (f) {
      std::fwrite(&image[0], 1, image.size(), f);
        std::fclose(f);
    }

    // Reopen.
    uint64_t vsize = 0;
  std::string err;
    std::shared_ptr<ReadOnlyIndex> idx = OpenIndexFile(path, &vsize, &err);
    MT_EXPECT_TRUE(idx != nullptr);
    if (!idx) { std::remove(path.c_str()); return; }
    MT_EXPECT_TRUE(static_cast<unsigned long long>(vsize) == 38ULL * 4096);

    // 3 mappings persisted; all should be present (tags reset to 0).
    MT_EXPECT_EQ(static_cast<int>(idx->Mappings().size()), 3);

    std::vector<SegmentMapping> out;
    idx->Lookup(Segment(0, 40), &out);
    MT_EXPECT_EQ(static_cast<int>(out.size()), 3);
    MT_EXPECT_TRUE(out[0] == SM(0, 10, 2));
  MT_EXPECT_TRUE(out[1] == SM(20, 5, 100));
    MT_EXPECT_TRUE(out[2] == SegmentMapping(30, 8, 0, true, 0));

    std::remove(path.c_str());
}

// file/helper.rs: compact_to write path — pack mappings, then OpenIndexFile
// reads them back and the compacted index round-trips.
namespace {
class MemSource : public CompactSource {
 public:
    // One flat buffer per tag; byte-addressed.
    std::vector<std::vector<uint8_t> > layers;
    bool ReadAt(uint8_t tag, uint64_t off, uint8_t* dst, uint64_t len) override {
 if (tag >= layers.size()) return false;
        const std::vector<uint8_t>& b = layers[tag];
      if (off + len > b.size()) return false;
        for (uint64_t i = 0; i < len; ++i) dst[i] = b[static_cast<size_t>(off + i)];
        return true;
  }
};
}  // namespace

MT_TEST(compact_to_then_open_roundtrip) {
    const uint64_t A = 512;  // ALIGNMENT

    // Source layer 0: 40 sectors of recognizable data (byte = sector index).
    MemSource src;
  src.layers.resize(1);
    src.layers[0].resize(static_cast<size_t>(40 * A), 0);
    for (uint64_t s = 0; s < 40; ++s) {
        for (uint64_t k = 0; k < A; ++k) {
            src.layers[0][static_cast<size_t>(s * A + k)] = static_cast<uint8_t>(s & 0xFF);
  }
    }

    // Mappings to compact: two data segments (from layer 0) + one zeroed.
 // moffset is in 512B sectors within the source layer.
    std::vector<SegmentMapping> mappings;
    mappings.push_back(SegmentMapping(0, 10, 0, false, 0));    // virt[0..10)  <- src sectors 0..10
    mappings.push_back(SegmentMapping(10, 5, 20, false, 0));   // virt[10..15) <- src sectors 20..25
    mappings.push_back(SegmentMapping(15, 8, 0, true, 0));     // virt[15..23) zeroed

    const uint64_t vsize = 23 * 4096;
    std::string path = "/tmp/agentenv_lsmt_compact.lsmt";

    std::string err;
    bool ok = CompactTo(path, &src, mappings, vsize, &err);
    MT_EXPECT_TRUE(ok);

    // Reopen and verify the index.
    uint64_t got_vsize = 0;
    std::string oerr;
    std::shared_ptr<ReadOnlyIndex> idx = OpenIndexFile(path, &got_vsize, &oerr);
    MT_EXPECT_TRUE(idx != nullptr);
    if (!idx) { std::remove(path.c_str()); return; }
    MT_EXPECT_TRUE(static_cast<unsigned long long>(got_vsize) == vsize);

    std::vector<SegmentMapping> out;
    idx->Lookup(Segment(0, 23), &out);
    // First two data mappings are physically contiguous in the dest file
    // (packed sequentially), so CompressRawIndex merges them into one 15-sector
    // segment starting at dest sector 8 (=4096/512). The zeroed segment stays
    // separate.
    MT_EXPECT_EQ(static_cast<int>(out.size()), 2);
    // data segment: virt [0,15), dest moffset 8.
    MT_EXPECT_EQ(static_cast<int>(out[0].Offset()), 0);
    MT_EXPECT_EQ(static_cast<int>(out[0].Length()), 15);
    MT_EXPECT_EQ(static_cast<int>(out[0].moffset), 8);
    MT_EXPECT_TRUE(!out[0].zeroed);
    // zeroed segment: virt [15,23).
    MT_EXPECT_EQ(static_cast<int>(out[1].Offset()), 15);
    MT_EXPECT_EQ(static_cast<int>(out[1].Length()), 8);
    MT_EXPECT_TRUE(out[1].zeroed);

    // Verify the packed data bytes on disk: dest sector 8 should hold src sector 0,
    // dest sector 18 (8+10) should hold src sector 20.
    FILE* f = std::fopen(path.c_str(), "rb");
    MT_EXPECT_TRUE(f != nullptr);
    if (f) {
        uint8_t b0 = 0, b10 = 0;
        std::fseek(f, static_cast<long>(8 * A), SEEK_SET);
 MT_EXPECT_TRUE(std::fread(&b0, 1, 1, f) == 1);
        std::fseek(f, static_cast<long>(18 * A), SEEK_SET);
      MT_EXPECT_TRUE(std::fread(&b10, 1, 1, f) == 1);
        MT_EXPECT_EQ(static_cast<int>(b0), 0);  // src sector 0 -> byte 0
        MT_EXPECT_EQ(static_cast<int>(b10), 20);  // src sector 20 -> byte 20
        std::fclose(f);
    }

    std::remove(path.c_str());
}

MT_MAIN
