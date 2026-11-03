// SPDX-License-Identifier: MIT
// Rust: src/image/oci_image.rs
//
// Split across three translation units by what the code touches:
//   oci_image.cc     the decisions made *about* bytes -- reference rewriting,
//                    digest validation, platform selection, classification.
//   oci_registry.cc  credentials and everything that forks `regctl`.
//   oci_convert.cc   the layer conversion pipeline.
//
// Rust's `async fn`s are blocking calls here; run them on a worker thread.
#ifndef AGENTENV_IMAGE_OCI_IMAGE_H_
#define AGENTENV_IMAGE_OCI_IMAGE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/process.h"
#include "agentenv/image/buildkit.h"
#include "agentenv/image/error.h"
#include "agentenv/image/local_layer.h"
#include "agentenv/image/metadata.h"
#include "agentenv/p2p/types.h"

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
/// Returns `ImageError`, not a flat string, because every failure here is a
/// property of the image the caller asked for (`kUnsupportedImage`, 4xx): a
/// manifest with no layers, turbo-OCI, tar-wrapped, unknown or mixed layers.
/// Reporting one as 500 tells a user to retry something that will never
/// succeed.
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

// ---- manifest documents ----

/// Rust `serde_json::from_value::<OciManifest>`. `config` and `layers` are
/// required, and each layer needs `mediaType`, `digest` and `size`; a missing
/// `annotations` is empty but an explicit null is a type error.
core::Expected<OciManifest, std::string> ParseOciManifest(const core::Json& value);

/// Rust `serde_json::from_value::<OciIndex>`.
core::Expected<OciIndex, std::string> ParseOciIndex(const core::Json& value);

// ---- registry credentials (oci_registry.cc) ----

/// Rust `REGCTL_GOMAXPROCS`.
extern const char* const kRegctlGomaxprocs;

/// Rust `RegistryConfig` — a private (0600) `regctl` config file, removed when
/// the last copy of the handle goes away. Each one names exactly one host, so
/// concurrent operations with different credentials never share a file.
class RegistryConfig {
 public:
    static core::Expected<RegistryConfig, std::string> Create(const std::string& host,
                                                              const std::string& username,
                                                              const std::string& password);
    const std::string& Path() const;

 private:
    struct Holder;
    std::shared_ptr<Holder> holder_;
};

/// Rust `RegistryAuth`. Holds secrets, so deliberately has no printable form.
struct RegistryAuth {
    enum Kind { kGeneral, kGcp, kAws };
    Kind kind;
    std::string username;
    std::string password;
    std::string service_account_json;
    std::string access_key_id;
    std::string secret_access_key;
    std::string region;

    RegistryAuth() : kind(kGeneral) {}
    static RegistryAuth General(const std::string& username, const std::string& password);
    static RegistryAuth Gcp(const std::string& service_account_json);
    static RegistryAuth Aws(const std::string& access_key_id,
                            const std::string& secret_access_key, const std::string& region);

    /// Rust `RegistryAuth::validate`.
    core::Expected<core::Unit, std::string> Validate() const;

    /// Rust `RegistryAuth::config`. For `kAws` this performs the ECR token
    /// exchange, a network call.
    core::Expected<RegistryConfig, std::string> Config(const std::string& host) const;
};

/// The HTTP half of Rust `ecr_credentials`, split out so the request can be
/// asserted without AWS.
struct EcrAuthorizationRequest {
    std::string url;
    /// Every header to send, `Authorization` included.
    std::map<std::string, std::string> headers;
    std::string body;
};
EcrAuthorizationRequest BuildEcrAuthorizationRequest(const std::string& access_key_id,
                                                     const std::string& secret_access_key,
                                                     const std::string& region,
                                                     const std::string& amz_date);

/// Rust's decoding half of `ecr_credentials` — `(username, password)` from the
/// `GetAuthorizationToken` response body.
core::Expected<std::pair<std::string, std::string>, std::string> ParseEcrAuthorizationResponse(
    const std::string& body);

// ---- regctl (oci_registry.cc) ----

/// Rust `regctl_command` — the argv and environment of one `regctl` run. All
/// invocations go through this so the GOMAXPROCS cap applies uniformly: regctl
/// is a Go binary forked per operation, and the default of one OS thread per
/// host CPU can exhaust the PID limit on a many-core host.
struct RegctlCommand {
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string> > env;
};
RegctlCommand MakeRegctlCommand(const std::string& binary,
                                const core::Optional<std::string>& registry_config);

/// Rust `ensure_regctl_binary` — a non-empty regular file.
core::Expected<core::Unit, ImageError> EnsureRegctlBinary(const std::string& path);

/// Rust `run_regctl` — retries failures with exponential backoff to absorb
/// transient registry errors. A 404 is not transient and fails immediately as
/// `kNotFound`, so the API layer can answer 4xx.
core::Expected<core::process::Output, ImageError> RunRegctl(
    const std::string& regctl_binary, const std::vector<std::string>& args,
    const core::Optional<std::string>& registry_config);

