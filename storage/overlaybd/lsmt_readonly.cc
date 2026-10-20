// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/lsmt/file/readonly.rs (+ helper.rs, vfile_io.rs)
#include "agentenv/storage/overlaybd/lsmt_readonly.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/storage/overlaybd/cache_pool.h"
#include "internal.h"

#include <cstring>
#include <sstream>

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

/// Rust's `ALIGNMENT`, as a byte count.
const uint64_t kAlign = kLsmtAlignment;

}  // namespace

core::Expected<core::Unit, std::string> CheckAlignment(uint64_t offset, size_t len) {
    if (offset % kAlign != 0 || len % static_cast<size_t>(kAlign) != 0) {
        std::ostringstream o;
        o << "IO must be aligned to " << kAlign << " bytes (offset: " << offset
          << ", len: " << len << ")";
        return core::make_unexpected(o.str());
    }
    return core::Unit();
}

/// Rust `vfile_io.rs :: DirectRead` / `CtxRead` and `read_exact` live in
/// `internal.h`: `LsmtFile` needs the same pair.
core::Expected<core::Unit, std::string> ReadExact(VirtualFile* file, uint64_t offset,
                                                    uint8_t* dst, size_t len) {
    return ReadExactWith(DirectLayerRead(), file, offset, dst, len);
}

core::Expected<HeaderTrailer, std::string> VerifyHt(VirtualFile* file, bool is_trailer,
                                                      uint64_t file_size) {
    uint64_t at = 0;
    if (is_trailer) {
        if (file_size < kLsmtHeaderSize) {
            return core::make_unexpected(std::string("File too small for trailer"));
        }
        at = file_size - kLsmtHeaderSize;
    }

    // The header and trailer each occupy a reserved 4096-byte region, of
    // which only the first 390 bytes are the struct. Reading the whole region
    // rather than just the struct matches Rust and keeps the read aligned.
    std::vector<uint8_t> bytes(static_cast<size_t>(kLsmtHeaderSize), 0);
    const core::Expected<size_t, std::string> read =
        file->ReadAtInto(at, &bytes[0], bytes.size());
    if (!read.ok()) return core::make_unexpected(read.error());
    if (read.value() < HeaderTrailer::kOnDiskSize) {
        return core::make_unexpected(std::string(
            is_trailer ? "failed to read file trailer." : "failed to read file header."));
    }

    HeaderTrailer ht;
    if (!HeaderTrailer::Deserialize(&bytes[0], read.value(), &ht)) {
        return core::make_unexpected(
            std::string(is_trailer ? "Invalid trailer" : "Invalid header"));
    }

    if (!is_trailer) {
        if (!ht.VerifyMagic() || !ht.IsHeader()) {
            return core::make_unexpected(std::string("header magic/type don't match"));
        }
        return ht;
    }

    // Stricter than the header check, and deliberately. The trailer carries
    // the index offset, and that offset is only meaningful once the file is
    // sealed: in a file still being written it may point at a region that is
    // not an index yet, which would parse as plausible garbage rather than
    // fail.
    if (!ht.VerifyMagic() || !ht.IsTrailer() || !ht.IsDataFile() || !ht.IsSealed()) {
        return core::make_unexpected(
            std::string("trailer magic, trailer type, file type or sealedness doesn't match"));
    }
    return ht;
}

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

LsmtReadOnlyFile::LsmtReadOnlyFile()
    : virtual_size_(0), max_io_size_(kLsmtMaxIoSize), file_type_(kLsmtFileReadOnly) {}

core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> LsmtReadOnlyFile::Open(
    std::shared_ptr<VirtualFile> file) {
    if (!file) return core::make_unexpected(std::string("lsmt open: null file"));

    const core::Expected<uint64_t, std::string> fsz = file->Size();
    if (!fsz.ok()) return core::make_unexpected(fsz.error());

    // Both ends are checked, though only the trailer is used afterwards. A
    // file whose header is wrong but whose trailer happens to parse is not an
    // LSMT file that lost its header -- it is something else with 390
    // plausible bytes at the end, and trusting it would build mappings
    // pointing into nothing.
    const core::Expected<HeaderTrailer, std::string> header =
        VerifyHt(file.get(), false, fsz.value());
    if (!header.ok()) return core::make_unexpected(header.error());

    const core::Expected<HeaderTrailer, std::string> trailer =
        VerifyHt(file.get(), true, fsz.value());
    if (!trailer.ok()) return core::make_unexpected(trailer.error());

    const uint64_t idx_off = trailer.value().index_offset;
    const uint64_t idx_count = trailer.value().index_size;

    std::vector<SegmentMapping> mappings;
    if (idx_count > 0) {
        // Bounded before allocating. The count comes out of the file, so a
        // corrupt trailer would otherwise be a request to allocate an
        // arbitrary amount of memory -- and the multiply itself can wrap.
        const uint64_t entry = static_cast<uint64_t>(sizeof(DiskSegmentMapping));
        if (idx_count > (~static_cast<uint64_t>(0)) / entry) {
            return core::make_unexpected(std::string("index region size overflows"));
        }
        const uint64_t region_bytes = idx_count * entry;
        if (idx_off > fsz.value() || region_bytes > fsz.value() - idx_off) {
            std::ostringstream o;
            o << "index region out of range (offset " << idx_off << ", " << idx_count
              << " entries, file " << fsz.value() << " bytes)";
            return core::make_unexpected(o.str());
        }

        std::vector<uint8_t> region(static_cast<size_t>(region_bytes), 0);
        const core::Expected<core::Unit, std::string> r =
            ReadExact(file.get(), idx_off, &region[0], region.size());
        if (!r.ok()) return core::make_unexpected(r.error());
        // Tags are reset to zero by the loader, which is right for a
        // single-file open: there is exactly one layer, so every mapping has
        // to resolve to it whatever tag the file recorded.
        mappings = LoadIndexAndResetTags(&region[0], region.size(),
                                         static_cast<size_t>(idx_count));
    }

    std::shared_ptr<LsmtReadOnlyFile> f(new LsmtReadOnlyFile());
    f->index_ = std::shared_ptr<LogIndex>(new ReadOnlyIndex(mappings));
    f->layers_.push_back(file);
    f->virtual_size_ = trailer.value().virtual_size;

    const char* raw = reinterpret_cast<const char*>(trailer.value().uuid);
    f->uuids_.push_back(std::string(raw, ::strnlen(raw, sizeof(trailer.value().uuid))));
    return f;
}

core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> LsmtReadOnlyFile::FromLayers(
    std::shared_ptr<LogIndex> index,
    const std::vector<std::shared_ptr<VirtualFile> >& layers, uint64_t virtual_size) {
    if (!index) return core::make_unexpected(std::string("lsmt stack: null index"));
    if (layers.empty()) return core::make_unexpected(std::string("lsmt stack: no layers"));
    for (size_t i = 0; i < layers.size(); ++i) {
        if (!layers[i]) {
            std::ostringstream o;
            o << "lsmt stack: null layer " << i;
            return core::make_unexpected(o.str());
        }
    }

    std::shared_ptr<LsmtReadOnlyFile> f(new LsmtReadOnlyFile());
    f->index_ = index;
    // Recovered rather than stored separately. Rust holds `index` and
    // `index_view` as two fields but assigns one `Arc<ReadOnlyIndex>` to both,
    // so the view is just the concrete type of the same object. A cast keeps
    // that invariant by construction -- two independent fields could drift.
    f->index_view_ = std::dynamic_pointer_cast<ReadOnlyIndex>(index);
    f->layers_ = layers;
    f->virtual_size_ = virtual_size;
    f->uuids_.resize(layers.size());
    return f;
}

core::Expected<core::Unit, std::string> LsmtReadOnlyFile::SetMaxIoSize(size_t size) {
    // Zero would make the read loop consume nothing per iteration and never
    // terminate; an unaligned step would leave the next iteration's offset
    // off a block boundary, so the index lookup would start mid-block.
    if (size == 0 || size % static_cast<size_t>(kAlign) != 0) {
        std::ostringstream o;
        o << "max io size must be a non-zero multiple of " << kAlign << " (got " << size << ")";
        return core::make_unexpected(o.str());
    }
    max_io_size_ = size;
    return core::Unit();
}

