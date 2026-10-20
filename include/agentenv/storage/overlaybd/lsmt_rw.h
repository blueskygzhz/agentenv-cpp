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

/// Rust's `(LSMTReadOnlyFile, Option<LayerDescriptor>)` tuple return from
/// `close_seal_and_reopen`, named because C++11 has no structured binding to
/// unpack a pair readably at the call site.
struct SealedReopen {
    std::shared_ptr<LsmtReadOnlyFile> file;
    /// Absent is normal: only the append-only layout with an unbroken digest
    /// can report one.
    core::Optional<LayerDescriptor> descriptor;
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

/// Rust `helper.rs :: initialize_file_rw_paths` — lay down a brand-new upper.
///
/// Creates (truncating) the data file and writes its header, then does the one
/// extra step the layout needs:
///
///  * **Sparse** — the file is extended to `virtual_size + kSpace`, because a
///    sparse upper addresses its blocks by offset with no index to redirect
///    them. The extension is a hole, so it costs nothing until written.
///  * **Log-structured / hybrid** — a second file is created for the index,
///    carrying the same header marked as an index file with its offset set to
///    the unsealed position. `index_path` is required here and refused when
///    absent rather than defaulted: a log-structured upper with no index has
///    no way to find any block it writes.
///
/// A fresh UUID is minted per call. It is what later identifies this layer, so
/// reusing one would make two uppers indistinguishable to anything that reads
/// the identity out of the header.
core::Expected<core::Unit, std::string> InitializeFileRwPaths(
    const std::string& data_path, const core::Optional<std::string>& index_path,
    uint64_t virtual_size, RwLayout layout);

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

    /// Rust `rw_data_file` / `rw_index_file` — the upper's own two files.
    ///
    /// Exposed for `StackFiles`, which reopens this upper over a set of lower
    /// layers. It hands these back to `Open` rather than duplicating them: the
    /// upper being stacked is the same upper, and opening its path a second
    /// time would give the stack a separate handle with its own append offset.
    std::shared_ptr<VirtualFile> RwDataFile() const { return rw_data_file_; }
    std::shared_ptr<VirtualFile> RwIndexFile() const { return rw_index_file_; }

    /// Rust `uuids` — layer identities, ordered `[lowers..., upper]`.
    ///
    /// The upper is last, matching the layer vector: a mapping's tag indexes
    /// into both, so the upper's tag has to be the largest.
    const std::vector<std::string>& Uuids() const { return uuids_; }

    /// Rust's `stacked.uuids = uuids` in `stack_files`.
    ///
    /// Set wholesale because stacking replaces the identity list entirely:
    /// `Open` fills in one entry for the upper, and the stacked file needs
    /// `[lowers..., upper]`. Appending instead would leave the upper's own
    /// entry at index 0, where a lower's tag points.
    void SetUuids(const std::vector<std::string>& uuids) { uuids_ = uuids; }

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

    /// Rust `close_seal_and_reopen` — seal this upper, then hand back a
    /// read-only view of the layer it just became.
    ///
    /// The reopened file is built from this upper's own data handle, not from
    /// its path: sealing wrote the index and trailer through that handle, and
    /// reopening the path would be a second handle onto the same bytes.
    ///
    /// The max io size carries over, because a caller that tuned this upper
    /// stacked the result expecting the same chunking.
    core::Expected<SealedReopen, std::string> CloseSealAndReopen();

    /// Rust `flatten` — write the whole image, as resolved through this
    /// stack, into `dest` as one self-contained sealed layer.
    core::Expected<core::Unit, std::string> Flatten(std::shared_ptr<VirtualFile> dest);

    /// Rust `flatten_with_args` — `Flatten` with a caller-chosen destination
    /// and layer identity.
    core::Expected<core::Unit, std::string> FlattenWithArgs(const CommitArgs& args);

    /// Rust `export_upper_as_sealed` — copy *only this upper's* mappings into
    /// `args.writer` as a standalone sealed layer.
    ///
    /// Unlike `CloseSeal` and `CloseSealAndReopen`, **this file stays
    /// writable**: nothing is sealed in place, the upper is only read from.
    /// That is what makes it usable as a snapshot of a running image.
    ///
    /// The output's parent is set to `Uuids()[0]`, the topmost lower — after
    /// `OpenFilesRo` + `StackFiles` the uuid vector reads
    /// `[newest_lower, ..., oldest_lower, upper]`, so index 0 is the upper's
    /// immediate parent in the chain. A single-layer file has no parent and
    /// the field is left zeroed.
    ///
    /// Any mapping buffered by group commit is flushed first; an exported
    /// layer that omitted them would read as holes where data was written.
    core::Expected<core::Unit, std::string> ExportUpperAsSealed(const CommitArgs& args);

    /// Rust `commit_with_args` — write this file out as one sealed layer with
    /// a caller-chosen identity.
    ///
    /// Refuses a stacked file. `Flatten` is the operation for that case: it
    /// resolves shadowing across the whole stack, whereas a commit is defined
    /// as "write *this* layer's own content", and the two cannot be the same
    /// call. Rust's message, `"not supported: commit stacked files"`, is kept
    /// verbatim because callers match on it.
    ///
    /// Group-committed mappings are flushed first, for the same reason
    /// `ExportUpperAsSealed` does it.
    core::Expected<core::Unit, std::string> CommitWithArgs(const CommitArgs& args);

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

// ---------------------------------------------------------------------------
// Rust: lsmt/file/stack.rs — the open/stack entry points
// ---------------------------------------------------------------------------

/// Rust `types.rs :: LayerInfo` — the description a new upper is created from.
///
/// It exists for its constructor. Rust's `LayerInfo::new` fills `uuid` with a
/// fresh v4, so every layer created through the normal path has a stable
/// identity; the expanded `CreateFileRw` below takes the uuid as a plain
/// argument, which made it possible to pass an empty one and silently produce
/// a layer with a nil identity. That is not a cosmetic loss: the premerged
/// index cache keys on layer uuids and refuses to engage when any is nil, and
/// a parent/child relationship cannot be expressed at all.
struct LayerInfo {
    std::shared_ptr<VirtualFile> fdata;
    /// Absent means the index lives in the data file.
    std::shared_ptr<VirtualFile> findex;
    uint64_t virtual_size;
    std::string parent_uuid;
    std::string uuid;
    std::vector<uint8_t> user_tag;
    RwLayout rw_layout;

