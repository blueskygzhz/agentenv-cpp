// SPDX-License-Identifier: MIT
// Rust: src/image/cache/service.rs — `ImageCacheService`, `SourceImageSession`,
// `ConversionSession`, and the conversion half of `ImageCacheOperationHold`.
//
// The fail-closed GC engine (`cache_gc.h`) and the bare hold lifecycle
// (`cache_hold.h`) were split out earlier; this service owns the layout and
// the lazily-opened metadata store and drives both. Async Rust methods are
// blocking here; the source-image lock is a `std::mutex` per config path.
#ifndef AGENTENV_IMAGE_CACHE_SERVICE_H_
#define AGENTENV_IMAGE_CACHE_SERVICE_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/cfg/image.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/image/cache.h"
#include "agentenv/image/cache_gc.h"
#include "agentenv/image/cache_hold.h"
#include "agentenv/image/cache_metadata_store.h"
#include "agentenv/image/cache_source_config.h"
#include "agentenv/image/local_layer.h"
#include "agentenv/image/metadata.h"
#include "agentenv/image/oci_image.h"
#include "agentenv/p2p/transport.h"

namespace agentenv {
namespace cfg {
struct AppConfig;
}  // namespace cfg

namespace image {
namespace cache {

/// Rust `IMAGE_CACHE_{CONFIG,INDEX,METADATA,STAGING}_DIR`.
extern const char* const kImageCacheConfigDir;
extern const char* const kImageCacheIndexDir;
extern const char* const kImageCacheMetadataDir;
extern const char* const kImageCacheStagingDir;

/// Rust `HoldNamespace` — neutral to sandbox lifecycle; callers map their own
/// states onto these.
enum class HoldNamespace {
    /// Transient lease for an owner that is starting or running.
    Runtime,
    /// Durable lease for an owner that is paused.
    Paused,
};
const char* HoldNamespaceAsStr(HoldNamespace ns);
const char* HoldNamespaceReleaseReason(HoldNamespace ns);

/// Rust `store::CachedImageConfig`.
struct CachedImageConfig {
    enum class Kind { Missing, Found };
    Kind kind;
    /// Set when `Found`.
    std::string image_config_path;
    /// `Found` without metadata means the sidecar is missing and must be
    /// rebuilt; the config itself is usable.
    core::Optional<ImageResolutionMetadata> metadata;

    CachedImageConfig() : kind(Kind::Missing) {}
};

/// Rust `store::OverlaybdLayerLocation`.
struct OverlaybdLayerLocation {
    enum class Kind { LocalFile, CacheDir };
    Kind kind;
    std::string path;

    OverlaybdLayerLocation() : kind(Kind::CacheDir) {}
};

class ImageCacheService;

/// Rust `ImageCacheOperationHold` as an `ImageConversion`: the lease that
/// protects every commit a conversion reuses or persists, plus the
/// lookup/store logic over the content-addressed commit cache and P2P.
///
/// The lease itself is `cache::ImageCacheOperationHold`; this adds the
/// per-operation memory of P2P lookups already tried, so a failing peer is
/// asked once per operation, not once per layer visit.
class OperationHoldConversion : public ImageConversion {
 public:
    OperationHoldConversion(std::shared_ptr<ImageCacheService> service,
                            std::shared_ptr<ImageCacheOperationHold> hold)
        : service_(std::move(service)), hold_(std::move(hold)) {}

    const std::shared_ptr<ImageCacheOperationHold>& hold() const { return hold_; }

    /// Rust `release_best_effort`.
    void ReleaseBestEffort(const std::string& reason);

    std::string RemoteLayerDirHint(const std::string& layer_digest) override;
    core::Expected<StagingDir, std::string> CreateTempStagingDir() override;
    core::Expected<core::Optional<LocalLayer>, std::string> LookupConvertedLayer(
        const LayerConversionKey& key) override;
    core::Expected<LocalLayer, std::string> StoreConvertedLayer(
        const LayerConversionKey& key, const std::string& temp_commit) override;

 private:
    std::string ConversionIndexPath(const LayerConversionKey& key) const;
    core::Expected<std::string, std::string> PersistOverlaybdCommit(
        const std::string& temp_commit, const std::string& digest, uint64_t size);
    core::Expected<core::Unit, std::string> WriteConversionIndex(const LayerConversionKey& key,
                                                                 const LocalLayer& layer);
    core::Expected<core::Optional<LocalLayer>, std::string> LookupRemoteConvertedLayer(
        const LayerConversionKey& key);

