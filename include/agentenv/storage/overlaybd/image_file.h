// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/image/image_file.rs
#ifndef AGENTENV_STORAGE_OVERLAYBD_IMAGE_FILE_H_
#define AGENTENV_STORAGE_OVERLAYBD_IMAGE_FILE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/rwlock.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/image_service.h"
#include "agentenv/storage/overlaybd/lsmt_readonly.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"
#include "agentenv/storage/overlaybd/prefetch.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `DEFAULT_BLOCK_SIZE`.
const uint32_t kImageDefaultBlockSize = 512;

/// Rust `IO_ENGINE_LIBAIO` — the engine id that selects direct I/O for a
/// local lower.
const uint32_t kIoEngineLibaio = 2;

/// Rust `RestackSnapshotTerminalFailure`'s message prefix.
///
/// Marks the failures of `CreateSnapshotAndRestack` that happened *after* the
/// live runtime was mutated, which the caller must treat as terminal for the
/// image rather than retrying.
///
/// A prefix rather than a type because errors here are strings; the same
/// trade-off `DownloadErrorCategory` makes in `bk_download.h`. Compare with
/// `IsRestackSnapshotTerminalFailure` rather than by hand so the spelling
/// lives in one place.
extern const char kRestackSnapshotTerminalPrefix[];

/// Wrap `message` as a terminal restack failure.
std::string MakeRestackSnapshotTerminalFailure(const std::string& message);

/// Whether `error` was produced by `MakeRestackSnapshotTerminalFailure`.
bool IsRestackSnapshotTerminalFailure(const std::string& error);

/// Rust `ensure_sealable_upper` — refuse an upper that cannot be sealed,
/// naming the `operation` the caller asked for.
///
/// Which files must be present is decided by the mode: a log-structured or
/// hybrid upper keeps its mappings in a separate index, and sealing one
/// without that index would produce a layer that owns nothing. Sparse carries
/// its index inside the data file, so the data path is all it needs.
core::Expected<core::Unit, std::string> EnsureSealableUpper(const UpperConfig& upper,
                                                               const std::string& operation);

/// Rust `ImageFileBase` — the image is either read-only or has a writable
/// upper, and nearly every operation dispatches on which.
///
/// Rust models this as an enum, so "exactly one of the two" is a type-level
/// fact. Two pointers cannot express that, so `Open` and
/// `CreateSnapshotAndRestack` are the only places that set them and both
/// leave exactly one non-null; `Writable` and `IsReadOnly` are written so
/// that a violation of the invariant degrades into a refusal rather than
/// serving the wrong layer.
class ImageFileBase {
 public:
    ImageFileBase() {}

    /// True only for a base that *is* a sealed stack. A base with neither
    /// pointer set is not read-only, it is unusable, and must not be reported
    /// as the former.
    bool IsReadOnly() const { return read_only_ && !read_write_; }
    /// Null for a read-only image; the writable upper otherwise.
    LsmtFile* Writable() const { return read_write_.get(); }
    bool IsOpen() const { return static_cast<bool>(read_only_) || static_cast<bool>(read_write_); }

    std::shared_ptr<LsmtReadOnlyFile> read_only_;
    std::shared_ptr<LsmtFile> read_write_;
};

/// Rust `LiveImageState` — everything an open image can have swapped under it
/// by a restack.
struct LiveImageState {
    ImageConfig config;
    ImageFileBase base;
    std::string premerged_index_cache_dir;
    PremergedIndexCachePolicy premerged_index_cache_policy;
};

/// Rust `ImageFile` — one open overlaybd image.
///
/// Reads dispatch to the stacked LSMT layers; writes go to the upper, if
/// there is one. Terminal operations (`CloseSeal`, `CreateSnapshotAndRestack`)
/// seal that upper and are documented individually.
class ImageFile : public VirtualFile {
 public:
    ~ImageFile();

    /// Rust `open` — build the layer stack, submit the background downloads
    /// it collected, and replay the prefetch trace if there is one.
    ///
    /// `device_key` identifies the consumer for the background downloads'
    /// readiness wait; absent for a consumer that is not a sandbox.
    static core::Expected<std::shared_ptr<ImageFile>, std::string> Open(
        const ImageConfig& config, const ImageService& image_service,
        const core::Optional<std::string>& device_key);

    /// Rust `size_bytes` / `num_lbas` / the public `block_size` field.
    uint64_t SizeBytes() const;
    uint64_t NumLbas() const;
    uint32_t BlockSize() const { return block_size_; }

    /// Rust `is_read_only`.
    bool IsReadOnly() const;

    /// Rust `data_stat` — unique bytes mapped by the merged index
    /// (`valid_data_size`) and the top layer's physical size
    /// (`total_data_size`).
    ///
    /// A cheap in-memory index scan; feeds observability of layer bloat and
    /// TRIM headroom.
    core::Expected<DataStat, std::string> GetDataStat() const;

