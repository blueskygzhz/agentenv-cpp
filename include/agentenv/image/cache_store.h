// SPDX-License-Identifier: MIT
// Rust: src/image/cache/store.rs — the ports the rest of AgentENV uses to
// reach the node-local image cache, and their `ImageCacheService` adapter.
//
// The data types (`CachedImageConfig`, `OverlaybdLayerLocation`) live in
// `cache_service.h`, beside the service that produces them.
#ifndef AGENTENV_IMAGE_CACHE_STORE_H_
#define AGENTENV_IMAGE_CACHE_STORE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/image/cache_service.h"
#include "agentenv/image/metadata.h"
#include "agentenv/image/oci_image.h"
#include "agentenv/sandbox/backend.h"

namespace agentenv {
namespace cfg {
struct AppConfig;
}  // namespace cfg

namespace image {
namespace cache {

/// Rust `RuntimeImageOwner`.
struct RuntimeImageOwner {
    enum class Kind { StartingSandbox, PausedSandbox };
    Kind kind;
    core::SandboxId id;

    static RuntimeImageOwner StartingSandbox(const core::SandboxId& id) {
        return RuntimeImageOwner(Kind::StartingSandbox, id);
    }
    static RuntimeImageOwner PausedSandbox(const core::SandboxId& id) {
        return RuntimeImageOwner(Kind::PausedSandbox, id);
    }

    bool operator==(const RuntimeImageOwner& o) const { return kind == o.kind && id == o.id; }
    bool operator!=(const RuntimeImageOwner& o) const { return !(*this == o); }

 private:
    RuntimeImageOwner(Kind k, const core::SandboxId& i) : kind(k), id(i) {}
};

/// Rust `SourceImageEntry` — one source image (manifest digest plus
/// repository scope) in the cache.
class SourceImageEntry {
 public:
    virtual ~SourceImageEntry() {}
    virtual core::Expected<CachedImageConfig, std::string> CachedConfig() = 0;
    /// The returned conversion holds the per-image lock and an operation hold
    /// until it is destroyed or handed to `PublishConfig`.
    virtual core::Expected<std::shared_ptr<ImageConversion>, std::string> BeginConversion() = 0;
    /// Rust takes `conversion` by value; pass the only reference.
    virtual core::Expected<std::string, std::string> PublishConfig(
        const core::Json& overlaybd_config, const ImageResolutionMetadata& metadata,
        std::shared_ptr<ImageConversion> conversion) = 0;
    virtual core::Expected<std::string, std::string> WriteMetadata(
        const ImageResolutionMetadata& metadata) = 0;
};

/// Rust `SourceImageStore`.
class SourceImageStore {
 public:
    virtual ~SourceImageStore() {}
    virtual core::Expected<std::unique_ptr<SourceImageEntry>, std::string> Open(
        const std::string& manifest_digest,
        const core::Optional<std::string>& repository_scope) = 0;
};

/// Rust `OverlaybdLayerStore`.
class OverlaybdLayerStore {
 public:
    virtual ~OverlaybdLayerStore() {}
    virtual OverlaybdLayerLocation LayerLocation(const std::string& digest, uint64_t size,
                                                 bool has_remote) const = 0;
    virtual std::vector<std::string> PublishableRoots() const = 0;
};

/// Rust `RuntimeImageRefs`.
class RuntimeImageRefs {
 public:
    virtual ~RuntimeImageRefs() {}
    virtual core::Expected<core::Unit, std::string> Pin(
        const RuntimeImageOwner& owner, const sandbox::RuntimeArtifactSet& artifacts) = 0;
    virtual void UnpinBestEffort(const RuntimeImageOwner& owner) = 0;
    virtual core::Expected<core::Unit, std::string> ReconcilePaused(
        const std::vector<core::SandboxId>& live_paused) = 0;
    virtual core::Expected<core::Unit, std::string> MaintainRunning(
        const std::vector<std::pair<core::SandboxId, sandbox::RuntimeArtifactSet> >&
            running) = 0;
};

/// Rust `LocalImageServices`.
struct LocalImageServices {
    std::shared_ptr<SourceImageStore> source_images;
    std::shared_ptr<OverlaybdLayerStore> overlaybd_layers;
    std::shared_ptr<RuntimeImageRefs> runtime_refs;
};

/// Rust `local_image_services_from_app_config`.
LocalImageServices LocalImageServicesFromAppConfig(const cfg::AppConfig& config);

/// Rust `test_local_image_services_from_service` (cfg(test) upstream).
LocalImageServices LocalImageServicesFromService(
    std::shared_ptr<ImageCacheService> cache,
    const core::Optional<std::pair<uint64_t, uint64_t> >& gc_watermark,
    uint64_t gc_min_age_secs);

}  // namespace cache
}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_CACHE_STORE_H_