    std::shared_ptr<ImageCacheService> service_;
    std::shared_ptr<ImageCacheOperationHold> hold_;
    std::set<p2p::P2pArtifactKey> attempted_p2p_lookups_;
};

/// Rust `ImageCacheService`.
class ImageCacheService : public std::enable_shared_from_this<ImageCacheService> {
 public:
    /// Rust `shared_from_app_config` — one service per stable cache root in
    /// the process, so every caller shares one metadata store and one set of
    /// source-image locks.
    static std::shared_ptr<ImageCacheService> SharedFromAppConfig(const cfg::AppConfig& config);
    /// Rust `shared_from_resolved_config`.
    static std::shared_ptr<ImageCacheService> SharedFromResolvedConfig(
        const cfg::ResolvedImageCacheConfig& config);
    /// Rust `from_resolved_config` — a fresh, unshared service.
    static std::shared_ptr<ImageCacheService> FromResolvedConfig(
        const cfg::ResolvedImageCacheConfig& config);

    /// Rust `initialize_p2p_transport` — once only; a second call is a
    /// programming error and aborts, as Rust's `assert!` panics.
    void InitializeP2pTransport(std::shared_ptr<p2p::P2pTransport> transport);
    std::shared_ptr<p2p::P2pTransport> P2pTransport() const;

    const std::string& commit_store() const { return commit_store_; }
    const std::string& index_dir() const { return index_dir_; }
    const std::string& config_dir() const { return config_dir_; }
    const std::string& metadata_store_path() const { return metadata_store_path_; }
    const std::string& staging_dir() const { return staging_dir_; }

    /// Rust `ensure_layout`.
    core::Expected<core::Unit, std::string> EnsureLayout() const;
    /// Rust `metadata_store` — opened on first use with `Sync` durability.
    core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> MetadataStore();

    // ---- metadata facts ----------------------------------------------------

    core::Expected<core::Unit, std::string> RecordSourceConfigRefsFromConfigPath(
        const std::string& image_config_path);
    void RecordSourceConfigRefsFromConfigPathBestEffort(const std::string& image_config_path,
                                                        const std::string& reason);
    core::Expected<core::Unit, std::string> RebuildMetadataFromConfigs();
    core::Expected<core::Unit, std::string> RecordHardCommitObject(
        const std::string& digest, const core::Optional<std::string>& file,
        const core::Optional<uint64_t>& size, const std::vector<p2p::P2pArtifactKey>& p2p_keys);
    core::Expected<core::Unit, std::string> AddHardCommitP2pKey(const std::string& digest,
                                                                const p2p::P2pArtifactKey& key);
    /// Rust `publish_converted_layer` — records P2P ownership first (closing
    /// the window where a catalog entry outlives the record GC needs to remove
    /// it), then publishes by reference. `false` when P2P is absent or has no
    /// local endpoint.
    core::Expected<bool, std::string> PublishConvertedLayer(const LayerConversionKey& key,
                                                            const LocalLayer& layer);
    /// Rust `import_hard_commit_trusted_descriptor` (test-only upstream).
    core::Expected<std::string, std::string> ImportHardCommitTrustedDescriptor(
        const std::string& source, const std::string& digest, uint64_t size);
    /// Rust `seed_and_record_hard_commit`.
    core::Expected<std::string, std::string> SeedAndRecordHardCommit(
        const std::string& source, const std::string& digest, uint64_t size,
        bool trusted_descriptor, const std::vector<p2p::P2pArtifactKey>& p2p_keys);

    // ---- holds -------------------------------------------------------------

    core::Expected<core::Unit, std::string> CreateOrReplaceHold(const ImageCacheHoldOwner& owner,
                                                                const std::set<HardCommitId>& refs);
    core::Expected<core::Unit, std::string> ReleaseHold(const ImageCacheHoldOwner& owner);
    void ReleaseHoldBestEffort(const ImageCacheHoldOwner& owner, const std::string& reason);
    core::Expected<std::vector<std::string>, std::string> ListHoldKeysInNamespace(
        const std::string& ns);
    /// Rust `cleanup_stale_runtime_holds` — drops `runtime/*` and
    /// `operation/*`; `paused/*` and config roots survive restarts.
    core::Expected<std::size_t, std::string> CleanupStaleRuntimeHolds();
    /// Rust `acquire_operation_hold_for_hard_commits`.
    core::Expected<std::shared_ptr<OperationHoldConversion>, std::string>
    AcquireOperationHoldForHardCommits(const std::string& operation,
                                       const std::set<std::string>& digests);
    /// Rust `begin_hard_commit_operation` — lazy; materialized on the first
    /// protected commit.
    core::Expected<std::shared_ptr<OperationHoldConversion>, std::string>
    BeginHardCommitOperation(const std::string& operation);
    /// Rust `acquire_hold_for_config_paths` — holds only the commit-store
    /// `file=` commits; fails closed on a commit-store lower without
    /// digest/size. No refs means no hold is written.
    core::Expected<core::Unit, std::string> AcquireHoldForConfigPaths(
        const ImageCacheHoldOwner& owner, const std::vector<std::string>& config_paths);

