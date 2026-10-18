// SPDX-License-Identifier: MIT
// Tests for LSMTFile — the writable upper layer.
// Rust: storage/overlaybd/src/lsmt/file/readwrite.rs
#include "microtest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"

using namespace agentenv::storage::overlaybd;
namespace core = agentenv::core;

namespace {

const uint64_t kBlk = 512;
const uint64_t kHdr = HeaderTrailer::kSpace;  // 4096

/// A growable in-memory VirtualFile with real sparse semantics, so the
/// sparse layout's extent-map recovery can actually be exercised.
class MemFile : public VirtualFile {
 public:
    MemFile() {}

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override {
        if (offset >= data_.size()) return ByteBuffer();
        const size_t avail = data_.size() - static_cast<size_t>(offset);
        const size_t take = avail < len ? avail : len;
        return ByteBuffer(data_.begin() + static_cast<size_t>(offset),
                          data_.begin() + static_cast<size_t>(offset) + take);
    }

    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                 size_t len) override {
        if (len == 0) return static_cast<size_t>(0);
        if (data_.size() < offset + len) {
            data_.resize(static_cast<size_t>(offset) + len, 0);
            written_.resize(data_.size(), false);
        }
        std::memcpy(&data_[static_cast<size_t>(offset)], data, len);
        for (size_t i = 0; i < len; ++i) written_[static_cast<size_t>(offset) + i] = true;
        return len;
    }

    core::Expected<uint64_t, std::string> Size() override {
        return static_cast<uint64_t>(data_.size());
    }

    core::Expected<core::Unit, std::string> Truncate(uint64_t size) override {
        data_.resize(static_cast<size_t>(size), 0);
        written_.resize(static_cast<size_t>(size), false);
        return core::Unit();
    }

    core::Expected<core::Unit, std::string> Sync() override { return core::Unit(); }

    /// A real hole punch, so a discard actually un-writes the range.
    core::Expected<core::Unit, std::string> Discard(uint64_t offset, uint64_t len) override {
        for (uint64_t i = 0; i < len; ++i) {
            const size_t at = static_cast<size_t>(offset + i);
            if (at >= data_.size()) break;
            data_[at] = 0;
            written_[at] = false;
        }
        return core::Unit();
    }

    /// Reports the next written byte, which is what the sparse layout's
    /// recovery reads as "this block belongs to the layer".
    core::Expected<core::Optional<uint64_t>, std::string> SeekData(uint64_t offset) override {
        for (size_t i = static_cast<size_t>(offset); i < written_.size(); ++i) {
            if (written_[i]) return core::Optional<uint64_t>(static_cast<uint64_t>(i));
        }
        return core::Optional<uint64_t>();
    }

    core::Expected<core::Optional<uint64_t>, std::string> SeekHole(uint64_t offset) override {
        for (size_t i = static_cast<size_t>(offset); i < written_.size(); ++i) {
            if (!written_[i]) return core::Optional<uint64_t>(static_cast<uint64_t>(i));
        }
        return core::Optional<uint64_t>(static_cast<uint64_t>(written_.size()));
    }

    const std::vector<uint8_t>& Raw() const { return data_; }

 private:
    std::vector<uint8_t> data_;
    std::vector<bool> written_;
};

std::vector<uint8_t> Pattern(size_t len, uint8_t first) {
    std::vector<uint8_t> v(len, 0);
    for (size_t i = 0; i < len; ++i) v[i] = static_cast<uint8_t>(first + (i / kBlk));
    return v;
}

struct Rw {
    std::shared_ptr<MemFile> data;
    std::shared_ptr<MemFile> index;
    std::shared_ptr<LsmtFile> file;
    std::string err;
};

Rw MakeRw(RwLayout layout, uint64_t vsize) {
    Rw r;
    r.data.reset(new MemFile());
    if (layout != kRwSparse) r.index.reset(new MemFile());
    core::Expected<std::shared_ptr<LsmtFile>, std::string> f = LsmtFile::CreateWithMetadata(
        r.data, r.index, vsize, layout, "upper-uuid", std::string(), 0, 0);
    if (!f.ok()) {
        r.err = f.error();
        return r;
    }
    r.file = f.value();
    return r;
}

size_t Write(const std::shared_ptr<LsmtFile>& f, uint64_t off,
             const std::vector<uint8_t>& d) {
    core::Expected<size_t, std::string> r = f->WriteAt(off, &d[0], d.size());
    return r.ok() ? r.value() : static_cast<size_t>(-1);
}

/// Reads, and fails the test loudly on error rather than returning empty.
///
/// An earlier version returned an empty vector on failure, which turned a
/// read error into a segfault at the call site's `[0]` -- so the actual
/// message ("Invalid layer tag 4", a real porting bug) never got printed.
std::vector<uint8_t> Read(const std::shared_ptr<LsmtFile>& f, uint64_t off, size_t len) {
    std::vector<uint8_t> dst(len, 0xEE);
    core::Expected<size_t, std::string> n = f->ReadAtInto(off, &dst[0], len);
    if (!n.ok()) {
        std::fprintf(stderr, "  FAIL: read at %llu failed: %s\n",
                     static_cast<unsigned long long>(off), n.error().c_str());
        std::exit(1);
    }
    dst.resize(n.value());
    if (dst.empty()) {
        std::fprintf(stderr, "  FAIL: read at %llu returned nothing\n",
                     static_cast<unsigned long long>(off));
        std::exit(1);
    }
    return dst;
}

}  // namespace

// ============================================================
// layout selection
// ============================================================

MT_TEST(each_layout_maps_to_its_own_file_type) {
    MT_EXPECT_EQ(static_cast<int>(RwLayoutFileType(kRwLogStructured)),
                 static_cast<int>(kLsmtFileWritable));
    MT_EXPECT_EQ(static_cast<int>(RwLayoutFileType(kRwSparse)),
                 static_cast<int>(kLsmtFileSparseWritable));
    MT_EXPECT_EQ(static_cast<int>(RwLayoutFileType(kRwHybridLogStructured)),
                 static_cast<int>(kLsmtFileHybridWritable));
    // Four distinct values, not a writable/read-only boolean: the write path
    // branches on this, and collapsing two layouts would send writes to the
    // wrong place.
    MT_EXPECT_TRUE(kLsmtFileWritable != kLsmtFileSparseWritable);
    MT_EXPECT_TRUE(kLsmtFileSparseWritable != kLsmtFileHybridWritable);
}

MT_TEST(only_sparse_can_be_refused_by_the_platform) {
    // Log-structured and hybrid store their index explicitly, so nothing
    // about them depends on the filesystem's notion of "unwritten".
    MT_EXPECT_TRUE(EnsureRwLayoutSupported(kRwLogStructured).ok());
    MT_EXPECT_TRUE(EnsureRwLayoutSupported(kRwHybridLogStructured).ok());
    // Supported on Linux; the point is that the check exists and is keyed to
    // sparse alone.
    MT_EXPECT_TRUE(EnsureRwLayoutSupported(kRwSparse).ok());
}

MT_TEST(create_refuses_a_non_sparse_layout_without_an_index_file) {
    std::shared_ptr<MemFile> data(new MemFile());
    // The index file is where a log-backed layer's mappings live. Creating
    // one without it would produce a layer that writes data it can never
    // find again after a restart.
    MT_EXPECT_TRUE(!LsmtFile::CreateWithMetadata(data, std::shared_ptr<VirtualFile>(), 4096,
                                                  kRwLogStructured, "u", std::string(), 0, 0)
                        .ok());
    MT_EXPECT_TRUE(!LsmtFile::CreateWithMetadata(data, std::shared_ptr<VirtualFile>(), 4096,
                                                  kRwHybridLogStructured, "u", std::string(),
                                                  0, 0)
                        .ok());
    // Sparse needs none: its mappings are the extent map.
    MT_EXPECT_TRUE(LsmtFile::CreateWithMetadata(data, std::shared_ptr<VirtualFile>(), 4096,
                                                 kRwSparse, "u", std::string(), 0, 0)
                       .ok());
}

