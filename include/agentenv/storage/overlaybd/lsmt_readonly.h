// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/lsmt/file/readonly.rs — LSMTReadOnlyFile,
// plus the pieces of file/helper.rs and file/types.rs it needs.
//
// The layer that turns an index plus a set of layer files into one readable
// address space. `lsmt.cc` already carries the index structures and the
// on-disk format; this is what reads *through* them, and it is the only thing
// `ImageFile` has to delegate to.
#ifndef AGENTENV_STORAGE_OVERLAYBD_LSMT_READONLY_H_
#define AGENTENV_STORAGE_OVERLAYBD_LSMT_READONLY_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/lsmt.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

// The index structures and the on-disk format live in a nested `lsmt`
// namespace; these are used unqualified throughout.
using lsmt::DiskSegmentMapping;
using lsmt::HeaderTrailer;
using lsmt::LoadIndexAndResetTags;
using lsmt::LogIndex;
using lsmt::ReadOnlyIndex;
using lsmt::Segment;
using lsmt::SegmentMapping;
using lsmt::kLsmtAlignment;
using lsmt::kNoPhysicalOffset;
using lsmt::kLsmtHeaderSize;

/// Rust `types.rs :: MAX_IO_SIZE` — the largest span resolved in one pass.
///
/// A read longer than this is split, so the index lookup and the temporary
/// mapping vector stay bounded no matter how large a request the guest makes.
const size_t kLsmtMaxIoSize = 4 * 1024 * 1024;

/// Rust `types.rs :: LSMTFileType`.
///
/// The three writable kinds are not interchangeable: they decide where a
/// write lands and whether the layer can be hashed as it grows, so the write
/// path branches on this rather than on a separate flag.
enum LsmtFileType {
    kLsmtFileReadOnly = 0,
    kLsmtFileWritable = 1,
    kLsmtFileSparseWritable = 2,
    kLsmtFileHybridWritable = 3
};

/// Rust `types.rs :: CommitArgs` — where a compacted layer goes plus the
/// identity it is stamped with.
///
/// `writer` is a `VirtualFile` rather than Rust's `Arc<dyn CompactWriter>`:
/// both ported callers reach this through `CommitArgs::new`, which wraps a
/// file in `VirtualFileWriter`. The general trait is still available one
/// level down as `lsmt::CompactSink`, so a non-file writer can be passed to
/// `lsmt::CompactTo` directly; nothing in the ported surface needs it.
///
/// Rust's `concurrency` field is absent: this port copies data blocks
/// sequentially, which is Rust's default of 1.
///
/// Declared here rather than in `lsmt_rw.h` because both the read-only and
/// the read-write file commit through it, exactly as Rust puts it in
/// `types.rs` for the same reason. `lsmt_rw.h` includes this header, so the
/// other placement would make `LsmtReadOnlyFile::CommitPreservingMetadata`
/// unable to name its own argument type.
struct CommitArgs {
    std::shared_ptr<VirtualFile> writer;
    /// Empty means the nil uuid is written out as text, not that the field is
    /// left zeroed -- see `lsmt::CompactTo`.
    std::string uuid;
    /// Empty leaves the field zeroed, which reads back as "no parent".
    std::string parent_uuid;
    std::vector<uint8_t> user_tag;

    CommitArgs() {}
    /// Rust `CommitArgs::new`.
    explicit CommitArgs(std::shared_ptr<VirtualFile> as_file) : writer(as_file) {}
};

/// Rust `types.rs :: DataStat`.
struct DataStat {
    uint64_t total_data_size;
    uint64_t valid_data_size;
    DataStat() : total_data_size(0), valid_data_size(0) {}
};

/// Rust `helper.rs :: check_alignment`.
///
/// Both the offset and the length must be multiples of the 512-byte
/// alignment. The index addresses blocks, not bytes, so an unaligned request
/// has no exact answer -- it would have to be widened, and the caller would
/// get back more than it asked for with no way to tell.
core::Expected<core::Unit, std::string> CheckAlignment(uint64_t offset, size_t len);

/// Rust `helper.rs :: verify_ht` — read and validate the header or trailer.
///
/// The two are checked differently on purpose. A header only has to be a
/// header; a trailer additionally has to say it is a *sealed data file*,
/// because that is what makes the index offset it carries trustworthy. An
/// unsealed trailer belongs to a file still being written, where that offset
/// may point at a region that is not an index yet.
core::Expected<HeaderTrailer, std::string> VerifyHt(VirtualFile* file, bool is_trailer,
                                                      uint64_t file_size);

