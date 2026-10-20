// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/image/image_service.rs
#include "agentenv/storage/overlaybd/image_service.h"

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "agentenv/core/fs.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/url.h"
#include "agentenv/storage/overlaybd/image_file.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"
#include "agentenv/storage/overlaybd/startup_pack_task.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

/// Rust `CONNECT_TIMEOUT`.
const long kConnectTimeoutMs = 1000;

/// Rust `OSS_SEALED_UPLOAD_PART_SIZE` / `_CONCURRENCY`.
///
/// Sized for the ceiling rather than for memory: this entry point is library
/// surface and can be handed an image of any size, and the part size is what
/// bounds how large that may be. S3 allows 10,000 parts, so 64 MiB caps one
/// object at ~625 GiB where 16 MiB would cap it at 156 GiB -- and exceeding
/// the cap is not caught up front, it fails partway through the upload. The
/// price is memory: peak is `(2 * concurrency + 2) * part_size`.
const size_t kOssSealedUploadPartSize = 64 * 1024 * 1024;
const size_t kOssSealedUploadConcurrency = 4;

/// Rust's closure in `build_file_cache`: store a cached blob under its URL's
/// last path segment.
std::string BasenameCacheTransform(const std::string& origin) {
    const std::string::size_type slash = origin.find_last_of('/');
    const std::string basename =
        slash == std::string::npos ? origin : origin.substr(slash + 1);
    // Rust's closure returns `None` for an empty basename, which leaves the
    // origin untouched; the C++ hook has no "no opinion" return, so the
    // identity mapping expresses the same thing.
    if (basename.empty()) return origin;
    return "/" + basename;
}

/// The trailing path component, for logs only.
std::string BasenameOf(const std::string& path) {
    const std::string::size_type slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string TrimTrailingSlashes(const std::string& s) {
    std::string::size_type end = s.size();
    while (end > 0 && s[end - 1] == '/') --end;
    return s.substr(0, end);
}

/// Rust `url::Url::port_or_known_default().unwrap_or(80)`.
///
/// Narrowed to the schemes an accelerator address can plausibly use: Rust's
/// table also covers ftp/ws/wss/file, none of which can appear here, and all
/// of which would fall back to 80 anyway except `file` (which has no host and
/// is rejected before this point).
uint16_t PortOrKnownDefault(const core::Url& url) {
    if (url.port.has_value()) return *url.port;
    if (url.scheme == "https") return 443;
    return 80;
}

/// One TCP connect attempt bounded by `kConnectTimeoutMs`, matching Rust's
/// `TcpStream::connect_timeout`.
///
/// Non-blocking plus `select` rather than a blocking connect: a blocking
/// connect to a black-holed address hangs for the kernel's SYN timeout
/// (~2 minutes), and this probe runs on the image-open path.
bool ConnectWithinTimeout(const struct addrinfo* ai) {
    const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) return false;

    bool connected = false;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
        const int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            connected = true;
        } else if (errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            struct timeval tv;
            tv.tv_sec = kConnectTimeoutMs / 1000;
            tv.tv_usec = (kConnectTimeoutMs % 1000) * 1000;
            if (::select(fd + 1, 0, &wset, 0, &tv) > 0) {
                // Readiness alone is not success: a refused connection also
                // reports writable, and only SO_ERROR separates the two.
                int err = 0;
                socklen_t len = sizeof(err);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                    connected = true;
                }
            }
        }
    }

    ::close(fd);
    return connected;
}

}  // namespace

/// Rust `ImageServiceInner`.
class ImageServiceInner {
 public:
    ImageServiceInner(const GlobalConfig& cfg, const std::string& path,
                      const core::Optional<std::string>& publish_url)
        : config_path(path),
          global_config(cfg),
          p2p_publish_url(publish_url),
          remote_mode(kRemoteOpenCached) {}