MT_TEST(create_is_sparse_or_log_structured_but_never_hybrid) {
    std::shared_ptr<MemFile> d1(new MemFile());
    std::shared_ptr<MemFile> i1(new MemFile());
    core::Expected<std::shared_ptr<LsmtFile>, std::string> a =
        LsmtFile::Create(d1, i1, 8 * kBlk, false);
    MT_EXPECT_TRUE(a.ok());
    if (a.ok()) {
        MT_EXPECT_EQ(static_cast<int>(a.value()->FileType()),
                     static_cast<int>(kLsmtFileWritable));
    }

    std::shared_ptr<MemFile> d2(new MemFile());
    core::Expected<std::shared_ptr<LsmtFile>, std::string> b =
        LsmtFile::Create(d2, std::shared_ptr<VirtualFile>(), 8 * kBlk, true);
    MT_EXPECT_TRUE(b.ok());
    if (b.ok()) {
        MT_EXPECT_EQ(static_cast<int>(b.value()->FileType()),
                     static_cast<int>(kLsmtFileSparseWritable));
    }
    // Hybrid is deliberately unreachable from the boolean constructor: a
    // caller that only knows "sparse or not" cannot have meant it.
}


// ============================================================
// write / read round trip, per layout
// ============================================================

MT_TEST(log_structured_appends_data_and_journals_every_mapping) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    MT_EXPECT_TRUE(rw.file.get() != 0);
    if (!rw.file) return;

    const std::vector<uint8_t> d = Pattern(2 * kBlk, 0xA0);
    MT_EXPECT_EQ(static_cast<int>(Write(rw.file, 0, d)), static_cast<int>(2 * kBlk));

    const std::vector<uint8_t> back = Read(rw.file, 0, static_cast<size_t>(2 * kBlk));
    MT_EXPECT_EQ(static_cast<int>(back.size()), static_cast<int>(2 * kBlk));
    if (back.size() == 2 * kBlk) {
        MT_EXPECT_EQ(static_cast<int>(back[0]), 0xA0);
        MT_EXPECT_EQ(static_cast<int>(back[static_cast<size_t>(kBlk)]), 0xA1);
    }

    // Data landed after the header, not at the logical offset: this layout
    // appends, and the index is what maps logical back to physical.
    core::Expected<uint64_t, std::string> dsz = rw.data->Size();
    MT_EXPECT_TRUE(dsz.ok());
    if (dsz.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(dsz.value()),
                     static_cast<long long>(kHdr + 2 * kBlk));
    }

    // One mapping appended to the index file, past its own header.
    core::Expected<uint64_t, std::string> isz = rw.index->Size();
    MT_EXPECT_TRUE(isz.ok());
    if (isz.ok()) MT_EXPECT_EQ(static_cast<long long>(isz.value()), static_cast<long long>(kHdr + 16));
}

MT_TEST(an_overwrite_appends_again_and_the_newer_mapping_wins) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;

    MT_EXPECT_EQ(static_cast<int>(Write(rw.file, 0, Pattern(kBlk, 0x11))),
                 static_cast<int>(kBlk));
    MT_EXPECT_EQ(static_cast<int>(Write(rw.file, 0, Pattern(kBlk, 0x22))),
                 static_cast<int>(kBlk));

    const std::vector<uint8_t> back = Read(rw.file, 0, static_cast<size_t>(kBlk));
    MT_EXPECT_EQ(static_cast<int>(back.size()), static_cast<int>(kBlk));
    if (!back.empty()) MT_EXPECT_EQ(static_cast<int>(back[0]), 0x22);

    // Nothing was reclaimed: the first copy is still on disk, unreferenced.
    // That gap between physical and valid is what a compact would recover.
    core::Expected<DataStat, std::string> st = rw.file->Stat();
    MT_EXPECT_TRUE(st.ok());
    if (st.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(st.value().total_data_size),
                     static_cast<long long>(2 * kBlk));
        MT_EXPECT_EQ(static_cast<long long>(st.value().valid_data_size),
                     static_cast<long long>(kBlk));
    }
}

MT_TEST(sparse_writes_in_place_at_the_logical_offset) {
    Rw rw = MakeRw(kRwSparse, 16 * kBlk);
    MT_EXPECT_TRUE(rw.file.get() != 0);
    if (!rw.file) return;

    // Pre-sized to the whole address space at create time, because the
    // extent map is the index and a write that extended the file would make
    // recovery depend on write order.
    core::Expected<uint64_t, std::string> sz0 = rw.data->Size();
    MT_EXPECT_TRUE(sz0.ok());
    if (sz0.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(sz0.value()),
                     static_cast<long long>(kHdr + 16 * kBlk));
    }

    MT_EXPECT_EQ(static_cast<int>(Write(rw.file, 4 * kBlk, Pattern(kBlk, 0x33))),
                 static_cast<int>(kBlk));
    const std::vector<uint8_t> back = Read(rw.file, 4 * kBlk, static_cast<size_t>(kBlk));
    if (!back.empty()) MT_EXPECT_EQ(static_cast<int>(back[0]), 0x33);

    // In place: the bytes are at header + logical offset in the raw file.
    MT_EXPECT_EQ(static_cast<int>(rw.data->Raw()[static_cast<size_t>(kHdr + 4 * kBlk)]), 0x33);
    // And the file did not grow.
    core::Expected<uint64_t, std::string> sz1 = rw.data->Size();
    if (sz1.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(sz1.value()),
                     static_cast<long long>(kHdr + 16 * kBlk));
    }
}

MT_TEST(sparse_journals_nothing_because_it_has_no_index_file) {
    Rw rw = MakeRw(kRwSparse, 16 * kBlk);
    if (!rw.file) return;
    MT_EXPECT_TRUE(!rw.index);
    // The write must still succeed. An append-index call that insisted on a
    // file would make the whole layout unusable.
    MT_EXPECT_EQ(static_cast<int>(Write(rw.file, 0, Pattern(kBlk, 0x44))),
                 static_cast<int>(kBlk));
}

MT_TEST(hybrid_appends_a_new_block_then_overwrites_it_in_place) {
    Rw rw = MakeRw(kRwHybridLogStructured, 16 * kBlk);
    MT_EXPECT_TRUE(rw.file.get() != 0);
    if (!rw.file) return;

    MT_EXPECT_EQ(static_cast<int>(Write(rw.file, 0, Pattern(kBlk, 0x55))),
                 static_cast<int>(kBlk));
    core::Expected<uint64_t, std::string> after_first = rw.data->Size();
    MT_EXPECT_TRUE(after_first.ok());

    // The layer now owns block 0, so the second write goes in place.
    MT_EXPECT_EQ(static_cast<int>(Write(rw.file, 0, Pattern(kBlk, 0x66))),
                 static_cast<int>(kBlk));
    core::Expected<uint64_t, std::string> after_second = rw.data->Size();
    MT_EXPECT_TRUE(after_second.ok());
    if (after_first.ok() && after_second.ok()) {
        // Unchanged, which is the whole point of hybrid: an overwrite of an
        // owned block costs no space.
        MT_EXPECT_EQ(static_cast<long long>(after_second.value()),
                     static_cast<long long>(after_first.value()));
    }
    const std::vector<uint8_t> back = Read(rw.file, 0, static_cast<size_t>(kBlk));
    if (!back.empty()) MT_EXPECT_EQ(static_cast<int>(back[0]), 0x66);
}

MT_TEST(a_write_is_split_by_max_io_size_without_a_seam) {
    Rw rw = MakeRw(kRwLogStructured, 64 * kBlk);
    if (!rw.file) return;
    MT_EXPECT_TRUE(rw.file->SetMaxIoSize(4096).ok());

    const std::vector<uint8_t> d = Pattern(40 * kBlk, 0x10);
    MT_EXPECT_EQ(static_cast<int>(Write(rw.file, 0, d)), static_cast<int>(40 * kBlk));

    const std::vector<uint8_t> back = Read(rw.file, 0, static_cast<size_t>(40 * kBlk));
    MT_EXPECT_EQ(static_cast<int>(back.size()), static_cast<int>(40 * kBlk));
    bool ok = !back.empty();
    for (size_t b = 0; b < 40 && ok; ++b) {
        if (back[b * static_cast<size_t>(kBlk)] != static_cast<uint8_t>(0x10 + b)) ok = false;
    }
    MT_EXPECT_TRUE(ok);
}