core::Expected<std::string, std::string> LsmtReadOnlyFile::GetUuid(size_t layer_idx) const {
    if (layer_idx >= uuids_.size()) {
        std::ostringstream o;
        o << "layer index " << layer_idx << " out of range (" << uuids_.size() << " layers)";
        return core::make_unexpected(o.str());
    }
    return uuids_[layer_idx];
}

// ---------------------------------------------------------------------------
// the resolver
// ---------------------------------------------------------------------------

template <typename Reader>
core::Expected<size_t, std::string> LsmtReadOnlyFile::ReadInternalIntoGeneric(
    const Reader& reader, uint64_t offset, uint8_t* dst, size_t len) {
    const core::Expected<core::Unit, std::string> al = CheckAlignment(offset, len);
    if (!al.ok()) return core::make_unexpected(al.error());

    if (offset >= virtual_size_) return static_cast<size_t>(0);

    // Clamped to the virtual size, so a read running off the end returns
    // what exists rather than resolving blocks past the declared address
    // space -- where the index has nothing and the result would be zeros
    // indistinguishable from a real hole.
    const uint64_t room = virtual_size_ - offset;
    const size_t read_len = room < static_cast<uint64_t>(len) ? static_cast<size_t>(room) : len;
    if (read_len == 0) return static_cast<size_t>(0);

    size_t remaining = read_len;
    uint64_t curr_offset = offset;
    size_t buf_pos = 0;

    while (remaining > 0) {
        const size_t step = remaining < max_io_size_ ? remaining : max_io_size_;

        const uint64_t begin_blk = curr_offset / kAlign;
        const uint64_t count_blk = (static_cast<uint64_t>(step) + kAlign - 1) / kAlign;
        const uint64_t end_blk = begin_blk + count_blk;

        std::vector<SegmentMapping> mappings;
        index_->Lookup(Segment(begin_blk, static_cast<uint32_t>(count_blk)), &mappings);

        uint64_t current_blk = begin_blk;

        for (size_t i = 0; i < mappings.size(); ++i) {
            const SegmentMapping& m = mappings[i];

            // A gap before this mapping is a hole: the virtual range is
            // addressable but nothing was ever written there. Zeros are the
            // defined contents, not a fallback -- this is what lets an image
            // declare a virtual size far larger than its stored bytes.
            if (m.Offset() > current_blk) {
                const uint64_t hole_blks = m.Offset() - current_blk;
                const size_t hole_bytes = static_cast<size_t>(hole_blks * kAlign);
                const size_t local = buf_pos + static_cast<size_t>((current_blk - begin_blk) * kAlign);
                const size_t room_left = step - (local - buf_pos);
                const size_t fill = hole_bytes < room_left ? hole_bytes : room_left;
                if (fill > 0) std::memset(dst + local, 0, fill);
                current_blk = m.Offset();
            }

            if (current_blk >= end_blk) break;

            const size_t local = buf_pos + static_cast<size_t>((current_blk - begin_blk) * kAlign);
            if (local - buf_pos >= step) break;

            const size_t chunk = static_cast<size_t>(static_cast<uint64_t>(m.Length()) * kAlign);
            const size_t room_left = step - (local - buf_pos);
            const size_t copy_len = chunk < room_left ? chunk : room_left;

            if (m.zeroed) {
                // Distinct from the gap above: this range *was* written, and
                // what was written was zeros. The index records it so that a
                // later layer cannot be seen through it -- dropping the
                // entry would expose whatever an underlying layer holds at
                // the same address.
                std::memset(dst + local, 0, copy_len);
            } else {
                const size_t layer_idx = static_cast<size_t>(m.tag);
                if (layer_idx >= layers_.size()) {
                    // Reported at read time rather than at open: the index
                    // may legitimately cover more layers than any one read
                    // touches, so this is only wrong once it is reached.
                    std::ostringstream o;
                    o << "Layer index " << layer_idx << " out of range";
                    return core::make_unexpected(o.str());
                }
                const uint64_t phys = m.moffset * kAlign;
                const core::Expected<core::Unit, std::string> r =
                    ReadExactWith(reader, layers_[layer_idx].get(), phys, dst + local, copy_len);
                if (!r.ok()) return core::make_unexpected(r.error());
            }
            current_blk = m.End();
        }

        // Anything the mappings did not cover, through the end of this step.
        if (current_blk < end_blk) {
            const size_t local = buf_pos + static_cast<size_t>((current_blk - begin_blk) * kAlign);
            if (local - buf_pos < step) {
                std::memset(dst + local, 0, buf_pos + step - local);
            }
        }

        remaining -= step;
        curr_offset += static_cast<uint64_t>(step);
        buf_pos += step;
    }

    return read_len;
}

core::Expected<size_t, std::string> LsmtReadOnlyFile::ReadInternalInto(uint64_t offset,
                                                                         uint8_t* dst,
                                                                         size_t len) {
    return ReadInternalIntoGeneric(DirectLayerRead(), offset, dst, len);
}

// ---------------------------------------------------------------------------
// VirtualFile
// ---------------------------------------------------------------------------

core::Expected<ByteBuffer, std::string> LsmtReadOnlyFile::ReadAt(uint64_t offset,
                                                                   size_t len) {
    if (len == 0 || offset >= virtual_size_) return ByteBuffer();
    const uint64_t room = virtual_size_ - offset;
    const size_t read_len = room < static_cast<uint64_t>(len) ? static_cast<size_t>(room) : len;

    // Zero-initialised, so a hole costs nothing to represent. Holes are
    // written again inside the resolver, which is redundant -- Rust notes the
    // same and keeps it, because this path is not the hot one: the ublk
    // target reads through `ReadAtInto` and never allocates here.
    ByteBuffer buf(read_len, 0);
    const core::Expected<size_t, std::string> n = ReadInternalInto(offset, &buf[0], read_len);
    if (!n.ok()) return core::make_unexpected(n.error());
    buf.resize(n.value());
    return buf;
}

core::Expected<size_t, std::string> LsmtReadOnlyFile::ReadAtInto(uint64_t offset,
                                                                   uint8_t* dst, size_t len) {
    if (len == 0) return static_cast<size_t>(0);
    if (dst == 0) {
        return core::make_unexpected(std::string("lsmt read_at_into: null destination"));
    }
    return ReadInternalInto(offset, dst, len);
}

core::Expected<ByteBuffer, std::string> LsmtReadOnlyFile::ReadAtWithCtx(const IoCtx& ctx,
                                                                          uint64_t offset,
                                                                          size_t len) {
    // Mirrors `ReadAt` bound for bound: same clamp, same zero-initialised
    // buffer for holes, same truncate to the bytes actually produced.
    if (len == 0 || offset >= virtual_size_) return ByteBuffer();
    const uint64_t room = virtual_size_ - offset;
    const size_t read_len = room < static_cast<uint64_t>(len) ? static_cast<size_t>(room) : len;

    ByteBuffer buf(read_len, 0);
    const core::Expected<size_t, std::string> n =
        ReadInternalIntoGeneric(CtxLayerRead(ctx), offset, &buf[0], read_len);
    if (!n.ok()) return core::make_unexpected(n.error());
    buf.resize(n.value());
    return buf;
}

core::Expected<size_t, std::string> LsmtReadOnlyFile::ReadAtIntoWithCtx(const IoCtx& ctx,
                                                                          uint64_t offset,
                                                                          uint8_t* dst,
                                                                          size_t len) {
    if (len == 0) return static_cast<size_t>(0);
    if (dst == 0) {
        return core::make_unexpected(std::string("lsmt read_at_into: null destination"));
    }
    return ReadInternalIntoGeneric(CtxLayerRead(ctx), offset, dst, len);
}

core::Expected<size_t, std::string> LsmtReadOnlyFile::WriteAt(uint64_t /*offset*/,
                                                                const uint8_t* /*data*/,
                                                                size_t /*len*/) {
    // Refused rather than inherited from the base class, which would also
    // refuse but with a message about the operation being unsupported. The
    // distinction matters: this file type *has* a write path in Rust
    // (`LSMTFile`), so the reason is that this handle is read-only, not that
    // LSMT cannot be written.
    return core::make_unexpected(std::string("File is read-only"));
}

