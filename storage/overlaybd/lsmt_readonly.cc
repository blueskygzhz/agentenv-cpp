// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/lsmt/file/readonly.rs (+ helper.rs, vfile_io.rs)
#include "agentenv/storage/overlaybd/lsmt_readonly.h"

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

core::Expected<core::Unit, std::string> ReadExact(VirtualFile* file, uint64_t offset,
                                                    uint8_t* dst, size_t len) {
    size_t done = 0;
    while (done < len) {
        const core::Expected<size_t, std::string> n =
            file->ReadAtInto(offset + done, dst + done, len - done);
        if (!n.ok()) return core::make_unexpected(n.error());
        if (n.value() == 0) {
            // A short read is a success at the `VirtualFile` boundary but a
            // failure here: the index said these bytes are data, so handing
            // back the zeros already in the buffer would silently substitute
            // a hole for whatever the layer was supposed to hold.
            std::ostringstream o;
            o << "unexpected EOF: still need " << (len - done) << " byte(s)";
            return core::make_unexpected(o.str());
        }
        done += n.value();
    }
    return core::Unit();
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

core::Expected<size_t, std::string> LsmtReadOnlyFile::ReadInternalInto(uint64_t offset,
                                                                         uint8_t* dst,
                                                                         size_t len) {
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
                    ReadExact(layers_[layer_idx].get(), phys, dst + local, copy_len);
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

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