MT_TEST(max_io_size_must_be_4k_aligned_here_not_512) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    // Coarser than the read-only file's 512, because the write path chunks
    // data appends by this and a sub-page step would issue partial-page
    // writes for no benefit.
    MT_EXPECT_TRUE(!rw.file->SetMaxIoSize(0).ok());
    MT_EXPECT_TRUE(!rw.file->SetMaxIoSize(512).ok());
    MT_EXPECT_TRUE(rw.file->SetMaxIoSize(4096).ok());
    MT_EXPECT_EQ(rw.file->GetMaxIoSize(), static_cast<size_t>(4096));
}

MT_TEST(unaligned_and_empty_writes) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    const std::vector<uint8_t> d(kBlk, 0);
    // Refused rather than widened: the index addresses blocks, so there is
    // no exact mapping for a partial one.
    MT_EXPECT_TRUE(!rw.file->WriteAt(1, &d[0], d.size()).ok());
    MT_EXPECT_TRUE(!rw.file->WriteAt(0, &d[0], 100).ok());
    // Zero length is accepted and writes nothing.
    core::Expected<size_t, std::string> z = rw.file->WriteAt(0, 0, 0);
    MT_EXPECT_TRUE(z.ok());
    if (z.ok()) MT_EXPECT_EQ(z.value(), static_cast<size_t>(0));
}

MT_TEST(a_read_of_a_never_written_block_is_zeros) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0x77));

    const std::vector<uint8_t> back = Read(rw.file, kBlk, static_cast<size_t>(kBlk));
    MT_EXPECT_EQ(static_cast<int>(back.size()), static_cast<int>(kBlk));
    bool zero = !back.empty();
    for (size_t i = 0; i < back.size(); ++i) if (back[i] != 0) zero = false;
    MT_EXPECT_TRUE(zero);
}

MT_TEST(reads_are_clamped_to_the_virtual_size) {
    Rw rw = MakeRw(kRwLogStructured, 2 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0x88));

    const std::vector<uint8_t> back = Read(rw.file, 0, static_cast<size_t>(8 * kBlk));
    MT_EXPECT_EQ(static_cast<int>(back.size()), static_cast<int>(2 * kBlk));

    core::Expected<uint64_t, std::string> sz = rw.file->Size();
    MT_EXPECT_TRUE(sz.ok());
    // The virtual size, not the physical one: an append-only upper is
    // already larger than the address space it serves.
    if (sz.ok()) MT_EXPECT_EQ(static_cast<long long>(sz.value()), static_cast<long long>(2 * kBlk));
}


// ============================================================
// reopen — index recovery
// ============================================================

MT_TEST(a_log_structured_layer_replays_its_index_journal) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0xA1));
    Write(rw.file, 2 * kBlk, Pattern(kBlk, 0xA2));
    MT_EXPECT_TRUE(rw.file->Sync().ok());

    core::Expected<std::shared_ptr<LsmtFile>, std::string> re = LsmtFile::Open(
        rw.data, rw.index, std::shared_ptr<ReadOnlyIndex>(),
        std::vector<std::shared_ptr<VirtualFile> >());
    MT_EXPECT_TRUE(re.ok());
    if (!re.ok()) return;

    MT_EXPECT_EQ(static_cast<int>(Read(re.value(), 0, static_cast<size_t>(kBlk))[0]), 0xA1);
    MT_EXPECT_EQ(static_cast<int>(Read(re.value(), 2 * kBlk, static_cast<size_t>(kBlk))[0]),
                 0xA2);
    // Block 1 was never written, so it is still a hole after replay.
    const std::vector<uint8_t> hole = Read(re.value(), kBlk, static_cast<size_t>(kBlk));
    MT_EXPECT_EQ(static_cast<int>(hole[0]), 0);
    // The virtual size came from the header, not from the file length.
    core::Expected<uint64_t, std::string> sz = re.value()->Size();
    if (sz.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(sz.value()), static_cast<long long>(16 * kBlk));
    }
}

MT_TEST(replay_order_decides_which_mapping_survives) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0x11));
    Write(rw.file, 0, Pattern(kBlk, 0x99));  // same block, later record

    core::Expected<std::shared_ptr<LsmtFile>, std::string> re = LsmtFile::Open(
        rw.data, rw.index, std::shared_ptr<ReadOnlyIndex>(),
        std::vector<std::shared_ptr<VirtualFile> >());
    MT_EXPECT_TRUE(re.ok());
    if (!re.ok()) return;
    // The journal is replayed in file order, so the last record for a block
    // is the current one. Replaying backwards would resurrect overwritten
    // data with no error anywhere.
    MT_EXPECT_EQ(static_cast<int>(Read(re.value(), 0, static_cast<size_t>(kBlk))[0]), 0x99);
}

MT_TEST(a_sparse_layer_recovers_its_index_from_the_extent_map) {
    Rw rw = MakeRw(kRwSparse, 16 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 4 * kBlk, Pattern(kBlk, 0x33));

    core::Expected<std::shared_ptr<LsmtFile>, std::string> re = LsmtFile::Open(
        rw.data, std::shared_ptr<VirtualFile>(), std::shared_ptr<ReadOnlyIndex>(),
        std::vector<std::shared_ptr<VirtualFile> >());
    MT_EXPECT_TRUE(re.ok());
    if (!re.ok()) return;
    // No index file was consulted: which blocks the layer owns came from
    // where the data actually is.
    MT_EXPECT_EQ(static_cast<int>(Read(re.value(), 4 * kBlk, static_cast<size_t>(kBlk))[0]),
                 0x33);
    const std::vector<uint8_t> hole = Read(re.value(), 0, static_cast<size_t>(kBlk));
    MT_EXPECT_EQ(static_cast<int>(hole[0]), 0);
}

MT_TEST(open_refuses_a_truncated_index_journal) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0xA1));

    // Chop a few bytes off the last record, as a crash mid-append would.
    core::Expected<uint64_t, std::string> isz = rw.index->Size();
    MT_EXPECT_TRUE(isz.ok());
    if (!isz.ok()) return;
    MT_EXPECT_TRUE(rw.index->Truncate(isz.value() - 5).ok());

    core::Expected<std::shared_ptr<LsmtFile>, std::string> re = LsmtFile::Open(
        rw.data, rw.index, std::shared_ptr<ReadOnlyIndex>(),
        std::vector<std::shared_ptr<VirtualFile> >());
    MT_EXPECT_TRUE(!re.ok());
    if (!re.ok()) {
        // Refused rather than rounded down to whole records. A trailing
        // partial record means the last append never completed, and silently
        // dropping it would accept an index the writer never finished.
        MT_EXPECT_TRUE(re.error().find("not aligned") != std::string::npos);
    }
}

MT_TEST(open_refuses_a_non_sparse_layer_with_no_index_file) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    core::Expected<std::shared_ptr<LsmtFile>, std::string> re = LsmtFile::Open(
        rw.data, std::shared_ptr<VirtualFile>(), std::shared_ptr<ReadOnlyIndex>(),
        std::vector<std::shared_ptr<VirtualFile> >());
    MT_EXPECT_TRUE(!re.ok());
    if (!re.ok()) {
        MT_EXPECT_TRUE(re.error().find("missing index file") != std::string::npos);
    }
}

MT_TEST(open_refuses_a_header_with_both_layout_flags_set) {
    std::shared_ptr<MemFile> data(new MemFile());
    HeaderTrailer h = HeaderTrailer::New();
    h.SetHeader();
    h.SetDataFile();
    h.SetSparseRw();
    h.SetHybridRw();   // both
    h.virtual_size = 8 * kBlk;
    MT_EXPECT_TRUE(WriteHeaderBlock(data.get(), h).ok());

    core::Expected<std::shared_ptr<LsmtFile>, std::string> re =
        LsmtFile::Open(data, std::shared_ptr<VirtualFile>(), std::shared_ptr<ReadOnlyIndex>(),
                       std::vector<std::shared_ptr<VirtualFile> >());
    MT_EXPECT_TRUE(!re.ok());
    if (!re.ok()) {
        // Refused rather than resolved by precedence: the two layouts put
        // data in different places, so picking one would read the layer with
        // the wrong addressing scheme and return plausible garbage.
        MT_EXPECT_TRUE(re.error().find("both set") != std::string::npos);
    }
}

