// SPDX-License-Identifier: MIT
// Rust: src/image/cache/service.rs
#include "agentenv/image/cache_service.h"

#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "agentenv/cfg.h"
#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/logging.h"
#include "agentenv/image/commit_index.h"
#include "agentenv/storage/overlaybd/layer_metadata.h"

namespace agentenv {
namespace image {
namespace cache {

const char* const kImageCacheConfigDir = "configs";
const char* const kImageCacheIndexDir = "indexes";
const char* const kImageCacheMetadataDir = "metadata";
const char* const kImageCacheStagingDir = "staging";

const char* HoldNamespaceAsStr(HoldNamespace ns) {
    return ns == HoldNamespace::Runtime ? kRuntimeHoldNamespace : kPausedHoldNamespace;
}

const char* HoldNamespaceReleaseReason(HoldNamespace ns) {
    return ns == HoldNamespace::Runtime ? "runtime_release" : "paused_release";
}

namespace {

/// The KvStore log inside `metadata/` (no Rust counterpart; see `MetadataStore`).
const char* const kMetadataStoreLogFile = "store.log";

template <typename T>
core::Expected<T, std::string> WithContext(const core::Expected<T, std::string>& result,
                                           const std::string& context) {
    if (result.ok()) return result;
    return core::make_unexpected(context + ": " + result.error());
}

/// Rust `read_overlaybd_layer_uuid` returns a `Uuid` that is nil when the
/// stored string does not parse; the C++ reader returns the raw string.
core::Expected<core::Uuid, std::string> ReadLayerUuid(const std::string& path) {
    const core::Expected<std::string, std::string> raw =
        storage::overlaybd::ReadOverlaybdLayerUuid(path);
    if (!raw.ok()) return core::make_unexpected(raw.error());
    core::Uuid uuid;
    if (!core::Uuid::Parse(raw.value(), &uuid)) return core::Uuid();
    return uuid;
}

}  // namespace

// ---- sessions ---------------------------------------------------------------

/// Rust `SourceImageSession` — the per-image lock plus the two sidecar paths.
struct ImageCacheService::SourceImageSession {
    std::shared_ptr<ImageCacheService> image_cache;
    std::shared_ptr<SourceImageLock> lock;
    std::string image_config_path;
    std::string metadata_path;

    /// Rust `lookup_cached_config`.
    core::Expected<CachedImageConfig, std::string> LookupCachedConfig() {
        CachedImageConfig out;
        if (!SourceImageConfigIsUsable(image_config_path)) return out;
        const core::Expected<core::Optional<ImageResolutionMetadata>, std::string> metadata =
            ReadSourceImageMetadata(metadata_path);
        if (!metadata.ok()) return core::make_unexpected(metadata.error());
        out.kind = CachedImageConfig::Kind::Found;
        out.image_config_path = image_config_path;
        out.metadata = metadata.value();
        image_cache->RecordSourceConfigRefsFromConfigPathBestEffort(
            image_config_path,
            metadata.value().has_value() ? "resolve_cache_hit" : "resolve_rebuild_metadata");
        return out;
    }

    /// Rust `begin_conversion`.
    core::Expected<std::shared_ptr<OperationHoldConversion>, std::string> BeginConversion() {
        const core::Expected<core::Unit, std::string> layout = image_cache->EnsureLayout();
        if (!layout.ok()) return core::make_unexpected(layout.error());
        return image_cache->BeginHardCommitOperation("oci-conversion");
    }

    /// Rust `complete_cached_metadata`.
    core::Expected<std::string, std::string> CompleteCachedMetadata(
        const ImageResolutionMetadata& metadata) {
        const core::Expected<core::Unit, std::string> layout = image_cache->EnsureLayout();
        if (!layout.ok()) return core::make_unexpected(layout.error());
        const core::Expected<core::Unit, std::string> written = WriteJsonAtomic(
            metadata_path, ImageResolutionMetadataToJson(metadata), "image metadata");
        if (!written.ok()) return core::make_unexpected(written.error());
        return image_cache_config_path();
    }

    std::string image_cache_config_path() const { return image_config_path; }
};

/// Rust `ConversionSession` — keeps the per-image lock and forwards to the
/// hold. Destroying it releases the hold, then the lock.
class ImageCacheService::ConversionSession : public ImageConversion {
 public:
    ConversionSession(std::shared_ptr<SourceImageSession> session,
                      std::shared_ptr<OperationHoldConversion> hold)
        : session_(std::move(session)), hold_(std::move(hold)) {}
    ~ConversionSession() {
        // Field order in Rust drops `_session` first, then `hold`; the hold's
        // release does not depend on the lock, so either order is safe. Release
        // the hold first so a waiter that takes the lock sees it gone.
        hold_.reset();
        session_.reset();
    }

    std::string RemoteLayerDirHint(const std::string& d) override {
        return hold_->RemoteLayerDirHint(d);
    }
    core::Expected<StagingDir, std::string> CreateTempStagingDir() override {
        return hold_->CreateTempStagingDir();
    }
    core::Expected<core::Optional<LocalLayer>, std::string> LookupConvertedLayer(
        const LayerConversionKey& key) override {
        return hold_->LookupConvertedLayer(key);
    }
    core::Expected<LocalLayer, std::string> StoreConvertedLayer(
        const LayerConversionKey& key, const std::string& temp_commit) override {
        return hold_->StoreConvertedLayer(key, temp_commit);
    }

