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
enum LsmtFileType {
    kLsmtFileReadOnly = 0,
    kLsmtFileWritable = 1
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

    // -- VirtualFile -------------------------------------------------------

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override;
    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst,
                                                    size_t len) override;
    /// Always refused -- see the implementation.
    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                 size_t len) override;
    core::Expected<uint64_t, std::string> Size() override;

 private:
    LsmtReadOnlyFile();
    LsmtReadOnlyFile(const LsmtReadOnlyFile&);
    LsmtReadOnlyFile& operator=(const LsmtReadOnlyFile&);

    /// Rust `read_internal_into_generic` — the resolver.
    core::Expected<size_t, std::string> ReadInternalInto(uint64_t offset, uint8_t* dst,
                                                           size_t len);

    std::shared_ptr<LogIndex> index_;
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
