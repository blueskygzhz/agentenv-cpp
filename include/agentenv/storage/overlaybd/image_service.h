// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/image/image_service.rs
#ifndef AGENTENV_STORAGE_OVERLAYBD_IMAGE_SERVICE_H_
#define AGENTENV_STORAGE_OVERLAYBD_IMAGE_SERVICE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/bk_download.h"
#include "agentenv/storage/overlaybd/cache_pool.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/oss.h"
#include "agentenv/storage/overlaybd/registryfs.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Declared, not included: `image_file.h` includes this header back, because
/// an `ImageFile` holds the service that opened it. Rust has the same cycle
/// and resolves it by being one crate.
class ImageFile;

/// Rust `CacheDownloadRequest` — one layer's background download, collected
/// while opening an image and submitted once the whole image is open.
///
/// Deliberately not submitted at open time: a half-open image whose open
/// later fails would otherwise leave downloads running for layers nobody
/// reads.
struct CacheDownloadRequest {
    std::shared_ptr<CachedFile> file;
    DownloadConfig config;
};

/// Rust `StartupPackPrefetch` — parameters for one startup pack prefetch.
struct StartupPackPrefetch {
    std::string url;
    uint64_t pack_size;
    std::string index_sha256;
    uint64_t mem_virtual_size;
    /// Hard bound on queueing plus downloading.
    uint64_t timeout_ms;

    StartupPackPrefetch() : pack_size(0), mem_virtual_size(0), timeout_ms(0) {}
};

/// Rust `RemoteOpenMode`.
enum RemoteOpenMode {
    /// Reads go straight to the P2P accelerator, which is its own cache.
    kRemoteOpenDirect,
    /// Reads go through the local file cache.
    kRemoteOpenCached
};

/// The lazily built remote side of a service: registry client, object store
/// and file cache. Rust `RemoteRuntime`.
///
/// One unit because they are built together and share a single init: a
/// caller that needs any of them pays for all three.
struct RemoteRuntime {
    std::shared_ptr<RegistryFs> underlay_registryfs;
    /// Absent when `ossConfig.enable` is false.
    std::shared_ptr<OssBackend> oss_backend;
    /// Absent only if the cache type selects no backend.
    std::shared_ptr<FileCacheBackend> file_cache;
};

/// The shared state behind every `ImageService` handle. Rust
/// `ImageServiceInner`.
class ImageServiceInner;

/// Rust `ImageService` — opens images against a `GlobalConfig`.
///
/// A cheap, copyable handle over shared state, exactly as Rust's
/// `#[derive(Clone)] struct ImageService { inner: Arc<..> }`. Copies are the
/// point: an `ImageFile` keeps one, which is what guarantees the file cache
/// outlives every `CachedFile` opened through it (see `CachedFile`'s lifetime
/// note in `cache_pool.h`).
///
/// Deviation from Rust, decided once for the whole port: there is no
/// `remote_io_runtime`. Rust pins remote I/O to a dedicated tokio runtime so
/// per-connection tasks do not bind to a per-device ublk queue runtime that
/// is dropped on teardown. A synchronous port has no runtimes to pin work to;
/// `globalConfig.remoteIoWorkers` is carried for config fidelity only, as
/// recorded on the field itself in `config.h`.
class ImageService {
 public:
    /// Rust `new` — from an in-memory config. `ConfigPath()` is then empty.
    static core::Expected<ImageService, std::string> New(const GlobalConfig& global_config);

    /// Rust `from_config_path`.
    static core::Expected<ImageService, std::string> FromConfigPath(const std::string& path);

    /// Rust `from_config_path_with_p2p_publish_url`.
    static core::Expected<ImageService, std::string> FromConfigPathWithP2pPublishUrl(
        const std::string& path, const core::Optional<std::string>& p2p_publish_url);

    /// Rust `config_path` — empty when built from an in-memory config.
    const std::string& ConfigPath() const;

    /// Rust `global_config`. Named `Get...` because `GlobalConfig()` would
    /// hide the type of the same name.
    const GlobalConfig& GetGlobalConfig() const;

