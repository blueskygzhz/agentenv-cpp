// SPDX-License-Identifier: MIT
// Rust: src/image/resolver.rs — the `ImageResolver` struct (here
// `DefaultImageResolver`; `layers.h` keeps the abstract `ImageResolver` seam
// its consumers call) and the free functions and constants around it:
// OverlayBD referrer discovery, the registry-auth/format rule, and the
// overlaybd `image.json` shape for a resolved image.
#ifndef AGENTENV_IMAGE_RESOLVER_H_
#define AGENTENV_IMAGE_RESOLVER_H_

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/image/buildkit.h"
#include "agentenv/image/cache_store.h"
#include "agentenv/image/error.h"
#include "agentenv/image/layers.h"
#include "agentenv/image/metadata.h"
#include "agentenv/image/oci_image.h"
#include "agentenv/snapshot/layers.h"
#include "agentenv/snapshot/repository/interfaces.h"

namespace agentenv {
namespace cfg {
struct AppConfig;
}  // namespace cfg

namespace image {

/// Rust `ImageResolver` (the struct). Resolves a reference to a local
/// overlaybd `image.json`:
///
///  * `aenv/<name>[:<tag>]` — an AgentENV image tag, read from the repository;
///  * `sha256:<hex>` — an AgentENV published image, read from the repository;
///  * anything else — an OCI reference, tried against each candidate
///    registry, then converted (or streamed) through the node image cache.
///
/// Calls block; Rust's are `async`.
class DefaultImageResolver : public ImageResolver {
 public:
    /// Rust `publish_image`'s `store_layers` — copies the built image's layers
    /// into the repository and returns their references.
    typedef std::function<snapshot::repository::RepositoryResult<
        std::vector<snapshot::OverlaybdLayerRef> >(snapshot::repository::SnapshotRepository&,
                                                   const std::string& image_config_path)>
        StoreLayersFn;

    /// Rust `ImageResolver::new`.
    explicit DefaultImageResolver(const cfg::AppConfig& config);

    /// Rust `with_repository`.
    DefaultImageResolver& WithRepository(
        std::shared_ptr<snapshot::repository::SnapshotRepository> repository);

    /// Rust `default_image`.
    const std::string& DefaultImage() const override { return default_image_; }

    /// Rust `resolve`.
    core::Expected<ResolvedBlockImage, ImageError> Resolve(const std::string& image_ref) override;

    /// Rust `resolve_with_registry_auth` — `auth` is optional.
    core::Expected<ResolvedBlockImage, ImageError> ResolveWithRegistryAuth(
        const std::string& image_ref, const RegistryAuth* auth);

    /// Rust `resolve_buildkit`.
    ///
    /// **Nothing calls this, and in production nothing can.** Stated here
    /// because the rest of this class is live, so a reader has every reason to
    /// assume this is too.
    ///
    /// Upstream's only caller is `ImageBuildService::import_build_image`
    /// (`src/api/impls/image_build.rs`), reached from the build worker
    /// (`src/api/image_build/worker.rs`). Neither is ported -- `api/
    /// image_build.cc` carries the session state and HEALTHCHECK rendering,
    /// not the worker -- so there is no host to call this from. Wiring it to
    /// something else would invent a call chain upstream does not have.
    ///
    /// It would also fail immediately: the caller must first obtain a
    /// `BuildkitContent`, and `BuildkitContent::Connect` goes through
    /// `MakeBuildkitContentWire`, which has no gRPC transport in this tree and
    /// always returns an error. See the header comment of
    /// `src/image/buildkit_wire.cc`.
    ///
    /// Kept rather than deleted because it is a *complete* port of the
    /// upstream function and the wire types it consumes are tested directly
    /// (`BuildkitContent` takes an injected wire); what is missing is above
    /// it, not in it.
    core::Expected<ResolvedBlockImage, std::string> ResolveBuildkit(
        const BuildkitContent& content, const std::string& digest);

    /// Rust `resolve_tools` — `None` when the image is neither overlaybd-native
    /// nor labelled as a tools drive. Uses a copy of this resolver, as Rust
    /// consumes `self`.
    core::Expected<core::Optional<ResolvedBlockImage>, ImageError> ResolveTools(
        const std::string& image_ref) const;

    /// Rust `publish_image` — returns the published image's digest.
    core::Expected<std::string, std::string> PublishImage(const ResolvedBlockImage& image,
                                                          const StoreLayersFn& store_layers);

    /// Rust tests assign `resolver.allowed_registries` directly.
    void SetAllowedRegistriesForTesting(
        const core::Optional<std::vector<std::string> >& allowed_registries) {
        allowed_registries_ = allowed_registries;
    }