core::Expected<uint64_t, std::string> LsmtReadOnlyFile::Size() {
    // The virtual size, not the file size. Everything above addresses the
    // decompressed, un-sparsed address space; the bytes on disk are an
    // implementation detail of this layer.
    return virtual_size_;
}

// ---------------------------------------------------------------------------
// introspection
// ---------------------------------------------------------------------------

DataStat LsmtReadOnlyFile::Stat() const {
    const uint64_t blocks = (virtual_size_ + kAlign - 1) / kAlign;
    std::vector<SegmentMapping> mappings;
    index_->Lookup(Segment(0, static_cast<uint32_t>(blocks)), &mappings);

    uint64_t valid_blocks = 0;
    for (size_t i = 0; i < mappings.size(); ++i) {
        // Zeroed mappings occupy index space but no data space, which is the
        // entire reason for recording them apart from real extents: they let
        // a layer say "this range is zeros" without storing the zeros.
        if (!mappings[i].zeroed) valid_blocks += mappings[i].Length();
    }

    DataStat s;
    s.total_data_size = valid_blocks * kAlign;
    s.valid_data_size = s.total_data_size;
    return s;
}

core::Expected<core::Unit, std::string> LsmtReadOnlyFile::Flatten(
    std::shared_ptr<VirtualFile> dest) {
    if (!dest) {
        return core::make_unexpected(std::string("flatten requires a destination writer"));
    }
    // The whole virtual size, not the mapped ranges only: flattening has to
    // resolve shadowing, and the merged index's lookup is what decides which
    // layer owns a block.
    const uint64_t blocks = (virtual_size_ + kAlign - 1) / kAlign;
    if (blocks > static_cast<uint64_t>(~static_cast<uint32_t>(0))) {
        return core::make_unexpected(std::string("flatten: virtual size too large to query"));
    }
    std::vector<SegmentMapping> mappings;
    index_->Lookup(Segment(0, static_cast<uint32_t>(blocks)), &mappings);

    // Rust `CommitArgs::from_writer`: no identity is supplied, so the output
    // carries the nil uuid and no parent.
    LayerStackCompactSource source(layers_);
    VirtualFileCompactSink sink(dest);
    std::string err;
    if (!lsmt::CompactTo(&sink, &source, mappings, virtual_size_,
                         lsmt::CompactCommitArgs(), &err)) {
        return core::make_unexpected(std::string("flatten: ") + err);
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> LsmtReadOnlyFile::CommitPreservingMetadata(
    const CommitArgs& args) {
    if (layers_.size() != 1) {
        return core::make_unexpected(
            std::string("commit_preserving_metadata requires a sealed single-file overlaybd"
                        " layer; stacked read-only layers are not supported"));
    }
    if (!args.writer) {
        return core::make_unexpected(std::string("commit requires a destination writer"));
    }
    if (!layers_[0]) {
        return core::make_unexpected(std::string("commit source layer is not open"));
    }

    const core::Expected<uint64_t, std::string> file_size = layers_[0]->Size();
    if (!file_size.ok()) return core::make_unexpected(file_size.error());

    const core::Expected<HeaderTrailer, std::string> trailer =
        VerifyHt(layers_[0].get(), true, file_size.value());
    if (!trailer.ok()) return core::make_unexpected(trailer.error());
    if (!trailer.value().IsSealed()) {
        return core::make_unexpected(std::string("Commit source must be sealed."));
    }

    // The identity is taken from the source, overriding whatever the caller
    // put in `args`: repacking a layer must not re-identify it, or the layer
    // below stops recognising it as its child.
    CommitArgs stamped = args;
    const core::Optional<std::string> uuid =
        ParseUuidField(trailer.value().uuid, sizeof(trailer.value().uuid));
    const core::Optional<std::string> parent_uuid =
        ParseUuidField(trailer.value().parent_uuid, sizeof(trailer.value().parent_uuid));
    const core::Optional<std::vector<uint8_t> > user_tag =
        ParseUserTagField(trailer.value().user_tag, sizeof(trailer.value().user_tag));
    // An absent field clears the corresponding arg rather than leaving the
    // caller's value in place: Rust assigns the `Option` straight across, so
    // a source with no parent produces an output with no parent.
    stamped.uuid = uuid.has_value() ? *uuid : std::string();
    stamped.parent_uuid = parent_uuid.has_value() ? *parent_uuid : std::string();
    stamped.user_tag = user_tag.has_value() ? *user_tag : std::vector<uint8_t>();

    const uint64_t blocks = (virtual_size_ + kAlign - 1) / kAlign;
    if (blocks > static_cast<uint64_t>(~static_cast<uint32_t>(0))) {
        return core::make_unexpected(std::string("commit: virtual size too large to query"));
    }
    std::vector<SegmentMapping> mappings;
    index_->Lookup(Segment(0, static_cast<uint32_t>(blocks)), &mappings);

    lsmt::CompactCommitArgs identity;
    identity.uuid = stamped.uuid;
    identity.parent_uuid = stamped.parent_uuid;
    identity.user_tag = stamped.user_tag;

    LayerStackCompactSource source(layers_);
    VirtualFileCompactSink sink(stamped.writer);
    std::string err;
    if (!lsmt::CompactTo(&sink, &source, mappings, virtual_size_, identity, &err)) {
        return core::make_unexpected(std::string("commit_preserving_metadata: ") + err);
    }
    return core::Unit();
}

core::Expected<size_t, std::string> LsmtReadOnlyFile::SeekDataSegments(
    uint64_t begin, uint64_t end, std::vector<Segment>* segs) const {
    if (segs == 0) {
        return core::make_unexpected(std::string("seek_data: null output"));
    }
    // An empty or inverted range is zero segments, not an error: a caller
    // walking a file hits this naturally at the end.
    if (end <= begin) return static_cast<size_t>(0);

    const uint64_t begin_blk = begin / kAlign;
    const uint64_t end_blk = (end + kAlign - 1) / kAlign;
    if (end_blk <= begin_blk) return static_cast<size_t>(0);

    std::vector<SegmentMapping> mappings;
    index_->Lookup(Segment(begin_blk, static_cast<uint32_t>(end_blk - begin_blk)), &mappings);
    for (size_t i = 0; i < mappings.size(); ++i) {
        // Zeroed ranges are not data. Reporting them would make a caller
        // copying "the data" write out zeros it could have skipped, which
        // for a sparse export defeats the point.
        if (mappings[i].zeroed) continue;
        segs->push_back(Segment(mappings[i].Offset(), mappings[i].Length()));
    }
    return segs->size();
}


// ---------------------------------------------------------------------------
// layer stacking
// ---------------------------------------------------------------------------

core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string>
LsmtReadOnlyFile::FromMergedLayers(std::shared_ptr<LogIndex> index,
                                     const std::vector<std::shared_ptr<VirtualFile> >& layers,
                                     uint64_t virtual_size,
                                     const std::vector<std::string>& uuids) {
    if (uuids.size() != layers.size()) {
        // Refused rather than padded. The uuid is how a caller identifies
        // which layer a mapping came from, so a vector that does not line up
        // with the layers would answer `GetUuid` with another layer's
        // identity -- worse than not answering at all.
        std::ostringstream o;
        o << "uuid count " << uuids.size() << " does not match " << layers.size() << " layers";
        return core::make_unexpected(o.str());
    }
    core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> f =
        FromLayers(index, layers, virtual_size);
    if (!f.ok()) return f;
    f.value()->uuids_ = uuids;
    return f;
}

core::Expected<std::vector<ReadOnlyLayerMetadata>, std::string> LoadReadonlyLayersMetadata(
    const std::vector<std::shared_ptr<VirtualFile> >& files) {
    std::vector<ReadOnlyLayerMetadata> out;
    out.reserve(files.size());

    for (size_t i = 0; i < files.size(); ++i) {
        if (!files[i]) {
            std::ostringstream o;
            o << "readonly layer " << i << " is null";
            return core::make_unexpected(o.str());
        }
        const core::Expected<uint64_t, std::string> fsz = files[i]->Size();
        if (!fsz.ok()) return core::make_unexpected(fsz.error());

        const core::Expected<HeaderTrailer, std::string> header =
            VerifyHt(files[i].get(), false, fsz.value());
        if (!header.ok()) {
            std::ostringstream o;
            o << "failed to read readonly layer " << i << " metadata: " << header.error();
            return core::make_unexpected(o.str());
        }
        const core::Expected<HeaderTrailer, std::string> trailer =
            VerifyHt(files[i].get(), true, fsz.value());
        if (!trailer.ok()) {
            std::ostringstream o;
            o << "failed to read readonly layer " << i << " metadata: " << trailer.error();
            return core::make_unexpected(o.str());
        }

        ReadOnlyLayerMetadata m;
        const char* raw = reinterpret_cast<const char*>(trailer.value().uuid);
        m.uuid = std::string(raw, ::strnlen(raw, sizeof(trailer.value().uuid)));
        m.file_size = fsz.value();
        m.virtual_size = trailer.value().virtual_size;
        m.index_offset = trailer.value().index_offset;
        m.index_size = trailer.value().index_size;
        m.header_version = header.value().version;
        m.header_sub_version = header.value().sub_version;
        m.trailer_version = trailer.value().version;
        m.trailer_sub_version = trailer.value().sub_version;
        out.push_back(m);
    }
    return out;
}

namespace {

/// Rust `stack.rs :: merge_readonly_indexes`.
///
/// Each layer's index is loaded with its tags reset, then the whole set is
/// reversed before merging: `ReadOnlyIndex::Merge` assigns the tag by
/// position with tag 0 meaning newest, while the caller supplies layers
/// oldest-first.
core::Expected<std::shared_ptr<ReadOnlyIndex>, std::string> MergeReadonlyIndexes(
    const std::vector<std::shared_ptr<VirtualFile> >& files,
    const std::vector<ReadOnlyLayerMetadata>& metadata) {
    if (files.size() != metadata.size()) {
        return core::make_unexpected(std::string("readonly files/metadata length mismatch"));
    }

    std::vector<ReadOnlyIndex> per_layer;
    per_layer.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        std::vector<SegmentMapping> mappings;
        if (metadata[i].index_size > 0) {
            const uint64_t entry = static_cast<uint64_t>(sizeof(DiskSegmentMapping));
            if (metadata[i].index_size > (~static_cast<uint64_t>(0)) / entry) {
                std::ostringstream o;
                o << "failed to load readonly layer " << i << " index: size overflows";
                return core::make_unexpected(o.str());
            }
            const uint64_t region_bytes = metadata[i].index_size * entry;
            if (metadata[i].index_offset > metadata[i].file_size ||
                region_bytes > metadata[i].file_size - metadata[i].index_offset) {
                std::ostringstream o;
                o << "failed to load readonly layer " << i << " index: region out of range";
                return core::make_unexpected(o.str());
            }
            std::vector<uint8_t> region(static_cast<size_t>(region_bytes), 0);
            const core::Expected<core::Unit, std::string> r =
                ReadExact(files[i].get(), metadata[i].index_offset, &region[0], region.size());
            if (!r.ok()) {
                std::ostringstream o;
                o << "failed to load readonly layer " << i << " index: " << r.error();
                return core::make_unexpected(o.str());
            }
            // Tags are reset here and reassigned by the merge below. A layer
            // records tags relative to the stack it was built in, which has
            // nothing to do with the stack it is being opened in now.
            mappings = LoadIndexAndResetTags(&region[0], region.size(),
                                             static_cast<size_t>(metadata[i].index_size));
        }
        per_layer.push_back(ReadOnlyIndex(mappings));
    }

    // Reversed so the newest layer comes first, which is what `Merge` treats
    // as tag 0. Without this the oldest layer would shadow the newest, and
    // every overwritten block would read back its pre-update contents --
    // silently, because both answers are well-formed.
    std::vector<const ReadOnlyIndex*> refs;
    refs.reserve(per_layer.size());
    for (size_t i = per_layer.size(); i-- > 0;) refs.push_back(&per_layer[i]);

    return std::shared_ptr<ReadOnlyIndex>(new ReadOnlyIndex(ReadOnlyIndex::Merge(refs)));
}

/// Rust `stack.rs :: build_readonly_stack_from_merged`.
core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> BuildReadonlyStackFromMerged(
    const std::vector<std::shared_ptr<VirtualFile> >& files,
    const std::vector<ReadOnlyLayerMetadata>& metadata,
    std::shared_ptr<ReadOnlyIndex> merged) {
    if (files.size() != metadata.size()) {
        return core::make_unexpected(std::string("readonly files/metadata length mismatch"));
    }

    // The last layer that declares one wins -- not the largest, and not the
    // first. Layers arrive oldest-first, so this is the newest declaration:
    // an image that grew keeps the size it grew to, and one whose newest
    // layer is silent inherits the last layer that spoke.
    uint64_t virtual_size = 0;
    for (size_t i = 0; i < metadata.size(); ++i) {
        if (metadata[i].virtual_size > 0) virtual_size = metadata[i].virtual_size;
    }

    std::vector<std::shared_ptr<VirtualFile> > ro_layers(files.rbegin(), files.rend());
    std::vector<std::string> uuids;
    uuids.reserve(metadata.size());
    for (size_t i = metadata.size(); i-- > 0;) uuids.push_back(metadata[i].uuid);

    return LsmtReadOnlyFile::FromMergedLayers(merged, ro_layers, virtual_size, uuids);
}

}  // namespace

core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> OpenFilesRo(
    const std::vector<std::shared_ptr<VirtualFile> >& files) {
    if (files.empty()) return core::make_unexpected(std::string("empty file list"));
    if (files.size() > kMaxStackLayers) {
        // The tag is one byte, so there is no room left to name a 256th
        // layer; accepting one would wrap it onto an existing layer.
        std::ostringstream o;
        o << "too many layers: " << files.size() << " > " << kMaxStackLayers;
        return core::make_unexpected(o.str());
    }

    const core::Expected<std::vector<ReadOnlyLayerMetadata>, std::string> metadata =
        LoadReadonlyLayersMetadata(files);
    if (!metadata.ok()) return core::make_unexpected(metadata.error());

    const core::Expected<std::shared_ptr<ReadOnlyIndex>, std::string> merged =
        MergeReadonlyIndexes(files, metadata.value());
    if (!merged.ok()) return core::make_unexpected(merged.error());

    return BuildReadonlyStackFromMerged(files, metadata.value(), merged.value());
}

// ---------------------------------------------------------------------------
// Premerged index cache
//
// Rust: lsmt/file/{types.rs, helper.rs, stack.rs}
//
// Merging the indices of a deep stack is deterministic work over immutable
// inputs, so the answer can be cached on disk and reused by the next open of
// the same layer set. Every failure here falls back to merging, because the
// cache is an optimisation and an unusable cache must not make an image
// unopenable.
// ---------------------------------------------------------------------------

const char* const kPremergedIndexExt = "pmidx";
const char kPremergedIndexMagic[8] = {'P', 'M', 'I', 'D', 'X', '0', '0', '1'};

namespace {

/// Rust `premerged_index_cache_limit_bytes`.
uint64_t PremergedIndexCacheLimitBytes(uint32_t cache_size_gb) {
    if (cache_size_gb == 0) return kMinPremergedIndexCacheBytes;
    const uint64_t gib = 1024ULL * 1024 * 1024;
    uint64_t budget = static_cast<uint64_t>(cache_size_gb) * gib / 16;
    if (budget < kMinPremergedIndexCacheBytes) budget = kMinPremergedIndexCacheBytes;
    if (budget > kMaxPremergedIndexCacheBytes) budget = kMaxPremergedIndexCacheBytes;
    return budget;
}

void AppendU32Le(std::vector<uint8_t>* out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void AppendU64Le(std::vector<uint8_t>* out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

uint32_t ReadU32LeAt(const uint8_t* b, size_t at) {
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
           (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

uint64_t ReadU64LeAt(const uint8_t* b, size_t at) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<uint64_t>(b[at + static_cast<size_t>(i)]);
    return v;
}

std::string HexLower(const uint8_t* bytes, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[(bytes[i] >> 4) & 0x0F]);
        out.push_back(digits[bytes[i] & 0x0F]);
    }
    return out;
}

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir[dir.size() - 1] == '/') return dir + name;
    return dir + "/" + name;
}

/// Rust's `uuid.is_nil()`. An all-zero UUID means the layer never got a stable
/// identity, so it cannot take part in a cache key.
bool IsNilUuid(const std::string& uuid) {
    if (uuid.empty()) return true;
    for (size_t i = 0; i < uuid.size(); ++i) {
        const char c = uuid[i];
        if (c != '0' && c != '-') return false;
    }
    return true;
}

/// Rust `PremergedIndexCacheKey`.
struct PremergedIndexCacheKey {
    uint8_t digest[32];
    std::string digest_hex;
    size_t layer_count;

    PremergedIndexCacheKey() : layer_count(0) { std::memset(digest, 0, sizeof(digest)); }

    std::string ArtifactPath(const std::string& cache_dir) const {
        return JoinPath(JoinPath(cache_dir, kPremergedIndexDir),
                        digest_hex + "." + kPremergedIndexExt);
    }
};

/// Rust `PremergedIndexCacheKey::from_metadata`.
///
/// False when any layer has a nil UUID: without a stable identity for every
/// layer the key could collide across different stacks, and a wrong hit would
/// serve one image's index for another's data.
bool BuildPremergedKey(const std::vector<ReadOnlyLayerMetadata>& metadata,
                       PremergedIndexCacheKey* out) {
    if (metadata.empty()) return false;
    for (size_t i = 0; i < metadata.size(); ++i) {
        if (IsNilUuid(metadata[i].uuid)) return false;
    }

    std::vector<uint8_t> key;
    const char* domain = "overlaybd-premerged-index-key-v1";
    key.insert(key.end(), domain, domain + std::strlen(domain));
    // Both versions go into the key, so bumping either one invalidates every
    // artifact without needing the directory to be cleared.
    AppendU32Le(&key, kPremergedIndexFormatVersion);
    AppendU32Le(&key, kPremergedIndexMergeVersion);
    AppendU32Le(&key, static_cast<uint32_t>(metadata.size()));

    // Reversed: the merged index tags layers top-to-bottom while this function
    // receives them bottom-to-top. The key follows the merged order so that it
    // describes the artifact it will name, not the call that produced it.
    for (size_t i = metadata.size(); i-- > 0;) {
        const ReadOnlyLayerMetadata& layer = metadata[i];
        // The UUID's bytes as stored. Rust hashes `uuid.as_bytes()` (16 raw
        // bytes) while this holds the textual form; the digest differs from
        // Rust's for the same stack, which is fine because the artifact is
        // private to one implementation -- but it does mean the two do not
        // share a cache directory.
        key.insert(key.end(), layer.uuid.begin(), layer.uuid.end());
        AppendU64Le(&key, layer.file_size);
        AppendU64Le(&key, layer.virtual_size);
        AppendU64Le(&key, layer.index_offset);
        AppendU64Le(&key, layer.index_size);
        key.push_back(layer.header_version);
        key.push_back(layer.header_sub_version);
        key.push_back(layer.trailer_version);
        key.push_back(layer.trailer_sub_version);
    }

    const core::Digest digest =
        core::Digest::Sha256(key.empty() ? 0 : &key[0], key.size());
    std::memcpy(out->digest, digest.bytes(), 32);
    out->digest_hex = HexLower(out->digest, 32);
    out->layer_count = metadata.size();
    return true;
}

/// Rust `PremergedIndexArtifactHeader`.
struct PremergedArtifactHeader {
    uint32_t format_version;
    uint32_t merge_version;
    uint32_t layer_count;
    uint64_t mapping_count;
    uint64_t body_len;
    uint8_t key_digest[32];
    uint8_t body_checksum[32];
};

/// Rust `build_premerged_artifact_header`.
std::vector<uint8_t> BuildPremergedArtifactHeader(const PremergedIndexCacheKey& key,
                                                  uint64_t mapping_count, uint64_t body_len,
                                                  const uint8_t* body_checksum) {
    std::vector<uint8_t> header;
    header.reserve(kPremergedIndexHeaderLen);
    header.insert(header.end(), kPremergedIndexMagic, kPremergedIndexMagic + 8);
    AppendU32Le(&header, kPremergedIndexFormatVersion);
    AppendU32Le(&header, kPremergedIndexMergeVersion);
    // The header's own length is recorded so a future longer header is
    // rejected rather than read as a short one with garbage fields.
    AppendU32Le(&header, static_cast<uint32_t>(kPremergedIndexHeaderLen));
    AppendU32Le(&header, static_cast<uint32_t>(key.layer_count));
    AppendU64Le(&header, mapping_count);
    AppendU64Le(&header, body_len);
    header.insert(header.end(), key.digest, key.digest + 32);
    header.insert(header.end(), body_checksum, body_checksum + 32);
    return header;
}

/// Rust `parse_premerged_artifact_header`.
core::Expected<PremergedArtifactHeader, std::string> ParsePremergedArtifactHeader(
    const std::vector<uint8_t>& bytes) {
    if (bytes.size() < kPremergedIndexHeaderLen) {
        return core::make_unexpected(std::string("premerged artifact header is truncated"));
    }
    if (std::memcmp(&bytes[0], kPremergedIndexMagic, 8) != 0) {
        return core::make_unexpected(std::string("premerged artifact magic mismatch"));
    }
    const uint8_t* b = &bytes[0];
    if (ReadU32LeAt(b, 16) != static_cast<uint32_t>(kPremergedIndexHeaderLen)) {
        return core::make_unexpected(std::string("premerged artifact header length mismatch"));
    }

    PremergedArtifactHeader header;
    header.format_version = ReadU32LeAt(b, 8);
    header.merge_version = ReadU32LeAt(b, 12);
    header.layer_count = ReadU32LeAt(b, 20);
    header.mapping_count = ReadU64LeAt(b, 24);
    header.body_len = ReadU64LeAt(b, 32);
    std::memcpy(header.key_digest, b + 40, 32);
    std::memcpy(header.body_checksum, b + 72, 32);
    return header;
}

/// Rust `serialize_premerged_mappings`.
std::vector<uint8_t> SerializePremergedMappings(const std::vector<SegmentMapping>& mappings) {
    std::vector<uint8_t> body;
    body.reserve(mappings.size() * 16);
    for (size_t i = 0; i < mappings.size(); ++i) {
        const DiskSegmentMapping disk = DiskSegmentMapping::FromMemory(mappings[i]);
        AppendU64Le(&body, disk.data_low);
        AppendU64Le(&body, disk.data_high);
    }
    return body;
}

/// Rust `deserialize_premerged_mappings`.
///
/// Every invariant the merge path would have established is re-checked here,
/// because a cached artifact is attacker-adjacent input: it is a file on disk
/// that a later read trusts as if it had been computed in this process.
core::Expected<std::vector<SegmentMapping>, std::string> DeserializePremergedMappings(
    const uint8_t* body, size_t body_len, size_t mapping_count, size_t layer_count) {
    const size_t stride = 16;
    if (mapping_count != 0 && mapping_count > body_len / stride) {
        return core::make_unexpected(std::string("premerged artifact body length overflow"));
    }
    if (body_len != mapping_count * stride) {
        return core::make_unexpected(std::string("premerged artifact body length mismatch"));
    }

    std::vector<SegmentMapping> mappings;
    mappings.reserve(mapping_count);
    bool have_previous_end = false;
    uint64_t previous_end = 0;

    for (size_t i = 0; i < mapping_count; ++i) {
        DiskSegmentMapping disk;
        disk.data_low = ReadU64LeAt(body, i * stride);
        disk.data_high = ReadU64LeAt(body, i * stride + 8);
        const SegmentMapping mapping = disk.ToMemory();

        if (mapping.Length() == 0) {
            return core::make_unexpected(std::string("premerged artifact contains empty mapping"));
        }
        // A non-zeroed mapping with no physical offset would resolve to no
        // bytes at all, which a read cannot represent.
        if (!mapping.zeroed && !mapping.HasPhysicalRange()) {
            return core::make_unexpected(std::string("data mapping has no physical offset"));
        }
        // The tag indexes straight into the layer vector, so one past the end
        // would read some other layer -- or past it.
        if (static_cast<size_t>(mapping.tag) >= layer_count) {
            return core::make_unexpected(std::string("premerged artifact layer tag out of range"));
        }
        if (have_previous_end && mapping.Offset() < previous_end) {
            // Lookup is a binary search over this list; unsorted or
            // overlapping input makes it answer with the wrong mapping rather
            // than fail.
            return core::make_unexpected(
                std::string("premerged artifact mappings are not sorted or overlap"));
        }
        have_previous_end = true;
        previous_end = mapping.End();
        mappings.push_back(mapping);
    }
    return mappings;
}

/// Rust `decode_premerged_index_artifact`.
core::Expected<std::shared_ptr<ReadOnlyIndex>, std::string> DecodePremergedIndexArtifact(
    const std::vector<uint8_t>& bytes, const PremergedIndexCacheKey& key) {
    const core::Expected<PremergedArtifactHeader, std::string> header =
        ParsePremergedArtifactHeader(bytes);
    if (!header.ok()) return core::make_unexpected(header.error());

    if (header.value().format_version != kPremergedIndexFormatVersion) {
        return core::make_unexpected(std::string("premerged artifact format version mismatch"));
    }
    if (header.value().merge_version != kPremergedIndexMergeVersion) {
        return core::make_unexpected(std::string("premerged artifact merge version mismatch"));
    }
    if (static_cast<size_t>(header.value().layer_count) != key.layer_count) {
        return core::make_unexpected(std::string("premerged artifact layer count mismatch"));
    }
    // The decisive check: the digest covers every layer's identity and
    // geometry, so a match means this artifact really describes this stack.
    // The filename alone would not, since a digest collision in the path is
    // indistinguishable from a hit.
    if (std::memcmp(header.value().key_digest, key.digest, 32) != 0) {
        return core::make_unexpected(std::string("premerged artifact key digest mismatch"));
    }

    const uint64_t body_len = header.value().body_len;
    const uint64_t mapping_count = header.value().mapping_count;
    if (body_len > static_cast<uint64_t>(SIZE_MAX) ||
        mapping_count > static_cast<uint64_t>(SIZE_MAX)) {
        return core::make_unexpected(
            std::string("premerged artifact body length does not fit usize"));
    }
    if (mapping_count != 0 && mapping_count > UINT64_MAX / 16) {
        return core::make_unexpected(std::string("premerged artifact body length overflow"));
    }
    if (body_len != mapping_count * 16) {
        return core::make_unexpected(
            std::string("premerged artifact mapping count/body length mismatch"));
    }
    if (static_cast<uint64_t>(bytes.size()) != kPremergedIndexHeaderLen + body_len) {
        return core::make_unexpected(std::string("premerged artifact file length mismatch"));
    }

    const uint8_t* body = &bytes[0] + kPremergedIndexHeaderLen;
    const core::Digest checksum =
        core::Digest::Sha256(body_len == 0 ? 0 : body, static_cast<size_t>(body_len));
    if (std::memcmp(checksum.bytes(), header.value().body_checksum, 32) != 0) {
        return core::make_unexpected(std::string("premerged artifact body checksum mismatch"));
    }

    const core::Expected<std::vector<SegmentMapping>, std::string> mappings =
        DeserializePremergedMappings(body, static_cast<size_t>(body_len),
                                     static_cast<size_t>(mapping_count), key.layer_count);
    if (!mappings.ok()) return core::make_unexpected(mappings.error());

    return std::shared_ptr<ReadOnlyIndex>(new ReadOnlyIndex(mappings.value()));
}

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>* out, bool* not_found) {
    *not_found = false;
    FILE* file = ::fopen(path.c_str(), "rb");
    if (file == 0) {
        *not_found = (errno == ENOENT);
        return false;
    }
    out->clear();
    uint8_t buffer[65536];
    for (;;) {
        const size_t got = ::fread(buffer, 1, sizeof(buffer), file);
        if (got > 0) out->insert(out->end(), buffer, buffer + got);
        if (got < sizeof(buffer)) break;
    }
    const bool failed = ::ferror(file) != 0;
    ::fclose(file);
    return !failed;
}