    // ---- liveness facade ---------------------------------------------------

    core::Expected<core::Unit, std::string> Protect(
        HoldNamespace ns, const std::string& owner_id,
        const std::vector<std::string>& image_config_paths);
    void ReleaseProtectionBestEffort(HoldNamespace ns, const std::string& owner_id);
    /// Rust `reconcile_namespace`.
    core::Expected<core::Unit, std::string> ReconcileNamespace(
        HoldNamespace ns, const std::vector<std::string>& live_owner_ids);
    /// Rust `run_maintenance` — optional capacity eviction, then fail-closed
    /// GC against the live refs of `running` (`(owner id, config paths)`).
    core::Expected<ImageCacheGcSummary, std::string> RunMaintenance(
        const std::vector<std::pair<std::string, std::vector<std::string> > >& running,
        const core::Optional<std::pair<uint64_t, uint64_t> >& watermark, uint64_t min_age_secs);

    // ---- GC ----------------------------------------------------------------

    core::Expected<ImageCacheLiveRuntimeRefs, std::string> LiveRefsFromRunning(
        const std::vector<std::pair<std::string, std::vector<std::string> > >& running);
    core::Expected<ImageCacheGcReport, std::string> RunGc(const ImageCacheLiveRuntimeRefs& live_refs,
                                                          bool rebuild_metadata);
    core::Expected<std::size_t, std::string> ReclaimStaleIndexes();
    core::Expected<core::Unit, std::string> EvictSourceConfigsOverCapacity(
        uint64_t high_watermark_bytes, uint64_t low_watermark_bytes, uint64_t min_age_secs);

    // ---- source images -----------------------------------------------------

    /// Rust `lookup_image_config` — under the per-image lock for its duration.
    core::Expected<CachedImageConfig, std::string> LookupImageConfig(
        const std::string& manifest_digest, const core::Optional<std::string>& repository_scope);
    /// Rust `begin_image_conversion` — the returned conversion carries the
    /// per-image lock and the operation hold until `PublishImageConfig`.
    core::Expected<std::shared_ptr<ImageConversion>, std::string> BeginImageConversion(
        const std::string& manifest_digest, const core::Optional<std::string>& repository_scope);
    /// Rust `publish_image_config` — the config root is recorded before the
    /// conversion is released, so its commits are never unprotected. The
    /// caller hands over its only reference to `conversion`.
    core::Expected<std::string, std::string> PublishImageConfig(
        const std::string& manifest_digest, const core::Optional<std::string>& repository_scope,
        const core::Json& overlaybd_config, const ImageResolutionMetadata& metadata,
        std::shared_ptr<ImageConversion> conversion);
    /// Rust `write_image_metadata`.
    core::Expected<std::string, std::string> WriteImageMetadata(
        const std::string& manifest_digest, const core::Optional<std::string>& repository_scope,
        const ImageResolutionMetadata& metadata);
    /// Rust `overlaybd_layer_location` — bind `file=` only for a cache-local
    /// layer with no registry fallback, so remote-recoverable commits stay
    /// collectable.
    OverlaybdLayerLocation LayerLocation(const std::string& digest, uint64_t size,
                                         bool has_remote) const;
    /// Rust `allowed_overlaybd_publish_roots`.
    std::vector<std::string> AllowedOverlaybdPublishRoots() const;

 private:
    explicit ImageCacheService(const cfg::ResolvedImageCacheConfig& config);

    struct SourceImageSession;
    class ConversionSession;

    core::Expected<std::pair<std::string, std::string>, std::string> SourceImagePaths(
        const std::string& manifest_digest, const core::Optional<std::string>& repository_scope)
        const;
    core::Expected<std::shared_ptr<SourceImageSession>, std::string> OpenSourceImage(
        const std::string& manifest_digest, const core::Optional<std::string>& repository_scope);
    core::Expected<std::set<HardCommitId>, std::string> CommitStoreHardCommitsFromConfigPaths(
        const std::vector<std::string>& config_paths, const std::string& context) const;
    void DeleteSourceImageConfigBestEffort(const CapacityEvictionCandidate& candidate,
                                           uint64_t evictable_before);
    core::Expected<ImageCacheGc, std::string> Gc();

    std::string commit_store_;
    std::string index_dir_;
    std::string config_dir_;
    std::string metadata_store_path_;
    std::string staging_dir_;

    std::mutex metadata_mu_;
    std::shared_ptr<ImageCacheMetadataStore> metadata_store_;
    std::shared_ptr<SourceImageCache> source_images_;

    mutable std::mutex transport_mu_;
    std::shared_ptr<p2p::P2pTransport> p2p_transport_;
};

}  // namespace cache
}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_CACHE_SERVICE_H_
