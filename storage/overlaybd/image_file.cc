// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/image/image_file.rs
#include "agentenv/storage/overlaybd/image_file.h"

#include <sys/stat.h>

#include <cstdio>

#include "agentenv/core/fs.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/logging.h"
#include "agentenv/storage/overlaybd/download_gate.h"
#include "agentenv/storage/overlaybd/image_helper.h"
#include "agentenv/storage/overlaybd/layer_metadata.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/switch_file.h"
#include "agentenv/storage/overlaybd/tar.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

const char kRestackSnapshotTerminalPrefix[] =
    "restack snapshot mutated live runtime before failing: ";

std::string MakeRestackSnapshotTerminalFailure(const std::string& message) {
    return std::string(kRestackSnapshotTerminalPrefix) + message;
}

bool IsRestackSnapshotTerminalFailure(const std::string& error) {
    const size_t n = sizeof(kRestackSnapshotTerminalPrefix) - 1;
    return error.size() >= n && error.compare(0, n, kRestackSnapshotTerminalPrefix) == 0;
}

core::Expected<core::Unit, std::string> EnsureSealableUpper(const UpperConfig& upper,
                                                               const std::string& operation) {
    const UpperMode mode = upper.WritableMode();
    const bool needs_index =
        mode == UpperMode::LogStructured || mode == UpperMode::HybridLogStructured;
    if (upper.data.empty() || (needs_index && upper.index.empty())) {
        return core::make_unexpected(operation + " requires a writable upper layer");
    }
    return core::Unit();
}

namespace {

/// Rust's `OpenedLowerLayer`.
struct OpenedLowerLayer {
    std::shared_ptr<VirtualFile> file;
    /// Absent when this layer is local, or when the background request could
    /// not be built.
    core::Optional<CacheDownloadRequest> download;
};

/// Rust's `OpenedLowerFiles`.
struct OpenedLowerFiles {
    /// Null when the image has no lowers at all.
    std::shared_ptr<LsmtReadOnlyFile> file;
    std::vector<CacheDownloadRequest> downloads;
};

/// Rust's `InitializedImageFile`.
struct InitializedImageFile {
    LiveImageState state;
    std::vector<CacheDownloadRequest> downloads;
    std::shared_ptr<Prefetcher> prefetcher;
    bool replay_prefetch;