/// Rust `try_read_premerged_index_artifact`.
///
/// Null for every failure, with a warning: a corrupt or unreadable artifact is
/// a cache miss, and treating it as an error would make a bad file in the
/// cache directory break every open of that image.
std::shared_ptr<ReadOnlyIndex> TryReadPremergedIndexArtifact(
    const std::string& cache_dir, const PremergedIndexCacheKey& key) {
    const std::string path = key.ArtifactPath(cache_dir);
    std::vector<uint8_t> bytes;
    bool not_found = false;
    if (!ReadWholeFile(path, &bytes, &not_found)) {
        // A missing artifact is the ordinary cold case, so it is silent.
        if (!not_found) {
            AGENTENV_WARN("path=" << path << " read premerged index artifact failed");
        }
        return std::shared_ptr<ReadOnlyIndex>();
    }
    const core::Expected<std::shared_ptr<ReadOnlyIndex>, std::string> decoded =
        DecodePremergedIndexArtifact(bytes, key);
    if (!decoded.ok()) {
        AGENTENV_WARN("err=" << decoded.error() << " path=" << path
                             << " ignore invalid premerged index artifact");
        return std::shared_ptr<ReadOnlyIndex>();
    }
    return decoded.value();
}

/// Rust `PREMERGED_INDEX_PRUNE_SCAN_FRACTION`.
const uint64_t kPremergedIndexPruneScanFraction = 8;