 private:
    std::shared_ptr<SourceImageSession> session_;
    std::shared_ptr<OperationHoldConversion> hold_;
};

// ---- construction -----------------------------------------------------------

ImageCacheService::ImageCacheService(const cfg::ResolvedImageCacheConfig& config)
    : commit_store_(config.commit_store),
      index_dir_(core::fs::Join(config.root_dir, kImageCacheIndexDir)),
      config_dir_(core::fs::Join(config.root_dir, kImageCacheConfigDir)),
      metadata_store_path_(core::fs::Join(config.root_dir, kImageCacheMetadataDir)),
      staging_dir_(core::fs::Join(config.root_dir, kImageCacheStagingDir)),
      source_images_(std::make_shared<SourceImageCache>()) {}

std::shared_ptr<ImageCacheService> ImageCacheService::FromResolvedConfig(
    const cfg::ResolvedImageCacheConfig& config) {
    return std::shared_ptr<ImageCacheService>(new ImageCacheService(config));
}

std::shared_ptr<ImageCacheService> ImageCacheService::SharedFromResolvedConfig(
    const cfg::ResolvedImageCacheConfig& config) {
    static std::mutex mu;
    static std::map<std::string, std::shared_ptr<ImageCacheService> >* services =
        new std::map<std::string, std::shared_ptr<ImageCacheService> >();
    const std::string key = StablePathIdentity(config.root_dir);
    std::lock_guard<std::mutex> guard(mu);
    std::map<std::string, std::shared_ptr<ImageCacheService> >::const_iterator it =
        services->find(key);
    if (it != services->end()) return it->second;
    std::shared_ptr<ImageCacheService> service = FromResolvedConfig(config);
    (*services)[key] = service;
    return service;
}

std::shared_ptr<ImageCacheService> ImageCacheService::SharedFromAppConfig(
    const cfg::AppConfig& config) {
    return SharedFromResolvedConfig(config.ImageCacheLayout());
}

void ImageCacheService::InitializeP2pTransport(std::shared_ptr<p2p::P2pTransport> transport) {
    std::lock_guard<std::mutex> guard(transport_mu_);
    if (p2p_transport_) {
        AGENTENV_ERROR("image cache P2P transport already initialized");
        std::abort();
    }
    p2p_transport_ = std::move(transport);
}

std::shared_ptr<p2p::P2pTransport> ImageCacheService::P2pTransport() const {
    std::lock_guard<std::mutex> guard(transport_mu_);
    return p2p_transport_;
}

core::Expected<core::Unit, std::string> ImageCacheService::EnsureLayout() const {
    const std::pair<const char*, const std::string*> dirs[] = {
        std::make_pair("overlaybd commit cache", &commit_store_),
        std::make_pair("image cache index", &index_dir_),
        std::make_pair("image cache config", &config_dir_),
        std::make_pair("image cache metadata", &metadata_store_path_),
        std::make_pair("image cache staging", &staging_dir_),
    };
    for (std::size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        const core::Expected<core::Unit, std::string> created =
            core::fs::CreateDirAll(*dirs[i].second);
        if (!created.ok()) {
            return core::make_unexpected(std::string("create ") + dirs[i].first + " dir " +
                                         *dirs[i].second + ": " + created.error());
        }
    }
    return core::Unit();
}

core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string>
ImageCacheService::MetadataStore() {
    std::lock_guard<std::mutex> guard(metadata_mu_);
    if (metadata_store_) return metadata_store_;
    // Rust opens RocksDB *as* the `metadata/` directory. The C++ KvStore is a
    // single log file, so it lives inside that directory instead.
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> opened =
        ImageCacheMetadataStore::Open(core::fs::Join(metadata_store_path_, kMetadataStoreLogFile),
                                      local_store::Durability::Sync);
    if (!opened.ok()) return core::make_unexpected(opened.error());
    metadata_store_ = opened.value();
    return metadata_store_;
}

core::Expected<std::pair<std::string, std::string>, std::string>
ImageCacheService::SourceImagePaths(const std::string& manifest_digest,
                                    const core::Optional<std::string>& repository_scope) const {
    const core::Expected<ImageCacheSourceImageKey, std::string> key =
        ImageCacheSourceImageKey::New(manifest_digest, repository_scope);
    if (!key.ok()) return core::make_unexpected(key.error());
    const std::string image_config_path =
        core::fs::Join(config_dir_, SourceImageConfigFilename(key.value()));
    return std::make_pair(image_config_path,
                          SourceImageMetadataPathForConfigPath(image_config_path));
}

core::Expected<std::shared_ptr<ImageCacheService::SourceImageSession>, std::string>
ImageCacheService::OpenSourceImage(const std::string& manifest_digest,
                                   const core::Optional<std::string>& repository_scope) {
    const core::Expected<std::pair<std::string, std::string>, std::string> paths =
        SourceImagePaths(manifest_digest, repository_scope);
    if (!paths.ok()) return core::make_unexpected(paths.error());
    std::shared_ptr<SourceImageSession> session = std::make_shared<SourceImageSession>();
    session->image_cache = shared_from_this();
    session->lock = source_images_->AcquireLock(paths.value().first);
    session->image_config_path = paths.value().first;
    session->metadata_path = paths.value().second;
    return session;
}

// ---- metadata facts ---------------------------------------------------------

core::Expected<std::set<HardCommitId>, std::string>
ImageCacheService::CommitStoreHardCommitsFromConfigPaths(
    const std::vector<std::string>& config_paths, const std::string& context) const {
    std::set<HardCommitId> objects;
    for (std::size_t i = 0; i < config_paths.size(); ++i) {
        const core::Expected<std::vector<HardCommitId>, std::string> digests =
            ImageCacheMetadataStore::CommitStoreHardCommitDigestsFromConfigPath(config_paths[i],
                                                                                commit_store_);
        if (!digests.ok()) {
            return core::make_unexpected(context + " " + config_paths[i] + ": " +
                                         digests.error());
        }
        objects.insert(digests.value().begin(), digests.value().end());
    }
    return objects;
}

core::Expected<core::Unit, std::string> ImageCacheService::RecordSourceConfigRefsFromConfigPath(
    const std::string& image_config_path) {
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        MetadataStore();
    if (!store.ok()) return core::make_unexpected(store.error());
    return store.value()->RecordConfigRefsFromConfigPath(image_config_path, commit_store_);
}

void ImageCacheService::RecordSourceConfigRefsFromConfigPathBestEffort(
    const std::string& image_config_path, const std::string& reason) {
    const core::Expected<core::Unit, std::string> recorded =
        RecordSourceConfigRefsFromConfigPath(image_config_path);
    if (!recorded.ok()) {
        AGENTENV_WARN("failed to record image cache source config refs: reason="
                      << reason << " config=" << image_config_path
                      << " error=" << recorded.error());
    }
}

core::Expected<core::Unit, std::string> ImageCacheService::RebuildMetadataFromConfigs() {
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        MetadataStore();
    if (!store.ok()) return core::make_unexpected(store.error());
    return store.value()->RebuildFromConfigs(config_dir_, commit_store_);
}

core::Expected<core::Unit, std::string> ImageCacheService::RecordHardCommitObject(
    const std::string& digest, const core::Optional<std::string>& file,
    const core::Optional<uint64_t>& size, const std::vector<p2p::P2pArtifactKey>& p2p_keys) {
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        MetadataStore();
    if (!store.ok()) return core::make_unexpected(store.error());
    const core::Expected<HardCommitId, std::string> id = HardCommitId::New(digest);
    if (!id.ok()) return core::make_unexpected(id.error());
    return store.value()->RecordHardCommitObject(
        id.value(), file, size, std::set<p2p::P2pArtifactKey>(p2p_keys.begin(), p2p_keys.end()));
}

core::Expected<core::Unit, std::string> ImageCacheService::AddHardCommitP2pKey(
    const std::string& digest, const p2p::P2pArtifactKey& key) {
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        MetadataStore();
    if (!store.ok()) return core::make_unexpected(store.error());
    const core::Expected<HardCommitId, std::string> id = HardCommitId::New(digest);
    if (!id.ok()) return core::make_unexpected(id.error());
    return store.value()->AddHardCommitP2pKey(id.value(), key);
}

core::Expected<bool, std::string> ImageCacheService::PublishConvertedLayer(
    const LayerConversionKey& key, const LocalLayer& layer) {
    const std::shared_ptr<p2p::P2pTransport> transport = P2pTransport();
    if (!transport) return false;
    p2p::P2pEndpoint endpoint;
    if (!transport->LocalEndpoint(&endpoint)) return false;

    const p2p::P2pArtifactKey p2p_key = key.P2pKey();
    const std::string metadata = ConvertedLayerP2pMetadata::FromLocalLayer(layer).ToJson().ToString();
    const core::Expected<core::Unit, std::string> owned = AddHardCommitP2pKey(layer.digest, p2p_key);
    if (!owned.ok()) {
        return core::make_unexpected("record converted image layer P2P ownership '" + p2p_key +
                                     "': " + owned.error());
    }

    p2p::P2pPublishRequest request = p2p::P2pPublishRequest::File(p2p_key, layer.path);
    request.WithPublishMode(p2p::P2pPublishMode::Reference).WithMetadata(metadata);
    const p2p::P2pResult<core::Unit> published = transport->Publish(request);
    if (!published.ok()) {
        return core::make_unexpected("publish converted image layer '" + p2p_key +
                                     "': " + published.error().ToString());
    }
    return true;
}

core::Expected<std::string, std::string> ImageCacheService::SeedAndRecordHardCommit(
    const std::string& source, const std::string& digest, uint64_t size, bool trusted_descriptor,
    const std::vector<p2p::P2pArtifactKey>& p2p_keys) {
    const core::Expected<std::string, std::string> cached =
        trusted_descriptor ? SeedCommitFileTrustedDescriptor(commit_store_, source, digest, size)
                           : SeedCommitFile(commit_store_, source, digest, size);
    if (!cached.ok()) return cached;
    const core::Expected<core::Unit, std::string> recorded = RecordHardCommitObject(
        digest, core::Optional<std::string>(cached.value()), core::Optional<uint64_t>(size),
        p2p_keys);
    if (!recorded.ok()) return core::make_unexpected(recorded.error());
    return cached;
}

core::Expected<std::string, std::string> ImageCacheService::ImportHardCommitTrustedDescriptor(
    const std::string& source, const std::string& digest, uint64_t size) {
    std::set<std::string> digests;
    digests.insert(digest);
    const core::Expected<std::shared_ptr<OperationHoldConversion>, std::string> hold =
        AcquireOperationHoldForHardCommits("import_hard_commit_trusted_descriptor", digests);
    if (!hold.ok()) return core::make_unexpected(hold.error());
    const core::Expected<std::string, std::string> result =
        SeedAndRecordHardCommit(source, digest, size, true, std::vector<p2p::P2pArtifactKey>());
    hold.value()->ReleaseBestEffort("hard_commit_import_release");
    return result;
}

// ---- holds ------------------------------------------------------------------

core::Expected<core::Unit, std::string> ImageCacheService::CreateOrReplaceHold(
    const ImageCacheHoldOwner& owner, const std::set<HardCommitId>& refs) {
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        MetadataStore();
    if (!store.ok()) return core::make_unexpected(store.error());
    return store.value()->CreateOrReplaceHold(owner, refs);
}

core::Expected<core::Unit, std::string> ImageCacheService::ReleaseHold(
    const ImageCacheHoldOwner& owner) {
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        MetadataStore();
    if (!store.ok()) return core::make_unexpected(store.error());
    return store.value()->ReleaseHold(owner);
}

void ImageCacheService::ReleaseHoldBestEffort(const ImageCacheHoldOwner& owner,
                                              const std::string& reason) {
    const core::Expected<core::Unit, std::string> released = ReleaseHold(owner);
    if (!released.ok()) {
        AGENTENV_WARN("failed to release image cache hold: reason="
                      << reason << " owner=" << owner.ToString() << " error=" << released.error());
    }
}

core::Expected<std::vector<std::string>, std::string> ImageCacheService::ListHoldKeysInNamespace(
    const std::string& ns) {
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        MetadataStore();
    if (!store.ok()) return core::make_unexpected(store.error());
    const core::Expected<std::vector<ImageCacheHoldOwner>, std::string> owners =
        store.value()->ListHoldOwnersInNamespaces(std::vector<std::string>(1, ns));
    if (!owners.ok()) return core::make_unexpected(owners.error());
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < owners.value().size(); ++i) keys.push_back(owners.value()[i].Key());
    return keys;
}

core::Expected<std::size_t, std::string> ImageCacheService::CleanupStaleRuntimeHolds() {
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        MetadataStore();
    if (!store.ok()) return core::make_unexpected(store.error());
    std::vector<std::string> namespaces;
    namespaces.push_back("runtime");
    namespaces.push_back("operation");
    const core::Expected<std::vector<ImageCacheHoldOwner>, std::string> released =
        store.value()->ReleaseHoldsInNamespaces(namespaces);
    if (!released.ok()) return core::make_unexpected(released.error());
    if (!released.value().empty()) {
        AGENTENV_INFO("released stale runtime image-cache holds at startup: count="
                      << released.value().size());
    }
    return released.value().size();
}

core::Expected<std::shared_ptr<OperationHoldConversion>, std::string>
ImageCacheService::AcquireOperationHoldForHardCommits(const std::string& operation,
                                                      const std::set<std::string>& digests) {
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        MetadataStore();
    if (!store.ok()) return core::make_unexpected(store.error());
    const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> hold =
        ImageCacheOperationHold::AcquireForHardCommits(store.value(), operation, digests);
    if (!hold.ok()) return core::make_unexpected(hold.error());
    return std::make_shared<OperationHoldConversion>(shared_from_this(), hold.value());
}

core::Expected<std::shared_ptr<OperationHoldConversion>, std::string>
ImageCacheService::BeginHardCommitOperation(const std::string& operation) {
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        MetadataStore();
    if (!store.ok()) return core::make_unexpected(store.error());
    const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> hold =
        ImageCacheOperationHold::Begin(store.value(), operation);
    if (!hold.ok()) return core::make_unexpected(hold.error());
    return std::make_shared<OperationHoldConversion>(shared_from_this(), hold.value());
}

core::Expected<core::Unit, std::string> ImageCacheService::AcquireHoldForConfigPaths(
    const ImageCacheHoldOwner& owner, const std::vector<std::string>& config_paths) {
    const core::Expected<std::set<HardCommitId>, std::string> refs =
        CommitStoreHardCommitsFromConfigPaths(config_paths, "parse image-cache hold config");
    if (!refs.ok()) return core::make_unexpected(refs.error());
    if (refs.value().empty()) return core::Unit();
    return CreateOrReplaceHold(owner, refs.value());
}

core::Expected<core::Unit, std::string> ImageCacheService::Protect(
    HoldNamespace ns, const std::string& owner_id,
    const std::vector<std::string>& image_config_paths) {
    const core::Expected<ImageCacheHoldOwner, std::string> owner =
        ImageCacheHoldOwner::New(HoldNamespaceAsStr(ns), owner_id);
    if (!owner.ok()) return core::make_unexpected(owner.error());
    return AcquireHoldForConfigPaths(owner.value(), image_config_paths);
}

void ImageCacheService::ReleaseProtectionBestEffort(HoldNamespace ns,
                                                    const std::string& owner_id) {
    const core::Expected<ImageCacheHoldOwner, std::string> owner =
        ImageCacheHoldOwner::New(HoldNamespaceAsStr(ns), owner_id);
    if (owner.ok()) ReleaseHoldBestEffort(owner.value(), HoldNamespaceReleaseReason(ns));
}

core::Expected<core::Unit, std::string> ImageCacheService::ReconcileNamespace(
    HoldNamespace ns, const std::vector<std::string>& live_owner_ids) {
    const core::Expected<std::size_t, std::string> cleaned = CleanupStaleRuntimeHolds();
    if (!cleaned.ok()) return core::make_unexpected(cleaned.error());
    const std::set<std::string> live(live_owner_ids.begin(), live_owner_ids.end());
    const core::Expected<std::vector<std::string>, std::string> keys =
        ListHoldKeysInNamespace(HoldNamespaceAsStr(ns));
    if (!keys.ok()) return core::make_unexpected(keys.error());
    for (std::size_t i = 0; i < keys.value().size(); ++i) {
        if (live.count(keys.value()[i]) != 0) continue;
        const core::Expected<ImageCacheHoldOwner, std::string> owner =
            ImageCacheHoldOwner::New(HoldNamespaceAsStr(ns), keys.value()[i]);
        if (owner.ok()) ReleaseHoldBestEffort(owner.value(), "startup_reconcile_orphan");
    }
    return core::Unit();
}

core::Expected<ImageCacheGcSummary, std::string> ImageCacheService::RunMaintenance(
    const std::vector<std::pair<std::string, std::vector<std::string> > >& running,
    const core::Optional<std::pair<uint64_t, uint64_t> >& watermark, uint64_t min_age_secs) {
    bool reconciled = false;
    if (watermark.has_value()) {
        const core::Expected<core::Unit, std::string> evicted =
            EvictSourceConfigsOverCapacity(watermark->first, watermark->second, min_age_secs);
        if (!evicted.ok()) return core::make_unexpected(evicted.error());
        reconciled = true;
    }
    const core::Expected<ImageCacheLiveRuntimeRefs, std::string> live_refs =
        LiveRefsFromRunning(running);
    if (!live_refs.ok()) return core::make_unexpected(live_refs.error());
    const core::Expected<ImageCacheGcReport, std::string> report =
        RunGc(live_refs.value(), !reconciled);
    if (!report.ok()) return core::make_unexpected(report.error());
    return SummaryFromReport(report.value());
}

// ---- GC -----------------------------------------------------------------------

core::Expected<ImageCacheLiveRuntimeRefs, std::string> ImageCacheService::LiveRefsFromRunning(
    const std::vector<std::pair<std::string, std::vector<std::string> > >& running) {
    ImageCacheLiveRuntimeRefs refs;
    for (std::size_t i = 0; i < running.size(); ++i) {
        const core::Expected<ImageCacheHoldOwner, std::string> owner =
            ImageCacheHoldOwner::New(kRuntimeHoldNamespace, running[i].first);
        if (!owner.ok()) return core::make_unexpected(owner.error());
        const core::Expected<std::set<HardCommitId>, std::string> objects =
            CommitStoreHardCommitsFromConfigPaths(running[i].second,
                                                  "parse live runtime image config");
        if (!objects.ok()) return core::make_unexpected(objects.error());
        for (std::set<HardCommitId>::const_iterator it = objects.value().begin();
             it != objects.value().end(); ++it) {
            refs[*it].push_back(owner.value());
        }
    }
    for (ImageCacheLiveRuntimeRefs::iterator it = refs.begin(); it != refs.end(); ++it) {
        std::sort(it->second.begin(), it->second.end());
        it->second.erase(std::unique(it->second.begin(), it->second.end()), it->second.end());
    }
    return refs;
}

core::Expected<ImageCacheGc, std::string> ImageCacheService::Gc() {
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        MetadataStore();
    if (!store.ok()) return core::make_unexpected(store.error());
    return ImageCacheGc(commit_store_, config_dir_, index_dir_, store.value(), P2pTransport());
}

core::Expected<ImageCacheGcReport, std::string> ImageCacheService::RunGc(
    const ImageCacheLiveRuntimeRefs& live_refs, bool rebuild_metadata) {
    core::Expected<ImageCacheGc, std::string> gc = Gc();
    if (!gc.ok()) return core::make_unexpected(gc.error());
    return gc.value().RunGc(live_refs, rebuild_metadata);
}

core::Expected<std::size_t, std::string> ImageCacheService::ReclaimStaleIndexes() {
    core::Expected<ImageCacheGc, std::string> gc = Gc();
    if (!gc.ok()) return core::make_unexpected(gc.error());
    return gc.value().ReclaimStaleIndexes();
}

core::Expected<core::Unit, std::string> ImageCacheService::EvictSourceConfigsOverCapacity(
    uint64_t high_watermark_bytes, uint64_t low_watermark_bytes, uint64_t min_age_secs) {
    const core::Expected<core::Unit, std::string> rebuilt = RebuildMetadataFromConfigs();
    if (!rebuilt.ok()) return rebuilt;
    const uint64_t now = UnixNowSecs();
    const uint64_t evictable_before = now > min_age_secs ? now - min_age_secs : 0;
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        MetadataStore();
    if (!store.ok()) return core::make_unexpected(store.error());
    const core::Expected<CapacityEvictionPlan, std::string> plan =
        store.value()->PlanCapacityEvictionFromStore(high_watermark_bytes, low_watermark_bytes,
                                                     evictable_before);
    if (!plan.ok()) return core::make_unexpected(plan.error());
    for (std::size_t i = 0; i < plan.value().candidates.size(); ++i) {
        DeleteSourceImageConfigBestEffort(plan.value().candidates[i], evictable_before);
    }
    return core::Unit();
}

void ImageCacheService::DeleteSourceImageConfigBestEffort(
    const CapacityEvictionCandidate& candidate, uint64_t evictable_before) {
    const ImageCacheConfigId& config_id = candidate.config_id;
    const std::string config_path = core::fs::Join(config_dir_, config_id.AsStr());
    const std::shared_ptr<SourceImageLock> lock = source_images_->AcquireLock(config_path);
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> metadata =
        MetadataStore();
    if (!metadata.ok()) {
        AGENTENV_WARN("failed to open image cache metadata while evicting source config: config="
                      << config_path << " error=" << metadata.error());
        return;
    }
    // Recheck under the lock: a resolve may have touched the config since
    // the plan was made.
    const core::Expected<core::Optional<uint64_t>, std::string> last_used =
        metadata.value()->ConfigLastUsed(config_id);
    if (!last_used.ok()) {
        AGENTENV_WARN("failed to recheck source image config eviction candidate: config="
                      << config_path << " error=" << last_used.error());
        return;
    }
    if (!last_used.value().has_value() || *last_used.value() != candidate.last_used ||
        *last_used.value() > evictable_before) {
        return;
    }
    if (::unlink(config_path.c_str()) != 0 && errno != ENOENT) {
        const int err = errno;
        AGENTENV_WARN("failed to evict source image config: config="
                      << config_path << " error=" << std::strerror(err));
        return;
    }
    const core::Expected<core::Unit, std::string> removed =
        metadata.value()->RemoveConfigRefs(config_id);
    if (!removed.ok()) {
        AGENTENV_WARN("failed to remove evicted source image config refs: config="
                      << config_path << " error=" << removed.error());
    }
    (void)core::fs::RemoveFile(SourceImageMetadataPathForConfigPath(config_path));
}

// ---- source images ----------------------------------------------------------

core::Expected<CachedImageConfig, std::string> ImageCacheService::LookupImageConfig(
    const std::string& manifest_digest, const core::Optional<std::string>& repository_scope) {
    const core::Expected<std::shared_ptr<SourceImageSession>, std::string> session =
        OpenSourceImage(manifest_digest, repository_scope);
    if (!session.ok()) return core::make_unexpected(session.error());
    return session.value()->LookupCachedConfig();
}

core::Expected<std::shared_ptr<ImageConversion>, std::string>
ImageCacheService::BeginImageConversion(const std::string& manifest_digest,
                                        const core::Optional<std::string>& repository_scope) {
    const core::Expected<std::shared_ptr<SourceImageSession>, std::string> session =
        OpenSourceImage(manifest_digest, repository_scope);
    if (!session.ok()) return core::make_unexpected(session.error());
    const core::Expected<std::shared_ptr<OperationHoldConversion>, std::string> hold =
        session.value()->BeginConversion();
    if (!hold.ok()) return core::make_unexpected(hold.error());
    return std::shared_ptr<ImageConversion>(new ConversionSession(session.value(), hold.value()));
}

core::Expected<std::string, std::string> ImageCacheService::PublishImageConfig(
    const std::string& manifest_digest, const core::Optional<std::string>& repository_scope,
    const core::Json& overlaybd_config, const ImageResolutionMetadata& metadata,
    std::shared_ptr<ImageConversion> conversion) {
    const core::Expected<core::Unit, std::string> layout = EnsureLayout();
    if (!layout.ok()) return core::make_unexpected(layout.error());
    const core::Expected<std::pair<std::string, std::string>, std::string> paths =
        SourceImagePaths(manifest_digest, repository_scope);
    if (!paths.ok()) return core::make_unexpected(paths.error());
    const core::Expected<core::Unit, std::string> config_written =
        WriteJsonAtomic(paths.value().first, overlaybd_config, "image config");
    if (!config_written.ok()) return core::make_unexpected(config_written.error());
    RecordSourceConfigRefsFromConfigPathBestEffort(paths.value().first,
                                                   "publish_source_image_config");
    const core::Expected<core::Unit, std::string> metadata_written = WriteJsonAtomic(
        paths.value().second, ImageResolutionMetadataToJson(metadata), "image metadata");
    if (!metadata_written.ok()) return core::make_unexpected(metadata_written.error());
    // Rust `drop(conversion)`: only now, after the config root is recorded.
    conversion.reset();
    return paths.value().first;
}

core::Expected<std::string, std::string> ImageCacheService::WriteImageMetadata(
    const std::string& manifest_digest, const core::Optional<std::string>& repository_scope,
    const ImageResolutionMetadata& metadata) {
    const core::Expected<std::shared_ptr<SourceImageSession>, std::string> session =
        OpenSourceImage(manifest_digest, repository_scope);
    if (!session.ok()) return core::make_unexpected(session.error());
    return session.value()->CompleteCachedMetadata(metadata);
}

OverlaybdLayerLocation ImageCacheService::LayerLocation(const std::string& digest, uint64_t size,
                                                        bool has_remote) const {
    OverlaybdLayerLocation out;
    if (!has_remote) {
        const core::Optional<std::string> file = CachedCommitFileIfUsable(commit_store_, digest, size);
        if (file.has_value()) {
            out.kind = OverlaybdLayerLocation::Kind::LocalFile;
            out.path = *file;
            return out;
        }
    }
    out.kind = OverlaybdLayerLocation::Kind::CacheDir;
    out.path = CommitDir(commit_store_, digest);
    return out;
}

std::vector<std::string> ImageCacheService::AllowedOverlaybdPublishRoots() const {
    return std::vector<std::string>(1, commit_store_);
}

// ---- operation hold as ImageConversion --------------------------------------

void OperationHoldConversion::ReleaseBestEffort(const std::string& reason) {
    (void)hold_->Release(reason);
}

std::string OperationHoldConversion::RemoteLayerDirHint(const std::string& layer_digest) {
    return CommitDir(service_->commit_store(), layer_digest);
}

core::Expected<StagingDir, std::string> OperationHoldConversion::CreateTempStagingDir() {
    // On the commit cache's filesystem, so `layer.commit` can be hard-linked
    // into it (avoids EXDEV).
    const std::string& parent = service_->staging_dir();
    const core::Expected<core::Unit, std::string> created = core::fs::CreateDirAll(parent);
    if (!created.ok()) {
        return core::make_unexpected("create staging dir " + parent + ": " + created.error());
    }
    std::string templ = core::fs::Join(parent, ".tmpXXXXXX");
    std::vector<char> buffer(templ.begin(), templ.end());
    buffer.push_back('\0');
    if (::mkdtemp(&buffer[0]) == NULL) {
        const int err = errno;
        return core::make_unexpected("create tempdir in " + parent + ": " + std::strerror(err));
    }
    return StagingDir(std::string(&buffer[0]));
}

std::string OperationHoldConversion::ConversionIndexPath(const LayerConversionKey& key) const {
    return IndexPath(service_->index_dir(), key.source_layer_digest, key.converter_id,
                     key.virtual_size_gib, key.mkfs, key.parent_commit_digest);
}

core::Expected<std::string, std::string> OperationHoldConversion::PersistOverlaybdCommit(
    const std::string& temp_commit, const std::string& digest, uint64_t size) {
    const core::Expected<core::Unit, std::string> held = hold_->Protect(digest);
    if (!held.ok()) return core::make_unexpected(held.error());
    return service_->SeedAndRecordHardCommit(temp_commit, digest, size, true,
                                             std::vector<p2p::P2pArtifactKey>());
}

core::Expected<core::Unit, std::string> OperationHoldConversion::WriteConversionIndex(
    const LayerConversionKey& key, const LocalLayer& layer) {
    const CommitIndex index =
        CommitIndex::OciLayer(key.source_layer_digest, layer.digest, layer.size, key.converter_id,
                              key.virtual_size_gib, key.mkfs, key.parent_commit_digest);
    return index.Write(ConversionIndexPath(key));
}

core::Expected<core::Optional<LocalLayer>, std::string>
OperationHoldConversion::LookupRemoteConvertedLayer(const LayerConversionKey& key) {
    typedef core::Optional<LocalLayer> Result;
    const std::shared_ptr<p2p::P2pTransport> transport = service_->P2pTransport();
    if (!transport) return Result();
    const p2p::P2pArtifactKey p2p_key = key.P2pKey();
    if (!attempted_p2p_lookups_.insert(p2p_key).second) {
        AGENTENV_DEBUG("skipping repeated P2P converted image layer lookup: key=" << p2p_key);
        return Result();
    }
    p2p::P2pArtifactDescriptor descriptor;
    const p2p::P2pResult<bool> found = transport->Lookup(p2p_key, &descriptor);
    if (!found.ok()) {
        AGENTENV_DEBUG("P2P converted image layer lookup failed; falling back to registry "
                       "conversion: key="
                       << p2p_key << " error=" << found.error().ToString());
        return Result();
    }
    if (!found.value()) return Result();

    const core::Expected<core::Json, core::AnyError> raw_metadata =
        core::Json::Parse(descriptor.metadata_json);
    core::Expected<ConvertedLayerP2pMetadata, std::string> metadata =
        raw_metadata.ok() ? ConvertedLayerP2pMetadata::Parse(raw_metadata.value())
                          : core::Expected<ConvertedLayerP2pMetadata, std::string>(
                                core::make_unexpected(raw_metadata.error().chain()));
    if (!metadata.ok()) {
        AGENTENV_DEBUG("P2P converted image layer metadata mismatch; falling back to registry "
                       "conversion: key="
                       << p2p_key << " error=" << metadata.error());
        return Result();
    }

    const core::Expected<StagingDir, std::string> staging = CreateTempStagingDir();
    if (!staging.ok()) return core::make_unexpected(staging.error());
    const std::string destination = core::fs::Join(staging.value().Path(), "overlaybd.commit");
    p2p::P2pFetchOptions options;
    options.advertise = false;
    const p2p::P2pResult<uint64_t> fetched =
        transport->FetchWithOptions(descriptor, destination, options);
    if (!fetched.ok()) {
        AGENTENV_DEBUG("P2P converted image layer fetch failed; falling back to registry "
                       "conversion: key="
                       << p2p_key << " error=" << fetched.error().ToString());
        return Result();
    }
    const core::Expected<core::FileDigest, std::string> described = core::DescribeFile(destination);
    if (!described.ok()) {
        AGENTENV_DEBUG("P2P converted image layer digest failed; falling back to registry "
                       "conversion: key="
                       << p2p_key << " error=" << described.error());
        return Result();
    }
    if (described.value().sha256 != metadata.value().commit_digest ||
        described.value().size != metadata.value().size) {
        AGENTENV_DEBUG("P2P converted image layer failed digest or size validation: key="
                       << p2p_key << " expected_digest=" << metadata.value().commit_digest
                       << " actual_digest=" << described.value().sha256
                       << " expected_size=" << metadata.value().size
                       << " actual_size=" << described.value().size);
        return Result();
    }
    const core::Expected<core::Uuid, std::string> uuid = ReadLayerUuid(destination);
    if (!uuid.ok()) {
        AGENTENV_DEBUG("P2P converted image layer is not a sealed OverlayBD layer: key="
                       << p2p_key << " error=" << uuid.error());
        return Result();
    }
    if (uuid.value() != key.expected_layer_uuid) {
        AGENTENV_DEBUG("P2P converted image layer UUID mismatch: key="
                       << p2p_key << " found_uuid=" << uuid.value().ToString()
                       << " expected_uuid=" << key.expected_layer_uuid.ToString());
        return Result();
    }

    const core::Expected<std::string, std::string> stored = PersistOverlaybdCommit(
        destination, metadata.value().commit_digest, metadata.value().size);
    if (!stored.ok()) return core::make_unexpected(stored.error());
    LocalLayer layer;
    layer.path = stored.value();
    layer.digest = metadata.value().commit_digest;
    layer.size = metadata.value().size;
    const core::Expected<bool, std::string> published = service_->PublishConvertedLayer(key, layer);
    if (!published.ok()) {
        AGENTENV_DEBUG("failed to publish reused converted image layer to P2P: key="
                       << p2p_key << " error=" << published.error());
    }
    const core::Expected<core::Unit, std::string> indexed = WriteConversionIndex(key, layer);
    if (!indexed.ok()) return core::make_unexpected(indexed.error());
    AGENTENV_INFO("reused converted user image layer from P2P: key=" << p2p_key
                                                                      << " digest=" << layer.digest);
    return Result(layer);
}

core::Expected<core::Optional<LocalLayer>, std::string>
OperationHoldConversion::LookupConvertedLayer(const LayerConversionKey& key) {
    typedef core::Optional<LocalLayer> Result;
    const std::string index_path = ConversionIndexPath(key);
    const core::Expected<core::Optional<CommitIndex>, std::string> read =
        CommitIndex::Read(index_path);
    if (!read.ok()) return core::make_unexpected(read.error());
    if (!read.value().has_value()) {
        AGENTENV_DEBUG("converted user image layer index is missing from cache: digest="
                       << key.source_layer_digest << " path=" << index_path);
        return LookupRemoteConvertedLayer(key);
    }
    const CommitIndex& index = *read.value();
    if (!index.MatchesOciContext(key.source_layer_digest, key.converter_id, key.virtual_size_gib,
                                 key.mkfs, key.parent_commit_digest)) {
        AGENTENV_DEBUG("converted user image layer index has stale conversion context: digest="
                       << key.source_layer_digest << " path=" << index_path);
        return LookupRemoteConvertedLayer(key);
    }

    // Protect the indexed commit BEFORE stat'ing its file: GC re-checks hold
    // referrers at delete time, so this blocks deletion of a commit about to
    // be reused. If GC already won, the stat below misses and the caller
    // converts afresh; both outcomes are safe.
    const core::Expected<core::Unit, std::string> held = hold_->Protect(index.commit_digest);
    if (!held.ok()) return core::make_unexpected(held.error());

    const std::string cached = CommitFile(service_->commit_store(), index.commit_digest);
    const core::Expected<core::fs::FileStat, std::string> stat = core::fs::Stat(cached);
    if (!(stat.ok() && stat.value().is_regular && stat.value().size > 0 &&
          stat.value().size == index.size)) {
        AGENTENV_INFO("converted user image layer commit is missing from cache: digest="
                      << key.source_layer_digest << " commit_digest=" << index.commit_digest
                      << " path=" << cached);
        return LookupRemoteConvertedLayer(key);
    }

    const core::Expected<core::Uuid, std::string> uuid = ReadLayerUuid(cached);
    if (!uuid.ok()) {
        AGENTENV_INFO("converted user image layer cache is not a valid sealed overlaybd layer: "
                      "digest="
                      << key.source_layer_digest << " commit_digest=" << index.commit_digest
                      << " path=" << cached << " error=" << uuid.error());
        return LookupRemoteConvertedLayer(key);
    }
    if (uuid.value() != key.expected_layer_uuid) {
        AGENTENV_INFO("converted user image layer cache has stale UUID: digest="
                      << key.source_layer_digest << " commit_digest=" << index.commit_digest
                      << " path=" << cached << " found_uuid=" << uuid.value().ToString()
                      << " expected_uuid=" << key.expected_layer_uuid.ToString());
        return LookupRemoteConvertedLayer(key);
    }

    AGENTENV_INFO("user image layer already converted, reusing content-addressed cache: digest="
                  << key.source_layer_digest << " commit_digest=" << index.commit_digest
                  << " path=" << cached);
    LocalLayer layer;
    layer.path = cached;
    layer.digest = index.commit_digest;
    layer.size = index.size;
    return Result(layer);
}

core::Expected<LocalLayer, std::string> OperationHoldConversion::StoreConvertedLayer(
    const LayerConversionKey& key, const std::string& temp_commit) {
    const core::Expected<core::FileDigest, std::string> described = core::DescribeFile(temp_commit);
    if (!described.ok()) {
        return core::make_unexpected("describe converted user image layer " + temp_commit + ": " +
                                     described.error());
    }
    const core::Expected<std::string, std::string> stored =
        PersistOverlaybdCommit(temp_commit, described.value().sha256, described.value().size);
    if (!stored.ok()) return core::make_unexpected(stored.error());
    LocalLayer layer;
    layer.path = stored.value();
    layer.digest = described.value().sha256;
    layer.size = described.value().size;
    const core::Expected<bool, std::string> published = service_->PublishConvertedLayer(key, layer);
    if (!published.ok()) {
        AGENTENV_DEBUG("failed to publish converted image layer to P2P: key="
                       << key.P2pKey() << " error=" << published.error());
    }
    const core::Expected<core::Unit, std::string> indexed = WriteConversionIndex(key, layer);
    if (!indexed.ok()) return core::make_unexpected(indexed.error());
    return layer;
}

}  // namespace cache

void InitializeImageCacheP2pTransport(const cfg::AppConfig& config,
                                      std::shared_ptr<p2p::P2pTransport> transport) {
    // No null guard, matching upstream, which cannot express one: its
    // `Arc<dyn P2pTransport>` is always inhabited. The same holds at the only
    // caller here -- `p2p::TransportFromConfig` returns a non-null transport
    // on every arm (a `disabled` node gets a `DisabledP2pTransport`) or an
    // error, so `main.cc` has nothing else to pass.
    //
    // A guard was tried and removed: `InitializeP2pTransport` tests the
    // stored pointer's truthiness rather than whether it was ever assigned,
    // so storing a null is indistinguishable from storing nothing. The branch
    // changed no observable behaviour, and the test written for it asserted a
    // difference that did not exist.
    cache::ImageCacheService::SharedFromAppConfig(config)->InitializeP2pTransport(
        std::move(transport));
}

}  // namespace image
}  // namespace agentenv