    InitializedImageFile() : replay_prefetch(false) {}
};

std::string TrimTrailingSlashes(const std::string& s) {
    std::string::size_type end = s.size();
    while (end > 0 && s[end - 1] == '/') --end;
    return s.substr(0, end);
}

/// Rust `is_not_found`, re-expressed as a probe of the path rather than a
/// match on the error text.
///
/// Rust tests the error *kind* — it walks the chain for an `io::Error` whose
/// kind is `NotFound`. There is no kind to test here, and matching
/// `strerror`'s text does not work: this process runs under
/// `LANG=zh_CN.UTF-8` in at least one deployment, where `strerror(ENOENT)` is
/// "没有那个文件或目录". An English-only match would silently never fire, and
/// the P2P-uuid fallback below would become dead code — the layer would fail
/// the open instead of being fetched from a peer.
///
/// `TryExists` reproduces the kind check exactly: only `ENOENT`/`ENOTDIR`
/// report absence, so `EACCES` or `ELOOP` keep the original open error, which
/// is what Rust's chain walk also does.
bool LocalLayerIsMissing(const std::string& path) {
    const core::Expected<bool, std::string> present = core::fs::TryExists(path);
    return present.ok() && !present.value();
}

core::Expected<uint64_t, std::string> BaseSize(const ImageFileBase& base) {
    if (base.read_write_) return base.read_write_->Size();
    if (base.read_only_) return base.read_only_->Size();
    return core::make_unexpected(std::string("image file has no base layer"));
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ImageFile::ImageFile(const ImageService& image_service)
    : size_bytes_(0),
      num_lbas_(0),
      block_size_(kImageDefaultBlockSize),
      image_service_(image_service) {}
ImageFile::~ImageFile() {
    // Rust's `Drop` takes the prefetcher so its worker threads are joined
    // before the layers it reads through go away.
    prefetcher_.reset();
}

namespace {

/// Rust `open_localfile_path` — the on-disk path of a local lower, if it has
/// one.
core::Expected<core::Optional<std::string>, std::string> OpenLocalfilePath(
    const LayerConfig& layer) {
    if (!layer.file.empty()) return core::Optional<std::string>(layer.file);
    if (layer.dir.empty()) return core::Optional<std::string>();

    // `TryExists`, not `Exists`: Rust writes `try_exists(..).await?` here, so
    // a directory that cannot be probed fails the open instead of being
    // reported as a layer with no local file -- which would send this layer
    // down the remote path and blame a missing digest for a permission error.
    const std::string commit = core::fs::Join(layer.dir, kCommitFileName);
    const core::Expected<bool, std::string> has_commit = core::fs::TryExists(commit);
    if (!has_commit.ok()) return core::make_unexpected(has_commit.error());
    if (has_commit.value()) return core::Optional<std::string>(commit);

    const std::string sealed = core::fs::Join(layer.dir, kSealedFileName);
    const core::Expected<bool, std::string> has_sealed = core::fs::TryExists(sealed);
    if (!has_sealed.ok()) return core::make_unexpected(has_sealed.error());
    if (has_sealed.value()) return core::Optional<std::string>(sealed);

    return core::Optional<std::string>();
}

/// Rust `open_ro_file` — a local sealed layer, through the tar adaptor and
/// the switch file.
core::Expected<std::shared_ptr<VirtualFile>, std::string> OpenRoFile(
    const std::string& path, const ImageService& image_service) {
    const bool direct_io = image_service.IoEngine() == kIoEngineLibaio;
    const core::Expected<std::shared_ptr<LocalFile>, std::string> file =
        LocalFile::Builder().Write(false).Create(false).DirectIo(direct_io).Open(path);
    if (!file.ok()) return core::make_unexpected(file.error());

    const core::Expected<std::shared_ptr<VirtualFile>, std::string> tar_file =
        NewTarFileAdaptor(file.value());
    if (!tar_file.ok()) return core::make_unexpected(tar_file.error());

    const core::Expected<std::shared_ptr<SwitchFile>, std::string> switch_file =
        NewSwitchFile(tar_file.value(), true, path.c_str());
    if (!switch_file.ok()) return core::make_unexpected(switch_file.error());
    return std::shared_ptr<VirtualFile>(switch_file.value());
}

/// Rust `open_ro_p2p_uuid` — a lower fetched through the P2P uuid facade.
core::Expected<OpenedLowerLayer, std::string> OpenRoP2pUuid(ImageService* image_service,
                                                            const LayerConfig& layer) {
    core::Uuid uuid;
    if (!core::Uuid::Parse(layer.uuid, &uuid)) {
        return core::make_unexpected("invalid overlaybd layer uuid '" + layer.uuid + "'");
    }
    const core::Optional<std::string> facade = image_service->P2pUuidAddress();
    if (!facade.has_value()) {
        return core::make_unexpected(
            std::string("lower layer local file is missing and p2p uuid facade is not "
                        "configured"));
    }

    const std::string url = TrimTrailingSlashes(*facade) + "/" + uuid.ToString();
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> remote_file =
        image_service->OpenSourceBlobWithSize(
            url, layer.size != 0 ? core::Optional<uint64_t>(layer.size)
                                 : core::Optional<uint64_t>());
    if (!remote_file.ok()) return core::make_unexpected(remote_file.error());

    const core::Expected<std::shared_ptr<VirtualFile>, std::string> tar_file =
        NewTarFileAdaptor(remote_file.value());
    if (!tar_file.ok()) return core::make_unexpected(tar_file.error());

    const core::Expected<std::shared_ptr<SwitchFile>, std::string> switch_file =
        NewSwitchFile(tar_file.value(), false, url.c_str());
    if (!switch_file.ok()) return core::make_unexpected(switch_file.error());

    OpenedLowerLayer opened;
    opened.file = switch_file.value();
    return opened;
}

/// Rust `open_ro_remote` — a lower fetched by digest from the registry or
/// object store.
core::Expected<OpenedLowerLayer, std::string> OpenRoRemote(ImageService* image_service,
                                                           const std::string& repo_blob_url,
                                                           const DownloadConfig& download_cfg,
                                                           bool collect_download_requests,
                                                           const LayerConfig& layer,
                                                           size_t index) {
    if (layer.digest.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%zu", index);
        return core::make_unexpected(std::string("lower layer ") + buf +
                                     " has no local file and no digest");
    }
    if (repo_blob_url.empty()) {
        return core::make_unexpected(std::string("repoBlobUrl is empty for remote lower layer"));
    }

    // The uuid facade is preferred when both are available: it serves the
    // layer from a peer rather than the registry.
    if (!layer.uuid.empty() && image_service->P2pUuidAddress().has_value()) {
        const core::Expected<OpenedLowerLayer, std::string> opened =
            OpenRoP2pUuid(image_service, layer);
        if (opened.ok()) return opened;
        AGENTENV_WARN("layer_index=" << index << " uuid=" << layer.uuid
                                     << " error=" << opened.error()
                                     << " p2p uuid lower open failed; falling back to remote"
                                        " digest");
    }

    const std::string url = TrimTrailingSlashes(repo_blob_url) + "/" + layer.digest;
    const core::Optional<uint64_t> source_size =
        layer.size != 0 ? core::Optional<uint64_t>(layer.size) : core::Optional<uint64_t>();

    std::shared_ptr<VirtualFile> remote_file;
    core::Optional<CacheDownloadRequest> download;
    if (collect_download_requests) {
        const core::Expected<
            std::pair<std::shared_ptr<VirtualFile>, CacheDownloadRequest>, std::string>
            with_download =
                image_service->OpenRemoteBlobForBkDownloadWithSize(url, source_size, download_cfg);
        if (with_download.ok()) {
            remote_file = with_download.value().first;
            download = core::Optional<CacheDownloadRequest>(with_download.value().second);
        } else {
            // Building the background request is an optimization; a cache or
            // source hiccup must not abort the image open. Logged as the
            // fixed category only -- the error text may embed the URL, which
            // can be a signed one.
            AGENTENV_WARN("error_category=bk_download_request_build_failed"
                          " falling back to foreground-only open for remote layer");
            const core::Expected<std::shared_ptr<VirtualFile>, std::string> plain =
                image_service->OpenRemoteBlobWithSize(url, source_size);
            if (!plain.ok()) return core::make_unexpected(plain.error());
            remote_file = plain.value();
        }
    } else {
        const core::Expected<std::shared_ptr<VirtualFile>, std::string> plain =
            image_service->OpenRemoteBlobWithSize(url, source_size);
        if (!plain.ok()) return core::make_unexpected(plain.error());
        remote_file = plain.value();
    }

    const core::Expected<std::shared_ptr<VirtualFile>, std::string> tar_file =
        NewTarFileAdaptor(remote_file);
    if (!tar_file.ok()) return core::make_unexpected(tar_file.error());

    const core::Expected<std::shared_ptr<SwitchFile>, std::string> switch_file =
        NewSwitchFile(tar_file.value(), false, url.c_str());
    if (!switch_file.ok()) return core::make_unexpected(switch_file.error());

    OpenedLowerLayer opened;
    opened.file = switch_file.value();
    opened.download = download;
    return opened;
}

/// Rust `open_lower_layer`.
core::Expected<OpenedLowerLayer, std::string> OpenLowerLayer(
    ImageService* image_service, const std::string& repo_blob_url,
    const DownloadConfig& download_cfg, bool collect_download_requests, Prefetcher* prefetcher,
    const LayerConfig& layer, size_t index) {
    const core::Expected<core::Optional<std::string>, std::string> local_path =
        OpenLocalfilePath(layer);
    if (!local_path.ok()) return core::make_unexpected(local_path.error());

    OpenedLowerLayer opened;
    if (local_path.value().has_value()) {
        const std::string& local = *local_path.value();
        const core::Expected<std::shared_ptr<VirtualFile>, std::string> file =
            OpenRoFile(local, *image_service);
        if (file.ok()) {
            opened.file = file.value();
        } else if (LocalLayerIsMissing(local) && !layer.uuid.empty()) {
            // A layer whose local copy is gone but which names a uuid can
            // still be served by a peer.
            const core::Expected<OpenedLowerLayer, std::string> p2p =
                OpenRoP2pUuid(image_service, layer);
            if (!p2p.ok()) return core::make_unexpected(p2p.error());
            opened = p2p.value();
        } else {
            return core::make_unexpected(file.error());
        }
    } else {
        const core::Expected<OpenedLowerLayer, std::string> remote =
            OpenRoRemote(image_service, repo_blob_url, download_cfg, collect_download_requests,
                         layer, index);
        if (!remote.ok()) return core::make_unexpected(remote.error());
        opened = remote.value();
    }

    if (prefetcher != 0) {
        opened.file = prefetcher->NewPrefetchFile(opened.file, static_cast<uint32_t>(index));
    }

    if (!layer.target_file.empty() || !layer.target_digest.empty() || !layer.gzip_index.empty()) {
        return core::make_unexpected(
            std::string("warp/targetDigest/gzipIndex lower layers are not migrated in Rust"
                        " image_file yet"));
    }
    return opened;
}

/// Rust `open_lowers`.
core::Expected<OpenedLowerFiles, std::string> OpenLowers(
    const std::vector<LayerConfig>& lowers, const std::string& repo_blob_url,
    const DownloadConfig& download_cfg, bool collect_download_requests,
    ImageService* image_service, Prefetcher* prefetcher) {
    OpenedLowerFiles result;
    if (lowers.empty()) return result;

    // Deviation: Rust opens the layers concurrently (`buffer_unordered`,
    // capped at PARALLEL_LOAD_INDEX) and then restores config order. This
    // port opens them in order, matching the decision already recorded for
    // index loading in `lsmt_readonly.cc`: there is no task runtime here.
    // The resulting order is the same one Rust reconstructs, so only the
    // cold-open latency differs.
    std::vector<std::shared_ptr<VirtualFile> > files;
    files.reserve(lowers.size());
    for (size_t index = 0; index < lowers.size(); ++index) {
        const std::string layer_repo_blob_url =
            lowers[index].EffectiveRepoBlobUrl(repo_blob_url);
        const core::Expected<OpenedLowerLayer, std::string> opened =
            OpenLowerLayer(image_service, layer_repo_blob_url, download_cfg,
                           collect_download_requests, prefetcher, lowers[index], index);
        if (!opened.ok()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%zu", index);
            return core::make_unexpected(std::string("failed to open lower layer ") + buf + ": " +
                                         opened.error());
        }
        files.push_back(opened.value().file);
        if (opened.value().download.has_value()) {
            result.downloads.push_back(*opened.value().download);
        }
    }

    const CacheConfig& cache_cfg = image_service->GetGlobalConfig().cache_config;
    const core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> merged =
        OpenFilesRoWithPremergedCache(files, cache_cfg.cache_dir,
                                      PremergedIndexCachePolicy::Hybrid(cache_cfg.cache_size_gb));
    if (!merged.ok()) return core::make_unexpected(merged.error());

    result.file = merged.value();
    return result;
}

/// Rust `open_upper`.
core::Expected<std::shared_ptr<LsmtFile>, std::string> OpenUpper(const UpperConfig& upper) {
    if (upper.data.empty()) return std::shared_ptr<LsmtFile>();
    if (!upper.target.empty() || !upper.gzip_index.empty()) {
        return core::make_unexpected(
            std::string("warp/gzipIndex upper layers are not migrated in Rust image_file yet"));
    }

    const core::Expected<std::shared_ptr<LocalFile>, std::string> data_file =
        LocalFile::OpenRw(upper.data, false);
    if (!data_file.ok()) return core::make_unexpected(data_file.error());

    std::shared_ptr<VirtualFile> idx_file;
    const UpperMode mode = upper.WritableMode();
    if (mode == UpperMode::LogStructured || mode == UpperMode::HybridLogStructured) {
        if (upper.index.empty()) {
            return core::make_unexpected(std::string("log-structured upper requires upper.index"));
        }
        const core::Expected<std::shared_ptr<LocalFile>, std::string> opened =
            LocalFile::OpenRw(upper.index, false);
        if (!opened.ok()) return core::make_unexpected(opened.error());
        idx_file = opened.value();
    }

    return OpenFileRw(data_file.value(), idx_file);
}

/// Rust `init_image_file`.
core::Expected<InitializedImageFile, std::string> InitImageFile(
    const ImageConfig& config, ImageService* image_service, const DownloadConfig& download_cfg) {
    std::vector<LayerConfig> lowers_config = config.lowers;
    std::shared_ptr<Prefetcher> prefetcher;
    bool skip_background_download = false;
    const int32_t concurrency = image_service->GetGlobalConfig().prefetch_config.concurrency;

    if (config.acceleration_layer && !lowers_config.empty()) {
        // The acceleration layer is a trace carrier, not image data: it is
        // popped off the stack and only its trace file is used.
        const LayerConfig accel_layer = lowers_config.back();
        lowers_config.pop_back();
        const std::string trace_file = core::fs::Join(accel_layer.dir, "trace");
        if (Prefetcher::DetectMode(trace_file) == PrefetchMode::Replay) {
            const core::Expected<std::shared_ptr<Prefetcher>, std::string> made =
                NewPrefetcher(trace_file, concurrency);
            if (!made.ok()) return core::make_unexpected(made.error());
            prefetcher = made.value();
        }
    } else if (!config.record_trace_path.empty()) {
        // Rust guards this with `try_exists(..).await?`, so an unprobeable
        // trace path fails the open rather than silently costing the boot its
        // prefetch.
        const core::Expected<bool, std::string> present =
            core::fs::TryExists(config.record_trace_path);
        if (!present.ok()) return core::make_unexpected(present.error());
        if (present.value()) {
            const PrefetchMode mode = Prefetcher::DetectMode(config.record_trace_path);
            if (mode != PrefetchMode::Disabled) {
                const core::Expected<std::shared_ptr<Prefetcher>, std::string> made =
                    NewPrefetcher(config.record_trace_path, concurrency);
                if (!made.ok()) return core::make_unexpected(made.error());
                prefetcher = made.value();
                // Recording wants to observe the boot's real read pattern,
                // which a background download would pollute.
                if (mode == PrefetchMode::Record) skip_background_download = true;
            }
        }
    }

    const core::Expected<OpenedLowerFiles, std::string> lowers =
        OpenLowers(lowers_config, config.repo_blob_url, download_cfg,
                   download_cfg.enable && !skip_background_download, image_service,
                   prefetcher.get());
    if (!lowers.ok()) return core::make_unexpected(lowers.error());

    const core::Expected<std::shared_ptr<LsmtFile>, std::string> upper_file =
        OpenUpper(config.upper);
    if (!upper_file.ok()) return core::make_unexpected(upper_file.error());

    InitializedImageFile out;
    out.prefetcher = prefetcher;
    out.replay_prefetch = prefetcher && prefetcher->Mode() == PrefetchMode::Replay &&
                          static_cast<bool>(lowers.value().file);
    out.state.config = config;
    out.state.premerged_index_cache_dir =
        image_service->GetGlobalConfig().cache_config.cache_dir;
    out.state.premerged_index_cache_policy = PremergedIndexCachePolicy::Hybrid(
        image_service->GetGlobalConfig().cache_config.cache_size_gb);

    const bool has_lower = static_cast<bool>(lowers.value().file);
    const bool has_upper = static_cast<bool>(upper_file.value());
    if (has_lower && has_upper) {
        const core::Expected<std::shared_ptr<LsmtFile>, std::string> stacked =
            StackFiles(upper_file.value(), lowers.value().file, false);
        if (!stacked.ok()) return core::make_unexpected(stacked.error());
        out.state.base.read_write_ = stacked.value();
        out.downloads = lowers.value().downloads;
    } else if (has_lower) {
        out.state.base.read_only_ = lowers.value().file;
        out.downloads = lowers.value().downloads;
    } else if (has_upper) {
        out.state.base.read_write_ = upper_file.value();
        // No lowers means nothing was opened remotely, so there is nothing to
        // download and no trace to replay against.
        out.replay_prefetch = false;
    } else {
        return core::make_unexpected(std::string("image config has no lower and no upper layer"));
    }
    return out;
}

}  // namespace

core::Expected<std::shared_ptr<ImageFile>, std::string> ImageFile::Open(
    const ImageConfig& config, const ImageService& image_service,
    const core::Optional<std::string>& device_key) {
    ImageService service = image_service;
    const DownloadConfig download_cfg = config.EffectiveDownload(service.GetGlobalConfig());

    const core::Expected<InitializedImageFile, std::string> initialized =
        InitImageFile(config, &service, download_cfg);
    if (!initialized.ok()) return core::make_unexpected(initialized.error());

    const core::Expected<uint64_t, std::string> size = BaseSize(initialized.value().state.base);
    if (!size.ok()) return core::make_unexpected(size.error());

    // Submitted only once the whole image is open: a half-open image whose
    // open later fails would otherwise leave downloads running for layers
    // nobody reads.
    const core::Expected<core::Unit, std::string> submitted =
        service.SubmitBkDownloads(initialized.value().downloads, device_key);
    if (!submitted.ok()) return core::make_unexpected(submitted.error());

    // Replayed before the image exists, as Rust does: a failed replay fails
    // the open, and constructing first would mean tearing an image back down.
    if (initialized.value().replay_prefetch && initialized.value().prefetcher) {
        const core::Expected<core::Unit, std::string> replayed =
            initialized.value().prefetcher->Replay();
        if (!replayed.ok()) return core::make_unexpected(replayed.error());
    }

    std::shared_ptr<ImageFile> image(new ImageFile(service));
    image->state_ = initialized.value().state;
    image->size_bytes_ = size.value();
    image->num_lbas_ = size.value() / kImageDefaultBlockSize;
    image->prefetcher_ = initialized.value().prefetcher;
    return image;
}

// ---------------------------------------------------------------------------
// Size metadata
// ---------------------------------------------------------------------------

uint64_t ImageFile::SizeBytes() const { return size_bytes_.load(); }

uint64_t ImageFile::NumLbas() const { return num_lbas_.load(); }

core::Expected<uint64_t, std::string> ImageFile::RefreshSizeMetadataLocked() {
    const core::Expected<uint64_t, std::string> size = BaseSize(state_.base);
    if (!size.ok()) return core::make_unexpected(size.error());
    size_bytes_.store(size.value());
    num_lbas_.store(size.value() / block_size_);
    return size.value();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool ImageFile::IsReadOnly() const {
    core::ReadGuard guard(state_lock_);
    return state_.base.IsReadOnly();
}

core::Expected<DataStat, std::string> ImageFile::GetDataStat() const {
    core::ReadGuard guard(state_lock_);
    if (state_.base.read_write_) return state_.base.read_write_->Stat();
    if (state_.base.read_only_) return state_.base.read_only_->Stat();
    return core::make_unexpected(std::string("image file has no base layer"));
}

core::Expected<std::string, std::string> ImageFile::GetUuid(size_t layer_idx) const {
    core::ReadGuard guard(state_lock_);
    if (state_.base.read_write_) return state_.base.read_write_->GetUuid(layer_idx);
    if (state_.base.read_only_) return state_.base.read_only_->GetUuid(layer_idx);
    return core::make_unexpected(std::string("image file has no base layer"));
}

core::Expected<core::Unit, std::string> ImageFile::Compact(
    const std::shared_ptr<VirtualFile>& as_file) const {
    core::ReadGuard guard(state_lock_);
    if (state_.base.read_write_) return state_.base.read_write_->Flatten(as_file);
    if (state_.base.read_only_) return state_.base.read_only_->Flatten(as_file);
    return core::make_unexpected(std::string("image file has no base layer"));
}

core::Expected<core::Unit, std::string> ImageFile::ExportUpperAsSealed(
    const CommitArgs& args) const {
    // Shared, unlike `CloseSeal`: nothing is sealed in place here, the upper
    // is only read from, which is what makes this usable on a running image.
    core::ReadGuard guard(state_lock_);
    LsmtFile* writable = state_.base.Writable();
    if (writable == 0) {
        return core::make_unexpected(
            std::string("export_upper_as_sealed requires a writable upper layer"));
    }
    return writable->ExportUpperAsSealed(args);
}

// ---------------------------------------------------------------------------
// Terminal operations
// ---------------------------------------------------------------------------

core::Expected<core::Optional<LayerDescriptor>, std::string> ImageFile::CloseSeal(
    const std::string& output_layer_path) {
    // Exclusive rather than shared, unlike `ExportUpperAsSealed`: this one
    // modifies the upper it is reading, so no I/O may be in flight beside it.
    core::WriteGuard guard(state_lock_);

    const core::Expected<core::Unit, std::string> sealable =
        EnsureSealableUpper(state_.config.upper, "close_seal");
    if (!sealable.ok()) return core::make_unexpected(sealable.error());

    const std::string upper_data_path = state_.config.upper.data;
    LsmtFile* current = state_.base.Writable();
    if (current == 0) {
        return core::make_unexpected(std::string("close_seal requires a writable upper layer"));
    }

    // Before the seal, so an output path that cannot exist is refused while
    // the upper is still intact. Absent for a bare file name, which needs no
    // directory created -- Rust filters exactly that case out.
    const core::Optional<std::string> output_dir = core::fs::Parent(output_layer_path);
    if (output_dir.has_value()) {
        const core::Expected<core::Unit, std::string> made = core::fs::CreateDirAll(*output_dir);
        if (!made.ok()) {
            return core::make_unexpected("create the layer's directory " + *output_dir + ": " +
                                         made.error());
        }
    }

    // The seal writes the compact index and trailer through the upper's open
    // handle -- `LsmtFile::CloseSeal` takes no path -- so the rename comes
    // last, which is what leaves a failed seal's data file where it was.
    const core::Expected<core::Optional<LayerDescriptor>, std::string> descriptor =
        current->CloseSeal();
    if (!descriptor.ok()) {
        return core::make_unexpected("seal the upper " + upper_data_path + ": " +
                                     descriptor.error());
    }
    if (std::rename(upper_data_path.c_str(), output_layer_path.c_str()) != 0) {
        return core::make_unexpected("move the sealed layer from " + upper_data_path + " to " +
                                     output_layer_path);
    }
    return descriptor.value();
}

core::Expected<core::Optional<LayerDescriptor>, std::string> ImageFile::CreateSnapshotAndRestack(
    const std::string& output_layer_path) {
    core::WriteGuard guard(state_lock_);

    const UpperMode upper_mode = state_.config.upper.WritableMode();
    const std::string upper_data_path = state_.config.upper.data;
    const core::Optional<std::string> upper_index_path =
        state_.config.upper.index.empty() ? core::Optional<std::string>()
                                          : core::Optional<std::string>(state_.config.upper.index);

    const core::Expected<core::Unit, std::string> sealable =
        EnsureSealableUpper(state_.config.upper, "create_snapshot_and_restack");
    if (!sealable.ok()) return core::make_unexpected(sealable.error());

    // Rust refuses only where `Path::parent()` is absent, which is the root
    // and the empty path. A bare file name is *not* refused: Rust gets
    // `Some("")` there and `create_dir_all("")` succeeds.
    // `core::fs::Parent` reports absence for both cases, so the two are
    // separated by hand -- otherwise a relative output path would be rejected
    // here and accepted by Rust.
    if (output_layer_path.empty() || output_layer_path == "/") {
        return core::make_unexpected("snapshot output path has no parent directory: " +
                                     output_layer_path);
    }
    const core::Optional<std::string> output_dir = core::fs::Parent(output_layer_path);
    if (output_dir.has_value()) {
        const core::Expected<core::Unit, std::string> made = core::fs::CreateDirAll(*output_dir);
        if (!made.ok()) {
            return core::make_unexpected("create snapshot output dir " + *output_dir + ": " +
                                         made.error());
        }
    }

    LsmtFile* current = state_.base.Writable();
    if (current == 0) {
        return core::make_unexpected(
            std::string("create_snapshot_and_restack requires a writable image"));
    }

    const uint64_t virtual_size = size_bytes_.load();
    const size_t max_io_size = current->GetMaxIoSize();
    const size_t group_commit_size = current->GetIndexGroupCommitSize();
    const std::string cache_dir = state_.premerged_index_cache_dir;
    const PremergedIndexCachePolicy cache_policy = state_.premerged_index_cache_policy;

    // Everything from here to the end of the block mutates the live runtime,
    // so each failure is wrapped as terminal: the image cannot be used again.
    //
    // `OpenFilesRo` consumes layers bottom-to-top and flips them into its
    // internal top-to-bottom representation.
    std::vector<std::shared_ptr<VirtualFile> > lower_files =
        current->LowerLayerFilesBottomToTop();

    const core::Expected<SealedReopen, std::string> reopened = current->CloseSealAndReopen();
    if (!reopened.ok()) {
        return core::make_unexpected(MakeRestackSnapshotTerminalFailure(reopened.error()));
    }
    const core::Optional<LayerDescriptor> descriptor = reopened.value().descriptor;
    const std::vector<std::shared_ptr<VirtualFile> >& sealed_layers =
        reopened.value().file->Layers();
    lower_files.insert(lower_files.end(), sealed_layers.begin(), sealed_layers.end());

    if (std::rename(upper_data_path.c_str(), output_layer_path.c_str()) != 0) {
        return core::make_unexpected(MakeRestackSnapshotTerminalFailure(
            "move sealed upper data from " + upper_data_path + " to " + output_layer_path));
    }

    const core::Expected<core::Unit, std::string> prepared =
        PrepareRuntimeUpper(upper_data_path, upper_index_path, virtual_size, upper_mode);
    if (!prepared.ok()) {
        return core::make_unexpected(MakeRestackSnapshotTerminalFailure(
            "prepare fresh upper after restack: " + prepared.error()));
    }

    const core::Expected<std::shared_ptr<LocalFile>, std::string> new_upper_data =
        LocalFile::OpenRw(upper_data_path, false);
    if (!new_upper_data.ok()) {
        return core::make_unexpected(MakeRestackSnapshotTerminalFailure(
            "open fresh upper data " + upper_data_path + ": " + new_upper_data.error()));
    }

    std::shared_ptr<VirtualFile> new_upper_index;
    if (upper_mode == UpperMode::LogStructured || upper_mode == UpperMode::HybridLogStructured) {
        if (!upper_index_path.has_value()) {
            return core::make_unexpected(MakeRestackSnapshotTerminalFailure(
                "log-structured upper lost its index path during restack"));
        }
        const core::Expected<std::shared_ptr<LocalFile>, std::string> opened =
            LocalFile::OpenRw(*upper_index_path, false);
        if (!opened.ok()) {
            return core::make_unexpected(MakeRestackSnapshotTerminalFailure(
                "open fresh upper index " + *upper_index_path + ": " + opened.error()));
        }
        new_upper_index = opened.value();
    }

    const core::Expected<std::shared_ptr<LsmtFile>, std::string> new_upper =
        OpenFileRw(new_upper_data.value(), new_upper_index);
    if (!new_upper.ok()) {
        return core::make_unexpected(MakeRestackSnapshotTerminalFailure(
            "open fresh upper after restack: " + new_upper.error()));
    }
    const core::Expected<core::Unit, std::string> io_sized =
        new_upper.value()->SetMaxIoSize(max_io_size);
    if (!io_sized.ok()) {
        return core::make_unexpected(MakeRestackSnapshotTerminalFailure(io_sized.error()));
    }
    new_upper.value()->SetIndexGroupCommit(group_commit_size);

    const core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> lower =
        OpenFilesRoWithPremergedCache(lower_files, cache_dir, cache_policy);
    if (!lower.ok()) {
        return core::make_unexpected(MakeRestackSnapshotTerminalFailure(
            "reopen lower stack after restack: " + lower.error()));
    }

    const core::Expected<std::shared_ptr<LsmtFile>, std::string> restacked =
        StackFiles(new_upper.value(), lower.value(), false);
    if (!restacked.ok()) {
        return core::make_unexpected(MakeRestackSnapshotTerminalFailure(
            "restack fresh upper onto sealed snapshot lower: " + restacked.error()));
    }

    // The snapshot is now a lower of this image, so the config has to name it
    // or a reopen would lose it.
    LayerConfig snapshot_layer;
    snapshot_layer.file = output_layer_path;
    if (descriptor.has_value()) {
        snapshot_layer.digest = descriptor->digest;
        snapshot_layer.size = descriptor->size;
    } else {
        // No descriptor means the layout could not report a digest; the size
        // still comes from the file itself.
        const core::Expected<uint64_t, std::string> on_disk =
            core::fs::FileSize(output_layer_path);
        snapshot_layer.size = on_disk.ok() ? on_disk.value() : 0;
    }
    // Rust parses the stamped uuid and keeps it only when it parses *and* is
    // not nil. Parsed rather than compared as text because
    // `ReadOverlaybdLayerUuid` hands back the raw field: an unparseable stamp
    // would otherwise be recorded verbatim, where Rust discards it. The
    // parsed form is re-rendered so the spelling is canonical.
    const core::Expected<std::string, std::string> raw_uuid =
        ReadOverlaybdLayerUuid(output_layer_path);
    core::Uuid parsed_uuid;
    if (raw_uuid.ok() && core::Uuid::Parse(raw_uuid.value(), &parsed_uuid) &&
        !parsed_uuid.is_nil()) {
        snapshot_layer.uuid = parsed_uuid.ToString();
    }
    state_.config.lowers.push_back(snapshot_layer);

    state_.base.read_only_.reset();
    state_.base.read_write_ = restacked.value();
    size_bytes_.store(virtual_size);
    num_lbas_.store(virtual_size / block_size_);
    return descriptor;
}

// ---------------------------------------------------------------------------
// VirtualFile
// ---------------------------------------------------------------------------

core::Expected<ByteBuffer, std::string> ImageFile::ReadAt(uint64_t offset, size_t len) {
    // Foreground reads all pass through here (background downloads read their
    // own source directly and never arrive), so this is the single place to
    // count in-flight foreground reads for the background-download gate.
    FgReadGuard fg_guard;
    core::ReadGuard guard(state_lock_);
    if (state_.base.read_write_) return state_.base.read_write_->ReadAt(offset, len);
    if (state_.base.read_only_) return state_.base.read_only_->ReadAt(offset, len);
    return core::make_unexpected(std::string("image file has no base layer"));
}

core::Expected<size_t, std::string> ImageFile::ReadAtInto(uint64_t offset, uint8_t* dst,
                                                             size_t len) {
    FgReadGuard fg_guard;
    core::ReadGuard guard(state_lock_);
    if (state_.base.read_write_) return state_.base.read_write_->ReadAtInto(offset, dst, len);
    if (state_.base.read_only_) return state_.base.read_only_->ReadAtInto(offset, dst, len);
    return core::make_unexpected(std::string("image file has no base layer"));
}

core::Expected<size_t, std::string> ImageFile::WriteAt(uint64_t offset, const uint8_t* data,
                                                          size_t len) {
    // Shared, as in Rust: the upper does its own locking underneath, and the
    // exclusive guard is reserved for the two operations that replace the
    // layer stack. The refresh runs inside the same guard, so a restack
    // cannot swap the base between the write and the size measured after it.
    core::ReadGuard guard(state_lock_);
    LsmtFile* writable = state_.base.Writable();
    if (writable == 0) {
        return core::make_unexpected(std::string("writing read-only image file"));
    }
    const core::Expected<size_t, std::string> written = writable->WriteAt(offset, data, len);
    if (!written.ok()) return written;

    const core::Expected<uint64_t, std::string> refreshed = RefreshSizeMetadataLocked();
    if (!refreshed.ok()) return core::make_unexpected(refreshed.error());
    return written.value();
}

core::Expected<uint64_t, std::string> ImageFile::Size() {
    core::ReadGuard guard(state_lock_);
    // Returns what this refresh computed rather than re-reading the counter:
    // the two can differ under concurrency, and Rust's `size()` is exactly
    // `refresh_size_metadata`.
    return RefreshSizeMetadataLocked();
}

core::Expected<core::Unit, std::string> ImageFile::Sync() {
    core::ReadGuard guard(state_lock_);
    if (state_.base.read_write_) return state_.base.read_write_->Sync();
    if (state_.base.read_only_) return state_.base.read_only_->Sync();
    return core::make_unexpected(std::string("image file has no base layer"));
}

core::Expected<core::Unit, std::string> ImageFile::Truncate(uint64_t size) {
    core::ReadGuard guard(state_lock_);
    LsmtFile* writable = state_.base.Writable();
    if (writable == 0) {
        return core::make_unexpected(std::string("truncating read-only image file"));
    }
    const core::Expected<core::Unit, std::string> done = writable->Truncate(size);
    if (!done.ok()) return done;

    const core::Expected<uint64_t, std::string> refreshed = RefreshSizeMetadataLocked();
    if (!refreshed.ok()) return core::make_unexpected(refreshed.error());
    return core::Unit();
}

core::Expected<core::Unit, std::string> ImageFile::Discard(uint64_t offset, uint64_t len) {
    core::ReadGuard guard(state_lock_);
    LsmtFile* writable = state_.base.Writable();
    if (writable == 0) {
        return core::make_unexpected(std::string("discarding read-only image file"));
    }
    const core::Expected<core::Unit, std::string> done = writable->Discard(offset, len);
    if (!done.ok()) return done;

    const core::Expected<uint64_t, std::string> refreshed = RefreshSizeMetadataLocked();
    if (!refreshed.ok()) return core::make_unexpected(refreshed.error());
    return core::Unit();
}

core::Expected<core::Optional<uint64_t>, std::string> ImageFile::SeekData(uint64_t offset) {
    core::ReadGuard guard(state_lock_);
    if (state_.base.read_write_) return state_.base.read_write_->SeekData(offset);
    if (state_.base.read_only_) return state_.base.read_only_->SeekData(offset);
    return core::make_unexpected(std::string("image file has no base layer"));
}

core::Expected<core::Optional<uint64_t>, std::string> ImageFile::SeekHole(uint64_t offset) {
    core::ReadGuard guard(state_lock_);
    if (state_.base.read_write_) return state_.base.read_write_->SeekHole(offset);
    if (state_.base.read_only_) return state_.base.read_only_->SeekHole(offset);
    return core::make_unexpected(std::string("image file has no base layer"));
}

core::Expected<core::Unit, std::string> ImageFile::EvictRange(uint64_t offset, uint64_t len) {
    core::ReadGuard guard(state_lock_);
    if (state_.base.read_write_) return state_.base.read_write_->EvictRange(offset, len);
    if (state_.base.read_only_) return state_.base.read_only_->EvictRange(offset, len);
    return core::make_unexpected(std::string("image file has no base layer"));
}

core::Expected<core::Unit, std::string> ImageFile::EvictAll() {
    core::ReadGuard guard(state_lock_);
    if (state_.base.read_write_) return state_.base.read_write_->EvictAll();
    if (state_.base.read_only_) return state_.base.read_only_->EvictAll();
    return core::make_unexpected(std::string("image file has no base layer"));
}

// ---------------------------------------------------------------------------
// Context-forwarding variants
// ---------------------------------------------------------------------------
//
// These exist to hand `ctx` down, not to use it here: `ImageFile` is always a
// wrapper, so dropping the context at this level would cut the entire stack
// below it back to synchronous I/O.

core::Expected<ByteBuffer, std::string> ImageFile::ReadAtWithCtx(const IoCtx& ctx,
                                                                    uint64_t offset,
                                                                    size_t len) {
    // Same foreground accounting as `ReadAt`: ublk queue threads enter here.
    FgReadGuard fg_guard;
    core::ReadGuard guard(state_lock_);
    if (state_.base.read_write_) return state_.base.read_write_->ReadAtWithCtx(ctx, offset, len);
    if (state_.base.read_only_) return state_.base.read_only_->ReadAtWithCtx(ctx, offset, len);
    return core::make_unexpected(std::string("image file has no base layer"));
}

core::Expected<size_t, std::string> ImageFile::ReadAtIntoWithCtx(const IoCtx& ctx,
                                                                    uint64_t offset,
                                                                    uint8_t* dst,
                                                                    size_t len) {
    FgReadGuard fg_guard;
    core::ReadGuard guard(state_lock_);
    if (state_.base.read_write_) {
        return state_.base.read_write_->ReadAtIntoWithCtx(ctx, offset, dst, len);
    }
    if (state_.base.read_only_) {
        return state_.base.read_only_->ReadAtIntoWithCtx(ctx, offset, dst, len);
    }
    return core::make_unexpected(std::string("image file has no base layer"));
}

core::Expected<size_t, std::string> ImageFile::WriteAtWithCtx(const IoCtx& ctx, uint64_t offset,
                                                                 const uint8_t* data,
                                                                 size_t len) {
    // No `FgReadGuard`, matching Rust: the write path is the one that does not
    // hold it. The size refresh stays inside the same guard as the write.
    core::ReadGuard guard(state_lock_);
    LsmtFile* writable = state_.base.Writable();
    if (writable == 0) {
        return core::make_unexpected(std::string("writing read-only image file"));
    }
    const core::Expected<size_t, std::string> written =
        writable->WriteAtWithCtx(ctx, offset, data, len);
    if (!written.ok()) return written;

    const core::Expected<uint64_t, std::string> refreshed = RefreshSizeMetadataLocked();
    if (!refreshed.ok()) return core::make_unexpected(refreshed.error());
    return written.value();
}

// ---------------------------------------------------------------------------
// Snapshot export
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> ExportUpperAsSnapshotLayer(
    const ImageService& image_service, ImageFile* image,
    const std::string& output_layer_path) {
    (void)image_service;  // Rust takes `_image_service` and does not use it.
    if (image == 0) {
        return core::make_unexpected(std::string("export snapshot requires an image"));
    }
    // Nothing accumulated, nothing to snapshot. Reported as success, and the
    // output file is deliberately not created -- see the header note.
    if (image->IsReadOnly()) return core::Unit();

    const core::Expected<std::shared_ptr<LocalFile>, std::string> output_file =
        LocalFile::New(output_layer_path);
    if (!output_file.ok()) {
        return core::make_unexpected("create overlaybd snapshot output failed: " +
                                     output_layer_path + ": " + output_file.error());
    }

    const core::Expected<core::Unit, std::string> exported =
        image->ExportUpperAsSealed(CommitArgs(output_file.value()));
    if (!exported.ok()) {
        return core::make_unexpected(
            "export overlaybd writable upper as snapshot failed: " + output_layer_path + ": " +
            exported.error());
    }
    return core::Unit();
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