/// Rust `PremergedIndexScanAccount` + `PremergedIndexPruneState`.
///
/// Growth accounting rather than a scan per write: listing and stat-ing the
/// whole directory costs far more than writing one artifact, so a scan is only
/// elected once accumulated growth could plausibly have breached the budget.
struct PremergedPruneState {
    std::mutex mu;
    uint64_t bytes_since_scan;
    bool scan_in_progress;
    bool initial_scan_done;

    PremergedPruneState()
        : bytes_since_scan(0), scan_in_progress(false), initial_scan_done(false) {}

    uint64_t Threshold(uint64_t max_dir_bytes) const {
        const uint64_t t = max_dir_bytes / kPremergedIndexPruneScanFraction;
        return t > 0 ? t : 1;
    }

    /// Rust `elect_scan`. The first charge on a fresh state always elects, so
    /// a directory already over budget at process start gets validated rather
    /// than waiting for a threshold's worth of new writes.
    bool ElectScan(uint64_t written_bytes, uint64_t max_dir_bytes) {
        std::lock_guard<std::mutex> lk(mu);
        bytes_since_scan += written_bytes;
        if (scan_in_progress) return false;
        const bool elected =
            !initial_scan_done || bytes_since_scan >= Threshold(max_dir_bytes);
        if (elected) {
            initial_scan_done = true;
            bytes_since_scan = 0;
            scan_in_progress = true;
        }
        return elected;
    }