// ---- manifest fetch (oci_registry.cc) ----

/// Rust `FetchedManifest`.
struct FetchedManifest {
    std::string manifest_digest;
    std::string selected_image_ref;
    core::Optional<std::string> repository_scope;
    ImageFormat format;
    OciManifest manifest;

    FetchedManifest() : format(ImageFormat::StandardOci) {}
    /// Rust `config_digest`.
    const std::string& ConfigDigest() const { return manifest.config.digest; }
};

/// Rust `fetch_oci_manifest` — resolves the manifest for this host's
/// architecture on linux without downloading any layer blob. Index documents
/// are followed through digest-qualified references, at most
/// `kMaxIndexResolutionDepth` deep.
core::Expected<FetchedManifest, ImageError> FetchOciManifest(
    const std::string& regctl_binary, const std::string& image_ref, const std::string& host_arch,
    const core::Optional<std::string>& registry_config);

/// Rust `fetch_oci_image_config_metadata` — the config blob is verified against
/// `config_digest` before it is parsed.
core::Expected<ImageResolutionMetadata, ImageError> FetchOciImageConfigMetadata(
    const std::string& regctl_binary, const std::string& image_ref,
    const std::string& config_digest, const core::Optional<std::string>& registry_config);

// ---- conversion (oci_convert.cc) ----

/// Rust `RemoteLayer`.
struct RemoteLayer {
    std::string digest;
    uint64_t size;
    std::string dir;

    RemoteLayer() : size(0) {}
    bool operator==(const RemoteLayer& o) const {
        return digest == o.digest && size == o.size && dir == o.dir;
    }
};

/// Rust `ResolvedImage`. `Local` carries cached `.commit` files from the
/// standard-OCI path; `Remote` carries registry descriptors, whose bytes the
/// overlaybd runtime's `registryfs_v2` backend fetches on demand.
struct ResolvedImage {
    enum Kind { kLocal, kRemote };
    Kind kind;
    std::vector<LocalLayer> local_layers;
    std::string repo_blob_url;
    std::vector<RemoteLayer> remote_layers;

    ResolvedImage() : kind(kLocal) {}
};

/// Rust `OverlaybdConversionEnv`.
struct OverlaybdConversionEnv {
    std::string install_root;
    std::string global_config;
    std::string converter_id;
    std::string regctl_binary;
    core::Optional<std::string> registry_config;
};

/// Rust `LayerConversionKey`.
struct LayerConversionKey {
    std::string source_layer_digest;
    std::string converter_id;
    uint64_t virtual_size_gib;
    bool mkfs;
    core::Optional<std::string> parent_commit_digest;
    core::Uuid expected_layer_uuid;

    LayerConversionKey() : virtual_size_gib(0), mkfs(false) {}

    /// The `serde_json::to_vec` form: camelCase, declaration order, and
    /// `parentCommitDigest` present as null when unset. `P2pKey` hashes these
    /// exact bytes, so it is part of the wire contract between peers.
    std::string ToJson() const;

    /// Rust `p2p_key` — stable across peers and excluding the output digest,
    /// so a peer can discover a converted layer before knowing its filename.
    p2p::P2pArtifactKey P2pKey() const;
};

/// Rust `ConvertedLayerP2pMetadata`.
struct ConvertedLayerP2pMetadata {
    std::string protocol;
    std::string commit_digest;
    uint64_t size;

    ConvertedLayerP2pMetadata() : size(0) {}
    static ConvertedLayerP2pMetadata FromLocalLayer(const LocalLayer& layer);
    /// Rejects any other protocol, so a future format is not read as this one.
    static core::Expected<ConvertedLayerP2pMetadata, std::string> Parse(const core::Json& raw);
    core::Json ToJson() const;
};

/// Rust `tempfile::TempDir` — a directory removed with its contents when the
/// last copy of the handle goes away.
class StagingDir {
 public:
    explicit StagingDir(const std::string& path);
    const std::string& Path() const;

 private:
    struct Holder;
    std::shared_ptr<Holder> holder_;
};

/// Rust `ImageConversion` — the cache the pipeline converts into.
class ImageConversion {
 public:
    virtual ~ImageConversion() {}
    virtual std::string RemoteLayerDirHint(const std::string& layer_digest) = 0;
    /// A fresh directory on the commit cache's filesystem, so `layer.commit`
    /// can be hard-linked into the cache (avoids EXDEV).
    virtual core::Expected<StagingDir, std::string> CreateTempStagingDir() = 0;
    virtual core::Expected<core::Optional<LocalLayer>, std::string> LookupConvertedLayer(
        const LayerConversionKey& key) = 0;
    virtual core::Expected<LocalLayer, std::string> StoreConvertedLayer(
        const LayerConversionKey& key, const std::string& temp_commit) = 0;
};

