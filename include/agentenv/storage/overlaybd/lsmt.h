// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/lsmt/{index,format}.rs — the LSMT in-memory index
// and on-disk header packing. This is a faithful C++11 port of the Rust logic.
#ifndef AGENTENV_STORAGE_OVERLAYBD_LSMT_H_
#define AGENTENV_STORAGE_OVERLAYBD_LSMT_H_

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace lsmt {

// ==== Rust: lsmt/index.rs :: Segment ====
struct Segment {
    uint64_t offset = 0;   // in units of ALIGNMENT (sector)
    uint32_t length = 0;   // in units of ALIGNMENT

    static const uint32_t kMaxLength = (1u << 14) - 1;

    Segment() {}
    Segment(uint64_t off, uint32_t len) : offset(off), length(len) {}

    uint64_t End() const { return offset + static_cast<uint64_t>(length); }

    /// Rust `forward_offset_to` — returns the consumed delta.
    uint64_t ForwardOffsetTo(uint64_t x);
    /// Rust `backward_end_to`.
    void BackwardEndTo(uint64_t x);
};

/// Rust `format.rs :: NO_PHYSICAL_OFFSET` == `DiskSegmentMapping::MOFFSET_MASK`.
///
/// A zero mapping without physical backing. Physical offsets occupy 55 bits on
/// disk, so the all-ones value is reserved as the "no backing" sentinel both in
/// memory and on disk.
///
/// This is deliberately NOT the same predicate as `zeroed`: a mapping can be
/// `zeroed` *and* still own a physical range (a "backed zero"), which RW replay
/// must preserve across reopen.
static const uint64_t kNoPhysicalOffset = (1ULL << 55) - 1;

// ==== Rust: lsmt/index.rs :: SegmentMapping ====
struct SegmentMapping {
    Segment  segment;
    uint64_t moffset = 0;
    bool     zeroed = false;
    uint8_t  tag = 0;

    SegmentMapping() {}
    SegmentMapping(uint64_t offset, uint32_t length, uint64_t moff, bool z, uint8_t t)
        : segment(offset, length), moffset(moff), zeroed(z), tag(t) {}

    uint64_t Offset() const { return segment.offset; }
    uint32_t Length() const { return segment.length; }
    uint64_t End() const { return segment.End(); }

    /// Rust `has_physical_range` — whether this mapping retains a physical
    /// range. It does NOT imply the range is readable data: `zeroed` controls
    /// read visibility, this controls space ownership.
    bool HasPhysicalRange() const { return moffset != kNoPhysicalOffset; }

    /// Rust `mend` — physical end, or the sentinel for an unbacked zero.
    uint64_t MEnd() const {
        return HasPhysicalRange() ? moffset + static_cast<uint64_t>(segment.length)
                                  : kNoPhysicalOffset;
    }
    void ForwardOffsetTo(uint64_t x);
    void BackwardEndTo(uint64_t x) { segment.BackwardEndTo(x); }

    /// Rust private `can_merge_with`.
    bool CanMergeWith(const SegmentMapping& next) const {
        return End() == next.Offset() &&
               zeroed == next.zeroed &&
               tag == next.tag &&
               HasPhysicalRange() == next.HasPhysicalRange() &&
               MEnd() == next.moffset &&
               (static_cast<uint64_t>(Length()) + static_cast<uint64_t>(next.Length())) <=
                   static_cast<uint64_t>(Segment::kMaxLength);
    }

    bool operator==(const SegmentMapping& o) const {
        return segment.offset == o.segment.offset &&
               segment.length == o.segment.length &&
               moffset == o.moffset && zeroed == o.zeroed && tag == o.tag;
    }
    bool operator!=(const SegmentMapping& o) const { return !(*this == o); }
};

/// Rust `compress_raw_index` — in-place merge of adjacent compatible mappings.
/// Returns the compressed count and truncates `mapping`.
size_t CompressRawIndex(std::vector<SegmentMapping>* mapping);
/// Rust `compress_raw_index_predict` — dry-run count without mutating.
size_t CompressRawIndexPredict(const std::vector<SegmentMapping>& mapping);

/// Rust trait `LogIndex`.
class LogIndex {
 public:
    virtual ~LogIndex() {}
    virtual size_t Lookup(Segment query, std::vector<SegmentMapping>* dst) const = 0;
};

/// Rust `ReadOnlyIndex` — a sorted immutable mapping list.
class ReadOnlyIndex : public LogIndex {
 public:
    ReadOnlyIndex() {}
    /// Rust `ReadOnlyIndex::new`.
    ///
    /// Normalizes every `zeroed` mapping to `kNoPhysicalOffset`: lower layers
    /// never lend physical space to a writable upper, and legacy zero offsets
    /// are arbitrary placeholders that lookup/merge would otherwise interpret
    /// as backed zeros. RW replay must NOT go through here, since it has to
    /// preserve backed-zero offsets across reopen.
    explicit ReadOnlyIndex(std::vector<SegmentMapping> mappings);

    /// Rust `merge` — combine indexes newest-first; assigns tag by layer index.
    static ReadOnlyIndex Merge(const std::vector<const ReadOnlyIndex*>& indexes);

    const std::vector<SegmentMapping>& Mappings() const { return mappings_; }
    size_t Lookup(Segment query, std::vector<SegmentMapping>* dst) const override;

 private:
    std::vector<SegmentMapping> mappings_;
};

/// Rust `MutableIndex` — a BTreeSet keyed by offset, with overlap resolution.
class MutableIndex : public LogIndex {
 public:
    void Insert(SegmentMapping m);
    size_t Len() const { return mappings_.size(); }
    bool Empty() const { return mappings_.empty(); }
    std::vector<SegmentMapping> Dump() const;
    size_t Lookup(Segment query, std::vector<SegmentMapping>* dst) const override;

 private:
    struct ByOffset {
        bool operator()(const SegmentMapping& a, const SegmentMapping& b) const {
            return a.segment.offset < b.segment.offset;
        }
    };
    std::set<SegmentMapping, ByOffset> mappings_;
    friend class ComboIndex;
};

/// Rust `ComboIndex` — a mutable upper layer over an optional read-only lower.
class ComboIndex : public LogIndex {
 public:
    ComboIndex(MutableIndex upper, std::shared_ptr<ReadOnlyIndex> lower,
               uint8_t ro_layers_count);
    void Insert(SegmentMapping m) { upper_.Insert(m); }
    size_t UpperLen() const { return upper_.Len(); }
    size_t Lookup(Segment query, std::vector<SegmentMapping>* dst) const override;

 private:
    MutableIndex upper_;
    std::shared_ptr<ReadOnlyIndex> lower_;
};

// ==== Rust: lsmt/format.rs :: DiskSegmentMapping (bit-packed) ====
struct DiskSegmentMapping {
    uint64_t data_low = 0;
    uint64_t data_high = 0;

    static DiskSegmentMapping FromMemory(const SegmentMapping& m);
    SegmentMapping ToMemory() const;
};

// ==== Rust: lsmt/format.rs :: HeaderTrailer (magic/flags), #[repr(C, packed)] ====
// On-disk layout is exactly 390 bytes; SPACE (block) is 4096.
struct HeaderTrailer {
    static const uint64_t kMagic0 = 0x00020100544d534cULL;  // "LSMT\0\1\2"
    static const uint64_t kSpace = 4096;
    static const size_t   kOnDiskSize = 390;

    uint64_t magic0 = 0;
    uint8_t  magic1[16];
    uint32_t size = static_cast<uint32_t>(kOnDiskSize);
    uint32_t flags = 0;
    uint64_t index_offset = 0;
    uint64_t index_size = 0;
    uint64_t virtual_size = 0;
    uint8_t  uuid[37];
  uint8_t  parent_uuid[37];
    uint16_t reserved = 0;
    uint8_t  version = 1;
    uint8_t  sub_version = 1;
    uint8_t  user_tag[256];

    HeaderTrailer();
    static HeaderTrailer New();
    bool VerifyMagic() const;

 /// Serialize to the exact 390-byte little-endian packed layout.
    void Serialize(uint8_t out[kOnDiskSize]) const;
    /// Parse from a >=390-byte buffer. Returns false if too short.
    static bool Deserialize(const uint8_t* buf, size_t len, HeaderTrailer* out);

    bool IsHeader() const { return (flags & (1u << 0)) != 0; }
    bool IsTrailer() const { return !IsHeader(); }
  bool IsDataFile() const { return (flags & (1u << 1)) != 0; }
    bool IsSealed() const { return (flags & (1u << 2)) != 0; }
    bool IsSparseRw() const { return (flags & (1u << 4)) != 0; }
    bool IsHybridRw() const { return (flags & (1u << 5)) != 0; }

    void SetHeader()   { flags |= (1u << 0); }
    void SetTrailer()  { flags &= ~1u; }
    void SetDataFile() { flags |= (1u << 1); }
    void SetIndexFile(){ flags &= ~(1u << 1); }
    void SetSealed()   { flags |= (1u << 2); }
    void SetSparseRw() { flags |= (1u << 4); }
    void SetHybridRw() { flags |= (1u << 5); }
};

/// Rust: lsmt/format.rs :: MAGIC1 (the 16-byte second magic / uuid bytes).
extern const uint8_t kHeaderTrailerMagic1[16];

/// Rust: lsmt/file/readonly.rs :: LSMTReadOnlyFile::open —
/// open a sealed single LSMT file and build its ReadOnlyIndex.
/// Returns nullptr + fills `err` on failure.
std::shared_ptr<ReadOnlyIndex>
    OpenIndexFile(const std::string& path, uint64_t* virtual_size_out, std::string* err);

/// Rust: lsmt/file/helper.rs :: load_index_and_reset_tags — parse a
/// DiskSegmentMapping[] region (tag reset to 0, skipping invalid entries).
std::vector<SegmentMapping>
    LoadIndexAndResetTags(const uint8_t* index_region, size_t region_len, size_t count);

// ==== Rust: lsmt/file/types.rs constants ====
static const uint64_t kLsmtAlignment  = 512;
static const uint64_t kLsmtHeaderSize = 4096;
static const uint64_t kInvalidSegmentOffset = (1ULL << 50) - 1;

/// A minimal source of aligned data blocks for CompactTo. `moffset`/`length`
/// are expressed in ALIGNMENT (512B) units, matching SegmentMapping.
/// Rust equivalent: the `&[Arc<dyn VirtualFile>]` src_layers, indexed by tag.
class CompactSource {
 public:
    virtual ~CompactSource() {}
  /// Read `len` bytes at byte offset `src_byte_off` from the layer `tag`.
    /// Returns false on short read / OOB.
    virtual bool ReadAt(uint8_t tag, uint64_t src_byte_off,
          uint8_t* dst, uint64_t len) = 0;
};

/// Rust: lsmt/file/helper.rs :: compact_to (single-threaded, no zero-detection
/// path — i.e. COMPACT_ZERO_DETECTION_ENABLED == false).
/// Packs non-zeroed mappings sequentially after the 4096B header, records zeroed
/// mappings without consuming space, compresses the index, writes an aligned
/// index array + sealed trailer. Writes a self-contained LSMT file at `path`.
bool CompactTo(const std::string& path,
           CompactSource* source,
               const std::vector<SegmentMapping>& mappings,
   uint64_t virtual_size,
         std::string* err);

}  // namespace lsmt
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_LSMT_H_