    /// Rust `scan_finished` — true when growth charged *during* the scan has
    /// already re-crossed the trigger, so the same caller runs another pass
    /// instead of leaving the directory over budget until the next write.
    bool ScanFinished(uint64_t max_dir_bytes) {
        std::lock_guard<std::mutex> lk(mu);
        scan_in_progress = false;
        if (bytes_since_scan >= Threshold(max_dir_bytes)) {
            bytes_since_scan = 0;
            scan_in_progress = true;
            return true;
        }
        return false;
    }

    /// Rust `scan_aborted` — concurrent charges are kept, so the next write
    /// re-elects rather than the failure resetting the accounting.
    void ScanAborted() {
        std::lock_guard<std::mutex> lk(mu);
        scan_in_progress = false;
    }
};

std::shared_ptr<PremergedPruneState> PremergedPruneStateFor(const std::string& cache_dir) {
    static std::mutex states_mu;
    static std::map<std::string, std::shared_ptr<PremergedPruneState> >* states =
        new std::map<std::string, std::shared_ptr<PremergedPruneState> >();
    std::lock_guard<std::mutex> lk(states_mu);
    std::shared_ptr<PremergedPruneState>& slot = (*states)[cache_dir];
    if (!slot) slot.reset(new PremergedPruneState());
    return slot;
}

