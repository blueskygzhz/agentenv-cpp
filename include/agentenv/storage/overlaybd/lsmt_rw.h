// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/lsmt/file/readwrite.rs — LSMTFile, plus the
// RW-side pieces of file/types.rs and file/helper.rs.
//
// The writable end of an LSMT stack: a mutable upper layer over zero or more
// sealed lowers. Reads resolve through the combined index exactly as
// `LsmtReadOnlyFile` does; writes land in the upper and are what this file is
// about.
#ifndef AGENTENV_STORAGE_OVERLAYBD_LSMT_RW_H_
#define AGENTENV_STORAGE_OVERLAYBD_LSMT_RW_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/lsmt.h"
#include "agentenv/storage/overlaybd/lsmt_readonly.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

using lsmt::ComboIndex;
using lsmt::MutableIndex;

/// Rust `types.rs :: ALIGNMENT_4K` — the granularity `SetMaxIoSize` requires
/// on a writable file, which is coarser than the 512-byte index alignment.
const size_t kLsmtAlignment4k = 4096;

/// Rust `types.rs :: RwLayout` — how the upper layer stores its data.
///
/// The three differ in where a write goes and how the index survives a
/// restart, and those two facts are linked:
///
///   * `kRwLogStructured` — data is appended, and every mapping is appended to
///     a separate index file. Nothing is ever overwritten, so the layer can be
///     hashed as it grows.
///   * `kRwSparse` — the upper is pre-sized to the whole virtual disk and
///     written in place at its logical offset. There is no index file: which
///     blocks the layer owns is recovered from the filesystem's extent map.
///   * `kRwHybridLogStructured` — writes in place where the layer already owns
///     the block, appends where it does not, and appends a mapping either way.
enum RwLayout {
    kRwLogStructured = 0,
    kRwSparse = 1,
    kRwHybridLogStructured = 2
};

/// Rust `RwLayout::file_type`.
LsmtFileType RwLayoutFileType(RwLayout layout);

/// Rust `RwLayout::ensure_supported` — refuse a layout this platform cannot
/// honour.
///
/// Only `kRwSparse` can fail, and only because it infers history from an
/// extent map. Checked at every entry point that creates *or* opens an upper,
/// not just where the scan happens: guarding the scan alone would let an
/// unsupported platform create a sparse upper, write to it successfully, and
/// then fail to reopen it on the next restart -- surfacing the problem after
/// the data is already there instead of before.
core::Expected<RwLayout, std::string> EnsureRwLayoutSupported(RwLayout layout);

/// Rust `types.rs :: LayerDescriptor` — the content identity of a sealed layer.
struct LayerDescriptor {
    std::string digest;  ///< `sha256:<hex>`
    uint64_t size;
    LayerDescriptor() : size(0) {}
};

/// Rust `types.rs :: AppendDigestTracker`.
///
/// Hashes the upper's bytes as they are appended, so sealing can report the
/// layer's digest without re-reading it. Only possible for the append-only
/// layout: the other two rewrite blocks in place, and a hash of a stream that
/// was overwritten describes nothing.
///
/// `Absorb` requires strictly contiguous offsets. A gap or a rewind means the
/// hash no longer corresponds to the file, and the tracker is abandoned rather
/// than producing a digest that looks valid.
class AppendDigestTracker {
 public:
    /// Rust `from_header` — seed from the file's 4096-byte header block.
    static core::Expected<AppendDigestTracker, std::string> FromHeader(VirtualFile* file);

    /// Rust `absorb`. Fails on an out-of-order offset.
    core::Expected<core::Unit, std::string> Absorb(uint64_t offset, const uint8_t* data,
                                                    size_t len);

    uint64_t Offset() const { return offset_; }

    /// Rust `descriptor`.
    LayerDescriptor Descriptor() const;

    /// Default-constructs an *inactive* tracker.
    ///
    /// Public only so `LsmtFile` can hold one by value. It is paired with a
    /// separate "is active" flag there rather than an optional, because the
    /// two states are not interchangeable: a tracker at offset zero has not
    /// absorbed the header, so its digest would be of an empty stream rather
    /// than of the file. Nothing reads this state without checking that flag.
    AppendDigestTracker() : offset_(0) {}

 private:
    core::Sha256Stream hasher_;
    uint64_t offset_;
};

/// Rust `types.rs :: WriteFragment` — one piece of a planned hybrid write.
struct WriteFragment {
    enum Kind {
        /// Overwrite a range the layer already owns. No mapping is published:
        /// one already points here.
        kInPlace,
        /// Overwrite a range the layer owns but had marked zeroed. A mapping
        /// *must* be published -- `kInPlace` alone would leave the range still
        /// reading as zero, including after reopen.
        kReuseZero,
        /// Extend the layer. Needs an EOF reservation before its mapping can
        /// be published.
        kAppend
    };