    /// Rust `io_engine`.
    uint32_t IoEngine() const;

    /// Rust `p2p_uuid_address` — the `/p2p-uuid` facade derived from the
    /// configured `/p2p-http` address, or nothing when P2P is disabled or the
    /// address is not a `/p2p-http` one.
    core::Optional<std::string> P2pUuidAddress() const;

    /// Rust `enable_acceleration` — probe the configured P2P address and
    /// switch the remote open mode accordingly. Returns whether acceleration
    /// is on.
    ///
    /// Also retargets an already-built registry client, so the mode can
    /// change after the remote runtime exists.
    bool EnableAcceleration();

    /// Rust `load_image_config` — read, resolve relative paths against the
    /// config's own location, then validate.
    core::Expected<ImageConfig, std::string> LoadImageConfig(const std::string& path) const;

    /// Rust `create_image_file` — the full open path, recording the outcome
    /// in the image config's `resultFile`.
    core::Expected<std::shared_ptr<ImageFile>, std::string> CreateImageFile(
        const std::string& path);

    /// Rust `oss_backend` — null when `ossConfig.enable` is false.
    ///
    /// **Initializes the whole remote runtime**, including the registry
    /// client and the file cache: they share one init. A caller serving only
    /// local images should check `ossConfig.enable` instead of asking.
    ///
    /// Borrowed from the service, as Rust's `Option<&OssBackend>` is: a
    /// second `OssBackend` over the same config would carry its own operator
    /// cache and re-resolve credentials, and a `credentialProcess` costs a
    /// subprocess per miss.
    core::Expected<std::shared_ptr<OssBackend>, std::string> GetOssBackend();

    /// Rust `open_remote_blob` / `open_remote_blob_with_size` — the cached
    /// read path. An absent size means "ask the source".
    core::Expected<std::shared_ptr<VirtualFile>, std::string> OpenRemoteBlob(
        const std::string& url);
    core::Expected<std::shared_ptr<VirtualFile>, std::string> OpenRemoteBlobWithSize(
        const std::string& url, const core::Optional<uint64_t>& source_size);

    /// Rust `open_source_blob` / `open_source_blob_with_size` — the raw
    /// backend source, never wrapped in the file cache.
    core::Expected<std::shared_ptr<VirtualFile>, std::string> OpenSourceBlob(
        const std::string& url);
    core::Expected<std::shared_ptr<VirtualFile>, std::string> OpenSourceBlobWithSize(
        const std::string& url, const core::Optional<uint64_t>& source_size);

    /// Rust `open_remote_blob_for_bk_download_with_size` — the cached handle
    /// plus the request that will later background-fill it.
    ///
    /// Fails when no file cache is configured: a background download has
    /// nowhere to land.
    core::Expected<std::pair<std::shared_ptr<VirtualFile>, CacheDownloadRequest>, std::string>
    OpenRemoteBlobForBkDownloadWithSize(const std::string& url,
                                        const core::Optional<uint64_t>& source_size,
                                        const DownloadConfig& config);

    /// Rust `submit_bk_downloads` — hand the collected requests to the cache
    /// scheduler.
    ///
    /// Best effort by design: a shut-down scheduler is downgraded to a
    /// warning, because foreground `CachedFile` reads still refill missing
    /// blocks from the origin. A missing cache backend is a configuration
    /// error and does fail.
    core::Expected<core::Unit, std::string> SubmitBkDownloads(
        const std::vector<CacheDownloadRequest>& requests,
        const core::Optional<std::string>& device_key);

    /// Rust `prefetch_startup_pack` — bind a v2 startup memory pack to the
    /// image's OSS lowers and hand the download to the scheduler.
    ///
    /// Reports absence (not an error) when the image has no bindable OSS
    /// lowers or no cache backend: the prefetch is an accelerator, and
    /// on-demand reads proceed either way.
    core::Expected<core::Optional<StartupPackHandle>, std::string> PrefetchStartupPack(
        const std::string& image_config_path, const StartupPackPrefetch& pack);

