// SPDX-License-Identifier: MIT
// Rust: src/image/oci_image.rs — the pure-logic subset.
//
// What stays out: everything that forks `regctl`, streams blobs, or drives the
// overlaybd conversion pipeline. Those need tokio::process and the overlaybd
// tools and are ported elsewhere.
//
// What is in: the decisions that are made *about* bytes rather than by moving
// them -- reference rewriting, digest validation, platform selection, layer
// classification. Each of these is a place where getting it wrong produces a
// plausible-looking wrong answer rather than a failure, so they are the part
// worth having under test on both sides.
#ifndef AGENTENV_IMAGE_OCI_IMAGE_H_
#define AGENTENV_IMAGE_OCI_IMAGE_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/image/error.h"
#include "agentenv/image/metadata.h"

namespace agentenv {
namespace image {

/// Rust `REGCTL_RETRY_ATTEMPTS` / `REGCTL_RETRY_BASE_DELAY`.
extern const uint32_t kRegctlRetryAttempts;
extern const int64_t kRegctlRetryBaseDelayMs;

/// Rust `MAX_INDEX_RESOLUTION_DEPTH` — how many nested indexes to follow.
///
/// A bound, not a tuning knob: an index may name another index, and a registry
/// is free to serve a cycle. Without a ceiling the resolver recurses until the
/// stack runs out.
extern const std::size_t kMaxIndexResolutionDepth;

/// Rust `LAYER_VIRTUAL_SIZE_GIB`.
extern const uint64_t kLayerVirtualSizeGib;

/// Rust `OVERLAYBD_TURBO_ARTIFACT_TYPE` and the annotation keys around it.
extern const char* const kOverlaybdTurboArtifactType;
extern const char* const kOverlaybdTurboAnnotationPrefix;
extern const char* const kOverlaybdVersionAnnotation;
/// The key Rust spells inline in `classify_layer`; named here so the two uses
/// (classification and the tests that build such a layer) cannot drift.
extern const char* const kOverlaybdBlobDigestAnnotation;

/// Rust `OciLayerDescriptor`.
struct OciLayerDescriptor {
    std::string media_type;
    std::string digest;
    /// Rust `size: u64`. Unsigned because a descriptor size is a byte count
    /// the registry asserts; a negative value has no meaning and would only
    /// provide a way to smuggle one past a later length check.
    uint64_t size;
    /// Rust `BTreeMap<String, String>` — ordered, because the map is rendered
    /// into error messages and an unordered one makes those messages unstable.
    std::map<std::string, std::string> annotations;