MT_TEST(the_upper_is_the_last_layer_so_its_tag_is_the_largest) {
    // Two stand-in lowers; their contents do not matter here, only that the
    // upper lands after them.
    std::vector<std::shared_ptr<VirtualFile> > lowers;
    lowers.push_back(std::shared_ptr<VirtualFile>(new MemFile()));
    lowers.push_back(std::shared_ptr<VirtualFile>(new MemFile()));

    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0xC1));

    core::Expected<std::shared_ptr<LsmtFile>, std::string> re =
        LsmtFile::Open(rw.data, rw.index, std::shared_ptr<ReadOnlyIndex>(), lowers);
    MT_EXPECT_TRUE(re.ok());
    if (!re.ok()) return;

    // Three layers: two lowers then the upper. A mapping's tag indexes
    // straight into this vector, so the upper has to be last for its tag to
    // be the largest -- the reverse of what OpenFilesRo takes.
    MT_EXPECT_EQ(static_cast<int>(re.value()->GetLowerFiles().size()), 3);
    MT_EXPECT_TRUE(re.value()->GetLowerFiles()[2] == rw.data);
    // And the replayed mapping resolves to the upper, not to a lower.
    MT_EXPECT_EQ(static_cast<int>(Read(re.value(), 0, static_cast<size_t>(kBlk))[0]), 0xC1);

    // The uuid slots line up with the layers: blank for the lowers, real for
    // the upper. Inventing lower identities would be worse than absence.
    core::Expected<std::string, std::string> u2 = re.value()->GetUuid(2);
    MT_EXPECT_TRUE(u2.ok());
    if (u2.ok()) MT_EXPECT_TRUE(u2.value() == "upper-uuid");
    MT_EXPECT_TRUE(re.value()->GetUuid(0).ok());
    MT_EXPECT_TRUE(!re.value()->GetUuid(3).ok());
}

MT_TEST(lower_files_are_handed_back_bottom_to_top_for_restacking) {
    std::shared_ptr<MemFile> l0(new MemFile());
    std::shared_ptr<MemFile> l1(new MemFile());
    std::vector<std::shared_ptr<VirtualFile> > lowers;
    lowers.push_back(l0);   // newest lower
    lowers.push_back(l1);   // older lower

    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    core::Expected<std::shared_ptr<LsmtFile>, std::string> re =
        LsmtFile::Open(rw.data, rw.index, std::shared_ptr<ReadOnlyIndex>(), lowers);
    MT_EXPECT_TRUE(re.ok());
    if (!re.ok()) return;

    const std::vector<std::shared_ptr<VirtualFile> > out =
        re.value()->LowerLayerFilesBottomToTop();
    MT_EXPECT_EQ(static_cast<int>(out.size()), 2);
    if (out.size() == 2) {
        // Reversed, and the upper excluded. Handing them over unreversed
        // would build a stack where the oldest layer shadows the newest.
        MT_EXPECT_TRUE(out[0] == l1);
        MT_EXPECT_TRUE(out[1] == l0);
    }
}


// ============================================================
// discard
// ============================================================

MT_TEST(a_discarded_range_reads_as_zeros_and_survives_reopen) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(2 * kBlk, 0xD1));

    MT_EXPECT_TRUE(rw.file->DiscardRange(0, kBlk).ok());
    MT_EXPECT_EQ(static_cast<int>(Read(rw.file, 0, static_cast<size_t>(kBlk))[0]), 0);
    // The second block is untouched.
    MT_EXPECT_EQ(static_cast<int>(Read(rw.file, kBlk, static_cast<size_t>(kBlk))[0]), 0xD2);

    core::Expected<std::shared_ptr<LsmtFile>, std::string> re = LsmtFile::Open(
        rw.data, rw.index, std::shared_ptr<ReadOnlyIndex>(),
        std::vector<std::shared_ptr<VirtualFile> >());
    MT_EXPECT_TRUE(re.ok());
    if (!re.ok()) return;
    // The zeroed mapping was journalled, so the discard is still in effect.
    // Had it only been applied in memory, the original data would reappear.
    MT_EXPECT_EQ(static_cast<int>(Read(re.value(), 0, static_cast<size_t>(kBlk))[0]), 0);
    MT_EXPECT_EQ(static_cast<int>(Read(re.value(), kBlk, static_cast<size_t>(kBlk))[0]), 0xD2);
}

MT_TEST(sparse_discard_punches_a_real_hole_so_recovery_agrees) {
    Rw rw = MakeRw(kRwSparse, 16 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(2 * kBlk, 0xE1));
    MT_EXPECT_TRUE(rw.file->DiscardRange(0, kBlk).ok());

    core::Expected<std::shared_ptr<LsmtFile>, std::string> re = LsmtFile::Open(
        rw.data, std::shared_ptr<VirtualFile>(), std::shared_ptr<ReadOnlyIndex>(),
        std::vector<std::shared_ptr<VirtualFile> >());
    MT_EXPECT_TRUE(re.ok());
    if (!re.ok()) return;
    // For sparse the extent map *is* the index, so the hole has to be real.
    // Leaving the blocks allocated would have them recovered as
    // owned-and-nonzero on reopen.
    MT_EXPECT_EQ(static_cast<int>(Read(re.value(), 0, static_cast<size_t>(kBlk))[0]), 0);
    MT_EXPECT_EQ(static_cast<int>(Read(re.value(), kBlk, static_cast<size_t>(kBlk))[0]), 0xE2);
}

MT_TEST(hybrid_discard_retains_the_space_for_reuse) {
    Rw rw = MakeRw(kRwHybridLogStructured, 16 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0xF1));
    core::Expected<uint64_t, std::string> before = rw.data->Size();
    MT_EXPECT_TRUE(before.ok());

    MT_EXPECT_TRUE(rw.file->DiscardRange(0, kBlk).ok());
    MT_EXPECT_EQ(static_cast<int>(Read(rw.file, 0, static_cast<size_t>(kBlk))[0]), 0);

    // Writing the same block again reuses the retained range rather than
    // appending, which is what distinguishes a hybrid discard from the
    // others: the mapping keeps its physical offset and is only marked
    // zeroed.
    Write(rw.file, 0, Pattern(kBlk, 0xF9));
    core::Expected<uint64_t, std::string> after = rw.data->Size();
    MT_EXPECT_TRUE(after.ok());
    if (before.ok() && after.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(after.value()),
                     static_cast<long long>(before.value()));
    }
    MT_EXPECT_EQ(static_cast<int>(Read(rw.file, 0, static_cast<size_t>(kBlk))[0]), 0xF9);
}

MT_TEST(discard_alignment_and_zero_length) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    MT_EXPECT_TRUE(rw.file->DiscardRange(0, 0).ok());
    MT_EXPECT_TRUE(!rw.file->DiscardRange(1, kBlk).ok());
    MT_EXPECT_TRUE(!rw.file->DiscardRange(0, 100).ok());
}

MT_TEST(a_discard_longer_than_one_segment_is_chunked) {
    // A mapping's length is 14 bits, so a discard spanning more blocks than
    // that needs several mappings. 20000 blocks exceeds the 16383 limit.
    const uint64_t vsize = 20000 * kBlk;
    Rw rw = MakeRw(kRwLogStructured, vsize);
    if (!rw.file) return;
    MT_EXPECT_TRUE(rw.file->DiscardRange(0, vsize).ok());
    MT_EXPECT_EQ(static_cast<int>(Read(rw.file, 0, static_cast<size_t>(kBlk))[0]), 0);
    MT_EXPECT_EQ(static_cast<int>(Read(rw.file, 19999 * kBlk, static_cast<size_t>(kBlk))[0]), 0);
}

// ============================================================
// hybrid write planning
// ============================================================
//
// Pure, so it can be exercised directly. The fragment split *is* the hybrid
// write decision: get it wrong and a write either lands on a block this layer
// does not own, or appends a copy of a block it already has.

MT_TEST(an_untouched_range_plans_as_a_single_append) {
    MutableIndex upper;
    core::Expected<std::vector<WriteFragment>, std::string> f =
        LsmtFile::PlanHybridWrite(upper, 0, static_cast<size_t>(2 * kBlk));
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(f.value().size()), 1);
    if (!f.value().empty()) {
        MT_EXPECT_EQ(static_cast<int>(f.value()[0].kind),
                     static_cast<int>(WriteFragment::kAppend));
        MT_EXPECT_EQ(static_cast<int>(f.value()[0].len), static_cast<int>(2 * kBlk));
    }
}

