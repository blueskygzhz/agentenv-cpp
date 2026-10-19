// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/lsmt/file/readwrite.rs (+ types.rs, helper.rs)
#include "agentenv/storage/overlaybd/lsmt_rw.h"

#include <cstring>
#include <sstream>

#include "agentenv/core/identity.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/sys.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

const uint64_t kAlign = kLsmtAlignment;      // 512
const uint64_t kHdr = kLsmtHeaderSize;       // 4096
const size_t kMappingSize = 16;              // DiskSegmentMapping on disk

/// Serialize one mapping into its 16-byte little-endian form.
///
/// Rust gets this from `zerocopy::IntoBytes` over a `#[repr(C)]` little-endian
/// struct; written out here because the C++ struct holds two host-order
/// `uint64_t` and the on-disk order has to be stated explicitly.
void EncodeMapping(const DiskSegmentMapping& d, uint8_t out[16]) {
    for (int k = 0; k < 8; ++k) {
        out[k] = static_cast<uint8_t>((d.data_low >> (8 * k)) & 0xFF);
    }
    for (int k = 0; k < 8; ++k) {
        out[8 + k] = static_cast<uint8_t>((d.data_high >> (8 * k)) & 0xFF);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// RwLayout
// ---------------------------------------------------------------------------

LsmtFileType RwLayoutFileType(RwLayout layout) {
    switch (layout) {
        case kRwSparse:
            return kLsmtFileSparseWritable;
        case kRwHybridLogStructured:
            return kLsmtFileHybridWritable;
        case kRwLogStructured:
        default:
            return kLsmtFileWritable;
    }
}

core::Expected<RwLayout, std::string> EnsureRwLayoutSupported(RwLayout layout) {
    if (layout != kRwSparse || sys::SparseExtentsAreReliable()) return layout;
    // Rust's wording. Refused rather than downgraded to another layout,
    // because the caller asked for a specific on-disk shape and silently
    // writing a different one would make the layer unreadable by whoever
    // expects what was requested.
    return core::make_unexpected(std::string(
        "sparse RW layout is not supported on this platform: it does not guarantee "
        "that unwritten regions are reported as holes, so the upper's extent map "
        "cannot be used to recover which blocks were written"));
}

// ---------------------------------------------------------------------------
// AppendDigestTracker
// ---------------------------------------------------------------------------

core::Expected<AppendDigestTracker, std::string> AppendDigestTracker::FromHeader(
    VirtualFile* file) {
    std::vector<uint8_t> raw(static_cast<size_t>(kHdr), 0);
    const core::Expected<size_t, std::string> read = file->ReadAtInto(0, &raw[0], raw.size());
    if (!read.ok()) return core::make_unexpected(read.error());
    if (read.value() != raw.size()) {
        // The header is the digest's starting point, so a partial one means
        // every digest derived from it would describe a file prefix that does
        // not exist.
        std::ostringstream o;
        o << "short read while initializing append digest tracker: expected " << kHdr
          << ", got " << read.value();
        return core::make_unexpected(o.str());
    }
    AppendDigestTracker t;
    t.hasher_.Update(&raw[0], raw.size());
    t.offset_ = kHdr;
    return t;
}

core::Expected<core::Unit, std::string> AppendDigestTracker::Absorb(uint64_t offset,
                                                                     const uint8_t* data,
                                                                     size_t len) {
    if (len == 0) return core::Unit();
    if (offset != offset_) {
        // Strictly contiguous, or nothing. A gap would hash a file that has a
        // hole the digest does not account for; a rewind would hash
        // overwritten bytes twice. Either way the result looks like a valid
        // digest of something, which is worse than having none.
        std::ostringstream o;
        o << "append digest tracker saw out-of-order write at " << offset << ", expected "
          << offset_;
        return core::make_unexpected(o.str());
    }
    hasher_.Update(data, len);
    if (offset_ > (~static_cast<uint64_t>(0)) - static_cast<uint64_t>(len)) {
        return core::make_unexpected(std::string("append digest size overflow"));
    }
    offset_ += static_cast<uint64_t>(len);
    return core::Unit();
}

LayerDescriptor AppendDigestTracker::Descriptor() const {
    LayerDescriptor d;
    // `PeekHex`, not a destructive finalize: sealing reports the descriptor
    // and then keeps absorbing the trailer bytes it is still writing.
    d.digest = hasher_.PeekHex();
    d.size = offset_;
    return d;
}

// ---------------------------------------------------------------------------
// sparse recovery
// ---------------------------------------------------------------------------

core::Expected<std::vector<SegmentMapping>, std::string> CreateMappingsFromSparse(
    VirtualFile* file, uint64_t start_offset) {
    if (start_offset % kAlign != 0) {
        std::ostringstream o;
        o << "start_offset " << start_offset
          << " of create_mappings_from_sparse must be aligned to " << kAlign << " bytes";
        return core::make_unexpected(o.str());
    }

    const core::Expected<uint64_t, std::string> fsz = file->Size();
    if (!fsz.ok()) return core::make_unexpected(fsz.error());
    const uint64_t file_size = fsz.value();

    std::vector<SegmentMapping> mappings;
    if (file_size <= start_offset) return mappings;

    uint64_t cursor = start_offset;
    for (;;) {
        const core::Expected<core::Optional<uint64_t>, std::string> d = file->SeekData(cursor);
        if (!d.ok()) return core::make_unexpected(d.error());
        // Absent means there is no data at or after the cursor: the scan is
        // finished, not failed.
        if (!d.value().has_value()) break;
        const uint64_t begin = *d.value();
        if (begin >= file_size) break;

        const core::Expected<core::Optional<uint64_t>, std::string> h = file->SeekHole(begin);
        if (!h.ok()) return core::make_unexpected(h.error());
        // No hole after this data means the data runs to the end of the file.
        uint64_t end = h.value().has_value() ? *h.value() : file_size;
        if (end > file_size) end = file_size;
        if (end <= begin) break;

        // An extent that starts before the region of interest: skip past it
        // without recording anything. This is the header block, which is data
        // but is not part of the virtual address space.
        if (begin < start_offset) {
            cursor = end > start_offset ? end : start_offset;
            continue;
        }

        if (begin % kAlign != 0 || end % kAlign != 0) {
            // Refused rather than truncated. The block arithmetic below
            // divides, and `end` is already clamped, so an unaligned boundary
            // would silently drop the trailing partial block -- which then
            // reads back as zeros with nothing raised anywhere. Both
            // platforms report boundaries at filesystem-block granularity, so
            // in practice this only fires on a source whose *size* is not a
            // multiple of the alignment.
            std::ostringstream o;
            o << "unaligned data extent [" << begin << ", " << end
              << ") reported for a file scanned by create_mappings_from_sparse: extent "
                 "boundaries must be multiples of "
              << kAlign << " bytes";
            return core::make_unexpected(o.str());
        }

        uint64_t logical = (begin - start_offset) / kAlign;
        uint64_t physical = begin / kAlign;
        uint64_t blocks = (end - begin) / kAlign;
        while (blocks > 0) {
            // Split at the segment length limit: a mapping's length is 14
            // bits, so one extent can need several mappings.
            const uint64_t step =
                blocks < static_cast<uint64_t>(Segment::kMaxLength)
                    ? blocks
                    : static_cast<uint64_t>(Segment::kMaxLength);
            mappings.push_back(SegmentMapping(logical, static_cast<uint32_t>(step), physical,
                                              false, 0));
            logical += step;
            physical += step;
            blocks -= step;
        }
        cursor = end;
    }
    return mappings;
}

// ---------------------------------------------------------------------------
// header helpers
// ---------------------------------------------------------------------------

HeaderTrailer BuildRwHeader(uint64_t virtual_size, RwLayout layout, const std::string& uuid,
                             const std::string& parent_uuid, const uint8_t* user_tag,
                             size_t user_tag_len) {
    HeaderTrailer h = HeaderTrailer::New();
    h.SetHeader();
    h.SetDataFile();
    h.virtual_size = virtual_size;
    // Deliberately *not* sealed: this is a writable layer, and a sealed flag
    // is what tells a reader the index offset in the trailer is trustworthy.
    switch (layout) {
        case kRwSparse:
            h.SetSparseRw();
            break;
        case kRwHybridLogStructured:
            h.SetHybridRw();
            break;
        case kRwLogStructured:
        default:
            break;
    }
    for (size_t i = 0; i < uuid.size() && i + 1 < sizeof(h.uuid); ++i) {
        h.uuid[i] = static_cast<uint8_t>(uuid[i]);
    }
    for (size_t i = 0; i < parent_uuid.size() && i + 1 < sizeof(h.parent_uuid); ++i) {
        h.parent_uuid[i] = static_cast<uint8_t>(parent_uuid[i]);
    }
    if (user_tag != 0) {
        const size_t n = user_tag_len < sizeof(h.user_tag) ? user_tag_len : sizeof(h.user_tag);
        for (size_t i = 0; i < n; ++i) h.user_tag[i] = user_tag[i];
    }
    return h;
}

core::Expected<core::Unit, std::string> WriteHeaderBlock(VirtualFile* file,
                                                           const HeaderTrailer& ht) {
    // The whole reserved region, not just the 390-byte struct: the remainder
    // has to be deterministic, or a freshly created layer would carry
    // whatever the filesystem left in those bytes into its digest.
    std::vector<uint8_t> block(static_cast<size_t>(kHdr), 0);
    uint8_t raw[HeaderTrailer::kOnDiskSize];
    ht.Serialize(raw);
    std::memcpy(&block[0], raw, sizeof(raw));

    const core::Expected<size_t, std::string> w = file->WriteAt(0, &block[0], block.size());
    if (!w.ok()) return core::make_unexpected(w.error());
    if (w.value() != block.size()) {
        return core::make_unexpected(std::string("short write for lsmt header block"));
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> UpdateHeaderVsize(VirtualFile* file, uint64_t vsize) {
    // Read-modify-write of the existing header, rather than rebuilding it.
    // The uuid, parent uuid, user tag and layout flags all live here, and a
    // rebuild would need every one of them passed back in -- any omission
    // would silently reset a field.
    const core::Expected<ByteBuffer, std::string> raw =
        file->ReadAt(0, static_cast<size_t>(kHdr));
    if (!raw.ok()) return core::make_unexpected(raw.error());
    if (raw.value().size() < HeaderTrailer::kOnDiskSize) {
        return core::make_unexpected(std::string("failed to read header while updating vsize"));
    }

    HeaderTrailer ht;
    if (!HeaderTrailer::Deserialize(&raw.value()[0], raw.value().size(), &ht)) {
        return core::make_unexpected(std::string("invalid header while updating vsize"));
    }
    ht.virtual_size = vsize;
    return WriteHeaderBlock(file, ht);
}


// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

LsmtFile::LsmtFile()
    : virtual_size_(0), rw_tag_(0), sealed_(false), max_io_size_(kLsmtMaxIoSize),
      file_type_(kLsmtFileWritable), group_commit_size_(0), rw_data_append_offset_(0),
      rw_index_append_offset_(0), has_append_digest_(false) {}

core::Expected<RwLayout, std::string> LsmtFile::RwLayoutFromHeader(const HeaderTrailer& h) {
    const bool sparse = h.IsSparseRw();
    const bool hybrid = h.IsHybridRw();
    if (sparse && hybrid) {
        // Refused rather than resolved by precedence. The two layouts put
        // data in different places, so picking one would read the layer with
        // the wrong addressing scheme and return plausible garbage.
        return core::make_unexpected(
            std::string("invalid RW header: sparse and hybrid flags are both set"));
    }
    if (sparse) return kRwSparse;
    if (hybrid) return kRwHybridLogStructured;
    return kRwLogStructured;
}

core::Expected<std::shared_ptr<LsmtFile>, std::string> LsmtFile::Open(
    std::shared_ptr<VirtualFile> rw_data_file, std::shared_ptr<VirtualFile> rw_index_file,
    std::shared_ptr<ReadOnlyIndex> lower_index,
    const std::vector<std::shared_ptr<VirtualFile> >& lower_layers) {
    if (!rw_data_file) return core::make_unexpected(std::string("lsmt rw open: null data file"));

    const core::Expected<uint64_t, std::string> fsz = rw_data_file->Size();
    if (!fsz.ok()) return core::make_unexpected(fsz.error());
    const uint64_t data_file_size = fsz.value();

    const core::Expected<HeaderTrailer, std::string> header =
        VerifyHt(rw_data_file.get(), false, data_file_size);
    if (!header.ok()) return core::make_unexpected(header.error());

    const core::Expected<RwLayout, std::string> raw_layout = RwLayoutFromHeader(header.value());
    if (!raw_layout.ok()) return core::make_unexpected(raw_layout.error());
    const core::Expected<RwLayout, std::string> layout =
        EnsureRwLayoutSupported(raw_layout.value());
    if (!layout.ok()) return core::make_unexpected(layout.error());

    // A file large enough to hold a trailer, whose trailer validates, is
    // sealed. Appending to it would overwrite the index its trailer points
    // at, leaving a file that still claims to be sealed and valid.
    if (data_file_size >= kHdr + 4096 &&
        VerifyHt(rw_data_file.get(), true, data_file_size).ok()) {
        return core::make_unexpected(std::string("Cannot open a Sealed LSMT file in RW mode."));
    }

    std::shared_ptr<LsmtFile> f(new LsmtFile());
    f->rw_tag_ = lower_layers.size();
    f->layers_ = lower_layers;
    f->layers_.push_back(rw_data_file);
    // One slot per lower, left blank: the lowers' identities are not read
    // here, and inventing them would be worse than reporting them absent.
    f->uuids_.assign(f->rw_tag_, std::string());
    {
        const char* raw = reinterpret_cast<const char*>(header.value().uuid);
        f->uuids_.push_back(std::string(raw, ::strnlen(raw, sizeof(header.value().uuid))));
    }

    MutableIndex mutable_index;
    uint64_t rw_index_append_offset = kHdr;

    if (layout.value() == kRwSparse) {
        // Redundant with `EnsureRwLayoutSupported` above, and kept anyway:
        // the hazard is not "this layout is sparse", it is "an extent map is
        // about to tell us which blocks this layer owns". Guarding the point
        // where that trust is placed keeps the check attached to its reason,
        // so a later refactor of the layout plumbing cannot quietly drop it.
        if (!sys::SparseExtentsAreReliable()) {
            return core::make_unexpected(std::string(
                "cannot recover a sparse upper's index on this platform: it does not "
                "guarantee that unwritten regions are reported as holes, so the extent "
                "map cannot tell us which blocks the upper owns, and over-reported "
                "blocks would mask the lower layers with zeros"));
        }
        const core::Expected<std::vector<SegmentMapping>, std::string> m =
            CreateMappingsFromSparse(rw_data_file.get(), kHdr);
        if (!m.ok()) return core::make_unexpected(m.error());
        // Tags left at zero. `ComboIndex` adds the lower count to every
        // upper mapping as it is constructed, so setting the tag here would
        // apply the offset twice and point at a layer past the end.
        for (size_t i = 0; i < m.value().size(); ++i) {
            mutable_index.Insert(m.value()[i]);
        }
    } else if (rw_index_file) {
        const core::Expected<uint64_t, std::string> isz = rw_index_file->Size();
        if (!isz.ok()) return core::make_unexpected(isz.error());
        rw_index_append_offset = isz.value();

        if (isz.value() > kHdr) {
            const uint64_t area = isz.value() - kHdr;
            if (area % static_cast<uint64_t>(kMappingSize) != 0) {
                // Refused rather than rounded down. A trailing partial record
                // means the process died mid-append, and reading the whole
                // records before it while ignoring the remnant would silently
                // accept an index whose last entry was never completed.
                std::ostringstream o;
                o << "Index file corrupted: size " << area << " is not aligned";
                return core::make_unexpected(o.str());
            }
            const uint64_t count = area / static_cast<uint64_t>(kMappingSize);
            std::vector<uint8_t> region(static_cast<size_t>(area), 0);
            const core::Expected<core::Unit, std::string> r =
                ReadExact(rw_index_file.get(), kHdr, &region[0], region.size());
            if (!r.ok()) return core::make_unexpected(r.error());

            const std::vector<SegmentMapping> loaded =
                LoadIndexAndResetTags(&region[0], region.size(), static_cast<size_t>(count));
            // Replayed in file order, so a later append overwrites an earlier
            // one at the same offset. That ordering *is* the log: the index
            // file is an append-only journal, and the last record for a block
            // is the current one.
            for (size_t i = 0; i < loaded.size(); ++i) {
                // The loader already reset every tag to zero, and that is
                // what has to go in: `ComboIndex` offsets them by the lower
                // count on construction.
                mutable_index.Insert(loaded[i]);
            }
        }
    } else {
        return core::make_unexpected(
            std::string("missing index file for non-sparse LSMT open"));
    }

    f->rw_data_file_ = rw_data_file;
    f->rw_index_file_ = rw_index_file;
    f->index_.reset(new ComboIndex(mutable_index, lower_index,
                                   static_cast<uint8_t>(f->rw_tag_)));
    f->virtual_size_ = header.value().virtual_size;
    f->file_type_ = RwLayoutFileType(layout.value());
    f->rw_data_append_offset_ = data_file_size;
    f->rw_index_append_offset_ = rw_index_append_offset;

    // Only an untouched append-only layer can be hashed from here: anything
    // already written happened before this process could observe it, so the
    // running digest would describe a prefix rather than the file.
    if (f->file_type_ == kLsmtFileWritable && data_file_size == kHdr) {
        const core::Expected<AppendDigestTracker, std::string> t =
            AppendDigestTracker::FromHeader(rw_data_file.get());
        if (!t.ok()) {
            std::ostringstream o;
            o << "initialize append digest tracker for rw data size " << data_file_size << ": "
              << t.error();
            return core::make_unexpected(o.str());
        }
        f->append_digest_ = t.value();
        f->has_append_digest_ = true;
    }
    return f;
}

core::Expected<std::shared_ptr<LsmtFile>, std::string> LsmtFile::CreateWithMetadata(
    std::shared_ptr<VirtualFile> rw_data_file, std::shared_ptr<VirtualFile> rw_index_file,
    uint64_t virtual_size, RwLayout layout, const std::string& uuid,
    const std::string& parent_uuid, const uint8_t* user_tag, size_t user_tag_len) {
    if (!rw_data_file) {
        return core::make_unexpected(std::string("lsmt rw create: null data file"));
    }
    const core::Expected<RwLayout, std::string> checked = EnsureRwLayoutSupported(layout);
    if (!checked.ok()) return core::make_unexpected(checked.error());

    const HeaderTrailer header =
        BuildRwHeader(virtual_size, layout, uuid, parent_uuid, user_tag, user_tag_len);
    const core::Expected<core::Unit, std::string> hw =
        WriteHeaderBlock(rw_data_file.get(), header);
    if (!hw.ok()) return core::make_unexpected(hw.error());

    if (layout == kRwSparse) {
        // Pre-sized to the whole virtual disk, because a sparse upper writes
        // at its logical offset and the extent map is what records which of
        // those offsets were used. Without the resize a write past the
        // current end would extend the file, and the recovered index would
        // then depend on write order rather than on which blocks were
        // written.
        const core::Expected<core::Unit, std::string> t =
            rw_data_file->Truncate(virtual_size + kHdr);
        if (!t.ok()) return core::make_unexpected(t.error());
    } else {
        if (!rw_index_file) {
            return core::make_unexpected(
                std::string("missing index file for non-sparse LSMT create"));
        }
        HeaderTrailer idx_header = header;
        idx_header.SetIndexFile();
        // Rust `set_unsealed_index_offset`: the index starts right after its
        // own header block. Left at zero it would point into the header, so a
        // reader recovering this upper would parse the header bytes as index
        // entries rather than finding the first real one.
        idx_header.index_offset = HeaderTrailer::kSpace;
        const core::Expected<core::Unit, std::string> iw =
            WriteHeaderBlock(rw_index_file.get(), idx_header);
        if (!iw.ok()) return core::make_unexpected(iw.error());
    }

    std::shared_ptr<LsmtFile> f(new LsmtFile());
    f->rw_data_file_ = rw_data_file;
    f->rw_index_file_ = rw_index_file;
    f->layers_.push_back(rw_data_file);
    f->rw_tag_ = 0;
    f->index_.reset(new ComboIndex(MutableIndex(), std::shared_ptr<ReadOnlyIndex>(), 0));
    f->virtual_size_ = virtual_size;
    f->uuids_.push_back(uuid);
    f->file_type_ = RwLayoutFileType(layout);
    f->rw_index_append_offset_ = kHdr;
    // Sparse starts at the physical end because it never appends; the other
    // two start right after the header, which is where their first append
    // goes.
    f->rw_data_append_offset_ = (layout == kRwSparse) ? virtual_size + kHdr : kHdr;

    if (f->file_type_ == kLsmtFileWritable) {
        const core::Expected<AppendDigestTracker, std::string> t =
            AppendDigestTracker::FromHeader(rw_data_file.get());
        if (!t.ok()) {
            return core::make_unexpected(
                std::string("initialize append digest tracker for created rw data: ") +
                t.error());
        }
        f->append_digest_ = t.value();
        f->has_append_digest_ = true;
    }
    return f;
}

core::Expected<std::shared_ptr<LsmtFile>, std::string> LsmtFile::Create(
    std::shared_ptr<VirtualFile> rw_data_file, std::shared_ptr<VirtualFile> rw_index_file,
    uint64_t virtual_size, bool sparse_rw) {
    // Hybrid is deliberately unreachable from here, matching Rust: it has to
    // be asked for explicitly, because a caller that only knows "sparse or
    // not" cannot have meant it.
    return CreateWithMetadata(rw_data_file, rw_index_file, virtual_size,
                              sparse_rw ? kRwSparse : kRwLogStructured, std::string(),
                              std::string(), 0, 0);
}

// ---------------------------------------------------------------------------
// accessors
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> LsmtFile::SetMaxIoSize(size_t size) {
    if (size == 0 || size % kLsmtAlignment4k != 0) {
        std::ostringstream o;
        o << "size " << size << " must be non-zero and aligned to 4K";
        return core::make_unexpected(o.str());
    }
    max_io_size_ = size;
    return core::Unit();
}

core::Expected<std::string, std::string> LsmtFile::GetUuid(size_t layer_idx) const {
    if (layer_idx >= uuids_.size()) {
        std::ostringstream o;
        o << "layer_idx " << layer_idx << " out of range";
        return core::make_unexpected(o.str());
    }
    return uuids_[layer_idx];
}

std::vector<std::shared_ptr<VirtualFile> > LsmtFile::LowerLayerFilesBottomToTop() const {
    // Reversed, because this layer holds them newest-first while
    // `OpenFilesRo` takes them oldest-first. Handing them over unreversed
    // would build a stack where the oldest layer shadows the newest.
    std::vector<std::shared_ptr<VirtualFile> > out;
    for (size_t i = rw_tag_; i-- > 0;) out.push_back(layers_[i]);
    return out;
}

core::Expected<uint64_t, std::string> LsmtFile::Size() {
    // The virtual size, not the physical one. For an append-only upper the
    // file is already larger than the address space it serves.
    return virtual_size_;
}


// ---------------------------------------------------------------------------
// append cursors
// ---------------------------------------------------------------------------
//
// Rust keeps these as atomics and uses a compare-exchange on every advance.
// The CAS is not there for concurrency -- appends are already serialized by
// the index write lock -- it is a tripwire for a future unsynchronized append
// path, and it reports an error rather than aborting. The equivalent here is
// the same expected-value check against the stored cursor.

core::Expected<uint64_t, std::string> LsmtFile::DataAppendOffset() const {
    if (rw_data_append_offset_ % kAlign != 0) {
        return core::make_unexpected(std::string("Underlying RW data file is not aligned"));
    }
    return rw_data_append_offset_;
}

core::Expected<core::Unit, std::string> LsmtFile::AdvanceDataAppendOffset(uint64_t start,
                                                                           size_t len) {
    if (start > (~static_cast<uint64_t>(0)) - static_cast<uint64_t>(len)) {
        return core::make_unexpected(std::string("rw data append offset overflow"));
    }
    if (rw_data_append_offset_ != start) {
        std::ostringstream o;
        o << "rw data append cursor race: expected " << start << ", got "
          << rw_data_append_offset_ << "; append serialization is broken";
        return core::make_unexpected(o.str());
    }
    rw_data_append_offset_ = start + static_cast<uint64_t>(len);
    return core::Unit();
}

core::Expected<uint64_t, std::string> LsmtFile::IndexAppendOffset() const {
    if (rw_index_append_offset_ % static_cast<uint64_t>(kMappingSize) != 0) {
        return core::make_unexpected(
            std::string("Underlying RW index file is not mapping-aligned"));
    }
    return rw_index_append_offset_;
}

core::Expected<core::Unit, std::string> LsmtFile::AdvanceIndexAppendOffset(uint64_t start,
                                                                            size_t len) {
    if (start > (~static_cast<uint64_t>(0)) - static_cast<uint64_t>(len)) {
        return core::make_unexpected(std::string("rw index append offset overflow"));
    }
    if (rw_index_append_offset_ != start) {
        std::ostringstream o;
        o << "rw index append cursor race: expected " << start << ", got "
          << rw_index_append_offset_ << "; append serialization is broken";
        return core::make_unexpected(o.str());
    }
    rw_index_append_offset_ = start + static_cast<uint64_t>(len);
    return core::Unit();
}

core::Expected<core::Unit, std::string> LsmtFile::AppendIndexBytes(const uint8_t* bytes,
                                                                     size_t len) {
    if (len == 0) return core::Unit();
    // Silently skipped when there is no index file, which is the sparse case:
    // its mappings live in the extent map, so there is nowhere to append and
    // nothing is lost.
    if (!rw_index_file_) return core::Unit();

    const core::Expected<uint64_t, std::string> at = IndexAppendOffset();
    if (!at.ok()) return core::make_unexpected(at.error());
    const core::Expected<size_t, std::string> w =
        rw_index_file_->WriteAt(at.value(), bytes, len);
    if (!w.ok()) return core::make_unexpected(w.error());
    if (w.value() != len) {
        return core::make_unexpected(std::string("short write appending lsmt index"));
    }
    return AdvanceIndexAppendOffset(at.value(), len);
}

core::Expected<core::Unit, std::string> LsmtFile::AppendIndexMapping(
    const DiskSegmentMapping& m) {
    uint8_t rec[16];
    EncodeMapping(m, rec);

    if (group_commit_size_ == 0) return AppendIndexBytes(rec, sizeof(rec));

    // Buffered. The capacity floor of one matters: a buffer size smaller than
    // a single record would otherwise compute a capacity of zero and never
    // flush, so every mapping would sit in memory until seal.
    size_t capacity = group_commit_size_ / kMappingSize;
    if (capacity == 0) capacity = 1;

    DiskSegmentMapping copy = m;
    group_commit_buf_.push_back(copy);
    if (group_commit_buf_.size() < capacity) return core::Unit();

    std::vector<uint8_t> bytes(group_commit_buf_.size() * kMappingSize, 0);
    for (size_t i = 0; i < group_commit_buf_.size(); ++i) {
        EncodeMapping(group_commit_buf_[i], &bytes[i * kMappingSize]);
    }
    const core::Expected<core::Unit, std::string> r = AppendIndexBytes(&bytes[0], bytes.size());
    // Cleared only on success. Dropping the buffer after a failed write would
    // lose the mappings outright; keeping it lets a later flush retry them.
    if (!r.ok()) return r;
    group_commit_buf_.clear();
    return core::Unit();
}

core::Expected<core::Unit, std::string> LsmtFile::FlushGroupCommit() {
    if (group_commit_buf_.empty()) return core::Unit();
    std::vector<uint8_t> bytes(group_commit_buf_.size() * kMappingSize, 0);
    for (size_t i = 0; i < group_commit_buf_.size(); ++i) {
        EncodeMapping(group_commit_buf_[i], &bytes[i * kMappingSize]);
    }
    const core::Expected<core::Unit, std::string> r = AppendIndexBytes(&bytes[0], bytes.size());
    if (!r.ok()) return r;
    group_commit_buf_.clear();
    return core::Unit();
}

// ---------------------------------------------------------------------------
// read
// ---------------------------------------------------------------------------
//
// Structurally identical to `LsmtReadOnlyFile`'s resolver, and deliberately
// so: a block is mapped, zeroed, or absent, and the only difference is that
// the combined index may resolve a block to the upper.

core::Expected<core::Unit, std::string> LsmtFile::ReadMappingsInto(
    const std::vector<SegmentMapping>& mappings, uint64_t begin_blk, uint64_t count_blk,
    size_t step, size_t buf_pos, uint8_t* buf) {
    uint64_t current_blk = begin_blk;
    const uint64_t end_blk = begin_blk + count_blk;

    for (size_t i = 0; i < mappings.size(); ++i) {
        const SegmentMapping& m = mappings[i];

        if (m.Offset() > current_blk) {
            const uint64_t hole_blks = m.Offset() - current_blk;
            const size_t hole_bytes = static_cast<size_t>(hole_blks * kAlign);
            const size_t local =
                buf_pos + static_cast<size_t>((current_blk - begin_blk) * kAlign);
            const size_t room = step - (local - buf_pos);
            const size_t fill = hole_bytes < room ? hole_bytes : room;
            if (fill > 0) std::memset(buf + local, 0, fill);
            current_blk = m.Offset();
        }

        if (current_blk >= end_blk) break;

        const size_t local = buf_pos + static_cast<size_t>((current_blk - begin_blk) * kAlign);
        if (local - buf_pos >= step) break;

        const size_t chunk = static_cast<size_t>(static_cast<uint64_t>(m.Length()) * kAlign);
        const size_t room = step - (local - buf_pos);
        const size_t copy_len = chunk < room ? chunk : room;

        if (m.zeroed) {
            std::memset(buf + local, 0, copy_len);
        } else {
            const size_t layer_idx = static_cast<size_t>(m.tag);
            if (layer_idx >= layers_.size()) {
                std::ostringstream o;
                o << "Invalid layer tag " << layer_idx;
                return core::make_unexpected(o.str());
            }
            const core::Expected<core::Unit, std::string> r =
                ReadExact(layers_[layer_idx].get(), m.moffset * kAlign, buf + local, copy_len);
            if (!r.ok()) return core::make_unexpected(r.error());
        }
        current_blk = m.End();
    }

    if (current_blk < end_blk) {
        const size_t local = buf_pos + static_cast<size_t>((current_blk - begin_blk) * kAlign);
        if (local - buf_pos < step) std::memset(buf + local, 0, buf_pos + step - local);
    }
    return core::Unit();
}

core::Expected<size_t, std::string> LsmtFile::ReadInternalInto(uint64_t offset, uint8_t* dst,
                                                                 size_t len) {
    const core::Expected<core::Unit, std::string> al = CheckAlignment(offset, len);
    if (!al.ok()) return core::make_unexpected(al.error());

    if (offset >= virtual_size_) return static_cast<size_t>(0);
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

        std::vector<SegmentMapping> mappings;
        index_->Lookup(Segment(begin_blk, static_cast<uint32_t>(count_blk)), &mappings);

        const core::Expected<core::Unit, std::string> r =
            ReadMappingsInto(mappings, begin_blk, count_blk, step, buf_pos, dst);
        if (!r.ok()) return core::make_unexpected(r.error());

        remaining -= step;
        curr_offset += static_cast<uint64_t>(step);
        buf_pos += step;
    }
    return read_len;
}

core::Expected<ByteBuffer, std::string> LsmtFile::ReadAt(uint64_t offset, size_t len) {
    if (len == 0 || offset >= virtual_size_) return ByteBuffer();
    const uint64_t room = virtual_size_ - offset;
    const size_t read_len = room < static_cast<uint64_t>(len) ? static_cast<size_t>(room) : len;
    ByteBuffer buf(read_len, 0);
    const core::Expected<size_t, std::string> n = ReadInternalInto(offset, &buf[0], read_len);
    if (!n.ok()) return core::make_unexpected(n.error());
    buf.resize(n.value());
    return buf;
}

core::Expected<size_t, std::string> LsmtFile::ReadAtInto(uint64_t offset, uint8_t* dst,
                                                           size_t len) {
    if (len == 0) return static_cast<size_t>(0);
    if (dst == 0) {
        return core::make_unexpected(std::string("lsmt rw read_at_into: null destination"));
    }
    return ReadInternalInto(offset, dst, len);
}


// ---------------------------------------------------------------------------
// hybrid write planning
// ---------------------------------------------------------------------------

core::Expected<std::vector<WriteFragment>, std::string> LsmtFile::PlanHybridWrite(
    const MutableIndex& upper, uint64_t logical_offset, size_t data_len) {
    const uint64_t start_blk = logical_offset / kAlign;
    const uint32_t block_count = static_cast<uint32_t>(data_len / static_cast<size_t>(kAlign));
    const uint64_t end_blk = start_blk + static_cast<uint64_t>(block_count);

    // Only the *upper's* own mappings. Planning against the combined index
    // would treat a lower layer's block as something this layer can overwrite
    // in place, which would corrupt a layer this file does not own.
    std::vector<SegmentMapping> upper_results;
    upper.Lookup(Segment(start_blk, block_count), &upper_results);

    std::vector<WriteFragment> fragments;
    uint64_t current_blk = start_blk;

    for (size_t i = 0; i < upper_results.size(); ++i) {
        const SegmentMapping& m = upper_results[i];

        // A gap in the upper has to be appended: this layer owns nothing here
        // yet, so there is no physical range to overwrite.
        if (m.Offset() > current_blk) {
            WriteFragment f;
            f.kind = WriteFragment::kAppend;
            f.logical_offset = current_blk * kAlign;
            f.data_offset = static_cast<size_t>((current_blk - start_blk) * kAlign);
            f.len = static_cast<size_t>((m.Offset() - current_blk) * kAlign);
            fragments.push_back(f);
            current_blk = m.Offset();
        }

        if (current_blk >= end_blk) break;

        const uint64_t fragment_end = m.End() < end_blk ? m.End() : end_blk;
        if (fragment_end <= current_blk) continue;

        const size_t data_offset = static_cast<size_t>((current_blk - start_blk) * kAlign);
        const size_t len = static_cast<size_t>((fragment_end - current_blk) * kAlign);

        if (m.zeroed && !m.HasPhysicalRange()) {
            // A zeroed mapping with no physical range owns no space, so there
            // is nothing to write into: this has to be an append.
            WriteFragment f;
            f.kind = WriteFragment::kAppend;
            f.logical_offset = current_blk * kAlign;
            f.data_offset = data_offset;
            f.len = len;
            fragments.push_back(f);
        } else {
            const uint64_t delta = current_blk - m.Offset();
            if (m.moffset > (~static_cast<uint64_t>(0)) - delta) {
                return core::make_unexpected(
                    std::string("hybrid write physical offset overflow"));
            }
            const uint64_t phys_offset = (m.moffset + delta) * kAlign;

            WriteFragment f;
            f.logical_offset = current_blk * kAlign;
            f.data_offset = data_offset;
            f.len = len;
            f.phys_offset = phys_offset;
            // A zeroed mapping that *does* own physical space: reuse it, but
            // publish a mapping too. Writing in place without one would leave
            // the range still recorded as zeroed, so it would read back as
            // zeros -- including after reopen, where the index is the only
            // record of what happened.
            f.kind = m.zeroed ? WriteFragment::kReuseZero : WriteFragment::kInPlace;
            fragments.push_back(f);
        }
        current_blk = fragment_end;
    }

    if (current_blk < end_blk) {
        WriteFragment f;
        f.kind = WriteFragment::kAppend;
        f.logical_offset = current_blk * kAlign;
        f.data_offset = static_cast<size_t>((current_blk - start_blk) * kAlign);
        f.len = static_cast<size_t>((end_blk - current_blk) * kAlign);
        fragments.push_back(f);
    }
    return fragments;
}

// ---------------------------------------------------------------------------
// write
// ---------------------------------------------------------------------------

core::Expected<size_t, std::string> LsmtFile::WriteInternal(uint64_t offset,
                                                              const uint8_t* data,
                                                              size_t len) {
    if (sealed_) return core::make_unexpected(std::string("File is sealed."));
    if (len == 0) return static_cast<size_t>(0);
    if (data == 0) return core::make_unexpected(std::string("lsmt rw write: null data"));

    const core::Expected<core::Unit, std::string> al = CheckAlignment(offset, len);
    if (!al.ok()) return core::make_unexpected(al.error());

    size_t done = 0;
    uint64_t current_offset = offset;

    while (done < len) {
        const size_t remaining = len - done;
        const size_t chunk_len = remaining < max_io_size_ ? remaining : max_io_size_;
        const uint8_t* chunk = data + done;

        if (file_type_ == kLsmtFileHybridWritable) {
            const core::Expected<std::vector<WriteFragment>, std::string> frags =
                PlanHybridWrite(index_->Upper(), current_offset, chunk_len);
            if (!frags.ok()) return core::make_unexpected(frags.error());

            std::vector<SegmentMapping> inserted;
            bool have_next_append = false;
            uint64_t next_append_offset = 0;

            for (size_t i = 0; i < frags.value().size(); ++i) {
                const WriteFragment& f = frags.value()[i];

                if (f.kind == WriteFragment::kInPlace) {
                    const core::Expected<size_t, std::string> w =
                        rw_data_file_->WriteAt(f.phys_offset, chunk + f.data_offset, f.len);
                    if (!w.ok()) return core::make_unexpected(w.error());
                    // No mapping published: one already points here, and it
                    // is unchanged.
                    continue;
                }

                uint64_t phys = f.phys_offset;
                if (f.kind == WriteFragment::kAppend) {
                    // Consecutive appends within one chunk reserve from a
                    // running cursor rather than re-reading the stored one,
                    // so two fragments cannot be handed the same offset.
                    if (have_next_append) {
                        phys = next_append_offset;
                    } else {
                        const core::Expected<uint64_t, std::string> at = DataAppendOffset();
                        if (!at.ok()) return core::make_unexpected(at.error());
                        phys = at.value();
                    }
                }

                const core::Expected<size_t, std::string> w =
                    rw_data_file_->WriteAt(phys, chunk + f.data_offset, f.len);
                if (!w.ok()) return core::make_unexpected(w.error());

                if (f.kind == WriteFragment::kAppend) {
                    const core::Expected<core::Unit, std::string> adv =
                        AdvanceDataAppendOffset(phys, f.len);
                    if (!adv.ok()) return core::make_unexpected(adv.error());
                    if (phys > (~static_cast<uint64_t>(0)) - static_cast<uint64_t>(f.len)) {
                        return core::make_unexpected(
                            std::string("hybrid append offset overflow"));
                    }
                    next_append_offset = phys + static_cast<uint64_t>(f.len);
                    have_next_append = true;
                }

                SegmentMapping m_mem(f.logical_offset / kAlign,
                                     static_cast<uint32_t>(f.len / static_cast<size_t>(kAlign)),
                                     phys / kAlign, false, static_cast<uint8_t>(rw_tag_));
                SegmentMapping m_disk = m_mem;
                // Tag zeroed on disk: the index file belongs to this layer
                // alone, so a tag recorded there would describe the stack
                // this layer currently sits in -- which is not the stack it
                // will be reopened in.
                m_disk.tag = 0;
                const core::Expected<core::Unit, std::string> ia =
                    AppendIndexMapping(DiskSegmentMapping::FromMemory(m_disk));
                if (!ia.ok()) return core::make_unexpected(ia.error());
                inserted.push_back(m_mem);
            }

            // Published after every fragment landed. Inserting as we went
            // would make a half-written chunk visible to a reader as fully
            // written.
            for (size_t i = 0; i < inserted.size(); ++i) index_->Insert(inserted[i]);

            done += chunk_len;
            current_offset += static_cast<uint64_t>(chunk_len);
            continue;
        }

        uint64_t phys_offset = 0;
        if (file_type_ == kLsmtFileSparseWritable) {
            // Written at its logical offset, past the header. There is no
            // cursor and no append: the extent map is what records that this
            // block now belongs to the layer.
            phys_offset = kHdr + current_offset;
            const core::Expected<size_t, std::string> w =
                rw_data_file_->WriteAt(phys_offset, chunk, chunk_len);
            if (!w.ok()) return core::make_unexpected(w.error());
        } else {
            const core::Expected<uint64_t, std::string> at = DataAppendOffset();
            if (!at.ok()) return core::make_unexpected(at.error());
            phys_offset = at.value();
            const core::Expected<size_t, std::string> w =
                rw_data_file_->WriteAt(phys_offset, chunk, chunk_len);
            if (!w.ok()) return core::make_unexpected(w.error());
            const core::Expected<core::Unit, std::string> adv =
                AdvanceDataAppendOffset(phys_offset, chunk_len);
            if (!adv.ok()) return core::make_unexpected(adv.error());

            if (has_append_digest_) {
                // Abandoned on failure rather than propagated. A digest that
                // lost track of the stream cannot be repaired, but the write
                // itself succeeded -- failing it would turn a missing
                // optional descriptor into lost data.
                if (!append_digest_.Absorb(phys_offset, chunk, chunk_len).ok()) {
                    has_append_digest_ = false;
                }
            }
        }

        SegmentMapping m_mem(current_offset / kAlign,
                             static_cast<uint32_t>(chunk_len / static_cast<size_t>(kAlign)),
                             phys_offset / kAlign, false, static_cast<uint8_t>(rw_tag_));
        index_->Insert(m_mem);

        if (file_type_ != kLsmtFileSparseWritable) {
            SegmentMapping m_disk = m_mem;
            m_disk.tag = 0;
            const core::Expected<core::Unit, std::string> ia =
                AppendIndexMapping(DiskSegmentMapping::FromMemory(m_disk));
            if (!ia.ok()) return core::make_unexpected(ia.error());
        }

        done += chunk_len;
        current_offset += static_cast<uint64_t>(chunk_len);
    }
    return done;
}

core::Expected<size_t, std::string> LsmtFile::WriteAt(uint64_t offset, const uint8_t* data,
                                                        size_t len) {
    return WriteInternal(offset, data, len);
}

// ---------------------------------------------------------------------------
// discard
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> LsmtFile::DiscardHybridRange(uint64_t offset,
                                                                       uint64_t len) {
    const Segment segment(offset / kAlign, static_cast<uint32_t>(len / kAlign));
    std::vector<SegmentMapping> mappings;
    index_->Upper().Lookup(segment, &mappings);

    // Each existing mapping keeps its physical range and is merely marked
    // zeroed, so a later write to the same block can reuse the space instead
    // of appending. Only gaps in the upper need a fresh marker.
    std::vector<SegmentMapping> zeros;
    uint64_t current = segment.offset;
    for (size_t i = 0; i < mappings.size(); ++i) {
        SegmentMapping m = mappings[i];
        if (current < m.Offset()) {
            zeros.push_back(SegmentMapping(current,
                                           static_cast<uint32_t>(m.Offset() - current),
                                           kNoPhysicalOffset, true,
                                           static_cast<uint8_t>(rw_tag_)));
        }
        current = m.End();
        m.zeroed = true;
        zeros.push_back(m);
    }
    if (current < segment.End()) {
        zeros.push_back(SegmentMapping(current,
                                       static_cast<uint32_t>(segment.End() - current),
                                       kNoPhysicalOffset, true,
                                       static_cast<uint8_t>(rw_tag_)));
    }

    for (size_t i = 0; i < zeros.size(); ++i) {
        SegmentMapping disk = zeros[i];
        disk.tag = 0;
        const core::Expected<core::Unit, std::string> ia =
            AppendIndexMapping(DiskSegmentMapping::FromMemory(disk));
        if (!ia.ok()) return core::make_unexpected(ia.error());
        index_->Insert(zeros[i]);
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> LsmtFile::DiscardRange(uint64_t offset, uint64_t len) {
    if (sealed_) return core::make_unexpected(std::string("File is sealed."));
    if (len == 0) return core::Unit();
    if (offset % kAlign != 0 || len % kAlign != 0) {
        std::ostringstream o;
        o << "discard must be aligned to " << kAlign << " bytes (offset: " << offset
          << ", len: " << len << ")";
        return core::make_unexpected(o.str());
    }

    // Chunked by the segment length limit, because one mapping cannot span
    // more blocks than its 14-bit length field holds.
    const uint64_t max_discard = static_cast<uint64_t>(Segment::kMaxLength) * kAlign;
    uint64_t remaining = len;
    uint64_t current_offset = offset;

    while (remaining > 0) {
        const uint64_t step = remaining < max_discard ? remaining : max_discard;

        if (file_type_ == kLsmtFileHybridWritable) {
            const core::Expected<core::Unit, std::string> r =
                DiscardHybridRange(current_offset, step);
            if (!r.ok()) return r;
            current_offset += step;
            remaining -= step;
            continue;
        }

        if (file_type_ == kLsmtFileSparseWritable) {
            // Punched for real, because for sparse the extent map *is* the
            // index: leaving the blocks allocated would have them recovered
            // as owned-and-nonzero on the next open.
            const core::Expected<core::Unit, std::string> d =
                rw_data_file_->Discard(kHdr + current_offset, step);
            if (!d.ok()) return core::make_unexpected(d.error());
        }

        // No physical range is retained on this path: a log-backed layout
        // appends only an index entry, and sparse has just punched the hole.
        const SegmentMapping m_mem(current_offset / kAlign,
                                   static_cast<uint32_t>(step / kAlign), kNoPhysicalOffset,
                                   true, static_cast<uint8_t>(rw_tag_));
        index_->Insert(m_mem);

        if (file_type_ != kLsmtFileSparseWritable) {
            SegmentMapping m_disk = m_mem;
            m_disk.tag = 0;
            const core::Expected<core::Unit, std::string> ia =
                AppendIndexMapping(DiskSegmentMapping::FromMemory(m_disk));
            if (!ia.ok()) return core::make_unexpected(ia.error());
        }

        current_offset += step;
        remaining -= step;
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> LsmtFile::Discard(uint64_t offset, uint64_t len) {
    return DiscardRange(offset, len);
}


// ---------------------------------------------------------------------------
// resize and introspection
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> LsmtFile::UpdateVsize(uint64_t vsize) {
    // The running digest covers the header, and the header is about to
    // change. Rust drops the tracker and rebuilds it by re-reading the file
    // -- but only when it was still in sync with the file end; a tracker that
    // had already fallen behind has nothing to rebuild from.
    const uint64_t size_before = rw_data_append_offset_;
    const bool rebuild = has_append_digest_ && append_digest_.Offset() == size_before &&
                         size_before >= kHdr;
    has_append_digest_ = false;

    virtual_size_ = vsize;
    const core::Expected<core::Unit, std::string> hu =
        UpdateHeaderVsize(rw_data_file_.get(), vsize);
    if (!hu.ok()) return hu;

    if (rebuild) {
        const core::Expected<AppendDigestTracker, std::string> seeded =
            AppendDigestTracker::FromHeader(rw_data_file_.get());
        if (!seeded.ok()) return core::make_unexpected(seeded.error());
        AppendDigestTracker t = seeded.value();

        std::vector<uint8_t> buffer(128 * 1024, 0);
        uint64_t at = kHdr;
        bool ok = true;
        while (at < size_before && ok) {
            const uint64_t left = size_before - at;
            const size_t want = left < static_cast<uint64_t>(buffer.size())
                                    ? static_cast<size_t>(left)
                                    : buffer.size();
            const core::Expected<size_t, std::string> read =
                rw_data_file_->ReadAtInto(at, &buffer[0], want);
            if (!read.ok()) return core::make_unexpected(read.error());
            if (read.value() != want) {
                std::ostringstream o;
                o << "short read while rebuilding append digest tracker: expected " << want
                  << ", got " << read.value();
                return core::make_unexpected(o.str());
            }
            if (!t.Absorb(at, &buffer[0], read.value()).ok()) ok = false;
            at += static_cast<uint64_t>(read.value());
        }
        // Reinstated only if the file end has not moved underneath the
        // rebuild. Otherwise the rebuilt digest describes a prefix, and
        // publishing it would report a digest for bytes that are no longer
        // the whole layer.
        if (ok && rw_data_append_offset_ == t.Offset()) {
            append_digest_ = t;
            has_append_digest_ = true;
        }
    }

    if (file_type_ == kLsmtFileSparseWritable) {
        // Resized to match, because for sparse the file length *is* the
        // address space. The cursor follows, since it names the physical end.
        const core::Expected<core::Unit, std::string> t =
            rw_data_file_->Truncate(vsize + kHdr);
        if (!t.ok()) return core::make_unexpected(t.error());
        rw_data_append_offset_ = vsize + kHdr;
    }

    if (rw_index_file_) {
        // The index file carries the same virtual size, so a reopen that only
        // looks at the index does not disagree with the data file.
        const core::Expected<core::Unit, std::string> iu =
            UpdateHeaderVsize(rw_index_file_.get(), vsize);
        if (!iu.ok()) return iu;
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> LsmtFile::Truncate(uint64_t size) {
    return UpdateVsize(size);
}

core::Expected<core::Unit, std::string> LsmtFile::Sync() {
    const core::Expected<core::Unit, std::string> f = FlushGroupCommit();
    if (!f.ok()) return f;
    // The index file first. A data block whose mapping has not reached disk
    // reads as a hole on reopen, which is recoverable; a mapping pointing at
    // data that has not reached disk reads as whatever was there, which is
    // not.
    if (rw_index_file_) {
        const core::Expected<core::Unit, std::string> i = rw_index_file_->Sync();
        if (!i.ok()) return i;
    }
    return rw_data_file_->Sync();
}

core::Expected<DataStat, std::string> LsmtFile::Stat() const {
    const core::Expected<uint64_t, std::string> fsz = rw_data_file_->Size();
    if (!fsz.ok()) return core::make_unexpected(fsz.error());
    // Saturating, because a file smaller than its own header is not something
    // to compute a negative size from.
    const uint64_t total = fsz.value() > kHdr ? fsz.value() - kHdr : 0;

    const uint64_t blocks = (virtual_size_ + kAlign - 1) / kAlign;
    std::vector<SegmentMapping> mappings;
    index_->Lookup(Segment(0, static_cast<uint32_t>(blocks)), &mappings);

    uint64_t valid_blocks = 0;
    for (size_t i = 0; i < mappings.size(); ++i) {
        if (!mappings[i].zeroed) valid_blocks += mappings[i].Length();
    }

    DataStat st;
    // `total` is the physical bytes this layer occupies, which for an
    // append-only upper exceeds the valid bytes: overwritten blocks are still
    // on disk, just unreferenced. The gap between the two is what a compact
    // would reclaim.
    st.total_data_size = total;
    st.valid_data_size = valid_blocks * kAlign;
    return st;
}

core::Expected<size_t, std::string> LsmtFile::SeekDataSegments(
    uint64_t begin, uint64_t end, std::vector<Segment>* segs) const {
    if (segs == 0) return core::make_unexpected(std::string("seek_data: null output"));
    if (end <= begin) return static_cast<size_t>(0);

    const uint64_t begin_blk = begin / kAlign;
    const uint64_t end_blk = (end + kAlign - 1) / kAlign;
    if (end_blk <= begin_blk) return static_cast<size_t>(0);

    std::vector<SegmentMapping> mappings;
    index_->Lookup(Segment(begin_blk, static_cast<uint32_t>(end_blk - begin_blk)), &mappings);
    for (size_t i = 0; i < mappings.size(); ++i) {
        if (mappings[i].zeroed) continue;
        segs->push_back(Segment(mappings[i].Offset(), mappings[i].Length()));
    }
    return segs->size();
}

// ---------------------------------------------------------------------------
// seal
// ---------------------------------------------------------------------------

void LsmtFile::AbsorbSealBytes(uint64_t offset, const uint8_t* data, size_t len) {
    if (!has_append_digest_) return;
    if (!append_digest_.Absorb(offset, data, len).ok()) has_append_digest_ = false;
}

core::Expected<core::Optional<LayerDescriptor>, std::string> LsmtFile::CloseSeal() {
    // Flipped first and checked atomically in Rust via `swap`. Sealing twice
    // would append a second index and trailer after the first, leaving a file
    // whose trailer points at an index that is no longer last.
    if (sealed_) {
        return core::make_unexpected(std::string("File has already been sealed."));
    }
    sealed_ = true;

    const core::Expected<core::Unit, std::string> fl = FlushGroupCommit();
    if (!fl.ok()) return core::make_unexpected(fl.error());

    const uint64_t blocks = (virtual_size_ + kAlign - 1) / kAlign;
    std::vector<SegmentMapping> compact_index;
    // Only this layer's mappings. The sealed file is a single layer, so a
    // mapping inherited from a lower would point into a file this one does
    // not contain.
    index_->Upper().Lookup(Segment(0, static_cast<uint32_t>(blocks)), &compact_index);

    for (size_t i = 0; i < compact_index.size(); ++i) {
        compact_index[i].tag = 0;
        if (compact_index[i].zeroed) {
            // A retained physical range is meaningless once sealed: the
            // sealed layer has no writer left to reuse it, and keeping the
            // offset would describe space the compacted file does not have.
            compact_index[i].moffset = kNoPhysicalOffset;
        }
    }

    const core::Expected<uint64_t, std::string> fsz = rw_data_file_->Size();
    if (!fsz.ok()) return core::make_unexpected(fsz.error());
    // At the current data EOF. For sparse that is exactly header + virtual
    // size, so index bytes cannot land on top of virtual data.
    const uint64_t index_offset = fsz.value();

    std::vector<uint8_t> index_bytes(compact_index.size() * kMappingSize, 0);
    for (size_t i = 0; i < compact_index.size(); ++i) {
        EncodeMapping(DiskSegmentMapping::FromMemory(compact_index[i]),
                      &index_bytes[i * kMappingSize]);
    }

    const size_t remainder = index_bytes.size() % static_cast<size_t>(kAlign);
    if (remainder != 0) {
        // Padded with 0xFF, not zeros: a zero-filled tail would decode as a
        // valid mapping of block zero, and the reader counts records rather
        // than scanning for a terminator.
        index_bytes.resize(index_bytes.size() + (static_cast<size_t>(kAlign) - remainder), 0xFF);
    }

    if (!index_bytes.empty()) {
        const core::Expected<size_t, std::string> w =
            rw_data_file_->WriteAt(index_offset, &index_bytes[0], index_bytes.size());
        if (!w.ok()) return core::make_unexpected(w.error());
        AbsorbSealBytes(index_offset, &index_bytes[0], index_bytes.size());
    }

    uint64_t trailer_offset = index_offset + static_cast<uint64_t>(index_bytes.size());
    if (trailer_offset % 4096 != 0) {
        // The trailer has to start on a 4096 boundary, because a reader finds
        // it by subtracting that block size from the file length.
        const uint64_t pad_len = 4096 - (trailer_offset % 4096);
        std::vector<uint8_t> pad(static_cast<size_t>(pad_len), 0);
        const core::Expected<size_t, std::string> w =
            rw_data_file_->WriteAt(trailer_offset, &pad[0], pad.size());
        if (!w.ok()) return core::make_unexpected(w.error());
        AbsorbSealBytes(trailer_offset, &pad[0], pad.size());
        trailer_offset += pad_len;
    }

    // Built from the existing header rather than from scratch, so the uuid,
    // parent uuid and user tag carry into the trailer.
    const core::Expected<ByteBuffer, std::string> raw =
        rw_data_file_->ReadAt(0, static_cast<size_t>(kHdr));
    if (!raw.ok()) return core::make_unexpected(raw.error());
    if (raw.value().size() < HeaderTrailer::kOnDiskSize) {
        return core::make_unexpected(
            std::string("failed to read rw data header when sealing"));
    }
    HeaderTrailer trailer;
    if (!HeaderTrailer::Deserialize(&raw.value()[0], raw.value().size(), &trailer)) {
        return core::make_unexpected(std::string("invalid rw data header"));
    }
    trailer.virtual_size = virtual_size_;
    trailer.index_offset = index_offset;
    trailer.index_size = compact_index.size();
    trailer.SetSealed();
    trailer.SetDataFile();
    trailer.SetTrailer();
    // The sparse and hybrid markers describe how a *writable* layer stores
    // its data. A sealed layer has one shape, so carrying them over would
    // make a reader take a recovery path that no longer applies.
    trailer.flags &= ~(1u << 4);
    trailer.flags &= ~(1u << 5);

    std::vector<uint8_t> trailer_buf(4096, 0);
    uint8_t enc[HeaderTrailer::kOnDiskSize];
    trailer.Serialize(enc);
    std::memcpy(&trailer_buf[0], enc, sizeof(enc));

    const core::Expected<size_t, std::string> tw =
        rw_data_file_->WriteAt(trailer_offset, &trailer_buf[0], trailer_buf.size());
    if (!tw.ok()) return core::make_unexpected(tw.error());
    AbsorbSealBytes(trailer_offset, &trailer_buf[0], trailer_buf.size());

    if (!has_append_digest_) {
        // Absent, not an error. Only the append-only layout can carry a
        // digest at all, and even there any out-of-order write abandons it.
        return core::Optional<LayerDescriptor>();
    }
    return core::Optional<LayerDescriptor>(append_digest_.Descriptor());
}


// ---------------------------------------------------------------------------
// initialize_file_rw_paths
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> InitializeFileRwPaths(
    const std::string& data_path, const core::Optional<std::string>& index_path,
    uint64_t virtual_size, RwLayout layout) {
    const core::Expected<RwLayout, std::string> checked = EnsureRwLayoutSupported(layout);
    if (!checked.ok()) return core::make_unexpected(checked.error());

    // A fresh identity per upper. This is what later distinguishes this layer
    // from any other, so minting it here rather than taking it from the caller
    // keeps two initializations from ever colliding.
    const std::string uuid = core::Uuid::GenV7().ToString();

    const HeaderTrailer header =
        BuildRwHeader(virtual_size, layout, uuid, std::string(), 0, 0);

    // Truncating open, matching Rust's `open_truncated_rw_file`: an
    // initialization starts from nothing, and reusing whatever was at the path
    // would leave stale blocks behind the new header.
    core::Expected<std::shared_ptr<LocalFile>, std::string> data =
        LocalFile::New(data_path);
    if (!data.ok()) {
        return core::make_unexpected(std::string("create overlaybd upper file ") + data_path +
                                     ": " + data.error());
    }
    const core::Expected<core::Unit, std::string> hw =
        WriteHeaderBlock(data.value().get(), header);
    if (!hw.ok()) {
        return core::make_unexpected(std::string("initialize overlaybd upper data ") +
                                     data_path + ": " + hw.error());
    }

    if (layout == kRwSparse) {
        // Extended to cover the whole virtual disk. A sparse upper writes at
        // its logical offset with no index to redirect it, so the range has to
        // be addressable up front -- and because the extension is a hole it
        // costs nothing until something writes into it.
        if (virtual_size > ~static_cast<uint64_t>(0) - kHdr) {
            return core::make_unexpected(std::string("sparse overlaybd upper size overflow"));
        }
        const core::Expected<core::Unit, std::string> t =
            data.value()->Truncate(virtual_size + kHdr);
        if (!t.ok()) {
            return core::make_unexpected(std::string("truncate sparse overlaybd upper ") +
                                         data_path + ": " + t.error());
        }
        return core::Unit();
    }

    // Log-structured and hybrid both keep their index in a second file.
    // Refused when absent rather than silently skipped: such an upper records
    // where each block went in the index, so without one nothing it writes
    // could be found again.
    if (!index_path.has_value()) {
        return core::make_unexpected(std::string(
            "log-structured overlaybd upper requires an index path during initialization"));
    }
    core::Expected<std::shared_ptr<LocalFile>, std::string> index =
        LocalFile::New(*index_path);
    if (!index.ok()) {
        return core::make_unexpected(std::string("create overlaybd upper file ") + *index_path +
                                     ": " + index.error());
    }

    HeaderTrailer index_header = header;
    index_header.SetIndexFile();
    // The index begins immediately after its own header block. Zero would
    // point back into the header, and a reader recovering this upper would
    // then read header bytes as index entries.
    index_header.index_offset = HeaderTrailer::kSpace;
    const core::Expected<core::Unit, std::string> iw =
        WriteHeaderBlock(index.value().get(), index_header);
    if (!iw.ok()) {
        return core::make_unexpected(std::string("initialize overlaybd upper index ") +
                                     *index_path + ": " + iw.error());
    }
    return core::Unit();
}


// ---------------------------------------------------------------------------
// Rust: lsmt/file/stack.rs
// ---------------------------------------------------------------------------

core::Expected<std::shared_ptr<LsmtFile>, std::string> CreateFileRw(
    std::shared_ptr<VirtualFile> fdata, std::shared_ptr<VirtualFile> findex,
    uint64_t virtual_size, RwLayout layout, const std::string& uuid,
    const std::string& parent_uuid, const uint8_t* user_tag, size_t user_tag_len) {
    return LsmtFile::CreateWithMetadata(fdata, findex, virtual_size, layout, uuid,
                                        parent_uuid, user_tag, user_tag_len);
}

LayerInfo LayerInfo::New(const std::shared_ptr<VirtualFile>& fdata,
                         const std::shared_ptr<VirtualFile>& findex, uint64_t virtual_size) {
    LayerInfo info;
    info.fdata = fdata;
    info.findex = findex;
    info.virtual_size = virtual_size;
    // v4, as Rust's `Uuid::new_v4()` is -- not v7. A layer uuid is published
    // inside an image, so it must not carry the creation time that v7 encodes.
    info.uuid = core::Uuid::GenV4().ToString();
    return info;
}

core::Expected<std::shared_ptr<LsmtFile>, std::string> CreateFileRw(const LayerInfo& info) {
    return CreateFileRw(info.fdata, info.findex, info.virtual_size, info.rw_layout, info.uuid,
                        info.parent_uuid,
                        info.user_tag.empty() ? 0 : &info.user_tag[0], info.user_tag.size());
}

core::Expected<std::shared_ptr<LsmtFile>, std::string> OpenFileRw(
    std::shared_ptr<VirtualFile> fdata, std::shared_ptr<VirtualFile> findex) {
    // No lower index and no lower layers: every mapping this upper holds
    // resolves into the upper itself.
    return LsmtFile::Open(fdata, findex, std::shared_ptr<ReadOnlyIndex>(),
                          std::vector<std::shared_ptr<VirtualFile> >());
}

core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> OpenFileRo(
    std::shared_ptr<VirtualFile> file) {
    return LsmtReadOnlyFile::Open(file);
}

core::Expected<core::Unit, std::string> VerifyLayerOrder(
    const std::vector<std::shared_ptr<VirtualFile> >& layers,
    const std::vector<std::string>& uuids, size_t start_layer) {
    if (layers.size() != uuids.size()) {
        return core::make_unexpected(std::string("layers and uuids size mismatch"));
    }
    // Nothing to check past the end. This is what lets a caller skip the
    // newest layer with `start_layer = 1` even on a one-layer stack.
    if (start_layer >= layers.size()) return core::Unit();

    bool have_parent = false;
    std::string parent_uuid;
    for (size_t i = start_layer; i < layers.size(); ++i) {
        if (have_parent && uuids[i] != parent_uuid) {
            // Refused rather than repaired. A stack whose layers do not name
            // each other was assembled from unrelated images, and reading it
            // would resolve blocks against the wrong base.
            std::ostringstream o;
            o << "parent uuid mismatch at layer " << i << ": got " << uuids[i]
              << ", expected " << parent_uuid;
            return core::make_unexpected(o.str());
        }

        // The last layer's header is not read: nothing sits above it whose
        // parent it would have to be.
        if (i + 1 < layers.size()) {
            if (!layers[i]) {
                std::ostringstream o;
                o << "verify layer order: null layer " << i;
                return core::make_unexpected(o.str());
            }
            const core::Expected<uint64_t, std::string> size = layers[i]->Size();
            if (!size.ok()) return core::make_unexpected(size.error());
            const core::Expected<HeaderTrailer, std::string> ht =
                VerifyHt(layers[i].get(), false, size.value());
            if (!ht.ok()) return core::make_unexpected(ht.error());

            // Rust `parse_uuid_field`: the field is a NUL-terminated string
            // inside a fixed 37-byte slot, and an empty one means "no parent"
            // rather than the empty uuid.
            const char* raw = reinterpret_cast<const char*>(ht.value().parent_uuid);
            const size_t len = ::strnlen(raw, sizeof(ht.value().parent_uuid));
            if (len == 0) {
                have_parent = false;
                parent_uuid.clear();
            } else {
                have_parent = true;
                parent_uuid.assign(raw, len);
            }
        }
    }
    return core::Unit();
}

core::Expected<std::shared_ptr<LsmtFile>, std::string> StackFiles(
    const std::shared_ptr<LsmtFile>& upper,
    const std::shared_ptr<LsmtReadOnlyFile>& lowers, bool check_order) {
    if (!upper) return core::make_unexpected(std::string("stack files: null upper"));
    if (!lowers) return core::make_unexpected(std::string("stack files: null lowers"));

    if (check_order) {
        // From layer 1: layer 0 is the newest lower, and its parent is the
        // upper, which is not part of this vector.
        const core::Expected<core::Unit, std::string> ordered =
            VerifyLayerOrder(lowers->Layers(), lowers->Uuids(), 1);
        if (!ordered.ok()) return core::make_unexpected(ordered.error());
    }

    const std::shared_ptr<ReadOnlyIndex> lower_index = lowers->IndexView();
    if (!lower_index) {
        return core::make_unexpected(std::string("missing lower readonly index"));
    }

    // The upper's own two handles are reused rather than reopened: this is the
    // same upper, and a second handle would carry its own append offset.
    core::Expected<std::shared_ptr<LsmtFile>, std::string> stacked = LsmtFile::Open(
        upper->RwDataFile(), upper->RwIndexFile(), lower_index, lowers->Layers());
    if (!stacked.ok()) return stacked;

    // Only when the upper does not declare one. A fresh upper is created with
    // size 0 and inherits the image's; one that was resized keeps its own,
    // because shrinking back to the lowers' would hide blocks it already owns.
    if (stacked.value()->VirtualSize() == 0) {
        const core::Expected<core::Unit, std::string> sized =
            stacked.value()->UpdateVsize(lowers->VirtualSize());
        if (!sized.ok()) return core::make_unexpected(sized.error());
    }

    // Carried over so stacking does not reset tuning the caller applied.
    const core::Expected<core::Unit, std::string> io =
        stacked.value()->SetMaxIoSize(upper->GetMaxIoSize());
    if (!io.ok()) return core::make_unexpected(io.error());
    stacked.value()->SetIndexGroupCommit(upper->GetIndexGroupCommitSize());

    // `[lowers..., upper]`, matching the layer vector so a mapping's tag
    // indexes into both. The upper contributes its first uuid, or the nil one
    // when it has none -- Rust's `unwrap_or_else(Uuid::nil)`.
    std::vector<std::string> uuids = lowers->Uuids();
    uuids.push_back(upper->Uuids().empty() ? std::string() : upper->Uuids()[0]);
    stacked.value()->SetUuids(uuids);
    return stacked;
}


namespace {

/// A `CompactSource` over a stack's layer vector, selecting by tag.
///
/// The tag is the index into the same vector `OpenFilesRo` built, so this is
/// the one place that invariant is cashed in. An out-of-range tag is a corrupt
/// index and is refused rather than clamped: clamping would silently read
/// another layer's bytes into the merged output.
class LayerStackCompactSource : public lsmt::CompactSource {
 public:
    explicit LayerStackCompactSource(const std::vector<std::shared_ptr<VirtualFile> >& layers)
        : layers_(layers) {}

    bool ReadAt(uint8_t tag, uint64_t src_byte_off, uint8_t* dst, uint64_t len) {
        if (static_cast<size_t>(tag) >= layers_.size()) return false;
        VirtualFile* file = layers_[tag].get();
        if (file == 0) return false;
        const core::Expected<size_t, std::string> got =
            file->ReadAtInto(src_byte_off, dst, static_cast<size_t>(len));
        // A short read is a failure, not a partial success: the compacted
        // output would otherwise carry whatever was already in the buffer.
        return got.ok() && got.value() == static_cast<size_t>(len);
    }

 private:
    const std::vector<std::shared_ptr<VirtualFile> >& layers_;
};

}  // namespace

core::Expected<core::Unit, std::string> MergeFilesRo(
    const std::vector<std::shared_ptr<VirtualFile> >& src_files,
    const std::string& output_path, const lsmt::CompactCommitArgs& args) {
    if (src_files.empty()) {
        return core::make_unexpected(std::string("empty src files"));
    }

    const core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> lower =
        OpenFilesRo(src_files);
    if (!lower.ok()) return core::make_unexpected(lower.error());

    // The whole virtual size, rounded up to a block. Querying only the mapped
    // ranges would miss nothing, but it would also have to be derived from the
    // same index this is about to ask -- so the full span is both simpler and
    // what Rust does (`virtual_size.div_ceil(ALIGNMENT)`).
    const uint64_t vsize = lower.value()->VirtualSize();
    const uint64_t blocks = (vsize + kLsmtAlignment - 1) / kLsmtAlignment;
    if (blocks > static_cast<uint64_t>(~static_cast<uint32_t>(0))) {
        return core::make_unexpected(std::string("merge: virtual size too large to query"));
    }

    std::vector<lsmt::SegmentMapping> mappings;
    lower.value()->Index()->Lookup(
        lsmt::Segment(0, static_cast<uint32_t>(blocks)), &mappings);

    LayerStackCompactSource source(lower.value()->Layers());
    std::string err;
    if (!lsmt::CompactTo(output_path, &source, mappings, vsize, args, &err)) {
        return core::make_unexpected(std::string("merge files ro: ") + err);
    }
    return core::Unit();
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
