// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/runtime.rs
//
// Materializing an overlaybd *runtime* directory: given a source `image.json`
// that may be shared and read-only, produce a private directory holding a
// rewritten config, a fresh writable upper, and — when the caller asked for a
// different virtual size — a resized image verified by reopening it.
//
// Why a separate directory at all: overlaybd resolves an `image.json`'s paths
// relative to the config, and a device needs its own upper. Writing into the
// source directory would make two devices share one upper and corrupt both.
//
// The directory is *claimed*, not created-if-missing: an existing directory
// may already back a running device, and adopting it would hand the same
// upper to a second writer.
#ifndef AGENTENV_STORAGE_UBLK_DAEMON_RUNTIME_H_
#define AGENTENV_STORAGE_UBLK_DAEMON_RUNTIME_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/image_service.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"
#include "agentenv/storage/ublk-daemon/config.h"

namespace agentenv {
namespace storage {
namespace ublk_daemon {

/// Rust `GIB`.
extern const uint64_t kGib;

/// Rust `RESIZE_OUTPUT_LIMIT` / `RESIZE_OUTPUT_TRUNCATED_MARKER`.
extern const std::size_t kResizeOutputLimit;
extern const char* const kResizeOutputTruncatedMarker;

/// Rust `RUNTIME_IMAGE_CONFIG_FILE` and friends — the names the daemon writes
/// into the runtime dir.
///
/// These are a wire contract with `overlaybd-resize` and with the overlaybd
/// loader, not internal choices: `resultFile` in the generated config points
/// at `kRuntimeResultFile` by name, and a rename here would leave the tool
/// writing its verdict where nothing looks for it.
extern const char* const kRuntimeImageConfigFile;
extern const char* const kRuntimeUpperDataFile;
extern const char* const kRuntimeUpperIndexFile;
extern const char* const kRuntimeResultFile;

/// Rust `InvalidRequest` versus a plain `anyhow::Error`.
///
/// Rust distinguishes the two by downcasting the boxed error at the server
/// boundary. There is no downcast here, so the distinction is carried as a
/// flag. It cannot be dropped: a request for a non-GiB-aligned size is the
/// caller's mistake and must come back as a rejection the client will not
/// retry, while a failure to open the image is ours and is retryable. Folding
/// both into one string would make every bad request look like a daemon fault.
struct RuntimeError {
    bool invalid_request;
    std::string message;

    RuntimeError() : invalid_request(false) {}
};

/// Builds a `RuntimeError` of each flavour.
RuntimeError InvalidRequest(const std::string& message);
RuntimeError InternalError(const std::string& message);

/// Rust `ImageServiceCache` (defined in `server.rs`).
///
/// Lives in this header rather than a server one because `server.rs` is not
/// ported — it is welded to the io_uring control ring — while `runtime.rs`,
/// which needs the cache, is. Keeping the cache out would mean inventing a
/// different seam for the one consumer that exists.
///
/// Keyed by *canonical* path so two spellings of the same global config share
/// one service. Each unique config produces its own `ImageService` with its
/// own cache directory and io pools, so duplicating one would double the
/// memory and give two caches of the same blocks.
class ImageServiceCache {
 public:
    explicit ImageServiceCache(const core::Optional<std::string>& p2p_publish_url);

    /// Rust `get_or_create`.
    ///
    /// The lock is held across creation, not just across the lookup. Dropping
    /// it in between would let two concurrent requests for the same config
    /// each build an `ImageService`; one would be discarded immediately, but
    /// it has already started background work and opened handles.
    core::Expected<overlaybd::ImageService, std::string> GetOrCreate(
        const std::string& global_config);

 private:
    ImageServiceCache(const ImageServiceCache&);
    ImageServiceCache& operator=(const ImageServiceCache&);

    /// Exclusive, not a reader/writer lock: `GetOrCreate` must hold it across
    /// the create, so there is no read-only path to optimize for.
    std::mutex lock_;
    std::map<std::string, overlaybd::ImageService> services_;
    core::Optional<std::string> p2p_publish_url_;
};

/// Rust `ResolvedSourceUpper` — a writable upper the *source* config already
/// names, resolved to absolute paths.
struct ResolvedSourceUpper {
    std::string data_path;
    core::Optional<std::string> index_path;
    core::Optional<std::string> target_path;
    core::Optional<std::string> gzip_index_path;
    overlaybd::UpperMode writable_mode;