/// Rust `premerged_index_locks` + `acquire/release_premerged_index_lock`.
///
/// Per-digest, so two different stacks never serialise against each other,
/// and weakly held so the map does not grow without bound. The lock exists to
/// stop N concurrent opens of the same image each computing the same merge and
/// each writing the same artifact.
struct PremergedLockRegistry {
    std::mutex mu;
    std::map<std::string, std::weak_ptr<std::mutex> > locks;
};

PremergedLockRegistry* PremergedLocks() {
    static PremergedLockRegistry* registry = new PremergedLockRegistry();
    return registry;
}

std::shared_ptr<std::mutex> AcquirePremergedIndexLock(const std::string& key) {
    PremergedLockRegistry* registry = PremergedLocks();
    std::lock_guard<std::mutex> lk(registry->mu);
    // Swept only when the map is large, so the common path stays O(log n)
    // rather than paying a full scan per open.
    if (registry->locks.size() > 1024) {
        for (std::map<std::string, std::weak_ptr<std::mutex> >::iterator it =
                 registry->locks.begin();
             it != registry->locks.end();) {
            if (it->second.expired()) {
                registry->locks.erase(it++);
            } else {
                ++it;
            }
        }
    }
    const std::map<std::string, std::weak_ptr<std::mutex> >::iterator found =
        registry->locks.find(key);
    if (found != registry->locks.end()) {
        const std::shared_ptr<std::mutex> existing = found->second.lock();
        if (existing) return existing;
    }
    std::shared_ptr<std::mutex> lock(new std::mutex());
    registry->locks[key] = lock;
    return lock;
}

void ReleasePremergedIndexLock(const std::string& key,
                               const std::shared_ptr<std::mutex>& lock) {
    PremergedLockRegistry* registry = PremergedLocks();
    std::lock_guard<std::mutex> lk(registry->mu);
    const std::map<std::string, std::weak_ptr<std::mutex> >::iterator found =
        registry->locks.find(key);
    if (found == registry->locks.end()) return;
    const std::shared_ptr<std::mutex> current = found->second.lock();
    if (!current) {
        // Someone else's entry already died; drop the dead slot.
        registry->locks.erase(found);
        return;
    }
    // Erased only when this caller holds the last reference *and* the entry is
    // still this very lock: a newer lock for the same digest must survive.
    if (current == lock && lock.use_count() <= 2) registry->locks.erase(found);
}

struct PremergedArtifactEntry {
    std::string path;
    uint64_t len;
    /// Seconds since the epoch. Ordering only, so the resolution is enough.
    int64_t modified;
};

bool PremergedEntryOlder(const PremergedArtifactEntry& a, const PremergedArtifactEntry& b) {
    return a.modified < b.modified;
}

/// Rust `prune_premerged_index_dir_blocking` — delete oldest artifacts until
/// the directory fits.
core::Expected<core::Unit, std::string> PrunePremergedIndexDir(const std::string& dir,
                                                                  uint64_t max_dir_bytes) {
    DIR* handle = ::opendir(dir.c_str());
    if (handle == 0) {
        return core::make_unexpected("read premerged index cache dir " + dir);
    }

    std::vector<PremergedArtifactEntry> entries;
    uint64_t total = 0;
    const std::string suffix = std::string(".") + kPremergedIndexExt;

    for (;;) {
        struct dirent* ent = ::readdir(handle);
        if (ent == 0) break;
        const std::string name(ent->d_name);
        // Extension, not prefix: the writer's temporary files live in the same
        // directory and must not be counted or deleted as artifacts.
        if (name.size() <= suffix.size()) continue;
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;

        const std::string path = JoinPath(dir, name);
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) {
            // Deleted concurrently with the scan; that only shrinks the
            // directory, so it is not a failure.
            if (errno == ENOENT) continue;
            ::closedir(handle);
            return core::make_unexpected("stat " + path);
        }
        if (!S_ISREG(st.st_mode)) continue;

        PremergedArtifactEntry entry;
        entry.path = path;
        entry.len = static_cast<uint64_t>(st.st_size);
        entry.modified = static_cast<int64_t>(st.st_mtime);
        total += entry.len;
        entries.push_back(entry);
    }
    ::closedir(handle);

    if (total <= max_dir_bytes) return core::Unit();

    // Oldest first. Modification time, not access time: an artifact is written
    // once and then only read, so mtime is the age of the layer set it
    // describes -- and atime is frequently disabled.
    std::sort(entries.begin(), entries.end(), PremergedEntryOlder);
    for (size_t i = 0; i < entries.size() && total > max_dir_bytes; ++i) {
        if (::unlink(entries[i].path.c_str()) == 0) {
            total -= entries[i].len;
        } else if (errno == ENOENT) {
            // Removed by someone else since the scan: those bytes left the
            // directory too, so they count as freed.
            total -= entries[i].len;
        } else {
            AGENTENV_WARN("path=" << entries[i].path
                                  << " remove premerged index artifact failed");
        }
    }
    return core::Unit();
}

/// Rust `prune_premerged_index_cache`.
void PrunePremergedIndexCache(const std::string& cache_dir, uint64_t written,
                              uint64_t max_dir_bytes) {
    const std::shared_ptr<PremergedPruneState> state = PremergedPruneStateFor(cache_dir);
    if (!state->ElectScan(written, max_dir_bytes)) return;

    const std::string dir = JoinPath(cache_dir, kPremergedIndexDir);
    for (;;) {
        const core::Expected<core::Unit, std::string> pruned =
            PrunePremergedIndexDir(dir, max_dir_bytes);
        if (!pruned.ok()) {
            AGENTENV_WARN("err=" << pruned.error() << " path=" << dir
                                 << " prune premerged index cache dir failed");
            state->ScanAborted();
            return;
        }
        if (!state->ScanFinished(max_dir_bytes)) return;
    }
}

/// Rust `write_premerged_index_artifact` — tmp file plus rename.
///
/// Atomic by rename, so a concurrent reader either sees the previous artifact
/// or the complete new one. A partially written file under the real name would
/// be read, fail its checksum, and be discarded -- correct but wasteful, and
/// it would mask the real one.
core::Expected<uint64_t, std::string> WritePremergedIndexArtifact(
    const std::string& cache_dir, const PremergedIndexCacheKey& key,
    const ReadOnlyIndex& index) {
    const std::string dir = JoinPath(cache_dir, kPremergedIndexDir);
    const core::Expected<core::Unit, std::string> made = core::fs::CreateDirAll(dir);
    if (!made.ok()) {
        return core::make_unexpected("create premerged index cache dir " + dir + ": " +
                                     made.error());
    }

    const std::vector<uint8_t> body = SerializePremergedMappings(index.Mappings());
    const core::Digest checksum =
        core::Digest::Sha256(body.empty() ? 0 : &body[0], body.size());

    std::vector<uint8_t> artifact = BuildPremergedArtifactHeader(
        key, static_cast<uint64_t>(index.Mappings().size()),
        static_cast<uint64_t>(body.size()), checksum.bytes());
    artifact.insert(artifact.end(), body.begin(), body.end());

    std::ostringstream tmp_name;
    // The pid and a counter rather than a UUID: the only requirement is that
    // two concurrent writers of the same digest pick different names.
    static std::atomic<uint64_t> tmp_counter(0);
    tmp_name << "." << key.digest_hex << "." << static_cast<long>(::getpid()) << "-"
             << tmp_counter.fetch_add(1) << ".tmp";
    const std::string tmp = JoinPath(dir, tmp_name.str());
    const std::string path = key.ArtifactPath(cache_dir);

    FILE* file = ::fopen(tmp.c_str(), "wb");
    if (file == 0) return core::make_unexpected("write " + tmp);
    bool ok = artifact.empty() ||
              ::fwrite(&artifact[0], 1, artifact.size(), file) == artifact.size();
    if (ok) ok = ::fflush(file) == 0;
    ::fclose(file);
    if (!ok) {
        ::unlink(tmp.c_str());
        return core::make_unexpected("write " + tmp);
    }

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return core::make_unexpected("rename premerged index artifact " + tmp + " to " + path);
    }
    return static_cast<uint64_t>(artifact.size());
}

}  // namespace