MT_TEST(a_range_the_layer_owns_plans_as_in_place) {
    MutableIndex upper;
    upper.Insert(SegmentMapping(0, 2, 8, false, 0));
    core::Expected<std::vector<WriteFragment>, std::string> f =
        LsmtFile::PlanHybridWrite(upper, 0, static_cast<size_t>(2 * kBlk));
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(f.value().size()), 1);
    if (!f.value().empty()) {
        MT_EXPECT_EQ(static_cast<int>(f.value()[0].kind),
                     static_cast<int>(WriteFragment::kInPlace));
        // The physical offset is derived from the mapping, so the write lands
        // on the space the layer already holds.
        MT_EXPECT_EQ(static_cast<long long>(f.value()[0].phys_offset),
                     static_cast<long long>(8 * kBlk));
    }
}

MT_TEST(a_zeroed_mapping_with_space_plans_as_reuse_not_in_place) {
    MutableIndex upper;
    upper.Insert(SegmentMapping(0, 1, 8, true, 0));  // zeroed, but owns block 8
    core::Expected<std::vector<WriteFragment>, std::string> f =
        LsmtFile::PlanHybridWrite(upper, 0, static_cast<size_t>(kBlk));
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(f.value().size()), 1);
    if (!f.value().empty()) {
        // Distinct from kInPlace, and that distinction is the whole point:
        // kInPlace publishes no mapping, so the range would stay recorded as
        // zeroed and read back as zeros -- including after reopen.
        MT_EXPECT_EQ(static_cast<int>(f.value()[0].kind),
                     static_cast<int>(WriteFragment::kReuseZero));
        MT_EXPECT_EQ(static_cast<long long>(f.value()[0].phys_offset),
                     static_cast<long long>(8 * kBlk));
    }
}

MT_TEST(a_zeroed_mapping_without_space_plans_as_append) {
    MutableIndex upper;
    upper.Insert(SegmentMapping(0, 1, kNoPhysicalOffset, true, 0));
    core::Expected<std::vector<WriteFragment>, std::string> f =
        LsmtFile::PlanHybridWrite(upper, 0, static_cast<size_t>(kBlk));
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(f.value().size()), 1);
    if (!f.value().empty()) {
        // It owns no space, so there is nothing to write into.
        MT_EXPECT_EQ(static_cast<int>(f.value()[0].kind),
                     static_cast<int>(WriteFragment::kAppend));
    }
}

MT_TEST(a_partially_owned_range_splits_into_several_fragments) {
    MutableIndex upper;
    // Owns block 1 only, of the four being written.
    upper.Insert(SegmentMapping(1, 1, 8, false, 0));
    core::Expected<std::vector<WriteFragment>, std::string> f =
        LsmtFile::PlanHybridWrite(upper, 0, static_cast<size_t>(4 * kBlk));
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    // append(block 0) + in-place(block 1) + append(blocks 2-3).
    MT_EXPECT_EQ(static_cast<int>(f.value().size()), 3);
    if (f.value().size() == 3) {
        MT_EXPECT_EQ(static_cast<int>(f.value()[0].kind),
                     static_cast<int>(WriteFragment::kAppend));
        MT_EXPECT_EQ(static_cast<int>(f.value()[0].len), static_cast<int>(kBlk));
        MT_EXPECT_EQ(static_cast<int>(f.value()[1].kind),
                     static_cast<int>(WriteFragment::kInPlace));
        MT_EXPECT_EQ(static_cast<int>(f.value()[2].kind),
                     static_cast<int>(WriteFragment::kAppend));
        MT_EXPECT_EQ(static_cast<int>(f.value()[2].len), static_cast<int>(2 * kBlk));
        // Each fragment names its slice of the caller's chunk, so the three
        // writes reassemble the original buffer with no overlap or gap.
        MT_EXPECT_EQ(static_cast<int>(f.value()[0].data_offset), 0);
        MT_EXPECT_EQ(static_cast<int>(f.value()[1].data_offset), static_cast<int>(kBlk));
        MT_EXPECT_EQ(static_cast<int>(f.value()[2].data_offset), static_cast<int>(2 * kBlk));
    }
}

MT_TEST(a_write_crossing_several_owned_runs_reassembles_exactly) {
    Rw rw = MakeRw(kRwHybridLogStructured, 16 * kBlk);
    if (!rw.file) return;
    // Own blocks 1 and 3, leaving 0, 2 and 4 to be appended.
    Write(rw.file, kBlk, Pattern(kBlk, 0x01));
    Write(rw.file, 3 * kBlk, Pattern(kBlk, 0x03));

    const std::vector<uint8_t> d = Pattern(5 * kBlk, 0x50);
    MT_EXPECT_EQ(static_cast<int>(Write(rw.file, 0, d)), static_cast<int>(5 * kBlk));

    const std::vector<uint8_t> back = Read(rw.file, 0, static_cast<size_t>(5 * kBlk));
    MT_EXPECT_EQ(static_cast<int>(back.size()), static_cast<int>(5 * kBlk));
    bool ok = true;
    for (size_t b = 0; b < 5; ++b) {
        if (back[b * static_cast<size_t>(kBlk)] != static_cast<uint8_t>(0x50 + b)) ok = false;
    }
    // Mixed in-place and appended fragments, read back as one contiguous
    // range. Two appends that were handed the same offset would show up here
    // as a repeated block.
    MT_EXPECT_TRUE(ok);
}


// ============================================================
// append digest
// ============================================================

MT_TEST(the_digest_tracks_contiguous_appends_only) {
    std::shared_ptr<MemFile> f(new MemFile());
    HeaderTrailer h = HeaderTrailer::New();
    h.SetHeader();
    h.SetDataFile();
    MT_EXPECT_TRUE(WriteHeaderBlock(f.get(), h).ok());

    core::Expected<AppendDigestTracker, std::string> t = AppendDigestTracker::FromHeader(f.get());
    MT_EXPECT_TRUE(t.ok());
    if (!t.ok()) return;
    AppendDigestTracker tr = t.value();
    // Seeded from the header, so the digest covers the whole file from byte
    // zero -- not just the data appended after it.
    MT_EXPECT_EQ(static_cast<long long>(tr.Offset()), static_cast<long long>(kHdr));

    const std::vector<uint8_t> d(512, 0xAB);
    MT_EXPECT_TRUE(tr.Absorb(kHdr, &d[0], d.size()).ok());
    MT_EXPECT_EQ(static_cast<long long>(tr.Offset()), static_cast<long long>(kHdr + 512));

    // A gap is refused: the digest would then describe a file with a hole it
    // does not account for.
    MT_EXPECT_TRUE(!tr.Absorb(kHdr + 1024, &d[0], d.size()).ok());
    // A rewind too: it would hash overwritten bytes twice.
    MT_EXPECT_TRUE(!tr.Absorb(kHdr, &d[0], d.size()).ok());
    // Either way the result would still look like a valid digest of
    // something, which is worse than reporting none.
    MT_EXPECT_EQ(static_cast<long long>(tr.Offset()), static_cast<long long>(kHdr + 512));

    // Zero length is a no-op, not an ordering violation.
    MT_EXPECT_TRUE(tr.Absorb(0, 0, 0).ok());

    LayerDescriptor desc = tr.Descriptor();
    MT_EXPECT_TRUE(desc.digest.find("sha256:") == 0);
    MT_EXPECT_EQ(static_cast<long long>(desc.size), static_cast<long long>(kHdr + 512));
    // Reporting the descriptor does not end the tracker: sealing reports it
    // and then keeps absorbing the trailer bytes it is still writing.
    MT_EXPECT_TRUE(tr.Absorb(kHdr + 512, &d[0], d.size()).ok());
}

MT_TEST(only_the_append_only_layout_carries_a_digest) {
    // Sparse and hybrid rewrite blocks in place, so a hash of the stream
    // would describe bytes that no longer exist.
    Rw sp = MakeRw(kRwSparse, 8 * kBlk);
    if (!sp.file) return;
    Write(sp.file, 0, Pattern(kBlk, 0x11));
    core::Expected<core::Optional<LayerDescriptor>, std::string> s1 = sp.file->CloseSeal();
    MT_EXPECT_TRUE(s1.ok());
    if (s1.ok()) MT_EXPECT_TRUE(!s1.value().has_value());

    Rw hy = MakeRw(kRwHybridLogStructured, 8 * kBlk);
    if (!hy.file) return;
    Write(hy.file, 0, Pattern(kBlk, 0x22));
    core::Expected<core::Optional<LayerDescriptor>, std::string> s2 = hy.file->CloseSeal();
    MT_EXPECT_TRUE(s2.ok());
    // Absent, and that is normal rather than a failure.
    if (s2.ok()) MT_EXPECT_TRUE(!s2.value().has_value());
}