    std::string config_path;
    GlobalConfig global_config;
    /// Carried for the publish side of the P2P facade; read by callers that
    /// register an image, not by the open path.
    core::Optional<std::string> p2p_publish_url;

    /// Rust's `OnceCell<RemoteRuntime>`. Guarded rather than atomic because
    /// initialization is the slow path (it builds a registry client and
    /// scans the cache directory) and must happen exactly once.
    std::unique_ptr<RemoteRuntime> remote_runtime;
    mutable std::mutex remote_runtime_mutex;

    /// Rust's `parking_lot::RwLock<RemoteOpenMode>`. A plain mutex: the
    /// critical section is a single enum load or store.
    RemoteOpenMode remote_mode;
    mutable std::mutex remote_mode_mutex;
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

core::Expected<ImageService, std::string> ImageService::WithGlobalConfigAndP2pPublishUrl(
    const GlobalConfig& global_config, const std::string& config_path,
    const core::Optional<std::string>& p2p_publish_url) {
    // No `remote_io_runtime` to build here; see the class comment.
    const std::shared_ptr<ImageServiceInner> inner(
        new ImageServiceInner(global_config, config_path, p2p_publish_url));
    return ImageService(inner);
}

core::Expected<ImageService, std::string> ImageService::New(const GlobalConfig& global_config) {
    return WithGlobalConfigAndP2pPublishUrl(global_config, std::string(),
                                            core::Optional<std::string>());
}

core::Expected<ImageService, std::string> ImageService::FromConfigPath(const std::string& path) {
    return FromConfigPathWithP2pPublishUrl(path, core::Optional<std::string>());
}

core::Expected<ImageService, std::string> ImageService::FromConfigPathWithP2pPublishUrl(
    const std::string& path, const core::Optional<std::string>& p2p_publish_url) {
    const core::Expected<GlobalConfig, std::string> cfg = LoadGlobalConfig(path);
    if (!cfg.ok()) return core::make_unexpected(cfg.error());
    return WithGlobalConfigAndP2pPublishUrl(cfg.value(), path, p2p_publish_url);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const std::string& ImageService::ConfigPath() const { return inner_->config_path; }

const GlobalConfig& ImageService::GetGlobalConfig() const { return inner_->global_config; }

uint32_t ImageService::IoEngine() const { return inner_->global_config.io_engine; }

core::Optional<std::string> ImageService::P2pUuidAddress() const {
    const P2PConfig& p2p = inner_->global_config.p2p_config;
    if (!p2p.enable) return core::Optional<std::string>();

    const std::string trimmed = TrimTrailingSlashes(p2p.address);
    const std::string suffix = "/p2p-http";
    // Rust `strip_suffix`: a facade address that is not the `/p2p-http` one
    // yields nothing rather than a guessed URL.
    if (trimmed.size() < suffix.size() ||
        trimmed.compare(trimmed.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return core::Optional<std::string>();
    }
    const std::string base = trimmed.substr(0, trimmed.size() - suffix.size());
    return core::Optional<std::string>(base + "/p2p-uuid");
}

std::string ImageService::CurrentAccelerateAddress() const {
    RemoteOpenMode mode;
    {
        std::lock_guard<std::mutex> guard(inner_->remote_mode_mutex);
        mode = inner_->remote_mode;
    }
    if (mode == kRemoteOpenDirect) return inner_->global_config.p2p_config.address;
    return std::string();
}

bool ImageService::EnableAcceleration() {
    const P2PConfig& p2p = inner_->global_config.p2p_config;
    // Probed *before* the lock is taken, as Rust does: `CheckAccelerateUrl`
    // spends up to the connect timeout on the network, and holding the mode
    // lock across it would stall every concurrent remote open for that long.
    const bool reachable = p2p.enable && CheckAccelerateUrl(p2p.address);
    const std::string accelerate_address = reachable ? p2p.address : std::string();

    {
        std::lock_guard<std::mutex> guard(inner_->remote_mode_mutex);
        inner_->remote_mode = reachable ? kRemoteOpenDirect : kRemoteOpenCached;
    }

    // Only an already-built runtime is retargeted; building one here would
    // turn a mode probe into a full remote init.
    {
        std::lock_guard<std::mutex> guard(inner_->remote_runtime_mutex);
        if (inner_->remote_runtime && inner_->remote_runtime->underlay_registryfs) {
            inner_->remote_runtime->underlay_registryfs->SetAccelerateAddress(accelerate_address);
        }
    }
    return !accelerate_address.empty();
}

void ImageService::SetRemoteModeDirectForTest() {
    std::lock_guard<std::mutex> guard(inner_->remote_mode_mutex);
    inner_->remote_mode = kRemoteOpenDirect;
}

// ---------------------------------------------------------------------------
// Remote runtime
// ---------------------------------------------------------------------------

core::Expected<std::shared_ptr<FileCacheBackend>, std::string> ImageService::BuildFileCache(
    const GlobalConfig& cfg) {
    const std::string& type = cfg.cache_config.cache_type;
    if (!type.empty() && type != "file") {
        if (type == "ocf" || type == "download") {
            return core::make_unexpected("cache type " + type +
                                         " is not migrated in Rust image_service yet");
        }
        return core::make_unexpected(std::string("unknown cache type: ") + type);
    }

    const core::Expected<core::Unit, std::string> made =
        core::fs::CreateDirAll(cfg.cache_config.cache_dir);
    if (!made.ok()) return core::make_unexpected(made.error());

    core::Expected<FileCacheBackendOptions, std::string> options =
        FileCacheBackendOptions::FromCacheConfig(cfg.cache_config);
    if (!options.ok()) return core::make_unexpected(options.error());

    // The cache-owned background download scheduler takes its node-level caps
    // from the global download config; per-image overrides never resize them.
    FileCacheBackendOptions normalized = options.value();
    normalized.bk_download_max_inflight_blocks = cfg.download.max_inflight_blocks;
    normalized.bk_download_max_concurrent_files = cfg.download.max_concurrent_files;
    normalized.bk_download_block_size = cfg.download.block_size;

    return FileCacheBackend::WithOptionsAndTransFunc(normalized, &BasenameCacheTransform);
}

core::Expected<RemoteRuntime, std::string> ImageService::BuildRemoteRuntime() {
    RemoteRuntime runtime;

    const core::Expected<std::shared_ptr<RegistryFs>, std::string> registryfs =
        RegistryFs::FromGlobalConfig(inner_->global_config);
    if (!registryfs.ok()) return core::make_unexpected(registryfs.error());
    runtime.underlay_registryfs = registryfs.value();
    runtime.underlay_registryfs->SetAccelerateAddress(CurrentAccelerateAddress());

    if (inner_->global_config.oss_config.enable) {
        const core::Expected<std::shared_ptr<OssBackend>, std::string> oss =
            OssBackend::New(inner_->global_config.oss_config);
        if (!oss.ok()) return core::make_unexpected(oss.error());
        runtime.oss_backend = oss.value();
    }

    const core::Expected<std::shared_ptr<FileCacheBackend>, std::string> cache =
        BuildFileCache(inner_->global_config);
    if (!cache.ok()) return core::make_unexpected(cache.error());
    runtime.file_cache = cache.value();

    return runtime;
}

core::Expected<RemoteRuntime*, std::string> ImageService::GetRemoteRuntime() {
    std::lock_guard<std::mutex> guard(inner_->remote_runtime_mutex);
    if (inner_->remote_runtime) return inner_->remote_runtime.get();

    const core::Expected<RemoteRuntime, std::string> built = BuildRemoteRuntime();
    // A failed init is not cached, matching Rust's `get_or_try_init`: the
    // next caller retries rather than inheriting a permanent failure.
    if (!built.ok()) return core::make_unexpected(built.error());

    inner_->remote_runtime.reset(new RemoteRuntime(built.value()));
    return inner_->remote_runtime.get();
}

core::Expected<std::shared_ptr<OssBackend>, std::string> ImageService::GetOssBackend() {
    const core::Expected<RemoteRuntime*, std::string> runtime = GetRemoteRuntime();
    if (!runtime.ok()) return core::make_unexpected(runtime.error());
    return runtime.value()->oss_backend;
}

core::Expected<std::shared_ptr<FileCacheBackend>, std::string> ImageService::FileCacheForTest() {
    const core::Expected<RemoteRuntime*, std::string> runtime = GetRemoteRuntime();
    if (!runtime.ok()) return core::make_unexpected(runtime.error());
    return runtime.value()->file_cache;
}

core::Expected<core::Optional<CachedFileStats>, std::string> ImageService::CachedFileStatsFor(
    const std::string& source) {
    const core::Expected<RemoteRuntime*, std::string> runtime = GetRemoteRuntime();
    if (!runtime.ok()) return core::make_unexpected(runtime.error());
    if (!runtime.value()->file_cache) return core::Optional<CachedFileStats>();

    CachedFileStats stats;
    if (!runtime.value()->file_cache->FileStats(source, &stats)) {
        return core::Optional<CachedFileStats>();
    }
    return core::Optional<CachedFileStats>(stats);
}

// ---------------------------------------------------------------------------
// Opening blobs
// ---------------------------------------------------------------------------

bool ImageService::IsOssUrl(const std::string& url) {
    const core::Expected<core::Url, std::string> parsed = core::Url::Parse(url);
    if (!parsed.ok()) return false;
    return parsed.value().scheme == "s3" || parsed.value().scheme == "oss";
}

core::Expected<std::shared_ptr<VirtualFile>, std::string>
ImageService::OpenBackendSourceWithSize(const std::string& url,
                                        const core::Optional<uint64_t>& source_size) {
    const core::Expected<RemoteRuntime*, std::string> runtime = GetRemoteRuntime();
    if (!runtime.ok()) return core::make_unexpected(runtime.error());

    if (IsOssUrl(url)) {
        if (!runtime.value()->oss_backend) {
            return core::make_unexpected(std::string("OSS backend not enabled in config"));
        }
        const core::Expected<std::shared_ptr<OssFile>, std::string> opened =
            runtime.value()->oss_backend->OpenWithSizeHint(
                url, source_size.has_value() ? *source_size : 0);
        if (!opened.ok()) return core::make_unexpected(opened.error());
        return std::shared_ptr<VirtualFile>(opened.value());
    }

    // A zero hint is what the registry backend reads as "probe the size", so
    // the absent case needs no separate call as it does in Rust.
    const core::Expected<std::shared_ptr<RegistryFile>, std::string> opened =
        runtime.value()->underlay_registryfs->OpenFile(
            url, source_size.has_value() ? *source_size : 0);
    if (!opened.ok()) return core::make_unexpected(opened.error());
    return std::shared_ptr<VirtualFile>(opened.value());
    // No `RuntimeDispatchFile` wrapper: there is no runtime to pin I/O to.
    // See the class comment.
}

core::Expected<std::shared_ptr<CachedFile>, std::string> ImageService::OpenCachedBlob(
    FileCacheBackend* cache, const std::string& url, const std::shared_ptr<VirtualFile>& source,
    const core::Optional<uint64_t>& source_size) {
    uint64_t size = 0;
    if (source_size.has_value()) {
        size = *source_size;
    } else {
        if (!source) {
            return core::make_unexpected(
                std::string("cannot size a cached blob without a source"));
        }
        const core::Expected<uint64_t, std::string> probed = source->Size();
        if (!probed.ok()) return core::make_unexpected(probed.error());
        size = probed.value();
    }
    return cache->OpenFile(url, source, size, 0);
}

core::Expected<std::shared_ptr<VirtualFile>, std::string> ImageService::OpenRemoteBlobWithSize(
    const std::string& url, const core::Optional<uint64_t>& source_size) {
    const core::Expected<RemoteRuntime*, std::string> runtime = GetRemoteRuntime();
    if (!runtime.ok()) return core::make_unexpected(runtime.error());

    const core::Expected<std::shared_ptr<VirtualFile>, std::string> source =
        OpenBackendSourceWithSize(url, source_size);
    if (!source.ok()) return core::make_unexpected(source.error());

    // OSS blobs always go through the file cache regardless of the remote
    // mode: `Direct` is only meaningful for the P2P accelerator, which is its
    // own cache. OSS has no P2P channel, so the local cache is wanted as a
    // read-ahead layer either way.
    if (IsOssUrl(url)) {
        if (!runtime.value()->file_cache) return source;
        const core::Expected<std::shared_ptr<CachedFile>, std::string> cached =
            OpenCachedBlob(runtime.value()->file_cache.get(), url, source.value(), source_size);
        if (!cached.ok()) return core::make_unexpected(cached.error());
        return std::shared_ptr<VirtualFile>(cached.value());
    }

    RemoteOpenMode mode;
    {
        std::lock_guard<std::mutex> guard(inner_->remote_mode_mutex);
        mode = inner_->remote_mode;
    }
    if (mode == kRemoteOpenDirect) return source;
    if (!runtime.value()->file_cache) return source;

    const core::Expected<std::shared_ptr<CachedFile>, std::string> cached =
        OpenCachedBlob(runtime.value()->file_cache.get(), url, source.value(), source_size);
    if (!cached.ok()) return core::make_unexpected(cached.error());
    return std::shared_ptr<VirtualFile>(cached.value());
}

core::Expected<std::shared_ptr<VirtualFile>, std::string> ImageService::OpenRemoteBlob(
    const std::string& url) {
    return OpenRemoteBlobWithSize(url, core::Optional<uint64_t>());
}

core::Expected<std::shared_ptr<VirtualFile>, std::string> ImageService::OpenSourceBlobWithSize(
    const std::string& url, const core::Optional<uint64_t>& source_size) {
    return OpenBackendSourceWithSize(url, source_size);
}

core::Expected<std::shared_ptr<VirtualFile>, std::string> ImageService::OpenSourceBlob(
    const std::string& url) {
    return OpenSourceBlobWithSize(url, core::Optional<uint64_t>());
}

core::Expected<std::pair<std::shared_ptr<VirtualFile>, CacheDownloadRequest>, std::string>
ImageService::OpenRemoteBlobForBkDownloadWithSize(const std::string& url,
                                                  const core::Optional<uint64_t>& source_size,
                                                  const DownloadConfig& config) {
    typedef std::pair<std::shared_ptr<VirtualFile>, CacheDownloadRequest> Result;

    const core::Expected<RemoteRuntime*, std::string> runtime = GetRemoteRuntime();
    if (!runtime.ok()) return core::make_unexpected(runtime.error());
    if (!runtime.value()->file_cache) {
        return core::make_unexpected(
            std::string("background download requires a file cache backend"));
    }

    const core::Expected<std::shared_ptr<VirtualFile>, std::string> source =
        OpenBackendSourceWithSize(url, source_size);
    if (!source.ok()) return core::make_unexpected(source.error());

    const core::Expected<std::shared_ptr<CachedFile>, std::string> cached =
        OpenCachedBlob(runtime.value()->file_cache.get(), url, source.value(), source_size);
    if (!cached.ok()) return core::make_unexpected(cached.error());

    // The same handle is both the read path and the download target: the
    // download fills the very entry the reads go through.
    CacheDownloadRequest request;
    request.file = cached.value();
    request.config = config;
    return Result(std::shared_ptr<VirtualFile>(cached.value()), request);
}

// ---------------------------------------------------------------------------
// Background downloads
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> ImageService::SubmitBkDownloads(
    const std::vector<CacheDownloadRequest>& requests,
    const core::Optional<std::string>& device_key) {
    if (requests.empty()) return core::Unit();

    const core::Expected<RemoteRuntime*, std::string> runtime = GetRemoteRuntime();
    if (!runtime.ok()) return core::make_unexpected(runtime.error());
    if (!runtime.value()->file_cache) {
        return core::make_unexpected(
            std::string("background download requires a file cache backend"));
    }

    std::vector<BkDownloadRequest> batch;
    batch.reserve(requests.size());
    for (size_t i = 0; i < requests.size(); ++i) {
        BkDownloadRequest entry;
        entry.file = requests[i].file;
        entry.config = requests[i].config;
        entry.device_key = device_key;
        batch.push_back(entry);
    }

    const core::Expected<core::Unit, std::string> result =
        SubmitBkDownloadBatch(runtime.value()->file_cache.get(), batch);
    if (result.ok()) return core::Unit();

    if (result.error() == std::string(kBkSubmitClosedMessage)) {
        // Downgraded, not failed: foreground `CachedFile` reads still refill
        // missing blocks from the origin, so a shut-down scheduler must not
        // fail an image open.
        //
        // Logged as the fixed category plus the device key's basename only;
        // the full path can expose tenant or host metadata.
        AGENTENV_WARN("error_category=" << kBkSubmitClosedCategory
                                        << " request_count=" << requests.size() << " device_key="
                                        << (device_key.has_value() ? BasenameOf(*device_key) : "")
                                        << " skipping background download submission; foreground"
                                           " reads refill from origin");
        return core::Unit();
    }
    return core::make_unexpected("submit background downloads: " + result.error());
}

// ---------------------------------------------------------------------------
// Startup pack
// ---------------------------------------------------------------------------

bool CollectStartupPackLayers(const ImageConfig& image_config,
                              std::vector<StartupPackLayerRef>* out) {
    out->clear();
    if (image_config.lowers.empty()) return false;

    out->reserve(image_config.lowers.size());
    for (size_t i = 0; i < image_config.lowers.size(); ++i) {
        const LayerConfig& lower = image_config.lowers[i];
        // A local-file lower is read from local bytes, not from the OSS
        // object: prefetching that object would warm a cache the device
        // never touches.
        if (!lower.file.empty()) {
            out->clear();
            return false;
        }
        const std::string base = lower.EffectiveRepoBlobUrl(image_config.repo_blob_url);
        if (base.empty() || lower.digest.empty() || lower.size == 0) {
            out->clear();
            return false;
        }
        StartupPackLayerRef ref;
        ref.url = TrimTrailingSlashes(base) + "/" + lower.digest;
        if (!ImageService::IsOssUrl(ref.url)) {
            out->clear();
            return false;
        }
        ref.digest = lower.digest;
        ref.size = lower.size;
        out->push_back(ref);
    }
    return true;
}

core::Expected<core::Optional<StartupPackHandle>, std::string> ImageService::PrefetchStartupPack(
    const std::string& image_config_path, const StartupPackPrefetch& pack) {
    const core::Expected<RemoteRuntime*, std::string> runtime = GetRemoteRuntime();
    if (!runtime.ok()) return core::make_unexpected(runtime.error());
    if (!runtime.value()->file_cache) return core::Optional<StartupPackHandle>();

    const core::Expected<ImageConfig, std::string> image_config =
        LoadImageConfig(image_config_path);
    if (!image_config.ok()) return core::make_unexpected(image_config.error());

    std::vector<StartupPackLayerRef> refs;
    if (!CollectStartupPackLayers(image_config.value(), &refs)) {
        return core::Optional<StartupPackHandle>();
    }

    StartupPackSubmission submission;
    submission.layers.reserve(refs.size());
    for (size_t i = 0; i < refs.size(); ++i) {
        const core::Expected<std::shared_ptr<VirtualFile>, std::string> source =
            OpenBackendSourceWithSize(refs[i].url, core::Optional<uint64_t>(refs[i].size));
        if (!source.ok()) return core::make_unexpected(source.error());

        const core::Expected<std::shared_ptr<CachedFile>, std::string> file =
            OpenCachedBlob(runtime.value()->file_cache.get(), refs[i].url, source.value(),
                           core::Optional<uint64_t>(refs[i].size));
        if (!file.ok()) return core::make_unexpected(file.error());

        StartupPackLayer layer;
        layer.digest = refs[i].digest;
        layer.size = refs[i].size;
        layer.file = file.value();
        layer.source = source.value();
        submission.layers.push_back(layer);
    }

    // The pack object itself bypasses the file cache: it is consumed exactly
    // once, and only its layer blocks belong in the cache. The size hint
    // avoids a stat probe.
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> pack_source =
        OpenBackendSourceWithSize(pack.url, core::Optional<uint64_t>(pack.pack_size));
    if (!pack_source.ok()) return core::make_unexpected(pack_source.error());

    submission.task_key = "startup-pack:" + pack.index_sha256;
    submission.pack_source = pack_source.value();
    submission.pack_size = pack.pack_size;
    submission.index_sha256 = pack.index_sha256;
    submission.mem_virtual_size = pack.mem_virtual_size;
    submission.timeout_ms = pack.timeout_ms;

    const core::Expected<StartupPackHandle, std::string> handle =
        SubmitStartupPack(runtime.value()->file_cache.get(), submission);
    if (!handle.ok()) return core::make_unexpected(handle.error());
    return core::Optional<StartupPackHandle>(handle.value());
}

// ---------------------------------------------------------------------------
// Image configs and image files
// ---------------------------------------------------------------------------

core::Expected<ImageConfig, std::string> ImageService::LoadImageConfig(
    const std::string& path) const {
    const core::Expected<std::string, std::string> raw = core::fs::ReadToString(path);
    if (!raw.ok()) return core::make_unexpected(raw.error());

    core::Expected<ImageConfig, std::string> cfg = ParseImageConfig(raw.value());
    if (!cfg.ok()) return core::make_unexpected("parse image config json failed: " + cfg.error());

    ImageConfig resolved = cfg.value();
    ResolveImageConfigLocalPaths(path, &resolved);
    const core::Expected<core::Unit, std::string> valid = ValidateImageConfig(resolved);
    if (!valid.ok()) return core::make_unexpected(valid.error());
    return resolved;
}

core::Expected<core::Unit, std::string> ImageService::SetResultFile(
    const std::string& filename, const std::string& data) const {
    if (filename.empty()) return core::Unit();

    // Absent for a bare file name, which needs no directory created -- Rust
    // guards the same way with `!parent.as_os_str().is_empty()`.
    const core::Optional<std::string> parent = core::fs::Parent(filename);
    if (parent.has_value()) {
        const core::Expected<core::Unit, std::string> made = core::fs::CreateDirAll(*parent);
        if (!made.ok()) return core::make_unexpected(made.error());
    }
    return core::fs::Write(filename, data);
}

core::Expected<std::shared_ptr<ImageFile>, std::string> ImageService::CreateImageFile(
    const std::string& path) {
    // The canonical path is the device identity used to key readiness waits;
    // a path that cannot be canonicalized is still usable as a key.
    std::string device_key = path;
    char* resolved = ::realpath(path.c_str(), 0);
    if (resolved != 0) {
        device_key = resolved;
        ::free(resolved);
    }

    const core::Expected<ImageConfig, std::string> image_config = LoadImageConfig(path);
    if (!image_config.ok()) return core::make_unexpected(image_config.error());
    const std::string result_file = image_config.value().result_file;

    // The return value is deliberately ignored, as in Rust: this probes and
    // records the acceleration mode, and an unreachable accelerator is a
    // fallback rather than an error.
    EnableAcceleration();

    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(image_config.value(), *this, core::Optional<std::string>(device_key));
    if (!image.ok()) {
        // Best effort: the open already failed, and failing to record that
        // would replace a useful error with a bookkeeping one.
        SetResultFile(result_file, "failed:" + image.error());
        return core::make_unexpected(image.error());
    }

    const core::Expected<core::Unit, std::string> recorded = SetResultFile(result_file, "success");
    if (!recorded.ok()) return core::make_unexpected(recorded.error());
    return image.value();
}

core::Expected<core::Unit, std::string> ImageService::ExportUpperAsOssSealed(
    ImageFile* image, const std::string& dest_url) {
    if (!IsOssUrl(dest_url)) {
        return core::make_unexpected(std::string("destination url must use oss:// or s3://"));
    }
    const core::Expected<RemoteRuntime*, std::string> runtime = GetRemoteRuntime();
    if (!runtime.ok()) return core::make_unexpected(runtime.error());
    if (!runtime.value()->oss_backend) {
        return core::make_unexpected(std::string("OSS backend not enabled in config"));
    }

    const std::string stage_dir =
        core::fs::Join(inner_->global_config.cache_config.cache_dir, "oss-stage");
    const core::Expected<core::Unit, std::string> made = core::fs::CreateDirAll(stage_dir);
    if (!made.ok()) {
        return core::make_unexpected("create oss stage dir " + stage_dir + ": " + made.error());
    }
    const std::string stage_path =
        core::fs::Join(stage_dir, core::Uuid::GenV4().ToString() + ".lsmt");

    const core::Expected<std::shared_ptr<LocalFile>, std::string> stage_file =
        LocalFile::New(stage_path);
    if (!stage_file.ok()) return core::make_unexpected(stage_file.error());

    const core::Expected<core::Unit, std::string> exported =
        image->ExportUpperAsSealed(CommitArgs(stage_file.value()));
    if (!exported.ok()) {
        core::fs::RemoveFile(stage_path);
        return core::make_unexpected(exported.error());
    }

    const core::Expected<core::Unit, std::string> synced = stage_file.value()->Sync();
    if (!synced.ok()) {
        core::fs::RemoveFile(stage_path);
        return core::make_unexpected(synced.error());
    }

    const core::Expected<core::Unit, std::string> uploaded =
        runtime.value()->oss_backend->UploadPath(dest_url, stage_path, kOssSealedUploadPartSize,
                                                 kOssSealedUploadConcurrency, 0);
    // Always cleaned up regardless of outcome: the staging file is a full
    // copy of the sealed upper and can be large, so leaving it across
    // failures would accumulate waste.
    core::fs::RemoveFile(stage_path);
    return uploaded;
}

// ---------------------------------------------------------------------------
// Accelerator probe
// ---------------------------------------------------------------------------

bool CheckAccelerateUrl(const std::string& address) {
    const core::Expected<core::Url, std::string> parsed = core::Url::Parse(address);
    if (!parsed.ok()) return false;
    if (parsed.value().host.empty()) return false;

    const uint16_t port = PortOrKnownDefault(parsed.value());
    char port_text[16];
    std::snprintf(port_text, sizeof(port_text), "%u", static_cast<unsigned>(port));

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = 0;
    if (::getaddrinfo(parsed.value().host.c_str(), port_text, &hints, &result) != 0) return false;

    // Rust's `.any(..)`: the first address that connects decides it.
    bool reachable = false;
    for (const struct addrinfo* ai = result; ai != 0 && !reachable; ai = ai->ai_next) {
        reachable = ConnectWithinTimeout(ai);
    }
    ::freeaddrinfo(result);
    return reachable;
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