 private:
    core::Expected<ResolvedBlockImage, ImageError> ResolvePublished(const std::string& digest);
    core::Expected<ResolvedBlockImage, ImageError> ResolveTag(const std::string& reference);
    core::Expected<ResolvedBlockImage, ImageError> ResolveFetchedManifest(
        const std::string& image_ref, const std::string& arch, FetchedManifest fetched,
        const core::Optional<std::string>& registry_config);
    OverlaybdConversionEnv ConversionEnv(const core::Optional<std::string>& registry_config) const;

    std::shared_ptr<snapshot::repository::SnapshotRepository> repository_;
    std::shared_ptr<cache::SourceImageStore> store_;
    std::string overlaybd_install_root_;
    std::string overlaybd_convert_global_config_;
    std::string overlaybd_oci_converter_id_;
    std::string regctl_binary_;
    std::string default_image_;
    std::vector<std::string> search_registries_;
    core::Optional<std::vector<std::string> > allowed_registries_;
    std::vector<std::string> try_referrers_overlaybd_prefixes_;
    bool convert_standard_oci_;
};

/// Rust `IMAGE_RESOLVE_STAGE_DURATION`.
extern const char* const kImageResolveStageDuration;

/// Rust `OVERLAYBD_NATIVE_ARTIFACT_TYPE` — the referrer artifactType
/// accelerated-container-image publishes. A registry wire value.
extern const char* const kOverlaybdNativeArtifactType;
/// Rust `ACR_ARTIFACT_STREAMING_ARTIFACT_TYPE` — what
/// `az acr artifact-streaming create` publishes. A registry wire value.
extern const char* const kAcrArtifactStreamingArtifactType;
/// Rust `OVERLAYBD_REFERRER_ARTIFACT_TYPES` — in preference order: an image
/// carrying both resolves to the accelerated-container-image one.
const std::vector<std::string>& OverlaybdReferrerArtifactTypes();

/// A discovered referrer: its digest and which of
/// `OverlaybdReferrerArtifactTypes()` it matched.
typedef std::pair<std::string, std::string> OverlaybdReferrer;

/// Rust `validate_registry_auth_for_format` — per-request registry
/// credentials only work for standard OCI images (including when a referrer
/// switched the format to OverlayBD). `UnsupportedImage`, so a 4xx.
core::Expected<core::Unit, ImageError> ValidateRegistryAuthForFormat(ImageFormat format,
                                                                     bool has_registry_auth);

/// Rust `resolved_from_cached_config`.
ResolvedBlockImage ResolvedFromCachedConfig(const std::string& source_image_ref,
                                            const std::string& image_config_path,
                                            const ImageResolutionMetadata& metadata);

/// Rust `should_try_overlaybd_referrers` — a plain prefix match.
bool ShouldTryOverlaybdReferrers(const std::string& image_ref,
                                 const std::vector<std::string>& prefixes);

/// Rust `parse_overlaybd_referrer` — the first descriptor of the most
/// preferred artifactType present; `None` when no descriptor matches any.
/// Errors carry Rust's `{:#}` chain.
core::Expected<core::Optional<OverlaybdReferrer>, std::string> ParseOverlaybdReferrer(
    const std::string& body);

/// Rust `discover_overlaybd_referrer` — runs
/// `regctl artifact list --format body <subject>` unfiltered (regctl accepts
/// one `--filter-artifact-type`, which cannot express "any of these") and
/// matches locally. A regctl failure is an error, never `None`: swallowing it
/// would silently fall back to pull-and-convert.
core::Expected<core::Optional<OverlaybdReferrer>, std::string> DiscoverOverlaybdReferrer(
    const std::string& regctl_binary, const std::string& subject_ref,
    const core::Optional<std::string>& registry_config);

/// Rust `try_resolve_overlaybd_referrer` — discovery, then the referrer's
/// manifest, which must itself be overlaybd-native.
core::Expected<core::Optional<std::pair<FetchedManifest, std::string> >, std::string>
TryResolveOverlaybdReferrer(const std::string& regctl_binary, const std::string& subject_ref,
                            const std::string& arch,
                            const core::Optional<std::string>& registry_config);

/// Rust `detect_arch` — `x86_64` or `aarch64` (Rust's `consts::ARCH` names).
core::Expected<std::string, std::string> DetectArch();

/// Rust `registry_label` — the registry host for metrics, `unknown` when the
/// reference has none.
std::string RegistryLabel(const std::string& image_ref);

/// Rust `overlaybd_image_config_json`. Local layers are read from `file`;
/// remote ones are fetched from `repoBlobUrl` into their `dir`.
core::Json OverlaybdImageConfigJson(const ResolvedImage& resolved);

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_RESOLVER_H_