MT_TEST(a_log_structured_layer_reports_a_digest_covering_the_sealed_file) {
    Rw rw = MakeRw(kRwLogStructured, 8 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(2 * kBlk, 0x33));

    core::Expected<core::Optional<LayerDescriptor>, std::string> sealed = rw.file->CloseSeal();
    MT_EXPECT_TRUE(sealed.ok());
    if (!sealed.ok()) return;
    MT_EXPECT_TRUE(sealed.value().has_value());
    if (!sealed.value().has_value()) return;

    MT_EXPECT_TRUE(sealed.value()->digest.find("sha256:") == 0);
    // The digest's size is the whole sealed file, index and trailer included
    // -- so it identifies what a consumer would download, not just the data
    // region.
    core::Expected<uint64_t, std::string> fsz = rw.data->Size();
    MT_EXPECT_TRUE(fsz.ok());
    if (fsz.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(sealed.value()->size),
                     static_cast<long long>(fsz.value()));
    }
}

MT_TEST(a_reopened_layer_with_existing_data_has_no_digest) {
    Rw rw = MakeRw(kRwLogStructured, 8 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0x44));

    core::Expected<std::shared_ptr<LsmtFile>, std::string> re = LsmtFile::Open(
        rw.data, rw.index, std::shared_ptr<ReadOnlyIndex>(),
        std::vector<std::shared_ptr<VirtualFile> >());
    MT_EXPECT_TRUE(re.ok());
    if (!re.ok()) return;

    core::Expected<core::Optional<LayerDescriptor>, std::string> sealed =
        re.value()->CloseSeal();
    MT_EXPECT_TRUE(sealed.ok());
    // Nothing was hashed: the existing bytes were written before this handle
    // existed, so a running digest would describe a prefix rather than the
    // file. Reporting absence beats reporting a wrong identity.
    if (sealed.ok()) MT_EXPECT_TRUE(!sealed.value().has_value());
}

// ============================================================
// seal
// ============================================================

MT_TEST(a_sealed_layer_reopens_through_the_read_only_path) {
    Rw rw = MakeRw(kRwLogStructured, 8 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0x61));
    Write(rw.file, 2 * kBlk, Pattern(kBlk, 0x62));
    MT_EXPECT_TRUE(rw.file->CloseSeal().ok());

    // The whole point of sealing: the layer becomes an ordinary read-only
    // LSMT file, openable by code that knows nothing about RW layouts.
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> ro =
        LsmtReadOnlyFile::Open(rw.data);
    MT_EXPECT_TRUE(ro.ok());
    if (!ro.ok()) return;

    std::vector<uint8_t> dst(static_cast<size_t>(3 * kBlk), 0xEE);
    core::Expected<size_t, std::string> n = ro.value()->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(dst[0]), 0x61);
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(kBlk)]), 0);      // hole
    MT_EXPECT_EQ(static_cast<int>(dst[static_cast<size_t>(2 * kBlk)]), 0x62);

    core::Expected<uint64_t, std::string> sz = ro.value()->Size();
    if (sz.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(sz.value()), static_cast<long long>(8 * kBlk));
    }
}

MT_TEST(a_sealed_sparse_layer_also_reopens_read_only) {
    Rw rw = MakeRw(kRwSparse, 8 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 2 * kBlk, Pattern(kBlk, 0x71));
    MT_EXPECT_TRUE(rw.file->CloseSeal().ok());

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> ro =
        LsmtReadOnlyFile::Open(rw.data);
    MT_EXPECT_TRUE(ro.ok());
    if (!ro.ok()) return;
    // The sparse and hybrid markers are cleared in the trailer: they describe
    // how a *writable* layer stores data, and a reader that saw them would
    // take a recovery path that no longer applies.
    std::vector<uint8_t> dst(static_cast<size_t>(kBlk), 0xEE);
    core::Expected<size_t, std::string> n =
        ro.value()->ReadAtInto(2 * kBlk, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (n.ok()) MT_EXPECT_EQ(static_cast<int>(dst[0]), 0x71);
}

MT_TEST(sealing_twice_is_refused) {
    Rw rw = MakeRw(kRwLogStructured, 8 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0x81));
    MT_EXPECT_TRUE(rw.file->CloseSeal().ok());

    core::Expected<core::Optional<LayerDescriptor>, std::string> again = rw.file->CloseSeal();
    MT_EXPECT_TRUE(!again.ok());
    if (!again.ok()) {
        // A second seal would append another index and trailer after the
        // first, leaving a file whose trailer points at an index that is no
        // longer last.
        MT_EXPECT_TRUE(again.error().find("already been sealed") != std::string::npos);
    }
}

MT_TEST(a_sealed_layer_refuses_writes_and_discards) {
    Rw rw = MakeRw(kRwLogStructured, 8 * kBlk);
    if (!rw.file) return;
    MT_EXPECT_TRUE(rw.file->CloseSeal().ok());
    MT_EXPECT_TRUE(rw.file->IsSealed());

    const std::vector<uint8_t> d(kBlk, 0x99);
    MT_EXPECT_TRUE(!rw.file->WriteAt(0, &d[0], d.size()).ok());
    MT_EXPECT_TRUE(!rw.file->DiscardRange(0, kBlk).ok());
}

MT_TEST(open_refuses_a_sealed_data_file_in_rw_mode) {
    Rw rw = MakeRw(kRwLogStructured, 8 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0x91));
    MT_EXPECT_TRUE(rw.file->CloseSeal().ok());

    core::Expected<std::shared_ptr<LsmtFile>, std::string> re = LsmtFile::Open(
        rw.data, rw.index, std::shared_ptr<ReadOnlyIndex>(),
        std::vector<std::shared_ptr<VirtualFile> >());
    MT_EXPECT_TRUE(!re.ok());
    if (!re.ok()) {
        // Appending to it would overwrite the index its trailer points at,
        // leaving a file that still claims to be sealed and valid.
        MT_EXPECT_TRUE(re.error().find("Sealed") != std::string::npos);
    }
}

MT_TEST(a_discarded_block_is_sealed_without_a_physical_offset) {
    Rw rw = MakeRw(kRwHybridLogStructured, 8 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0xA9));
    // Hybrid retains the space, so the live mapping still has an offset.
    MT_EXPECT_TRUE(rw.file->DiscardRange(0, kBlk).ok());
    MT_EXPECT_TRUE(rw.file->CloseSeal().ok());

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> ro =
        LsmtReadOnlyFile::Open(rw.data);
    MT_EXPECT_TRUE(ro.ok());
    if (!ro.ok()) return;
    std::vector<uint8_t> dst(static_cast<size_t>(kBlk), 0xEE);
    core::Expected<size_t, std::string> n = ro.value()->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    // Zeros. The retained offset was dropped at seal: a sealed layer has no
    // writer left to reuse the space, so keeping it would describe space the
    // compacted file does not have.
    if (n.ok()) MT_EXPECT_EQ(static_cast<int>(dst[0]), 0);
}