/// Rust `LayerBlobSource` — where the pipeline gets a layer's bytes.
class LayerBlobSource {
 public:
    virtual ~LayerBlobSource() {}
    virtual core::Expected<std::string, ImageError> WaitLayerBlob(
        std::size_t idx, const OciLayerDescriptor& layer) = 0;
};

/// Rust `LayerConversionRequest`.
struct LayerConversionRequest {
    std::size_t idx;
    OciLayerDescriptor layer;
    std::string blob_path;
    std::string layer_work;
    bool mkfs;
    /// The complete lower stack below this layer, as an owned snapshot.
    std::vector<std::string> lower_paths;
    core::Uuid layer_uuid;
    core::Optional<core::Uuid> parent_uuid;

    LayerConversionRequest() : idx(0), mkfs(false) {}
};

/// Rust's `OverlaybdLayerConverter`, behind a seam so the ordering rules of the
/// pipeline can be tested without the overlaybd binaries.
class LayerConverter {
 public:
    virtual ~LayerConverter() {}
    /// Returns the path of the sealed `layer.commit`.
    virtual core::Expected<std::string, std::string> ConvertLayer(
        const LayerConversionRequest& request) = 0;
};

/// Rust `layer_conversion_key`.
LayerConversionKey MakeLayerConversionKey(const OciLayerDescriptor& layer,
                                          const std::string& converter_id, bool mkfs,
                                          const core::Optional<std::string>& parent_commit_digest,
                                          const core::Uuid& expected_layer_uuid);

/// Rust `cached_standard_oci_lowers` — the lowers when every layer is cached,
/// or `nullopt` as soon as one is not. A hit depends on the parent's commit
/// digest, so this walks in image order.
core::Expected<core::Optional<std::vector<LocalLayer> >, std::string> CachedStandardOciLowers(
    const OciManifest& manifest, const OverlaybdConversionEnv& conversion,
    ImageConversion* sink);

/// Rust `convert_standard_oci_layers_pipeline_inner` — converts in strict image
/// order, reusing cached layers and waiting only for blobs it actually needs.
core::Expected<std::vector<LocalLayer>, ImageError> ConvertStandardOciLayersPipelineInner(
    const OciManifest& manifest, const std::string& image_ref, const std::string& work_root,
    const OverlaybdConversionEnv& conversion, ImageConversion* sink, LayerConverter* converter,
    LayerBlobSource* producer);

/// Rust `RegctlImageCopyProducer` — a background `regctl image copy` into an
/// OCI layout, restarted on failure with backoff; `WaitLayerBlob` polls the
/// layout for the next needed blob. The child is killed if the producer is
/// dropped.
class RegctlImageCopyProducer : public LayerBlobSource {
 public:
    static core::Expected<std::shared_ptr<RegctlImageCopyProducer>, ImageError> Start(
        const std::string& regctl_binary, const std::string& image_ref,
        const std::string& layout_dir, const core::Optional<std::string>& registry_config);

    core::Expected<std::string, ImageError> WaitLayerBlob(
        std::size_t idx, const OciLayerDescriptor& layer) override;

    /// Rust `abort` — kill and reap the child and drain its pipes.
    core::Expected<core::Unit, std::string> Abort();

    uint32_t AttemptsStarted() const { return attempts_started_; }

 private:
    RegctlImageCopyProducer() : attempts_started_(0), backoff_ms_(0) {}
    core::Expected<core::Unit, ImageError> SpawnChild();

    std::string regctl_binary_;
    core::Optional<std::string> registry_config_;
    std::string image_ref_;
    std::string layout_dir_;
    std::shared_ptr<core::process::Child> child_;
    uint32_t attempts_started_;
    int64_t backoff_ms_;
};

/// Rust `convert_fetched_oci_image_to_overlaybd`.
core::Expected<ResolvedImage, ImageError> ConvertFetchedOciImageToOverlaybd(
    const std::string& image_ref, const FetchedManifest& fetched,
    const OverlaybdConversionEnv& conversion, ImageConversion* sink,
    const std::string& host_arch);

/// Rust `fetch_content_manifest` — the BuildKit counterpart of
/// `FetchOciManifest`. Builder output is trusted less than its digest suggests:
/// it must be standard OCI, at most 1024 layers and 64 GiB, and target
/// linux/`arch`.
core::Expected<std::pair<FetchedManifest, ImageResolutionMetadata>, std::string>
FetchContentManifest(const BuildkitContent& content, const std::string& digest,
                     const std::string& arch);

/// Rust `convert_content_image`.
core::Expected<ResolvedImage, ImageError> ConvertContentImage(
    const BuildkitContent& content, const FetchedManifest& fetched,
    const OverlaybdConversionEnv& conversion, ImageConversion* sink);

/// `FetchContentManifest` and `ConvertContentImage` with the converter
/// supplied, for tests.
core::Expected<ResolvedImage, ImageError> ConvertContentImageWith(
    const BuildkitContent& content, const FetchedManifest& fetched,
    const OverlaybdConversionEnv& conversion, ImageConversion* sink, LayerConverter* converter);

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_OCI_IMAGE_H_