    OciLayerDescriptor() : size(0) {}
};

/// Rust `OciManifestConfigDescriptor`.
///
/// Kept as its own one-field struct rather than reusing `OciLayerDescriptor`:
/// the config descriptor has no annotations and is never classified, and
/// sharing the layer type invites `classify_layer(manifest.config)`.
struct OciManifestConfigDescriptor {
    std::string digest;
};

/// Rust `OciManifest`.
struct OciManifest {
    /// Rust `artifact_type: Option<String>` — OCI 1.1 `artifactType`.
    ///
    /// Optional, not a string plus a flag: the turbo-OCI check compares
    /// against a specific value, and an absent field must not compare equal to
    /// an empty one.
    core::Optional<std::string> artifact_type;
    OciManifestConfigDescriptor config;
    std::vector<OciLayerDescriptor> layers;
};

/// Rust `OciPlatform`.
struct OciPlatform {
    core::Optional<std::string> architecture;
    core::Optional<std::string> os;
};

/// Rust `OciManifestDescriptor`.
struct OciManifestDescriptor {
    std::string media_type;
    std::string digest;
    /// Absent for a single-platform index entry, which is why selection has to
    /// treat "no platform" as a distinct case rather than an empty match.
    core::Optional<OciPlatform> platform;
};

/// Rust `OciIndex`.
struct OciIndex {
    std::vector<OciManifestDescriptor> manifests;
};

/// Rust `LayerClass`.
enum class LayerClass {
    /// Standard OCI tar layer (tar / tar+gzip / tar+zstd).
    StandardTar,
    /// Overlaybd-native: the blob is already a sealed overlaybd lower.
    OverlaybdNative,
    /// Turbo-OCI: the blob is only an index over the original OCI layer.
    OverlaybdTurbo,
    /// Tar-wrapped overlaybd: a tar blob with an overlaybd blob nested inside.
    OverlaybdTarWrapped,
    /// Unrecognized mediaType — fail closed rather than guess.
    Unknown,
};

/// Rust `ImageFormat`.
enum class ImageFormat {
    StandardOci,
    OverlaybdNative,
};

/// Rust `impl Display for ImageFormat` — `standard_oci` / `overlaybd_native`.
///
/// These exact strings are emitted as metric labels and log fields, so they
/// are part of the observable surface and cannot be prettified.
const char* ImageFormatToString(ImageFormat format);

/// Rust `is_overlaybd_native_media_type`.
bool IsOverlaybdNativeMediaType(const std::string& media_type);

/// Rust `is_standard_tar_media_type`.
bool IsStandardTarMediaType(const std::string& media_type);

/// Rust `classify_layer`.
LayerClass ClassifyLayer(const OciLayerDescriptor& layer);

/// Rust `classify_manifest`.
///
/// Returns `ImageError`, not a flat string, because the two failure classes
/// here are answered differently by the API layer: a manifest with no layers
/// is a server-side oddity (`kOther`, 5xx), while turbo-OCI, tar-wrapped, or
/// unknown layers are a property of the image the caller asked for
/// (`kUnsupportedImage`, 4xx). Reporting the second as 500 tells a user to
/// retry something that will never succeed.
core::Expected<ImageFormat, ImageError> ClassifyManifest(const OciManifest& manifest);

/// Rust `select_manifest_for_platform` — index of the chosen entry.
///
/// An index rather than a pointer so the result stays valid if the caller
/// copies the vector, and so the "which entry" answer can be compared in
/// tests without pointer arithmetic.
///
/// A single-entry index is accepted whatever its platform says: that is what a
/// single-platform image looks like, and a registry is allowed to omit the
/// platform block entirely there.
core::Expected<std::size_t, std::string> SelectManifestForPlatform(
    const std::vector<OciManifestDescriptor>& manifests, const std::string& arch,
    const std::string& os);

/// Rust `parse_oci_image_config`.
///
/// `raw_config` is the top-level `config` object only -- not the whole
/// document. Keeping `created`/`architecture`/`rootfs` out of it matters
/// because the value is forwarded to consumers as "the container config"; a
/// consumer that merges it into a runtime spec would otherwise see history and
/// layer digests in there.
core::Expected<ImageResolutionMetadata, std::string> ParseOciImageConfig(const std::string& raw);

/// Rust `verify_blob_digest` — sha256 only, fail closed on anything else.
///
/// Rejecting other algorithms rather than skipping the check: a digest this
/// code cannot compute must not be treated as verified.
core::Expected<core::Unit, std::string> VerifyBlobDigest(const std::string& expected_digest,
                                                         const void* data, std::size_t len);

/// Rust `parse_image_ref`'s tuple return.
struct ParsedImageRef {
    /// Translated for wire use: `docker.io` and `index.docker.io` both become
    /// `registry-1.docker.io`, which is the host that actually serves the v2
    /// API. The untranslated names have no `/v2/` endpoint.
    std::string host;
    std::string repository;
};

/// Rust `parse_image_ref` — split a fully-qualified reference.
///
/// Shortnames are rejected here rather than expanded. Expansion is
/// `ImageRefCandidates`' job and depends on operator config; doing it again at
/// this level would silently reintroduce an implicit Docker Hub default below
/// the layer that was configured to forbid it.
core::Expected<ParsedImageRef, std::string> ParseImageRef(const std::string& image_ref);

/// Rust `repository_cache_key`.
std::string RepositoryCacheKey(const std::string& host, const std::string& repository);

/// Rust `image_ref_with_digest` — repoint a reference at a manifest digest,
/// replacing any existing tag or digest.
core::Expected<std::string, std::string> ImageRefWithDigest(const std::string& image_ref,
                                                            const std::string& digest);

/// Rust `image_ref_repository` — strip the tag/digest, leaving
/// `host/repository`.
///
/// Unlike `ParseImageRef` this keeps the host exactly as written, because the
/// result is handed to `regctl`, which does its own docker.io aliasing;
/// pre-translating would hand it a host it then aliases a second time.
core::Expected<std::string, std::string> ImageRefRepository(const std::string& image_ref);

/// Rust `host_arch_to_oci` — map a host architecture name onto the OCI one.
///
/// Unsupported architectures are an error rather than a pass-through: the
/// value selects a manifest, and passing `riscv64` through would match nothing
/// and surface as "no manifest for your platform" instead of naming the
/// actual, fixable problem.
core::Expected<std::string, std::string> HostArchToOci(const std::string& host_arch);

/// Rust `is_valid_digest_algorithm` — the OCI algorithm grammar.
bool IsValidDigestAlgorithm(const std::string& algo);

/// Rust `blob_path_for_digest`.
///
/// Both digest components arrive verbatim from a registry-served manifest and
/// are otherwise unvalidated. Without the grammar check, `Join` turns a
/// hostile digest into an arbitrary host path: an absolute algorithm such as
/// `/etc:passwd` replaces the base entirely, and
/// `sha256:../../../../root/.ssh/id_rsa` traverses out of the layout
/// (CWE-22, GHSA-hw7r-gmc9-84jv F3).
core::Expected<std::string, std::string> BlobPathForDigest(const std::string& layout_dir,
                                                           const std::string& digest);

/// Rust `uuid_from_layer_digest` — a stable UUID derived from a layer digest.
///
/// Derived, never random: the value becomes the LSMT layer identity, so
/// re-deriving it for the same layer on a later run has to produce the same
/// UUID or the layer is treated as a different one. The version nibble is set
/// to 8 ("custom") and the variant to RFC 4122, matching
/// `Builder::from_custom_bytes`, so a reader can tell the value was computed
/// rather than generated.
core::Uuid UuidFromLayerDigest(const std::string& digest);

/// Rust `regctl_stderr_is_not_found`.
///
/// Matches only the `[http 404]` marker. Deliberately narrow: a DNS failure or
/// a 401 must keep its 5xx classification, since reporting either as "image
/// not found" sends the operator looking for the wrong problem.
bool RegctlStderrIsNotFound(const std::string& stderr_text);

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_OCI_IMAGE_H_