/// Rust `LSMTReadOnlyFile`.
///
/// Reads resolve through the index: every 512-byte block of the virtual
/// address space either maps to a physical range in one of the layers, is
/// explicitly recorded as zeroed, or is absent. All three are legal, and the
/// last two both read back as zeros -- which is what lets an image declare a
/// virtual size far larger than the bytes it stores.
class LsmtReadOnlyFile : public VirtualFile {
 public:
    /// Rust `open` — a single sealed LSMT file.
    ///
    /// Takes a shared handle because the file is kept as layer 0 and read
    /// from for the lifetime of this object.
    static core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> Open(
        std::shared_ptr<VirtualFile> file);

    /// Rust's struct literal in `stack.rs` — a prebuilt index over several
    /// layers, where the tag of each mapping selects the layer.
    ///
    /// A mapping whose tag is out of range is a corrupt index, and is
    /// reported at read time rather than here: the index may legitimately be
    /// larger than the range any one read touches.
    static core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> FromLayers(
        std::shared_ptr<LogIndex> index,
        const std::vector<std::shared_ptr<VirtualFile> >& layers, uint64_t virtual_size);

    /// Rust `from_merged_layers` — as above, with the per-layer uuids the
    /// merge already read. Layers and uuids must be in the same order, which
    /// is the merged index's order (tag 0 newest), not the caller's.
    static core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> FromMergedLayers(
        std::shared_ptr<LogIndex> index,
        const std::vector<std::shared_ptr<VirtualFile> >& layers, uint64_t virtual_size,
        const std::vector<std::string>& uuids);

    /// Rust `get_max_io_size` / `set_max_io_size`.
    ///
    /// Must stay a non-zero multiple of the alignment: zero would make the
    /// read loop spin without consuming its input, and an unaligned step
    /// would put the next iteration's offset off a block boundary.
    size_t GetMaxIoSize() const { return max_io_size_; }
    core::Expected<core::Unit, std::string> SetMaxIoSize(size_t size);

    uint64_t VirtualSize() const { return virtual_size_; }
    LsmtFileType FileType() const { return file_type_; }
    std::shared_ptr<LogIndex> Index() const { return index_; }
    const std::vector<std::shared_ptr<VirtualFile> >& Layers() const { return layers_; }

    /// Rust `index_view` — the index as a concrete `ReadOnlyIndex`.
    ///
    /// Rust keeps this as a second field alongside `index`, but both always
    /// hold the *same object*: `open` and `from_merged_layers` each clone one
    /// `Arc<ReadOnlyIndex>` into both. So this is not extra state, it is the
    /// concrete type recovered from the polymorphic one -- which is what
    /// `StackFiles` needs, because an upper stacks onto a read-only index
    /// specifically, not onto any `LogIndex`.
    ///
    /// Null when the index is some other kind (a mutable or combo index), in
    /// which case there is nothing sealed to stack onto.
    std::shared_ptr<ReadOnlyIndex> IndexView() const { return index_view_; }

    /// Rust `uuids` — one per layer, in the same order as `Layers`.
    ///
    /// That order is the merged index's (tag 0 newest), not the order the
    /// caller passed to `OpenFilesRo`; a mapping's tag indexes straight into
    /// both vectors, so they have to agree.
    const std::vector<std::string>& Uuids() const { return uuids_; }

    /// Rust `get_uuid`.
    core::Expected<std::string, std::string> GetUuid(size_t layer_idx) const;

    /// Rust `data_stat` — how many bytes the layers actually hold.
    ///
    /// Zeroed mappings are excluded: they occupy index space but no data
    /// space, which is the entire reason for recording them separately.
    DataStat Stat() const;

    /// Rust `seek_data` — the mapped (non-zeroed) spans in a byte range.
    ///
    /// Reports block segments, not byte offsets, because that is the
    /// granularity the index works in.
    core::Expected<size_t, std::string> SeekDataSegments(uint64_t begin, uint64_t end,
                                                           std::vector<Segment>* segs) const;

    /// Rust `flatten` — write the whole image, as resolved through these
    /// layers, into `dest` as one self-contained sealed layer.
    ///
    /// The query spans the entire virtual size rather than the mapped ranges
    /// only, because flattening has to resolve shadowing: a block present in
    /// two layers must come out as the newer one's, which is the decision the
    /// merged index's lookup already makes.
    ///
    /// `dest` is written but neither synced nor closed; its owner decides
    /// when the bytes have to be durable.
    core::Expected<core::Unit, std::string> Flatten(std::shared_ptr<VirtualFile> dest);

    /// Rust `commit_preserving_metadata` — rewrite a sealed layer into
    /// `args.writer`, carrying its stamped identity across.
    ///
    /// The identity comes from the *source trailer*, not from `args`: uuid,
    /// parent uuid and user tag are overwritten on the copy that is passed
    /// down. That is the whole point of the operation -- repacking a layer
    /// must not re-identify it, or the layer below would stop recognising it
    /// as its child and `VerifyLayerOrder` would reject the stack.
    ///
    /// Requires exactly one layer, and requires it sealed. A stacked
    /// read-only file has no single identity to preserve, and an unsealed
    /// source has no trailer to read one from.
    core::Expected<core::Unit, std::string> CommitPreservingMetadata(const CommitArgs& args);