PremergedIndexCachePolicy PremergedIndexCachePolicy::Hybrid(uint32_t cache_size_gb) {
    PremergedIndexCachePolicy policy;
    policy.read = true;
    policy.write = true;
    policy.max_dir_bytes = PremergedIndexCacheLimitBytes(cache_size_gb);
    return policy;
}

PremergedIndexCachePolicy PremergedIndexCachePolicy::Disabled() {
    PremergedIndexCachePolicy policy;
    policy.read = false;
    policy.write = false;
    // The floor rather than zero: a disabled policy still carries a sane
    // budget, so enabling writes later cannot prune the directory to nothing.
    policy.max_dir_bytes = kMinPremergedIndexCacheBytes;
    return policy;
}

core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string>
OpenFilesRoWithPremergedCache(const std::vector<std::shared_ptr<VirtualFile> >& files,
                                 const std::string& cache_dir,
                                 const PremergedIndexCachePolicy& policy) {
    if (files.empty()) return core::make_unexpected(std::string("empty file list"));
    if (files.size() > kMaxStackLayers) {
        std::ostringstream o;
        o << "too many layers: " << files.size() << " > " << kMaxStackLayers;
        return core::make_unexpected(o.str());
    }

    // Nothing to gain either way, or nowhere to put it.
    if (!policy.read && !policy.write) return OpenFilesRo(files);
    if (cache_dir.empty()) return OpenFilesRo(files);

    const core::Expected<std::vector<ReadOnlyLayerMetadata>, std::string> metadata =
        LoadReadonlyLayersMetadata(files);
    if (!metadata.ok()) return core::make_unexpected(metadata.error());

    PremergedIndexCacheKey key;
    if (!BuildPremergedKey(metadata.value(), &key)) {
        // No stable identity for this stack, so nothing may be cached under
        // any name. Merge and return, exactly as the uncached path would.
        const core::Expected<std::shared_ptr<ReadOnlyIndex>, std::string> merged =
            MergeReadonlyIndexes(files, metadata.value());
        if (!merged.ok()) return core::make_unexpected(merged.error());
        return BuildReadonlyStackFromMerged(files, metadata.value(), merged.value());
    }

    // Probed before taking the digest lock: a hit is the common case and must
    // not queue behind another open's merge.
    if (policy.read) {
        const std::shared_ptr<ReadOnlyIndex> hit =
            TryReadPremergedIndexArtifact(cache_dir, key);
        if (hit) return BuildReadonlyStackFromMerged(files, metadata.value(), hit);
    }

    const std::shared_ptr<std::mutex> lock = AcquirePremergedIndexLock(key.digest_hex);
    {
        std::unique_lock<std::mutex> guard(*lock);

        // Re-probed under the lock: whoever held it may have just written the
        // artifact this caller is about to compute. Without this, N concurrent
        // cold opens would each do the full merge in turn.
        if (policy.read) {
            const std::shared_ptr<ReadOnlyIndex> hit =
                TryReadPremergedIndexArtifact(cache_dir, key);
            if (hit) {
                const core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> result =
                    BuildReadonlyStackFromMerged(files, metadata.value(), hit);
                guard.unlock();
                ReleasePremergedIndexLock(key.digest_hex, lock);
                return result;
            }
        }

        const core::Expected<std::shared_ptr<ReadOnlyIndex>, std::string> merged =
            MergeReadonlyIndexes(files, metadata.value());
        if (!merged.ok()) {
            guard.unlock();
            ReleasePremergedIndexLock(key.digest_hex, lock);
            return core::make_unexpected(merged.error());
        }

        if (policy.write) {
            // Deviation: Rust hands the write and the prune tail to a spawned
            // task and returns immediately, so an open never waits for its own
            // cache fill. There is no task runtime here, and a detached thread
            // would have to keep the index alive past this scope while the
            // caller may already be reading through it -- so the write runs
            // inline. The cost is the artifact write (one sequential file) on
            // the cold path only; correctness is unaffected because a failed
            // write is ignored either way.
            const core::Expected<uint64_t, std::string> written =
                WritePremergedIndexArtifact(cache_dir, key, *merged.value());
            if (!written.ok()) {
                AGENTENV_WARN("err=" << written.error() << " digest=" << key.digest_hex
                                     << " write premerged index artifact failed");
            }

            const core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> result =
                BuildReadonlyStackFromMerged(files, metadata.value(), merged.value());

            // The lock goes before the prune, as Rust orders it: a directory
            // scan must not block the next opener of this same digest.
            guard.unlock();
            ReleasePremergedIndexLock(key.digest_hex, lock);
            if (written.ok()) {
                PrunePremergedIndexCache(cache_dir, written.value(), policy.max_dir_bytes);
            }
            return result;
        }

        const core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> result =
            BuildReadonlyStackFromMerged(files, metadata.value(), merged.value());
        guard.unlock();
        ReleasePremergedIndexLock(key.digest_hex, lock);
        return result;
    }
}


// ---------------------------------------------------------------------------
// probes
// ---------------------------------------------------------------------------

int IsLsmt(VirtualFile* file) {
    if (file == 0) return 0;
    const core::Expected<uint64_t, std::string> size = file->Size();
    if (!size.ok()) return 0;

    // The header first: without it the file is not LSMT-shaped at all, and
    // checking the trailer alone could match arbitrary trailing bytes.
    if (!VerifyHt(file, false, size.value()).ok()) return 0;
    // Then the trailer, which is what says the layer was sealed. A header
    // without one is an upper still being appended to -- readable as a file,
    // but not as a finished layer.
    if (VerifyHt(file, true, size.value()).ok()) return 1;
    return 0;
}

core::Expected<std::shared_ptr<ReadOnlyIndex>, std::string> OpenFileIndex(VirtualFile* file) {
    if (file == 0) {
        return core::make_unexpected(std::string("open_file_index: null file"));
    }
    const core::Expected<uint64_t, std::string> size = file->Size();
    if (!size.ok()) return core::make_unexpected(size.error());

    // Through the trailer, so an unsealed file is refused: the index of an
    // upper still being written has no fixed offset to read from.
    const core::Expected<HeaderTrailer, std::string> trailer =
        VerifyHt(file, true, size.value());
    if (!trailer.ok()) return core::make_unexpected(trailer.error());

    const uint64_t index_offset = trailer.value().index_offset;
    const uint64_t index_count = trailer.value().index_size;
    const uint64_t region_bytes = index_count * sizeof(DiskSegmentMapping);
    if (index_count != 0 && region_bytes / index_count != sizeof(DiskSegmentMapping)) {
        return core::make_unexpected(std::string("open_file_index: index size overflow"));
    }
    if (region_bytes > static_cast<uint64_t>(static_cast<size_t>(-1))) {
        return core::make_unexpected(std::string("open_file_index: index too large"));
    }

    const core::Expected<ByteBuffer, std::string> raw =
        file->ReadAt(index_offset, static_cast<size_t>(region_bytes));
    if (!raw.ok()) return core::make_unexpected(raw.error());
    if (raw.value().size() != static_cast<size_t>(region_bytes)) {
        return core::make_unexpected(
            std::string("open_file_index: short read of index region"));
    }

    // Tags are reset to 0 because this index describes one layer read on its
    // own. A tag inherited from whatever stack produced the file would index
    // into a layer vector this caller does not have.
    const std::vector<SegmentMapping> mappings = LoadIndexAndResetTags(
        region_bytes == 0 ? 0 : &raw.value()[0], static_cast<size_t>(region_bytes),
        static_cast<size_t>(index_count));
    return std::shared_ptr<ReadOnlyIndex>(new ReadOnlyIndex(mappings));
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