    ResolvedSourceUpper() : writable_mode(overlaybd::UpperMode::LogStructured) {}
    bool operator==(const ResolvedSourceUpper& o) const;
    bool operator!=(const ResolvedSourceUpper& o) const { return !(*this == o); }
};

/// Rust `enum ResolvedUpperMode { Existing(..), Create(..), Absent }`.
///
/// A tagged struct because C++11 has no sum type. The three cases are not
/// interchangeable: `Absent` writes an empty upper (read-only device),
/// `Create` lays down fresh files, and `Existing` points the runtime config at
/// the source's own upper — which is why resizing is only allowed for
/// `Create`, since the other two would rewrite storage the daemon does not own.
struct ResolvedUpperMode {
    enum Kind { kAbsent, kCreate, kExisting };

    Kind kind;
    /// Valid only when `kind == kCreate`.
    overlaybd::UpperMode create_mode;
    /// Valid only when `kind == kExisting`.
    ResolvedSourceUpper existing;

    ResolvedUpperMode() : kind(kAbsent), create_mode(overlaybd::UpperMode::LogStructured) {}

    static ResolvedUpperMode Absent();
    static ResolvedUpperMode Create(overlaybd::UpperMode mode);
    static ResolvedUpperMode Existing(const ResolvedSourceUpper& upper);
};

/// Rust `MaterializeOverlaybdRuntimeRequest`.
struct MaterializeOverlaybdRuntimeRequest {
    ImageServiceCache* image_service_cache;
    std::string source_image_config;
    std::string global_config;
    std::string runtime_dir;
    bool read_only;
    overlaybd::UpperMode runtime_upper_mode;
    /// Absent means "keep the base size", which is distinct from asking for a
    /// size that happens to equal it: only the latter is checked for alignment.
    core::Optional<uint64_t> requested_virtual_size;
    /// Absent means "go read it". Supplying it skips opening the image, which
    /// is the expensive part of the happy path.
    core::Optional<uint64_t> known_source_virtual_size;
    /// Null when no resize tool is configured. Resizing then fails as an
    /// invalid request rather than silently producing the base size.
    const ResizeToolSpec* resize_tool;
    /// Dedicated global config for the `overlaybd-resize` child only
    /// (isolated cacheDir, download disabled). `global_config` above still
    /// drives size resolution and the in-process reopen/verification.
    std::string resize_global_config;
    /// Rust `Arc<tokio::sync::Mutex<()>>` — serializes `overlaybd-resize`
    /// children daemon-wide, because they all share the one isolated resize
    /// cacheDir. Null disables the serialization, which is only safe when the
    /// caller knows it is the sole resizer.
    std::mutex* resize_permit;
    bool allow_shrink;

    MaterializeOverlaybdRuntimeRequest()
        : image_service_cache(0),
          read_only(false),
          runtime_upper_mode(overlaybd::UpperMode::LogStructured),
          resize_tool(0),
          resize_permit(0),
          allow_shrink(false) {}
};

/// Rust `MaterializedOverlaybdRuntime`.
///
/// Deliberately **not** RAII: Rust's `ClaimedRuntimeDir` has no `Drop`, and
/// adding one here would delete the directory out from under a device that is
/// already serving from it. Removal is only ever explicit, via `Rollback`.
class MaterializedOverlaybdRuntime {
 public:
    MaterializedOverlaybdRuntime() : actual_virtual_size(0) {}

    std::string runtime_image_config_path;
    uint64_t actual_virtual_size;

    /// Rust `rollback(self)` — removes the claimed directory and everything
    /// the daemon put in it.
    ///
    /// Rust consumes `self`, so a second call cannot compile. Here the claim
    /// is cleared instead, making a repeat call a no-op: a double rollback
    /// after the path was reused for a new device would delete the new one.
    void Rollback();

    const std::string& claimed_runtime_dir() const { return claimed_runtime_dir_; }

    /// Records the claim. Only `MaterializeOverlaybdRuntime` has a reason to
    /// call it; Rust enforces that with module privacy, which has no exact
    /// equivalent here short of a friend declaration that would need this very
    /// type to be complete.
    void SetClaimedRuntimeDir(const std::string& path) { claimed_runtime_dir_ = path; }