    Kind kind;
    uint64_t logical_offset;  ///< unused for kInPlace
    size_t data_offset;       ///< offset within the caller's chunk
    size_t len;
    uint64_t phys_offset;     ///< unused for kAppend

    WriteFragment()
        : kind(kInPlace), logical_offset(0), data_offset(0), len(0), phys_offset(0) {}
};

/// Rust `helper.rs :: create_mappings_from_sparse` — recover an upper's index
/// from its extent map.
///
/// Walks data/hole boundaries from `start_offset`. An extent boundary that is
/// not block-aligned is refused rather than truncated: the block arithmetic
/// would silently drop the trailing partial block, which then reads back as
/// zeros with nothing reporting it.
core::Expected<std::vector<SegmentMapping>, std::string> CreateMappingsFromSparse(
    VirtualFile* file, uint64_t start_offset);

/// Rust `helper.rs :: rw_header` — build the header block for a new upper.
HeaderTrailer BuildRwHeader(uint64_t virtual_size, RwLayout layout, const std::string& uuid,
                             const std::string& parent_uuid, const uint8_t* user_tag,
                             size_t user_tag_len);

/// Rust `helper.rs :: write_header_block` — write a header into its reserved
/// 4096-byte region.
core::Expected<core::Unit, std::string> WriteHeaderBlock(VirtualFile* file,
                                                           const HeaderTrailer& ht);

/// Rust `helper.rs :: update_header_vsize` — rewrite only the virtual size.
core::Expected<core::Unit, std::string> UpdateHeaderVsize(VirtualFile* file, uint64_t vsize);

/// Rust `LSMTFile`.
///
/// Layer order is `[newest lower, ..., oldest lower, upper]`, and the upper's
/// position in that vector is its tag. That is the reverse of what
/// `OpenFilesRo` takes, and it is not an accident: a mapping's tag indexes
/// straight into this vector, so the upper has to be last for its tag to be
/// the largest.
class LsmtFile : public VirtualFile {
 public:
    /// Rust `open` — reopen an existing upper, recovering its index.
    ///
    /// `lower_layers` is newest-first. Refuses a sealed data file: a sealed
    /// trailer means the layer is immutable, and appending to it would
    /// overwrite the index the trailer points at.
    static core::Expected<std::shared_ptr<LsmtFile>, std::string> Open(
        std::shared_ptr<VirtualFile> rw_data_file,
        std::shared_ptr<VirtualFile> rw_index_file,
        std::shared_ptr<ReadOnlyIndex> lower_index,
        const std::vector<std::shared_ptr<VirtualFile> >& lower_layers);

    /// Rust `create` — a fresh single-layer upper.
    static core::Expected<std::shared_ptr<LsmtFile>, std::string> Create(
        std::shared_ptr<VirtualFile> rw_data_file,
        std::shared_ptr<VirtualFile> rw_index_file, uint64_t virtual_size, bool sparse_rw);

    /// Rust `create_with_metadata`.
    static core::Expected<std::shared_ptr<LsmtFile>, std::string> CreateWithMetadata(
        std::shared_ptr<VirtualFile> rw_data_file,
        std::shared_ptr<VirtualFile> rw_index_file, uint64_t virtual_size, RwLayout layout,
        const std::string& uuid, const std::string& parent_uuid, const uint8_t* user_tag,
        size_t user_tag_len);

    LsmtFileType FileType() const { return file_type_; }
    uint64_t VirtualSize() const { return virtual_size_; }
    bool IsSealed() const { return sealed_; }

    /// Rust `get_max_io_size` / `set_max_io_size`.
    ///
    /// 4K-aligned here, not 512 as on the read-only file: the write path
    /// chunks data appends by this value, and a sub-page step would issue
    /// partial-page writes to the upper for no benefit.
    size_t GetMaxIoSize() const { return max_io_size_; }
    core::Expected<core::Unit, std::string> SetMaxIoSize(size_t size);

    /// Rust `set_index_group_commit` / `get_index_group_commit_size`.
    ///
    /// Zero writes each mapping through immediately. A non-zero buffer trades
    /// crash granularity for fewer index writes: buffered mappings are lost if
    /// the process dies, so the data they describe reads as a hole on reopen.
    void SetIndexGroupCommit(size_t buffer_size) { group_commit_size_ = buffer_size; }
    size_t GetIndexGroupCommitSize() const { return group_commit_size_; }

    /// Rust `get_uuid`.
    core::Expected<std::string, std::string> GetUuid(size_t layer_idx) const;
    /// Rust `get_lower_files` — every layer, including the upper.
    const std::vector<std::shared_ptr<VirtualFile> >& GetLowerFiles() const { return layers_; }
    /// Rust `lower_layer_files_bottom_to_top` — the lowers only, reversed into
    /// the order `OpenFilesRo` expects.
    std::vector<std::shared_ptr<VirtualFile> > LowerLayerFilesBottomToTop() const;

