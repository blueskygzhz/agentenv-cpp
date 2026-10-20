// SPDX-License-Identifier: MIT
// Tests for the compact/flatten/export surface: the `lsmt::CompactSink`
// primitive and the four `LsmtFile`/`LsmtReadOnlyFile` entry points that
// `image_file` drives (flatten, export_upper_as_sealed, close_seal_and_reopen).
//
// Rust: storage/overlaybd/src/lsmt/file/{readwrite,readonly,helper}.rs
#include "microtest.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/lsmt_readonly.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"

using namespace agentenv::storage::overlaybd;
namespace core = agentenv::core;

namespace {

const uint64_t kBlk = 512;
const uint64_t kHdr = HeaderTrailer::kSpace;  // 4096

/// Minimal growable in-memory file, enough for the layers these tests stack.
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

    core::Expected<core::Unit, std::string> Discard(uint64_t offset, uint64_t len) override {
        for (uint64_t i = 0; i < len; ++i) {
            const size_t at = static_cast<size_t>(offset + i);
            if (at >= data_.size()) break;
            data_[at] = 0;
            written_[at] = false;
        }
        return core::Unit();
    }

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

// ---- CompactTo primitive harness ----------------------------------------

/// Deterministic bytes keyed by (tag, block), so a mapping copied from the
/// wrong layer or the wrong offset is visible in the output.
class FillSource : public lsmt::CompactSource {
 public:
    bool ReadAt(uint8_t tag, uint64_t src_byte_off, uint8_t* dst, uint64_t len) {
        for (uint64_t i = 0; i < len; ++i) {
            const uint64_t blk = (src_byte_off + i) / kBlk;
            dst[i] = static_cast<uint8_t>(tag * 31u + blk);
        }
        return true;
    }
};

class VecSink : public lsmt::CompactSink {
 public:
    bool WriteAt(uint64_t off, const uint8_t* data, uint64_t len) {
        if (bytes_.size() < off + len) bytes_.resize(static_cast<size_t>(off + len), 0);
        std::memcpy(&bytes_[static_cast<size_t>(off)], data, static_cast<size_t>(len));
        return true;
    }
    const std::vector<uint8_t>& Bytes() const { return bytes_; }