    /// Rust `export_upper_as_oss_sealed` — seal the writable upper into a
    /// staging file and upload it. The staging file is always removed.
    core::Expected<core::Unit, std::string> ExportUpperAsOssSealed(ImageFile* image,
                                                                      const std::string& dest_url);

    /// Rust `is_oss_url` — an `oss://` or `s3://` URL.
    static bool IsOssUrl(const std::string& url);

    // -- test seams, matching Rust's `#[cfg(test)]` methods ----------------

    /// Rust `cached_file_stats`.
    core::Expected<core::Optional<CachedFileStats>, std::string> CachedFileStatsFor(
        const std::string& source);

    /// Rust `file_cache_for_test`.
    core::Expected<std::shared_ptr<FileCacheBackend>, std::string> FileCacheForTest();

    /// Rust `set_remote_mode_direct_for_test`.
    void SetRemoteModeDirectForTest();

 private:
    explicit ImageService(const std::shared_ptr<ImageServiceInner>& inner) : inner_(inner) {}

    static core::Expected<ImageService, std::string> WithGlobalConfigAndP2pPublishUrl(
        const GlobalConfig& global_config, const std::string& config_path,
        const core::Optional<std::string>& p2p_publish_url);

    /// Rust `build_file_cache` — null only for a cache type selecting none.
    static core::Expected<std::shared_ptr<FileCacheBackend>, std::string> BuildFileCache(
        const GlobalConfig& cfg);

    /// Rust `remote_runtime` — the `OnceCell` init. A failed init is not
    /// cached, matching `get_or_try_init`.
    ///
    /// Returns a borrowed pointer, as Rust returns `&RemoteRuntime`: the
    /// runtime is owned by the shared inner state and is never replaced once
    /// built, so the pointer stays valid for as long as any `ImageService`
    /// handle does.
    core::Expected<RemoteRuntime*, std::string> GetRemoteRuntime();

    /// Rust `build_remote_runtime`.
    ///
    /// Takes the accelerator address rather than reading it, because the
    /// caller holds the runtime lock and the address lives behind the mode
    /// lock -- see the lock-order note in `GetRemoteRuntime`.
    core::Expected<RemoteRuntime, std::string> BuildRemoteRuntime(
        const std::string& accelerate_address);

    /// Rust `current_accelerate_address`.
    std::string CurrentAccelerateAddress() const;

    /// Rust `open_backend_source_with_size` — registry or object store,
    /// chosen by URL scheme.
    core::Expected<std::shared_ptr<VirtualFile>, std::string> OpenBackendSourceWithSize(
        const std::string& url, const core::Optional<uint64_t>& source_size);

    /// Rust `open_cached_blob`.
    static core::Expected<std::shared_ptr<CachedFile>, std::string> OpenCachedBlob(
        FileCacheBackend* cache, const std::string& url,
        const std::shared_ptr<VirtualFile>& source,
        const core::Optional<uint64_t>& source_size);

    /// Rust `set_result_file` — a no-op for an empty filename.
    core::Expected<core::Unit, std::string> SetResultFile(const std::string& filename,
                                                             const std::string& data) const;

    std::shared_ptr<ImageServiceInner> inner_;
};

/// Rust `collect_startup_pack_layers` — the OSS lowers of a memory image as
/// `(url, digest, size)`, bottom to top.
///
/// Reports absence when *any* lower cannot be bound: the pack binds by exact
/// object-table match, so a partial binding is useless.
struct StartupPackLayerRef {
    std::string url;
    std::string digest;
    uint64_t size;

    StartupPackLayerRef() : size(0) {}
};
bool CollectStartupPackLayers(const ImageConfig& image_config,
                              std::vector<StartupPackLayerRef>* out);

/// Rust `check_accelerate_url` — whether a TCP connection to the address
/// succeeds within the connect timeout.
bool CheckAccelerateUrl(const std::string& address);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_IMAGE_SERVICE_H_
