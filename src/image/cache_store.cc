// SPDX-License-Identifier: MIT
// Rust: src/image/cache/store.rs — `ImageCacheStore` and its entry.
#include "agentenv/image/cache_store.h"

#include "agentenv/cfg.h"
#include "agentenv/core/logging.h"

namespace agentenv {
namespace image {
namespace cache {

namespace {

/// Rust `ImageCacheSourceImageEntry`.
class ImageCacheSourceImageEntry : public SourceImageEntry {
 public:
    ImageCacheSourceImageEntry(std::shared_ptr<ImageCacheService> cache,
                               const std::string& manifest_digest,
                               const core::Optional<std::string>& repository_scope)
        : cache_(std::move(cache)),
          manifest_digest_(manifest_digest),
          repository_scope_(repository_scope) {}

    core::Expected<CachedImageConfig, std::string> CachedConfig() override {
        return cache_->LookupImageConfig(manifest_digest_, repository_scope_);
    }

    core::Expected<std::shared_ptr<ImageConversion>, std::string> BeginConversion() override {
        return cache_->BeginImageConversion(manifest_digest_, repository_scope_);
    }

    core::Expected<std::string, std::string> PublishConfig(
        const core::Json& overlaybd_config, const ImageResolutionMetadata& metadata,
        std::shared_ptr<ImageConversion> conversion) override {
        return cache_->PublishImageConfig(manifest_digest_, repository_scope_, overlaybd_config,
                                          metadata, std::move(conversion));
    }

    core::Expected<std::string, std::string> WriteMetadata(
        const ImageResolutionMetadata& metadata) override {
        return cache_->WriteImageMetadata(manifest_digest_, repository_scope_, metadata);
    }

 private:
    std::shared_ptr<ImageCacheService> cache_;
    std::string manifest_digest_;
    core::Optional<std::string> repository_scope_;
};

/// Rust `owner_namespace`.
HoldNamespace OwnerNamespace(const RuntimeImageOwner& owner) {
    return owner.kind == RuntimeImageOwner::Kind::StartingSandbox ? HoldNamespace::Runtime
                                                                  : HoldNamespace::Paused;
}

/// Rust `ImageCacheStore` — one object behind all three ports.
class ImageCacheStore : public SourceImageStore,
                        public OverlaybdLayerStore,
                        public RuntimeImageRefs {
 public:
    ImageCacheStore(std::shared_ptr<ImageCacheService> cache,
                    const core::Optional<std::pair<uint64_t, uint64_t> >& gc_watermark,
                    uint64_t gc_min_age_secs)
        : cache_(std::move(cache)), gc_watermark_(gc_watermark), gc_min_age_secs_(gc_min_age_secs) {}

    // SourceImageStore
    core::Expected<std::unique_ptr<SourceImageEntry>, std::string> Open(
        const std::string& manifest_digest,
        const core::Optional<std::string>& repository_scope) override {
        return std::unique_ptr<SourceImageEntry>(
            new ImageCacheSourceImageEntry(cache_, manifest_digest, repository_scope));
    }

    // OverlaybdLayerStore
    OverlaybdLayerLocation LayerLocation(const std::string& digest, uint64_t size,
                                         bool has_remote) const override {
        return cache_->LayerLocation(digest, size, has_remote);
    }

    std::vector<std::string> PublishableRoots() const override {
        return cache_->AllowedOverlaybdPublishRoots();
    }

    // RuntimeImageRefs
    core::Expected<core::Unit, std::string> Pin(
        const RuntimeImageOwner& owner, const sandbox::RuntimeArtifactSet& artifacts) override {
        return cache_->Protect(OwnerNamespace(owner), owner.id.ToString(),
                               artifacts.overlaybd_image_config_paths);
    }

    void UnpinBestEffort(const RuntimeImageOwner& owner) override {
        cache_->ReleaseProtectionBestEffort(OwnerNamespace(owner), owner.id.ToString());
    }

    core::Expected<core::Unit, std::string> ReconcilePaused(
        const std::vector<core::SandboxId>& live_paused) override {
        std::vector<std::string> ids;
        ids.reserve(live_paused.size());
        for (std::size_t i = 0; i < live_paused.size(); ++i) {
            ids.push_back(live_paused[i].ToString());
        }
        return cache_->ReconcileNamespace(HoldNamespace::Paused, ids);
    }

    core::Expected<core::Unit, std::string> MaintainRunning(
        const std::vector<std::pair<core::SandboxId, sandbox::RuntimeArtifactSet> >& running)
        override {
        std::vector<std::pair<std::string, std::vector<std::string> > > filtered;
        for (std::size_t i = 0; i < running.size(); ++i) {
            if (running[i].second.IsEmpty()) continue;
            filtered.push_back(std::make_pair(running[i].first.ToString(),
                                              running[i].second.overlaybd_image_config_paths));
        }
        const core::Expected<ImageCacheGcSummary, std::string> summary =
            cache_->RunMaintenance(filtered, gc_watermark_, gc_min_age_secs_);
        if (!summary.ok()) return core::make_unexpected(summary.error());
        AGENTENV_INFO("image cache maintenance pass complete: reclaimed="
                      << summary.value().collected
                      << " freed_bytes=" << summary.value().freed_bytes
                      << " retained=" << summary.value().retained);
        return core::Unit();
    }

 private:
    std::shared_ptr<ImageCacheService> cache_;
    core::Optional<std::pair<uint64_t, uint64_t> > gc_watermark_;
    uint64_t gc_min_age_secs_;
};

/// Rust `services_from_store`.
LocalImageServices ServicesFromStore(const std::shared_ptr<ImageCacheStore>& store) {
    LocalImageServices services;
    services.source_images = store;
    services.overlaybd_layers = store;
    services.runtime_refs = store;
    return services;
}

}  // namespace

LocalImageServices LocalImageServicesFromAppConfig(const cfg::AppConfig& config) {
    std::shared_ptr<ImageCacheService> cache = ImageCacheService::SharedFromAppConfig(config);
    const cfg::ResolvedImageCacheGcConfig gc = config.image.cache.GcSchedule();
    const cfg::ResolvedImageCacheConfig layout = config.ImageCacheLayout();
    core::Optional<std::pair<uint64_t, uint64_t> > watermark;
    uint64_t high = 0;
    uint64_t low = 0;
    if (gc.WatermarkBytes(layout.capacity_bytes, &high, &low)) {
        watermark = std::make_pair(high, low);
    }
    return ServicesFromStore(
        std::make_shared<ImageCacheStore>(cache, watermark, gc.min_age_secs));
}

LocalImageServices LocalImageServicesFromService(
    std::shared_ptr<ImageCacheService> cache,
    const core::Optional<std::pair<uint64_t, uint64_t> >& gc_watermark,
    uint64_t gc_min_age_secs) {
    return ServicesFromStore(
        std::make_shared<ImageCacheStore>(std::move(cache), gc_watermark, gc_min_age_secs));
}

}  // namespace cache
}  // namespace image
}  // namespace agentenv