 private:
    std::vector<uint8_t> bytes_;
};

std::vector<uint8_t> SlurpFile(const std::string& path) {
    std::vector<uint8_t> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    uint8_t buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

// ---- sealed-layer / stack helpers ---------------------------------------

std::vector<uint8_t> Pattern(size_t len, uint8_t first) {
    std::vector<uint8_t> v(len, 0);
    for (size_t i = 0; i < len; ++i) v[i] = static_cast<uint8_t>(first + (i / kBlk));
    return v;
}

/// Build a sealed single-layer LSMT in memory: create a log-structured upper,
/// write the given blocks, seal it. The returned file is what a lower layer
/// looks like on disk.
std::shared_ptr<MemFile> SealedLayer(const std::string& uuid, uint64_t vsize,
                                     const std::vector<uint64_t>& blocks, uint8_t first) {
    std::shared_ptr<MemFile> data(new MemFile());
    std::shared_ptr<MemFile> index(new MemFile());
    core::Expected<std::shared_ptr<LsmtFile>, std::string> f = LsmtFile::CreateWithMetadata(
        data, index, vsize, kRwLogStructured, uuid, std::string(), 0, 0);
    if (!f.ok()) {
        std::fprintf(stderr, "  FAIL: create sealed layer: %s\n", f.error().c_str());
        std::exit(1);
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        const std::vector<uint8_t> d =
            Pattern(static_cast<size_t>(kBlk), static_cast<uint8_t>(first + blocks[i]));
        core::Expected<size_t, std::string> w = f.value()->WriteAt(blocks[i] * kBlk, &d[0], d.size());
        if (!w.ok()) {
            std::fprintf(stderr, "  FAIL: write sealed layer: %s\n", w.error().c_str());
            std::exit(1);
        }
    }
    if (!f.value()->CloseSeal().ok()) {
        std::fprintf(stderr, "  FAIL: seal layer\n");
        std::exit(1);
    }
    return data;
}

/// Read one block through any VirtualFile, failing loudly.
uint8_t FirstByteAt(const std::shared_ptr<VirtualFile>& f, uint64_t off) {
    std::vector<uint8_t> dst(static_cast<size_t>(kBlk), 0xEE);
    core::Expected<size_t, std::string> n = f->ReadAtInto(off, &dst[0], dst.size());
    if (!n.ok()) {
        std::fprintf(stderr, "  FAIL: read at %llu: %s\n",
                     static_cast<unsigned long long>(off), n.error().c_str());
        std::exit(1);
    }
    return dst[0];
}

std::string HeaderParentUuid(const std::shared_ptr<MemFile>& file) {
    core::Expected<uint64_t, std::string> sz = file->Size();
    if (!sz.ok()) return std::string("<no size>");
    core::Expected<HeaderTrailer, std::string> ht = VerifyHt(file.get(), false, sz.value());
    if (!ht.ok()) return std::string("<bad header: ") + ht.error() + ">";
    const uint8_t* p = ht.value().parent_uuid;
    return std::string(reinterpret_cast<const char*>(p),
                       ::strnlen(reinterpret_cast<const char*>(p), sizeof(ht.value().parent_uuid)));
}

}  // namespace

// ============================================================
// the CompactSink primitive
// ============================================================

MT_TEST(path_compact_delegates_to_the_sink_and_truncates) {
    // The path overload is now a wrapper around the sink one, so comparing
    // their bytes cannot detect a regression in the shared body -- both run
    // it. What it *does* pin is the wrapper: that it opens the destination
    // with the truncating semantics `fopen(path, "wb+")` had. A stale,
    // larger file left behind by a previous attempt must not survive as a
    // tail of garbage after the trailer.
    //
    // (The guard that the shared body still behaves as before is the
    // pre-existing suite: MergeFilesRo and the readonly/rw tests all read
    // back layers this function wrote.)
    std::vector<lsmt::SegmentMapping> mappings;
    mappings.push_back(lsmt::SegmentMapping(0, 2, 8, false, 0));    // real data, tag 0
    mappings.push_back(lsmt::SegmentMapping(2, 1, 0, true, 0));     // zeroed, no space
    mappings.push_back(lsmt::SegmentMapping(3, 3, 16, false, 1));   // real data, tag 1

    const uint64_t vsize = 6 * kBlk;
    lsmt::CompactCommitArgs args;
    args.uuid = "11111111-2222-3333-4444-555555555555";
    args.parent_uuid = "99999999-8888-7777-6666-555555555555";
    args.user_tag.push_back(0xAB);

    FillSource src_a;
    VecSink sink;
    std::string err;
    MT_EXPECT_TRUE(lsmt::CompactTo(&sink, &src_a, mappings, vsize, args, &err));
    MT_EXPECT_TRUE(!sink.Bytes().empty());

    char tmpl[] = "/tmp/agentenv_compact_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    MT_EXPECT_TRUE(fd >= 0);
    // Pre-fill well past the output's length: without O_TRUNC these bytes
    // would still be there afterwards.
    {
        const std::vector<uint8_t> junk(1u << 20, 0xFF);
        MT_EXPECT_TRUE(::write(fd, &junk[0], junk.size()) == static_cast<ssize_t>(junk.size()));
    }
    ::close(fd);
    MT_EXPECT_TRUE(SlurpFile(tmpl).size() == (1u << 20));

    FillSource src_b;
    MT_EXPECT_TRUE(lsmt::CompactTo(std::string(tmpl), &src_b, mappings, vsize, args, &err));
    const std::vector<uint8_t> on_disk = SlurpFile(tmpl);
    ::remove(tmpl);

    MT_EXPECT_EQ(static_cast<long long>(on_disk.size()),
                 static_cast<long long>(sink.Bytes().size()));
    MT_EXPECT_TRUE(on_disk == sink.Bytes());
}

MT_TEST(sink_compact_output_reopens_as_a_sealed_layer) {
    // Not just "same bytes as before" but "a valid layer": the sink path has
    // to land the header, index and trailer where a reader looks for them.
    std::vector<lsmt::SegmentMapping> mappings;
    mappings.push_back(lsmt::SegmentMapping(0, 1, 8, false, 0));
    mappings.push_back(lsmt::SegmentMapping(1, 1, 9, false, 0));

    std::shared_ptr<MemFile> dest(new MemFile());
    VirtualFile* dest_vf = dest.get();
    (void)dest_vf;

    // Write through a sink bound to the MemFile by hand, mirroring what
    // `VirtualFileCompactSink` does inside the library.
    class MemSink : public lsmt::CompactSink {
     public:
        explicit MemSink(std::shared_ptr<MemFile> f) : f_(f) {}
        bool WriteAt(uint64_t off, const uint8_t* data, uint64_t len) {
            core::Expected<size_t, std::string> n =
                f_->WriteAt(off, data, static_cast<size_t>(len));
            return n.ok() && n.value() == static_cast<size_t>(len);
        }
     private:
        std::shared_ptr<MemFile> f_;
    };

    FillSource src;
    MemSink sink(dest);
    std::string err;
    MT_EXPECT_TRUE(lsmt::CompactTo(&sink, &src, mappings, 2 * kBlk, lsmt::CompactCommitArgs(), &err));

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> ro =
        LsmtReadOnlyFile::Open(dest);
    MT_EXPECT_TRUE(ro.ok());
    if (!ro.ok()) return;
    MT_EXPECT_EQ(static_cast<long long>(ro.value()->VirtualSize()),
                 static_cast<long long>(2 * kBlk));
    // tag 0, blocks 8 and 9 of the source.
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(ro.value(), 0)), 8);
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(ro.value(), kBlk)), 9);
}