    /// The same operation against an arbitrary sink.
    ///
    /// `CommitArgs::writer` is a `VirtualFile`, which is the wrong shape for a
    /// consumer that is not a file -- the dense-export digest, for instance,
    /// only hashes the stream. Rust has no such split because its
    /// `CommitArgs.writer` is already the `CompactWriter` trait; this overload
    /// is where that trait object enters, and the `CommitArgs` form is a thin
    /// wrapper over it.
    ///
    /// The identity is still taken from the source trailer, not from the
    /// caller. Nothing else differs.
    core::Expected<core::Unit, std::string> CommitPreservingMetadataTo(lsmt::CompactSink* sink);

    // -- VirtualFile -------------------------------------------------------

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override;
    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst,
                                                    size_t len) override;
    /// Rust `read_at_with_ctx` / `read_at_into_with_ctx`.
    ///
    /// These run the *same* resolver as the plain reads; only the per-layer
    /// read primitive differs. That is why the resolver is a template over a
    /// reader strategy (see `DirectLayerRead` / `CtxLayerRead`) instead of a
    /// plain member: duplicating a hundred lines of hole-and-mapping
    /// arithmetic to add a context parameter is how the two paths drift, and
    /// the drift would only show up as a hole read from the wrong layer.
    core::Expected<ByteBuffer, std::string> ReadAtWithCtx(const IoCtx& ctx, uint64_t offset,
                                                            size_t len) override;
    core::Expected<size_t, std::string> ReadAtIntoWithCtx(const IoCtx& ctx, uint64_t offset,
                                                            uint8_t* dst, size_t len) override;
    /// Always refused -- see the implementation.
    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                 size_t len) override;
    core::Expected<uint64_t, std::string> Size() override;

 private:
    LsmtReadOnlyFile();
    LsmtReadOnlyFile(const LsmtReadOnlyFile&);
    LsmtReadOnlyFile& operator=(const LsmtReadOnlyFile&);

    /// Rust `read_internal_into_generic` — the resolver, generic in the reader
    /// strategy. Defined in the .cc, where its two instantiations live.
    template <typename Reader>
    core::Expected<size_t, std::string> ReadInternalIntoGeneric(const Reader& reader,
                                                                  uint64_t offset,
                                                                  uint8_t* dst, size_t len);

    /// Rust `read_internal_into` — the plain resolver.
    core::Expected<size_t, std::string> ReadInternalInto(uint64_t offset, uint8_t* dst,
                                                           size_t len);

    std::shared_ptr<LogIndex> index_;
    /// The same object as `index_` when it is a `ReadOnlyIndex`; see
    /// `IndexView`.
    std::shared_ptr<ReadOnlyIndex> index_view_;
    std::vector<std::shared_ptr<VirtualFile> > layers_;
    uint64_t virtual_size_;
    size_t max_io_size_;
    std::vector<std::string> uuids_;
    LsmtFileType file_type_;
};

/// Rust `types.rs :: MAX_STACK_LAYERS`.
///
/// A hard ceiling because the layer selector is the mapping's `tag`, a single
/// byte. A 256th layer would have no tag left to name it.
const size_t kMaxStackLayers = 255;

/// Rust `types.rs :: ReadOnlyLayerMetadata` — what one sealed layer declares
/// about itself, read from its header and trailer.
struct ReadOnlyLayerMetadata {
    std::string uuid;
    uint64_t file_size;
    uint64_t virtual_size;
    uint64_t index_offset;
    uint64_t index_size;
    uint8_t header_version;
    uint8_t header_sub_version;
    uint8_t trailer_version;
    uint8_t trailer_sub_version;

    ReadOnlyLayerMetadata()
        : file_size(0), virtual_size(0), index_offset(0), index_size(0),
          header_version(0), header_sub_version(0), trailer_version(0),
          trailer_sub_version(0) {}
};

/// Rust `helper.rs :: load_readonly_layers_metadata`.
///
/// Rust loads these concurrently and reorders by index afterwards; this is
/// sequential. The result is identical -- the concurrency is throughput for a
/// deep stack, not part of the contract -- and the ordering it carefully
/// restores is simply preserved here.
core::Expected<std::vector<ReadOnlyLayerMetadata>, std::string> LoadReadonlyLayersMetadata(
    const std::vector<std::shared_ptr<VirtualFile> >& files);