    /// Rust `LayerInfo::new` — defaults a fresh v4 uuid, no parent, and the
    /// log-structured layout.
    static LayerInfo New(const std::shared_ptr<VirtualFile>& fdata,
                         const std::shared_ptr<VirtualFile>& findex, uint64_t virtual_size);

 private:
    LayerInfo() : virtual_size(0), rw_layout(kRwLogStructured) {}
};

/// Rust `stack.rs :: create_file_rw` — a fresh upper from a layer description.
core::Expected<std::shared_ptr<LsmtFile>, std::string> CreateFileRw(const LayerInfo& info);

/// The expanded form. Prefer the `LayerInfo` overload: an empty `uuid` here is
/// stored verbatim, giving the layer a nil identity.
core::Expected<std::shared_ptr<LsmtFile>, std::string> CreateFileRw(
    std::shared_ptr<VirtualFile> fdata, std::shared_ptr<VirtualFile> findex,
    uint64_t virtual_size, RwLayout layout, const std::string& uuid,
    const std::string& parent_uuid, const uint8_t* user_tag, size_t user_tag_len);

/// Rust `stack.rs :: open_file_rw` — reopen an upper with no lowers under it.
///
/// The empty lower vector is the point: this is the single-layer case, where
/// every mapping resolves into the upper itself.
core::Expected<std::shared_ptr<LsmtFile>, std::string> OpenFileRw(
    std::shared_ptr<VirtualFile> fdata, std::shared_ptr<VirtualFile> findex);

/// Rust `stack.rs :: open_file_ro` — open one sealed layer.
core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> OpenFileRo(
    std::shared_ptr<VirtualFile> file);

/// Rust `helper.rs :: validate_rw_header_pair_paths` — structural check on an
/// unsealed RW layer pair, by path.
///
/// For external tools that mutate an OverlayBD upper in place and want a
/// cheap check before the image is reopened. Reads only the two header blocks
/// and the file sizes, so it costs two opens and two stats.
///
/// `index_path` must be absent for `kRwSparse` and present otherwise, which
/// is checked rather than assumed: a sparse layer carrying an index file, or
/// a log-structured one missing it, is a layout the reader cannot interpret.
core::Expected<core::Unit, std::string> ValidateRwHeaderPairPaths(
    const std::string& data_path, const core::Optional<std::string>& index_path,
    uint64_t expected_virtual_size, RwLayout expected_layout);

/// Rust `stack.rs :: verify_layer_order` — check that each layer names the one
/// below it as its parent.
///
/// Walks from `start_layer` upward, comparing each layer's uuid against the
/// `parent_uuid` recorded in the previous layer's header. A mismatch means the
/// stack was assembled from layers that do not belong together, and reading it
/// would silently resolve blocks against the wrong base image.
///
/// The last layer is not probed for a parent: nothing sits above it to check
/// against, and its header is not read at all. `start_layer >= layers.size()`
/// succeeds trivially, which is what lets a caller skip the newest layer by
/// passing 1 without special-casing a one-layer stack.
core::Expected<core::Unit, std::string> VerifyLayerOrder(
    const std::vector<std::shared_ptr<VirtualFile> >& layers,
    const std::vector<std::string>& uuids, size_t start_layer);

/// Rust `stack.rs :: stack_files` — reopen `upper` on top of `lowers`.
///
/// `lowers` must be in the merged index's order (newest first), which is what
/// `OpenFilesRo` already produces. The returned file reads a block from the
/// upper when the upper owns it and falls through to the lowers otherwise.
///
/// Three things carry over from the inputs rather than being re-derived:
///
///  * The **virtual size** is taken from the lowers only when the upper does
///    not declare one. A fresh upper is created with size 0 and inherits the
///    image's; an upper that was resized keeps its own, because shrinking it
///    back to the lowers' would hide blocks it already owns.
///  * The upper's **max io size** and **group commit size**, so stacking does
///    not silently reset tuning the caller applied.
///  * The **uuids**, as `[lowers..., upper]` — the same order as the layer
///    vector, since a mapping's tag indexes into both.
///
/// Requires the lowers to carry a read-only index view; a stack over a
/// mutable index has nothing sealed underneath it to resolve against.
core::Expected<std::shared_ptr<LsmtFile>, std::string> StackFiles(
    const std::shared_ptr<LsmtFile>& upper,
    const std::shared_ptr<LsmtReadOnlyFile>& lowers, bool check_order);

/// Rust `stack.rs :: merge_files_ro` — flatten several sealed layers into one.
///
/// Opens the layers as a stack, asks the merged index for every mapping across
/// the whole virtual size, and compacts those into a single self-contained
/// layer at `output_path`.
///
/// The query covers the entire image rather than only the mapped parts,
/// because a merge has to resolve shadowing: a block present in two layers
/// must come out as the newer one's, which is what the merged index's lookup
/// already decides.
core::Expected<core::Unit, std::string> MergeFilesRo(
    const std::vector<std::shared_ptr<VirtualFile> >& src_files,
    const std::string& output_path, const lsmt::CompactCommitArgs& args);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_LSMT_RW_H_