MT_TEST(group_commit_buffers_mappings_until_flushed) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    // Room for four records.
    rw.file->SetIndexGroupCommit(4 * 16);
    MT_EXPECT_EQ(rw.file->GetIndexGroupCommitSize(), static_cast<size_t>(64));

    core::Expected<uint64_t, std::string> base = rw.index->Size();
    MT_EXPECT_TRUE(base.ok());
    if (!base.ok()) return;

    Write(rw.file, 0, Pattern(kBlk, 0x01));
    Write(rw.file, kBlk, Pattern(kBlk, 0x02));
    core::Expected<uint64_t, std::string> mid = rw.index->Size();
    MT_EXPECT_TRUE(mid.ok());
    // Nothing on disk yet. The data is written, but a crash here would lose
    // the mappings and the blocks would read as holes on reopen -- which is
    // the trade this setting makes.
    if (mid.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(mid.value()),
                     static_cast<long long>(base.value()));
    }
    // In-memory reads are unaffected.
    MT_EXPECT_EQ(static_cast<int>(Read(rw.file, 0, static_cast<size_t>(kBlk))[0]), 0x01);

    MT_EXPECT_TRUE(rw.file->FlushGroupCommit().ok());
    core::Expected<uint64_t, std::string> after = rw.index->Size();
    MT_EXPECT_TRUE(after.ok());
    if (after.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(after.value()),
                     static_cast<long long>(base.value() + 32));
    }

    // And a reopen now sees them.
    core::Expected<std::shared_ptr<LsmtFile>, std::string> re = LsmtFile::Open(
        rw.data, rw.index, std::shared_ptr<ReadOnlyIndex>(),
        std::vector<std::shared_ptr<VirtualFile> >());
    MT_EXPECT_TRUE(re.ok());
    if (re.ok()) {
        MT_EXPECT_EQ(static_cast<int>(Read(re.value(), 0, static_cast<size_t>(kBlk))[0]), 0x01);
    }
}

MT_TEST(group_commit_flushes_itself_once_the_buffer_fills) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    rw.file->SetIndexGroupCommit(2 * 16);  // two records

    core::Expected<uint64_t, std::string> base = rw.index->Size();
    if (!base.ok()) return;
    Write(rw.file, 0, Pattern(kBlk, 0x01));
    Write(rw.file, kBlk, Pattern(kBlk, 0x02));
    core::Expected<uint64_t, std::string> after = rw.index->Size();
    MT_EXPECT_TRUE(after.ok());
    // Flushed on reaching capacity, without an explicit call.
    if (after.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(after.value()),
                     static_cast<long long>(base.value() + 32));
    }
}

MT_TEST(a_buffer_smaller_than_one_record_still_flushes) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    // Below the 16-byte record size. The capacity floor of one matters: a
    // computed capacity of zero would never flush, so every mapping would
    // sit in memory until seal.
    rw.file->SetIndexGroupCommit(4);

    core::Expected<uint64_t, std::string> base = rw.index->Size();
    if (!base.ok()) return;
    Write(rw.file, 0, Pattern(kBlk, 0x01));
    core::Expected<uint64_t, std::string> after = rw.index->Size();
    MT_EXPECT_TRUE(after.ok());
    if (after.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(after.value()),
                     static_cast<long long>(base.value() + 16));
    }
}

MT_TEST(seal_flushes_buffered_mappings_rather_than_losing_them) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    rw.file->SetIndexGroupCommit(64);
    Write(rw.file, 0, Pattern(kBlk, 0xB1));
    MT_EXPECT_TRUE(rw.file->CloseSeal().ok());

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> ro =
        LsmtReadOnlyFile::Open(rw.data);
    MT_EXPECT_TRUE(ro.ok());
    if (!ro.ok()) return;
    std::vector<uint8_t> dst(static_cast<size_t>(kBlk), 0xEE);
    core::Expected<size_t, std::string> n = ro.value()->ReadAtInto(0, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    // The mapping reached the sealed index. Had seal skipped the flush, the
    // sealed layer would read as a hole.
    if (n.ok()) MT_EXPECT_EQ(static_cast<int>(dst[0]), 0xB1);
}

// ============================================================
// resize
// ============================================================

MT_TEST(growing_the_virtual_size_exposes_new_zeroed_space) {
    Rw rw = MakeRw(kRwLogStructured, 2 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0xC1));

    MT_EXPECT_TRUE(rw.file->UpdateVsize(8 * kBlk).ok());
    core::Expected<uint64_t, std::string> sz = rw.file->Size();
    if (sz.ok()) MT_EXPECT_EQ(static_cast<long long>(sz.value()), static_cast<long long>(8 * kBlk));
    // The new range is addressable and reads as zeros.
    MT_EXPECT_EQ(static_cast<int>(Read(rw.file, 4 * kBlk, static_cast<size_t>(kBlk))[0]), 0);

    // And the header carries it, so a reopen agrees.
    core::Expected<std::shared_ptr<LsmtFile>, std::string> re = LsmtFile::Open(
        rw.data, rw.index, std::shared_ptr<ReadOnlyIndex>(),
        std::vector<std::shared_ptr<VirtualFile> >());
    MT_EXPECT_TRUE(re.ok());
    if (re.ok()) {
        core::Expected<uint64_t, std::string> rsz = re.value()->Size();
        if (rsz.ok()) {
            MT_EXPECT_EQ(static_cast<long long>(rsz.value()),
                         static_cast<long long>(8 * kBlk));
        }
    }
}

MT_TEST(resizing_a_sparse_layer_resizes_the_file_itself) {
    Rw rw = MakeRw(kRwSparse, 4 * kBlk);
    if (!rw.file) return;
    MT_EXPECT_TRUE(rw.file->UpdateVsize(16 * kBlk).ok());

    core::Expected<uint64_t, std::string> fsz = rw.data->Size();
    MT_EXPECT_TRUE(fsz.ok());
    // For sparse the file length *is* the address space, so it has to follow.
    if (fsz.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(fsz.value()),
                     static_cast<long long>(kHdr + 16 * kBlk));
    }
    // A write at the new end lands in place.
    MT_EXPECT_EQ(static_cast<int>(Write(rw.file, 15 * kBlk, Pattern(kBlk, 0xD5))),
                 static_cast<int>(kBlk));
    MT_EXPECT_EQ(static_cast<int>(Read(rw.file, 15 * kBlk, static_cast<size_t>(kBlk))[0]), 0xD5);
}

MT_TEST(the_index_file_header_tracks_the_virtual_size_too) {
    Rw rw = MakeRw(kRwLogStructured, 2 * kBlk);
    if (!rw.file) return;
    MT_EXPECT_TRUE(rw.file->UpdateVsize(32 * kBlk).ok());

    // Both headers carry it, so a reopen that consults the index does not
    // disagree with the data file.
    core::Expected<ByteBuffer, std::string> raw = rw.index->ReadAt(0, static_cast<size_t>(kHdr));
    MT_EXPECT_TRUE(raw.ok());
    if (!raw.ok()) return;
    HeaderTrailer h;
    MT_EXPECT_TRUE(HeaderTrailer::Deserialize(&raw.value()[0], raw.value().size(), &h));
    MT_EXPECT_EQ(static_cast<long long>(h.virtual_size), static_cast<long long>(32 * kBlk));
}

MT_TEST(the_digest_survives_a_resize_by_being_rebuilt) {
    Rw rw = MakeRw(kRwLogStructured, 4 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0xE1));

    // The running digest covers the header, and the header is about to
    // change -- so it is dropped and rebuilt by re-reading the file.
    MT_EXPECT_TRUE(rw.file->UpdateVsize(8 * kBlk).ok());

    core::Expected<core::Optional<LayerDescriptor>, std::string> sealed = rw.file->CloseSeal();
    MT_EXPECT_TRUE(sealed.ok());
    if (!sealed.ok()) return;
    MT_EXPECT_TRUE(sealed.value().has_value());
    if (sealed.value().has_value()) {
        core::Expected<uint64_t, std::string> fsz = rw.data->Size();
        if (fsz.ok()) {
            MT_EXPECT_EQ(static_cast<long long>(sealed.value()->size),
                         static_cast<long long>(fsz.value()));
        }
    }
}

MT_TEST(truncate_is_the_resize_path) {
    Rw rw = MakeRw(kRwLogStructured, 2 * kBlk);
    if (!rw.file) return;
    // `Truncate` on this file means "set the virtual size", not "shorten the
    // backing file": the backing file for an append-only upper is already
    // larger than the address space and shrinking it would discard data.
    MT_EXPECT_TRUE(rw.file->Truncate(16 * kBlk).ok());
    core::Expected<uint64_t, std::string> sz = rw.file->Size();
    if (sz.ok()) MT_EXPECT_EQ(static_cast<long long>(sz.value()), static_cast<long long>(16 * kBlk));
}