// ============================================================
// LsmtReadOnlyFile::Flatten
// ============================================================

MT_TEST(readonly_flatten_resolves_shadowing_into_one_layer) {
    const uint64_t vsize = 8 * kBlk;
    // Lower owns blocks 0..3 (pattern base 0x10), upper lower owns 2..3
    // (base 0x40) and so shadows two of them.
    std::shared_ptr<MemFile> l0 = SealedLayer("layer-0-uuid", vsize, std::vector<uint64_t>(), 0);
    {
        std::vector<uint64_t> b;
        b.push_back(0); b.push_back(1); b.push_back(2); b.push_back(3);
        l0 = SealedLayer("layer-0-uuid", vsize, b, 0x10);
    }
    std::vector<uint64_t> b1;
    b1.push_back(2); b1.push_back(3);
    std::shared_ptr<MemFile> l1 = SealedLayer("layer-1-uuid", vsize, b1, 0x40);

    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(l0);   // bottom
    files.push_back(l1);   // top
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> stack = OpenFilesRo(files);
    MT_EXPECT_TRUE(stack.ok());
    if (!stack.ok()) return;

    // Through the stack: 0,1 from l0; 2,3 from l1.
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(stack.value(), 0 * kBlk)), 0x10);
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(stack.value(), 1 * kBlk)), 0x11);
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(stack.value(), 2 * kBlk)), 0x42);
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(stack.value(), 3 * kBlk)), 0x43);

    std::shared_ptr<MemFile> dest(new MemFile());
    core::Expected<core::Unit, std::string> flat = stack.value()->Flatten(dest);
    MT_EXPECT_TRUE(flat.ok());
    if (!flat.ok()) {
        std::fprintf(stderr, "  flatten: %s\n", flat.error().c_str());
        return;
    }

    // The flattened layer answers identically on its own, with no lowers.
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> flattened =
        LsmtReadOnlyFile::Open(dest);
    MT_EXPECT_TRUE(flattened.ok());
    if (!flattened.ok()) return;
    MT_EXPECT_EQ(static_cast<long long>(flattened.value()->VirtualSize()),
                 static_cast<long long>(vsize));
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(flattened.value(), 0 * kBlk)), 0x10);
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(flattened.value(), 1 * kBlk)), 0x11);
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(flattened.value(), 2 * kBlk)), 0x42);
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(flattened.value(), 3 * kBlk)), 0x43);
    // Never-written blocks stay zeros rather than becoming whatever was in a
    // copy buffer.
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(flattened.value(), 6 * kBlk)), 0);

    MT_EXPECT_TRUE(!dest->Raw().empty());
}

MT_TEST(readonly_flatten_refuses_a_null_destination) {
    std::vector<uint64_t> b;
    b.push_back(0);
    std::shared_ptr<MemFile> l0 = SealedLayer("solo", 2 * kBlk, b, 0x20);
    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(l0);
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> stack = OpenFilesRo(files);
    MT_EXPECT_TRUE(stack.ok());
    if (!stack.ok()) return;
    MT_EXPECT_TRUE(!stack.value()->Flatten(std::shared_ptr<VirtualFile>()).ok());
}

