// SPDX-License-Identifier: MIT
// Rust: src/image/resolver.rs — the free functions and constants around
// `ImageResolver`: OverlayBD referrer discovery, the registry-auth/format
// rule, and the overlaybd `image.json` shape for a resolved image.
//
// `ImageResolver::resolve` itself drives `cache::ImageCacheService` and is not
// here; `layers.h` keeps the seam its consumers call.
#ifndef AGENTENV_IMAGE_RESOLVER_H_
#define AGENTENV_IMAGE_RESOLVER_H_

#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/image/error.h"
#include "agentenv/image/layers.h"
#include "agentenv/image/metadata.h"
#include "agentenv/image/oci_image.h"

namespace agentenv {
namespace image {

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