    /// Rust `index`.
    std::shared_ptr<ComboIndex> Index() const { return index_; }

    /// Rust `update_vsize`.
    core::Expected<core::Unit, std::string> UpdateVsize(uint64_t vsize);
    /// Rust `data_stat`.
    core::Expected<DataStat, std::string> Stat() const;
    /// Rust `seek_data`.
    core::Expected<size_t, std::string> SeekDataSegments(uint64_t begin, uint64_t end,
                                                           std::vector<Segment>* segs) const;

    /// Rust `discard_range`.
    core::Expected<core::Unit, std::string> DiscardRange(uint64_t offset, uint64_t len);

    /// Rust `flush_group_commit` — write out any buffered mappings.
    core::Expected<core::Unit, std::string> FlushGroupCommit();

    /// Rust `close_seal` — append the index and a sealed trailer.
    ///
    /// Returns the layer's descriptor when one is available, which is only
    /// the append-only layout with an unbroken digest. Absent is normal, not
    /// an error.
    core::Expected<core::Optional<LayerDescriptor>, std::string> CloseSeal();

    /// Rust `plan_hybrid_write`. Exposed for testing: the fragment split is
    /// the whole of the hybrid write decision, and it is pure.
    static core::Expected<std::vector<WriteFragment>, std::string> PlanHybridWrite(
        const MutableIndex& upper, uint64_t logical_offset, size_t data_len);

    // -- VirtualFile -------------------------------------------------------

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override;
    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst,
                                                    size_t len) override;
    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                 size_t len) override;
    core::Expected<uint64_t, std::string> Size() override;
    core::Expected<core::Unit, std::string> Discard(uint64_t offset, uint64_t len) override;
    core::Expected<core::Unit, std::string> Truncate(uint64_t size) override;
    core::Expected<core::Unit, std::string> Sync() override;

 private:
    LsmtFile();
    LsmtFile(const LsmtFile&);
    LsmtFile& operator=(const LsmtFile&);

    static core::Expected<RwLayout, std::string> RwLayoutFromHeader(const HeaderTrailer& h);

    core::Expected<uint64_t, std::string> DataAppendOffset() const;
    core::Expected<core::Unit, std::string> AdvanceDataAppendOffset(uint64_t start, size_t len);
    core::Expected<uint64_t, std::string> IndexAppendOffset() const;
    core::Expected<core::Unit, std::string> AdvanceIndexAppendOffset(uint64_t start,
                                                                       size_t len);
    core::Expected<core::Unit, std::string> AppendIndexBytes(const uint8_t* bytes, size_t len);
    core::Expected<core::Unit, std::string> AppendIndexMapping(const DiskSegmentMapping& m);

    core::Expected<size_t, std::string> ReadInternalInto(uint64_t offset, uint8_t* dst,
                                                           size_t len);
    core::Expected<core::Unit, std::string> ReadMappingsInto(
        const std::vector<SegmentMapping>& mappings, uint64_t begin_blk, uint64_t count_blk,
        size_t step, size_t buf_pos, uint8_t* buf);
    core::Expected<size_t, std::string> WriteInternal(uint64_t offset, const uint8_t* data,
                                                        size_t len);
    core::Expected<core::Unit, std::string> DiscardHybridRange(uint64_t offset, uint64_t len);

    void AbsorbSealBytes(uint64_t offset, const uint8_t* data, size_t len);

    std::shared_ptr<ComboIndex> index_;
    std::vector<std::shared_ptr<VirtualFile> > layers_;
    std::shared_ptr<VirtualFile> rw_data_file_;
    std::shared_ptr<VirtualFile> rw_index_file_;
    uint64_t virtual_size_;
    size_t rw_tag_;
    bool sealed_;
    size_t max_io_size_;
    std::vector<std::string> uuids_;
    LsmtFileType file_type_;
    size_t group_commit_size_;
    std::vector<DiskSegmentMapping> group_commit_buf_;
    /// Rust `rw_data_append_offset` — an in-memory EOF cursor, so the append
    /// hot path does not call `size()` per write.
    ///
    /// Log/hybrid: the real file EOF, advanced after each append. Sparse: set
    /// to `virtual_size + HEADER_SIZE`, because sparse writes address their
    /// logical offset and never append -- the cursor exists only so the
    /// resize and seal paths can still name the physical end.
    uint64_t rw_data_append_offset_;
    uint64_t rw_index_append_offset_;
    bool has_append_digest_;
    AppendDigestTracker append_digest_;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_LSMT_RW_H_