// ============================================================
// LsmtFile::Flatten / ExportUpperAsSealed
// ============================================================

namespace {

struct Stacked {
    std::shared_ptr<MemFile> l0;
    std::shared_ptr<MemFile> l1;
    std::shared_ptr<MemFile> upper_data;
    std::shared_ptr<MemFile> upper_index;
    std::shared_ptr<LsmtFile> file;
};

/// Two sealed lowers with a writable upper stacked on top. Blocks 0..3 come
/// from the lowers, the upper owns whatever the test writes.
Stacked MakeStack(uint64_t vsize) {
    Stacked s;
    std::vector<uint64_t> b0;
    b0.push_back(0); b0.push_back(1); b0.push_back(2); b0.push_back(3);
    s.l0 = SealedLayer("layer-0-uuid", vsize, b0, 0x10);
    std::vector<uint64_t> b1;
    b1.push_back(2); b1.push_back(3);
    s.l1 = SealedLayer("layer-1-uuid", vsize, b1, 0x40);

    std::vector<std::shared_ptr<VirtualFile> > files;
    files.push_back(s.l0);
    files.push_back(s.l1);
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> lowers = OpenFilesRo(files);
    if (!lowers.ok()) {
        std::fprintf(stderr, "  FAIL: open lowers: %s\n", lowers.error().c_str());
        std::exit(1);
    }

    s.upper_data.reset(new MemFile());
    s.upper_index.reset(new MemFile());
    core::Expected<std::shared_ptr<LsmtFile>, std::string> up = LsmtFile::CreateWithMetadata(
        s.upper_data, s.upper_index, vsize, kRwLogStructured, "upper-uuid", std::string(), 0, 0);
    if (!up.ok()) {
        std::fprintf(stderr, "  FAIL: create upper: %s\n", up.error().c_str());
        std::exit(1);
    }
    core::Expected<std::shared_ptr<LsmtFile>, std::string> st =
        StackFiles(up.value(), lowers.value(), false);
    if (!st.ok()) {
        std::fprintf(stderr, "  FAIL: stack: %s\n", st.error().c_str());
        std::exit(1);
    }
    s.file = st.value();
    return s;
}

}  // namespace

MT_TEST(rw_flatten_includes_lowers_and_upper) {
    const uint64_t vsize = 8 * kBlk;
    Stacked s = MakeStack(vsize);

    const std::vector<uint8_t> up = Pattern(static_cast<size_t>(kBlk), 0x90);
    MT_EXPECT_TRUE(s.file->WriteAt(5 * kBlk, &up[0], up.size()).ok());

    std::shared_ptr<MemFile> dest(new MemFile());
    core::Expected<core::Unit, std::string> flat = s.file->Flatten(dest);
    MT_EXPECT_TRUE(flat.ok());
    if (!flat.ok()) {
        std::fprintf(stderr, "  flatten: %s\n", flat.error().c_str());
        return;
    }

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> ro =
        LsmtReadOnlyFile::Open(dest);
    MT_EXPECT_TRUE(ro.ok());
    if (!ro.ok()) return;
    // Flatten is the whole image: lowers *and* upper.
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(ro.value(), 0 * kBlk)), 0x10);
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(ro.value(), 2 * kBlk)), 0x42);
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(ro.value(), 5 * kBlk)), 0x90);
}

MT_TEST(export_upper_as_sealed_carries_only_the_upper) {
    const uint64_t vsize = 8 * kBlk;
    Stacked s = MakeStack(vsize);

    const std::vector<uint8_t> up = Pattern(static_cast<size_t>(kBlk), 0x90);
    MT_EXPECT_TRUE(s.file->WriteAt(5 * kBlk, &up[0], up.size()).ok());

    std::shared_ptr<MemFile> dest(new MemFile());
    core::Expected<core::Unit, std::string> ex = s.file->ExportUpperAsSealed(CommitArgs(dest));
    MT_EXPECT_TRUE(ex.ok());
    if (!ex.ok()) {
        std::fprintf(stderr, "  export: %s\n", ex.error().c_str());
        return;
    }

    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> ro =
        LsmtReadOnlyFile::Open(dest);
    MT_EXPECT_TRUE(ro.ok());
    if (!ro.ok()) return;
    // The upper's own block is there...
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(ro.value(), 5 * kBlk)), 0x90);
    // ...and the lowers' blocks are *not*. This is the difference from
    // Flatten, and the reason an exported layer can be stacked back onto the
    // same lowers without duplicating them.
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(ro.value(), 0 * kBlk)), 0);
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(ro.value(), 2 * kBlk)), 0);
    // The virtual size still describes the whole image, so the exported layer
    // addresses the same space it was taken from.
    MT_EXPECT_EQ(static_cast<long long>(ro.value()->VirtualSize()),
                 static_cast<long long>(vsize));
}