/// Rust `stack.rs :: open_files_ro` — one readable view over sealed layers.
///
/// `files` is **bottom-to-top**: oldest base first, newest lower last. That
/// matters, because the merged index tags layers the other way round (tag 0
/// is newest), so the layer vector is reversed to match. Hand them over in
/// the wrong order and every read silently resolves to the wrong layer --
/// no error, just the contents of some other version of the image.
core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> OpenFilesRo(
    const std::vector<std::shared_ptr<VirtualFile> >& files);

/// Rust `types.rs :: PremergedIndexCachePolicy`.
///
/// Merging the indices of a deep stack is pure computation over data that
/// never changes, so the result can be cached on disk and reused by the next
/// open of the same layer set. `read` and `write` are separate so a cache can
/// be consumed without being grown (and vice versa, for a warming pass).
struct PremergedIndexCachePolicy {
    bool read;
    bool write;
    /// Budget for the artifact directory. Enforced by deleting the oldest
    /// artifacts, not by refusing to write: an entry that cannot be cached
    /// still has to be *usable*.
    uint64_t max_dir_bytes;

    PremergedIndexCachePolicy() : read(false), write(false), max_dir_bytes(0) {}

    /// Rust `hybrid` — a sixteenth of the block cache budget, clamped.
    ///
    /// Derived from the cache size rather than configured separately because
    /// the two compete for the same disk; a fraction keeps the index cache
    /// proportionate without another knob to get wrong.
    static PremergedIndexCachePolicy Hybrid(uint32_t cache_size_gb);
    /// Rust `disabled` — merge every time.
    static PremergedIndexCachePolicy Disabled();
};

/// Rust `MIN_PREMERGED_INDEX_CACHE_BYTES` / `MAX_PREMERGED_INDEX_CACHE_BYTES`.
const uint64_t kMinPremergedIndexCacheBytes = 64ULL * 1024 * 1024;
const uint64_t kMaxPremergedIndexCacheBytes = 1024ULL * 1024 * 1024;

/// Rust `PREMERGED_INDEX_EXT` / `_MAGIC` / `_FORMAT_VERSION` /
/// `_MERGE_VERSION` / `_HEADER_LEN`.
///
/// Two versions, not one: `format` covers the artifact's own layout, `merge`
/// covers the semantics of the merge that produced it. Changing how layers
/// are merged must invalidate every artifact even though the file layout is
/// untouched, and a single version could not express that.
extern const char* const kPremergedIndexExt;
extern const char kPremergedIndexMagic[8];
const uint32_t kPremergedIndexFormatVersion = 1;
const uint32_t kPremergedIndexMergeVersion = 1;
/// magic(8) + format(4) + merge(4) + header_len(4) + layer_count(4)
/// + mapping_count(8) + body_len(8) + key_digest(32) + body_checksum(32)
const size_t kPremergedIndexHeaderLen = 104;

/// Rust `stack.rs :: open_files_ro_with_premerged_cache`.
///
/// Falls back to `OpenFilesRo` -- never fails -- whenever the cache cannot
/// help: both directions disabled, an empty `cache_dir`, or a layer set with
/// no stable identity. A cache is an optimisation, so every one of its own
/// failures has to degrade into the uncached path rather than into an error.
core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string>
OpenFilesRoWithPremergedCache(const std::vector<std::shared_ptr<VirtualFile> >& files,
                                 const std::string& cache_dir,
                                 const PremergedIndexCachePolicy& policy);

/// Rust `stack.rs :: is_lsmt` — 1 when `file` is a sealed LSMT layer, 0
/// otherwise.
///
/// Returns 0 rather than an error for anything unreadable, matching Rust.
/// This is a probe used to decide how to open a file, so "cannot tell" and
/// "not one" lead to the same next step; propagating an error would make a
/// caller handle a failure it is about to ignore anyway.
///
/// Both ends of the file are checked, and the order is the test: a valid
/// *header* says the file is LSMT-shaped, and a valid *trailer* says it was
/// sealed. A file with only the header is an unsealed upper, which is still
/// being written and must not be read as a finished layer.
int IsLsmt(VirtualFile* file);

/// Rust `stack.rs :: open_file_index` — load just the index of a sealed layer.
///
/// Reads through the trailer, so it refuses an unsealed file: the index of an
/// upper still being appended to has no fixed location to read from.
core::Expected<std::shared_ptr<ReadOnlyIndex>, std::string> OpenFileIndex(VirtualFile* file);

/// Rust `vfile_io.rs :: read_exact` — fill `dst` completely or fail.
///
/// Distinct from `VirtualFile::ReadAtInto`, where a short read is a success
/// the caller inspects. Here a short read means the layer file is smaller
/// than its own index claims, so there is no sensible partial answer to hand
/// back -- the bytes would be zeros that the index said were data.
core::Expected<core::Unit, std::string> ReadExact(VirtualFile* file, uint64_t offset,
                                                    uint8_t* dst, size_t len);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_LSMT_READONLY_H_