    /// Rust `get_uuid` — the identity of the layer at `layer_idx`.
    core::Expected<std::string, std::string> GetUuid(size_t layer_idx) const;

    /// Rust `compact` — flatten the whole image into `as_file`.
    core::Expected<core::Unit, std::string> Compact(
        const std::shared_ptr<VirtualFile>& as_file) const;

    /// Rust `export_upper_as_sealed` — write the upper's own mappings out as
    /// a sealed layer, leaving this image writable and usable.
    core::Expected<core::Unit, std::string> ExportUpperAsSealed(const CommitArgs& args) const;

    /// Rust `close_seal` — seal the writable upper and move the finished
    /// layer to `output_layer_path`.
    ///
    /// Terminal: **the image must not be used after this returns.** Its upper
    /// is sealed, so every write answers "File is sealed.", and the data file
    /// no longer exists at the path the config still names. Nothing tries to
    /// leave a recoverable state behind, because there is none worth
    /// recovering to -- the caller asked for this upper to become a layer.
    ///
    /// The move is a plain rename, so **an existing file at
    /// `output_layer_path` is replaced.**
    core::Expected<core::Optional<LayerDescriptor>, std::string> CloseSeal(
        const std::string& output_layer_path);

    /// Rust `create_snapshot_and_restack` — the same seal as `CloseSeal`,
    /// followed by stacking a fresh upper on the result, for a caller that
    /// has to keep serving.
    ///
    /// A failure after the live runtime has been mutated is reported through
    /// `IsRestackSnapshotTerminalFailure`, and is terminal for this image.
    core::Expected<core::Optional<LayerDescriptor>, std::string> CreateSnapshotAndRestack(
        const std::string& output_layer_path);

    // -- VirtualFile -------------------------------------------------------

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override;
    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst,
                                                      size_t len) override;
    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                   size_t len) override;
    core::Expected<uint64_t, std::string> Size() override;
    core::Expected<core::Unit, std::string> Sync() override;
    core::Expected<core::Unit, std::string> Truncate(uint64_t size) override;
    core::Expected<core::Unit, std::string> Discard(uint64_t offset, uint64_t len) override;
    core::Expected<core::Optional<uint64_t>, std::string> SeekData(uint64_t offset) override;
    core::Expected<core::Optional<uint64_t>, std::string> SeekHole(uint64_t offset) override;

 private:
    /// Takes the service by value because the field below must be a live
    /// handle from the moment the image exists.
    explicit ImageFile(const ImageService& image_service);
    ImageFile(const ImageFile&);
    ImageFile& operator=(const ImageFile&);

    /// Rust `refresh_size_metadata` — recompute the size from the base,
    /// because a write may have grown the image. Returns the fresh size, as
    /// Rust does, so `Size()` cannot hand back a value another thread
    /// replaced between the refresh and the read.
    ///
    /// Requires the state lock to be held. Shared is enough: the size is read
    /// from the base, and the counters it updates are atomic. Holding it
    /// across both halves is the point -- Rust calls this while still holding
    /// its read guard, so a restack cannot swap the base in between.
    core::Expected<uint64_t, std::string> RefreshSizeMetadataLocked();

    /// Guards `state_`, and is a reader/writer lock because Rust's is.
    ///
    /// Rust takes a **shared** read guard for every data-path operation --
    /// `read_at`, `write_at`, `discard`, `truncate`, `sync`, `seek_*` -- and
    /// an **exclusive** one only in `close_seal` and
    /// `create_snapshot_and_restack`, which replace the layer stack. Writes
    /// are shared because the upper does its own locking underneath. A plain
    /// mutex would serialise every concurrent read of a block device's
    /// backing image, which is a change in behaviour rather than in
    /// implementation.
    core::RwLock state_lock_;
    LiveImageState state_;

    /// Rust keeps these in `AtomicU64`, and `size_bytes()` / `num_lbas()`
    /// load them without taking the state lock at all. Atomic here for the
    /// same reason: these two are read per I/O by callers that only want the
    /// geometry.
    std::atomic<uint64_t> size_bytes_;
    std::atomic<uint64_t> num_lbas_;
    const uint32_t block_size_;

    std::shared_ptr<Prefetcher> prefetcher_;

    /// Not a field Rust has, and required here.
    ///
    /// A lower opened through the file cache is a `CachedFile`, which holds a
    /// **raw** `FileCacheBackend*` (see the lifetime note on `CachedFile` in
    /// `cache_pool.h`). Rust's `CachedFile` instead holds a `FileCacheBackend`
    /// clone and keeps the backend alive by itself, which is why Rust's
    /// `ImageFile` can drop the service after `open` returns. Here, dropping
    /// the last `ImageService` while this image is open would free the
    /// backend under those lowers. Holding one copy -- it is a handle over
    /// shared state -- makes the image's own lifetime sufficient.
    ///
    /// No cycle: the service never holds an `ImageFile`.
    ImageService image_service_;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_IMAGE_FILE_H_