MT_TEST(export_upper_as_sealed_leaves_the_source_writable) {
    // Unlike CloseSeal, export is read-only with respect to the live image.
    // A caller snapshotting a running device depends on exactly this.
    const uint64_t vsize = 8 * kBlk;
    Stacked s = MakeStack(vsize);

    const std::vector<uint8_t> a = Pattern(static_cast<size_t>(kBlk), 0x90);
    MT_EXPECT_TRUE(s.file->WriteAt(5 * kBlk, &a[0], a.size()).ok());

    std::shared_ptr<MemFile> dest(new MemFile());
    MT_EXPECT_TRUE(s.file->ExportUpperAsSealed(CommitArgs(dest)).ok());

    MT_EXPECT_TRUE(!s.file->IsSealed());
    const std::vector<uint8_t> b = Pattern(static_cast<size_t>(kBlk), 0xC0);
    MT_EXPECT_TRUE(s.file->WriteAt(6 * kBlk, &b[0], b.size()).ok());
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(s.file, 6 * kBlk)), 0xC0);
    // And the write that landed after the export is not in the exported copy.
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> ro =
        LsmtReadOnlyFile::Open(dest);
    MT_EXPECT_TRUE(ro.ok());
    if (!ro.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(ro.value(), 6 * kBlk)), 0);
}

MT_TEST(export_upper_as_sealed_stamps_the_topmost_lower_as_parent) {
    const uint64_t vsize = 8 * kBlk;
    Stacked s = MakeStack(vsize);
    const std::vector<uint8_t> up = Pattern(static_cast<size_t>(kBlk), 0x90);
    MT_EXPECT_TRUE(s.file->WriteAt(5 * kBlk, &up[0], up.size()).ok());

    // `[newest_lower, ..., oldest_lower, upper]`, so index 0 is the parent.
    MT_EXPECT_EQ(static_cast<int>(s.file->Uuids().size()), 3);
    MT_EXPECT_TRUE(s.file->Uuids()[0] == std::string("layer-1-uuid"));

    std::shared_ptr<MemFile> dest(new MemFile());
    // A caller-supplied parent is deliberately overridden: the parent is a
    // property of the stack, not a choice.
    CommitArgs args(dest);
    args.parent_uuid = "caller-should-not-win";
    MT_EXPECT_TRUE(s.file->ExportUpperAsSealed(args).ok());
    MT_EXPECT_TRUE(HeaderParentUuid(dest) == std::string("layer-1-uuid"));
}

MT_TEST(export_upper_as_sealed_leaves_a_lone_layer_parentless) {
    // No lowers: there is nothing the exported layer descends from, and
    // inventing a parent would make a later layer-order check fail.
    std::shared_ptr<MemFile> data(new MemFile());
    std::shared_ptr<MemFile> index(new MemFile());
    core::Expected<std::shared_ptr<LsmtFile>, std::string> f = LsmtFile::CreateWithMetadata(
        data, index, 4 * kBlk, kRwLogStructured, "solo-uuid", std::string(), 0, 0);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    const std::vector<uint8_t> d = Pattern(static_cast<size_t>(kBlk), 0x55);
    MT_EXPECT_TRUE(f.value()->WriteAt(0, &d[0], d.size()).ok());
    MT_EXPECT_EQ(static_cast<int>(f.value()->Uuids().size()), 1);

    std::shared_ptr<MemFile> dest(new MemFile());
    MT_EXPECT_TRUE(f.value()->ExportUpperAsSealed(CommitArgs(dest)).ok());
    MT_EXPECT_TRUE(HeaderParentUuid(dest).empty());
}

MT_TEST(export_and_flatten_refuse_a_null_writer) {
    Stacked s = MakeStack(8 * kBlk);
    MT_EXPECT_TRUE(!s.file->ExportUpperAsSealed(CommitArgs()).ok());
    MT_EXPECT_TRUE(!s.file->FlattenWithArgs(CommitArgs()).ok());
}