 private:
    std::string claimed_runtime_dir_;
};

/// Rust `materialize_overlaybd_runtime`.
///
/// Claims `runtime_dir`, fills it, and removes it again on any failure — a
/// half-built runtime dir would block the next attempt at the same path with
/// "already exists" and leave the caller unable to make progress.
core::Expected<MaterializedOverlaybdRuntime, RuntimeError> MaterializeOverlaybdRuntime(
    const MaterializeOverlaybdRuntimeRequest& request);

/// Rust `resolve_overlaybd_virtual_size`.
///
/// Reads the trailer of the topmost *local* lower first and only opens the
/// whole image if that fails. Opening is correct but costs an `ImageService`
/// plus every layer's handles, which is wasted when one trailer read answers
/// the question.
core::Expected<uint64_t, std::string> ResolveOverlaybdVirtualSize(
    ImageServiceCache* image_service_cache, const std::string& image_config,
    const std::string& global_config);

/// Rust `validate_requested_virtual_size`.
///
/// Exposed because the rejection matrix is the part a caller gets wrong:
/// absent and equal-to-base both pass unchecked, while any *other* value must
/// be non-zero, GiB-aligned, and not a shrink unless shrinking was enabled.
core::Expected<uint64_t, std::string> ValidateRequestedVirtualSize(
    const core::Optional<uint64_t>& requested_virtual_size, uint64_t base_virtual_size,
    bool allow_shrink);

/// Rust `runtime_upper_index_path` — the index file's path, or nothing for a
/// sparse upper, which has no index.
core::Optional<std::string> RuntimeUpperIndexPath(const std::string& runtime_dir,
                                                  overlaybd::UpperMode mode);

/// Rust `impl From<UpperMode> for RwLayout`.
overlaybd::RwLayout RwLayoutFromUpperMode(overlaybd::UpperMode mode);

/// Rust `overlaybd_library_path` — `lib_dir` prepended to the inherited
/// `LD_LIBRARY_PATH`.
///
/// Prepended, not appended: the tool must find the overlaybd libraries it was
/// built against, and a system copy earlier on the path would win. An empty
/// inherited value is dropped rather than joined, since a trailing `:` means
/// "the current directory" to the loader.
core::Expected<std::string, std::string> OverlaybdLibraryPath(
    const std::string& lib_dir, const core::Optional<std::string>& inherited);

/// Rust `read_bounded_output` — reads `fd` to EOF, keeping at most
/// `kResizeOutputLimit` bytes including the truncation marker.
///
/// The marker matters: a diagnostic silently cut short reads as the tool's
/// complete output, and the missing tail is exactly where the real error
/// usually is. The limit exists because the output goes into an error string
/// and a looping tool could otherwise print without bound.
core::Expected<std::string, std::string> ReadBoundedOutput(int fd);

/// Rust `run_resize_tool` — one `overlaybd-resize` child, to completion.
///
/// Runs with `cwd` set to the config's directory, because the generated
/// `image.json` names its upper and `resultFile` relatively and the tool
/// resolves them against the cwd, not against the config.
///
/// Both stdout and stderr land in the error on a non-zero exit: the tool
/// reports some failures on one and some on the other, and dropping either
/// leaves failures that say nothing. A timeout is reported as a timeout rather
/// than as a signal death, even though the child is SIGKILLed to get there.
core::Expected<core::Unit, std::string> RunResizeTool(const ResizeToolSpec& tool,
                                                      const std::string& image_config,
                                                      const std::string& resize_global_config,
                                                      uint64_t target_size);

/// Rust `resolve_upper_mode`.
core::Expected<ResolvedUpperMode, std::string> ResolveUpperMode(
    const std::string& image_config_path, bool read_only,
    overlaybd::UpperMode runtime_upper_mode);

/// Rust `resolve_source_upper` — the source config's own writable upper, if it
/// declares a complete one.
core::Expected<core::Optional<ResolvedSourceUpper>, std::string> ResolveSourceUpper(
    const std::string& image_config_path);

/// Rust `materialize_overlaybd_image_config` — the source config with every
/// lower path retargeted at `runtime_dir` and the chosen `upper` substituted.
core::Expected<core::Json, std::string> MaterializeOverlaybdImageConfig(
    const std::string& image_config_path, const std::string& runtime_dir,
    const ResolvedUpperMode& upper_mode);

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_DAEMON_RUNTIME_H_