MT_TEST(seek_data_reports_live_blocks_and_skips_discarded_ones) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0x01));
    Write(rw.file, 2 * kBlk, Pattern(kBlk, 0x02));
    MT_EXPECT_TRUE(rw.file->DiscardRange(2 * kBlk, kBlk).ok());

    std::vector<Segment> segs;
    core::Expected<size_t, std::string> n =
        rw.file->SeekDataSegments(0, 16 * kBlk, &segs);
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    // One span. The discarded block is not data: reporting it would make a
    // caller copying "the data" write out zeros it could have skipped.
    MT_EXPECT_EQ(static_cast<int>(segs.size()), 1);
    if (segs.size() == 1) MT_EXPECT_EQ(static_cast<long long>(segs[0].offset), 0LL);

    MT_EXPECT_TRUE(rw.file->SeekDataSegments(100, 100, &segs).ok());
    MT_EXPECT_TRUE(!rw.file->SeekDataSegments(0, 100, 0).ok());
}

MT_TEST(sync_flushes_buffered_mappings_before_the_files) {
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    rw.file->SetIndexGroupCommit(1024);
    Write(rw.file, 0, Pattern(kBlk, 0xF1));

    core::Expected<uint64_t, std::string> before = rw.index->Size();
    if (!before.ok()) return;
    MT_EXPECT_TRUE(rw.file->Sync().ok());
    core::Expected<uint64_t, std::string> after = rw.index->Size();
    MT_EXPECT_TRUE(after.ok());
    // A sync that left mappings in memory would report durability it has not
    // achieved: the data would be on disk with nothing pointing at it.
    if (after.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(after.value()),
                     static_cast<long long>(before.value() + 16));
    }
}


// ============================================================
// why the digest is gated on layout, not merely left unfed
// ============================================================

MT_TEST(a_sparse_layer_reports_no_digest_rather_than_a_header_only_one) {
    Rw rw = MakeRw(kRwSparse, 8 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(2 * kBlk, 0x11));

    core::Expected<core::Optional<LayerDescriptor>, std::string> sealed = rw.file->CloseSeal();
    MT_EXPECT_TRUE(sealed.ok());
    if (!sealed.ok()) return;

    // Absent. Seeding a tracker here and simply never feeding it would also
    // "work": the sparse write path does not absorb, so at seal the tracker
    // would still sit at the header and report a digest of the first 4096
    // bytes plus the index and trailer -- a well-formed digest of a file that
    // does not exist. The gate is what makes the difference observable.
    MT_EXPECT_TRUE(!sealed.value().has_value());
}

MT_TEST(a_hybrid_layer_reports_no_digest_either) {
    Rw rw = MakeRw(kRwHybridLogStructured, 8 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(2 * kBlk, 0x22));
    // In-place overwrite: the bytes the digest would have hashed are gone.
    Write(rw.file, 0, Pattern(kBlk, 0x99));

    core::Expected<core::Optional<LayerDescriptor>, std::string> sealed = rw.file->CloseSeal();
    MT_EXPECT_TRUE(sealed.ok());
    if (sealed.ok()) MT_EXPECT_TRUE(!sealed.value().has_value());
}

MT_TEST(a_reported_digest_always_covers_the_whole_file) {
    // The invariant that makes a present descriptor trustworthy, and the one
    // an unfed tracker would violate: size equals the sealed file length.
    Rw rw = MakeRw(kRwLogStructured, 16 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(4 * kBlk, 0x31));
    Write(rw.file, 8 * kBlk, Pattern(2 * kBlk, 0x41));

    core::Expected<core::Optional<LayerDescriptor>, std::string> sealed = rw.file->CloseSeal();
    MT_EXPECT_TRUE(sealed.ok());
    if (!sealed.ok() || !sealed.value().has_value()) {
        MT_EXPECT_TRUE(false);
        return;
    }
    core::Expected<uint64_t, std::string> fsz = rw.data->Size();
    MT_EXPECT_TRUE(fsz.ok());
    if (fsz.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(sealed.value()->size),
                     static_cast<long long>(fsz.value()));
        // And strictly more than the header, which is what a tracker that
        // was seeded but never fed would report.
        MT_EXPECT_TRUE(sealed.value()->size > kHdr);
    }
}

MT_TEST(the_digest_matches_a_hash_of_the_sealed_bytes) {
    Rw rw = MakeRw(kRwLogStructured, 8 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(2 * kBlk, 0x51));

    core::Expected<core::Optional<LayerDescriptor>, std::string> sealed = rw.file->CloseSeal();
    MT_EXPECT_TRUE(sealed.ok());
    if (!sealed.ok() || !sealed.value().has_value()) {
        MT_EXPECT_TRUE(false);
        return;
    }

    // Hash the file independently. This is the assertion that an unfed or
    // mis-fed tracker cannot pass: it pins the digest to the actual bytes
    // rather than to its own bookkeeping.
    const std::vector<uint8_t>& raw = rw.data->Raw();
    const std::string expect = agentenv::core::Sha256Digest(&raw[0], raw.size());
    MT_EXPECT_TRUE(sealed.value()->digest == expect);
}

MT_TEST(a_write_that_breaks_the_append_order_drops_the_digest) {
    Rw rw = MakeRw(kRwLogStructured, 8 * kBlk);
    if (!rw.file) return;
    Write(rw.file, 0, Pattern(kBlk, 0x61));

    // Reach past the layer and append through the file directly, so the
    // layer's cursor and the real EOF disagree. The next layer write then
    // lands somewhere the tracker is not expecting.
    const std::vector<uint8_t> stray(kBlk, 0xFF);
    core::Expected<uint64_t, std::string> end = rw.data->Size();
    MT_EXPECT_TRUE(end.ok());
    if (!end.ok()) return;
    MT_EXPECT_TRUE(rw.data->WriteAt(end.value(), &stray[0], stray.size()).ok());

    // The layer still appends at its own cursor, which now points *into*
    // the stray bytes -- contiguous from the tracker's view. So instead,
    // force the disagreement the other way: resize, which drops the tracker
    // and only rebuilds it when the cursor still matches the file end.
    MT_EXPECT_TRUE(rw.file->UpdateVsize(16 * kBlk).ok());

    core::Expected<core::Optional<LayerDescriptor>, std::string> sealed = rw.file->CloseSeal();
    MT_EXPECT_TRUE(sealed.ok());
    if (!sealed.ok()) return;
    // Either absent, or present and still exactly covering the file. What
    // must not happen is a present descriptor that is wrong.
    if (sealed.value().has_value()) {
        core::Expected<uint64_t, std::string> fsz = rw.data->Size();
        if (fsz.ok()) {
            MT_EXPECT_EQ(static_cast<long long>(sealed.value()->size),
                         static_cast<long long>(fsz.value()));
        }
        const std::vector<uint8_t>& raw = rw.data->Raw();
        MT_EXPECT_TRUE(sealed.value()->digest ==
                       agentenv::core::Sha256Digest(&raw[0], raw.size()));
    }
}

MT_TEST(the_sparse_append_cursor_names_the_physical_end) {
    Rw rw = MakeRw(kRwSparse, 8 * kBlk);
    if (!rw.file) return;

    // A sparse layer never appends, so the cursor exists only so the resize
    // and seal paths can name the physical end. Shrinking the address space
    // has to move it, or a later seal would place the index inside the
    // virtual data region.
    MT_EXPECT_TRUE(rw.file->UpdateVsize(4 * kBlk).ok());
    core::Expected<uint64_t, std::string> fsz = rw.data->Size();
    MT_EXPECT_TRUE(fsz.ok());
    if (fsz.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(fsz.value()),
                     static_cast<long long>(kHdr + 4 * kBlk));
    }

    Write(rw.file, 3 * kBlk, Pattern(kBlk, 0x71));
    MT_EXPECT_TRUE(rw.file->CloseSeal().ok());

    // Reopened read-only: the index landed after the last virtual block, not
    // on top of it.
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> ro =
        LsmtReadOnlyFile::Open(rw.data);
    MT_EXPECT_TRUE(ro.ok());
    if (!ro.ok()) return;
    std::vector<uint8_t> dst(static_cast<size_t>(kBlk), 0xEE);
    core::Expected<size_t, std::string> n =
        ro.value()->ReadAtInto(3 * kBlk, &dst[0], dst.size());
    MT_EXPECT_TRUE(n.ok());
    if (n.ok()) MT_EXPECT_EQ(static_cast<int>(dst[0]), 0x71);
    core::Expected<uint64_t, std::string> vsz = ro.value()->Size();
    if (vsz.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(vsz.value()), static_cast<long long>(4 * kBlk));
    }
}

MT_MAIN