// ============================================================
// LsmtFile::CloseSealAndReopen
// ============================================================

MT_TEST(close_seal_and_reopen_returns_a_readable_sealed_view) {
    const uint64_t vsize = 8 * kBlk;
    Stacked s = MakeStack(vsize);
    const std::vector<uint8_t> up = Pattern(static_cast<size_t>(kBlk), 0x90);
    MT_EXPECT_TRUE(s.file->WriteAt(5 * kBlk, &up[0], up.size()).ok());
    MT_EXPECT_TRUE(s.file->SetMaxIoSize(64 * 1024).ok());

    core::Expected<SealedReopen, std::string> out = s.file->CloseSealAndReopen();
    MT_EXPECT_TRUE(out.ok());
    if (!out.ok()) {
        std::fprintf(stderr, "  close_seal_and_reopen: %s\n", out.error().c_str());
        return;
    }
    MT_EXPECT_TRUE(static_cast<bool>(out.value().file));

    // The reopened view is the upper alone -- it is that one data file, now
    // sealed -- so it answers for the upper's block and not the lowers'.
    MT_EXPECT_EQ(static_cast<int>(FirstByteAt(out.value().file, 5 * kBlk)), 0x90);
    MT_EXPECT_EQ(static_cast<long long>(out.value().file->VirtualSize()),
                 static_cast<long long>(vsize));
    // Tuning carries over, which is what lets the caller restack the result
    // without re-applying it.
    MT_EXPECT_EQ(static_cast<long long>(out.value().file->GetMaxIoSize()),
                 static_cast<long long>(64 * 1024));
    // The source is sealed now: this is the terminal variant.
    MT_EXPECT_TRUE(s.file->IsSealed());
}

MT_TEST(close_seal_and_reopen_reports_the_append_only_digest) {
    // A log-structured upper with an unbroken append history can name itself,
    // which is what the snapshot path records as the new lower's descriptor.
    std::shared_ptr<MemFile> data(new MemFile());
    std::shared_ptr<MemFile> index(new MemFile());
    core::Expected<std::shared_ptr<LsmtFile>, std::string> f = LsmtFile::CreateWithMetadata(
        data, index, 4 * kBlk, kRwLogStructured, "solo-uuid", std::string(), 0, 0);
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    const std::vector<uint8_t> d = Pattern(static_cast<size_t>(kBlk), 0x55);
    MT_EXPECT_TRUE(f.value()->WriteAt(0, &d[0], d.size()).ok());

    core::Expected<SealedReopen, std::string> out = f.value()->CloseSealAndReopen();
    MT_EXPECT_TRUE(out.ok());
    if (!out.ok()) return;
    MT_EXPECT_TRUE(out.value().descriptor.has_value());
    if (out.value().descriptor.has_value()) {
        const LayerDescriptor& desc = *out.value().descriptor;
        MT_EXPECT_TRUE(desc.digest.compare(0, 7, "sha256:") == 0);
        // The descriptor sizes the sealed file, so it has to cover the index
        // and trailer the seal just appended.
        core::Expected<uint64_t, std::string> sz = data->Size();
        MT_EXPECT_TRUE(sz.ok());
        if (sz.ok()) {
            MT_EXPECT_EQ(static_cast<long long>(desc.size),
                         static_cast<long long>(sz.value()));
        }
    }
}

// ============================================================
// ImageConfig::EffectiveDownload
// ============================================================

MT_TEST(effective_download_is_selected_by_presence_not_by_value) {
    GlobalConfig g;
    g.download.block_size = 111111;
    ImageConfig c;

    // Absent: the node's policy.
    MT_EXPECT_EQ(static_cast<long long>(c.EffectiveDownload(g).block_size),
                 static_cast<long long>(111111));
    MT_EXPECT_TRUE(&c.EffectiveDownload(g) == &g.download);

    // Present: the image's, even when it happens to equal the default --
    // presence is what selects, which is why the flag is tracked apart from
    // the value.
    c.has_download_override = true;
    c.download_override = DownloadConfig();
    MT_EXPECT_TRUE(&c.EffectiveDownload(g) == &c.download_override);
    c.download_override.block_size = 222222;
    MT_EXPECT_EQ(static_cast<long long>(c.EffectiveDownload(g).block_size),
                 static_cast<long long>(222222));
}

MT_MAIN
