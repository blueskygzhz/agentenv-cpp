// SPDX-License-Identifier: MIT
// Rust: src/image/resolver.rs — `ImageResolver` and its free functions.
#include "agentenv/image/resolver.h"

#include <sys/utsname.h>

#include <map>

#include "agentenv/cfg.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/process.h"
#include "agentenv/image/catalog.h"
#include "agentenv/image/published_image.h"
#include "agentenv/image/reference.h"
#include "agentenv/observability/metrics.h"
#include "agentenv/observability/prometheus.h"
#include "agentenv/setup/overlaybd.h"
#include "agentenv/snapshot/image_tag.h"

namespace agentenv {
namespace image {

const char* const kImageResolveStageDuration = "agentenv_image_resolve_stage_duration_seconds";
const char* const kOverlaybdNativeArtifactType =
    "application/vnd.containerd.overlaybd.native.v1+json";
const char* const kAcrArtifactStreamingArtifactType = "application/vnd.azure.artifact.streaming.v1";

const std::vector<std::string>& OverlaybdReferrerArtifactTypes() {
    static const std::vector<std::string> types = {kOverlaybdNativeArtifactType,
                                                   kAcrArtifactStreamingArtifactType};
    return types;
}

core::Expected<core::Unit, ImageError> ValidateRegistryAuthForFormat(ImageFormat format,
                                                                     bool has_registry_auth) {
    if (format == ImageFormat::OverlaybdNative && has_registry_auth) {
        return core::make_unexpected(UnsupportedImage(
            "fromImageRegistry is not supported for OverlayBD images (including OverlayBD "
            "referrers); use a standard OCI image or omit fromImageRegistry and configure "
            "server-side registry credentials"));
    }
    return core::Unit();
}

ResolvedBlockImage ResolvedFromCachedConfig(const std::string& source_image_ref,
                                            const std::string& image_config_path,
                                            const ImageResolutionMetadata& metadata) {
    ResolvedBlockImage out;
    out.image_ref = source_image_ref;
    out.overlaybd_config_path = image_config_path;
    out.base_context = metadata.base_context;
    out.raw_config = metadata.raw_config;
    return out;
}

bool ShouldTryOverlaybdReferrers(const std::string& image_ref,
                                 const std::vector<std::string>& prefixes) {
    for (std::size_t i = 0; i < prefixes.size(); ++i) {
        if (image_ref.compare(0, prefixes[i].size(), prefixes[i]) == 0) return true;
    }
    return false;
}

namespace {

/// Rust `ReferrerDescriptor` — `digest` required, `artifactType` optional
/// (`#[serde(default)]` on an `Option`: absent or null is None). Other keys
/// are ignored.
struct ReferrerDescriptor {
    std::string digest;
    core::Optional<std::string> artifact_type;
};

core::Expected<std::vector<ReferrerDescriptor>, std::string> ParseReferrersIndex(
    const core::Json& value) {
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("invalid type: expected struct ReferrersIndex"));
    }
    std::vector<ReferrerDescriptor> out;
    const core::JsonObject& fields = value.as_object();
    const core::JsonObject::const_iterator manifests = fields.find("manifests");
    // `#[serde(default)] Vec`: absent is empty, null is a type error.
    if (manifests == fields.end()) return out;
    if (manifests->second.kind() != core::Json::Kind::Array) {
        return core::make_unexpected(
            std::string("invalid type: field `manifests`, expected a sequence"));
    }
    const core::JsonArray& items = manifests->second.as_array();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].kind() != core::Json::Kind::Object) {
            return core::make_unexpected(
                std::string("invalid type: expected struct ReferrerDescriptor"));
        }
        const core::JsonObject& item = items[i].as_object();
        ReferrerDescriptor descriptor;
        const core::JsonObject::const_iterator digest = item.find("digest");
        if (digest == item.end()) {
            return core::make_unexpected(std::string("missing field `digest`"));
        }
        if (digest->second.kind() != core::Json::Kind::String) {
            return core::make_unexpected(
                std::string("invalid type: field `digest`, expected a string"));
        }
        descriptor.digest = digest->second.as_string();
        const core::JsonObject::const_iterator artifact = item.find("artifactType");
        if (artifact != item.end() && artifact->second.kind() != core::Json::Kind::Null) {
            if (artifact->second.kind() != core::Json::Kind::String) {
                return core::make_unexpected(
                    std::string("invalid type: field `artifactType`, expected a string"));
            }
            descriptor.artifact_type = core::Optional<std::string>(artifact->second.as_string());
        }
        out.push_back(descriptor);
    }
    return out;
}

}  // namespace

core::Expected<core::Optional<OverlaybdReferrer>, std::string> ParseOverlaybdReferrer(
    const std::string& body) {
    const core::Expected<core::Json, core::AnyError> value = core::Json::Parse(body);
    if (!value.ok()) {
        return core::make_unexpected("parse referrers index JSON: " + value.error().chain());
    }
    const core::Expected<std::vector<ReferrerDescriptor>, std::string> index =
        ParseReferrersIndex(value.value());
    if (!index.ok()) {
        return core::make_unexpected("parse referrers index JSON: " + index.error());
    }
    const std::vector<std::string>& types = OverlaybdReferrerArtifactTypes();
    for (std::size_t t = 0; t < types.size(); ++t) {
        const ReferrerDescriptor* selected = NULL;
        std::size_t candidates = 0;
        for (std::size_t i = 0; i < index.value().size(); ++i) {
            const ReferrerDescriptor& d = index.value()[i];
            if (!d.artifact_type.has_value() || *d.artifact_type != types[t]) continue;
            if (selected == NULL) selected = &d;
            ++candidates;
        }
        if (selected == NULL) continue;
        if (candidates > 1) {
            AGENTENV_WARN("multiple overlaybd-native OCI referrers found; using first: "
                          "artifact_type="
                          << types[t] << " selected_digest=" << selected->digest
                          << " candidates=" << candidates);
        }
        return core::Optional<OverlaybdReferrer>(OverlaybdReferrer(selected->digest, types[t]));
    }
    return core::Optional<OverlaybdReferrer>();
}

core::Expected<core::Optional<OverlaybdReferrer>, std::string> DiscoverOverlaybdReferrer(
    const std::string& regctl_binary, const std::string& subject_ref,
    const core::Optional<std::string>& registry_config) {
    const core::Expected<core::Unit, ImageError> present = EnsureRegctlBinary(regctl_binary);
    if (!present.ok()) {
        return core::make_unexpected("regctl is required to query OCI referrers: " +
                                     present.error().ToString());
    }

    RegctlCommand command = MakeRegctlCommand(regctl_binary, registry_config);
    command.argv.push_back("artifact");
    command.argv.push_back("list");
    command.argv.push_back("--format");
    command.argv.push_back("body");
    command.argv.push_back(subject_ref);
    core::process::RunOptions options;
    options.env = command.env;
    const core::Expected<core::process::Output, std::string> output =
        core::process::RunWithOptions(command.argv, options);
    if (!output.ok()) {
        return core::make_unexpected("spawn regctl artifact list: " + output.error());
    }
    if (!output.value().success()) {
        return core::make_unexpected(
            "regctl artifact list failed for " + subject_ref + ": " +
            core::TrimUnicodeWhitespace(core::FromUtf8Lossy(output.value().stderr_text)));
    }
    const std::string& body = output.value().stdout_text;
    if (core::FromUtf8Lossy(body) != body) {
        return core::make_unexpected(std::string("regctl output is not UTF-8"));
    }
    const core::Expected<core::Optional<OverlaybdReferrer>, std::string> parsed =
        ParseOverlaybdReferrer(body);
    if (!parsed.ok()) {
        return core::make_unexpected("parse regctl referrers response for " + subject_ref + ": " +
                                     parsed.error());
    }
    return parsed;
}

core::Expected<core::Optional<std::pair<FetchedManifest, std::string> >, std::string>
TryResolveOverlaybdReferrer(const std::string& regctl_binary, const std::string& subject_ref,
                            const std::string& arch,
                            const core::Optional<std::string>& registry_config) {
    typedef core::Optional<std::pair<FetchedManifest, std::string> > Result;
    const core::Expected<core::Optional<OverlaybdReferrer>, std::string> discovered =
        DiscoverOverlaybdReferrer(regctl_binary, subject_ref, registry_config);
    if (!discovered.ok()) return core::make_unexpected(discovered.error());
    if (!discovered.value().has_value()) return Result();
    const std::string& referrer_digest = discovered.value()->first;
    const std::string& artifact_type = discovered.value()->second;

    const core::Expected<std::string, std::string> referrer_ref =
        ImageRefWithDigest(subject_ref, referrer_digest);
    if (!referrer_ref.ok()) {
        return core::make_unexpected("build overlaybd referrer reference for " + subject_ref +
                                     ": " + referrer_ref.error());
    }
    const core::Expected<FetchedManifest, ImageError> referrer =
        FetchOciManifest(regctl_binary, referrer_ref.value(), arch, registry_config);
    if (!referrer.ok()) {
        return core::make_unexpected("fetch overlaybd referrer manifest for " +
                                     referrer_ref.value() + ": " + referrer.error().ToString());
    }
    if (referrer.value().format != ImageFormat::OverlaybdNative) {
        return core::make_unexpected("OCI referrer " + referrer_ref.value() +
                                     " advertised artifactType " + artifact_type +
                                     " but its manifest is not overlaybd-native");
    }
    return Result(std::make_pair(referrer.value(), artifact_type));
}

core::Expected<std::string, std::string> DetectArch() {
    // Rust reads the compile-time target; this build's target is the same
    // compile-time fact.
#if defined(__x86_64__)
    return std::string("x86_64");
#elif defined(__aarch64__)
    return std::string("aarch64");
#else
    struct utsname name;
    const std::string other = uname(&name) == 0 ? std::string(name.machine) : "unknown";
    return core::make_unexpected("unsupported architecture: " + other);
#endif
}

std::string RegistryLabel(const std::string& image_ref) {
    const core::Optional<std::string> host = RegistryHostOf(image_ref);
    return host.has_value() ? *host : std::string("unknown");
}

core::Json OverlaybdImageConfigJson(const ResolvedImage& resolved) {
    core::JsonArray lowers;
    std::string repo_blob_url;
    if (resolved.kind == ResolvedImage::kLocal) {
        for (std::size_t i = 0; i < resolved.local_layers.size(); ++i) {
            const LocalLayer& layer = resolved.local_layers[i];
            core::JsonObject lower;
            lower["file"] = core::Json(layer.path);
            lower["digest"] = core::Json(layer.digest);
            lower["size"] = core::Json(static_cast<int64_t>(layer.size));
            lowers.push_back(core::Json(lower));
        }
    } else {
        repo_blob_url = resolved.repo_blob_url;
        for (std::size_t i = 0; i < resolved.remote_layers.size(); ++i) {
            const RemoteLayer& layer = resolved.remote_layers[i];
            core::JsonObject lower;
            lower["digest"] = core::Json(layer.digest);
            lower["size"] = core::Json(static_cast<int64_t>(layer.size));
            lower["dir"] = core::Json(layer.dir);
            lowers.push_back(core::Json(lower));
        }
    }
    core::JsonObject config;
    config["repoBlobUrl"] = core::Json(repo_blob_url);
    config["lowers"] = core::Json(lowers);
    config["upper"] = core::Json(core::JsonObject());
    config["resultFile"] = core::Json(std::string());
    return core::Json(config);
}

// ---- DefaultImageResolver (Rust `ImageResolver`) ----------------------------

namespace {

/// Rust `ImageError::Other(anyhow)` built from a `?` on an anyhow error.
template <typename T>
core::Expected<T, ImageError> Other(const std::string& message) {
    return core::make_unexpected(OtherError(message));
}

/// Rust `metrics::counter!("agentenv_image_resolve_cache_total", ...)`.
void CountResolveCache(const char* result, ImageFormat format, const std::string& registry) {
    observability::CounterIncr(std::string("agentenv_image_resolve_cache_total{result=\"") +
                               result + "\",image_format=\"" + ImageFormatToString(format) +
                               "\",registry=\"" + registry + "\"}");
}

/// Rust `detect_arch()` then `host_arch_to_oci`.
core::Expected<std::string, std::string> HostOciArch() {
    const core::Expected<std::string, std::string> arch = DetectArch();
    if (!arch.ok()) return core::make_unexpected(arch.error());
    return HostArchToOci(arch.value());
}

/// Removes a directory on scope exit (Rust `tempfile::TempDir`).
class ScopedTempDir {
 public:
    explicit ScopedTempDir(const std::string& path) : path_(path) {}
    ~ScopedTempDir() { core::fs::RemoveDirAll(path_); }
    const std::string& path() const { return path_; }

 private:
    ScopedTempDir(const ScopedTempDir&);
    ScopedTempDir& operator=(const ScopedTempDir&);
    std::string path_;
};

}  // namespace

DefaultImageResolver::DefaultImageResolver(const cfg::AppConfig& config)
    : store_(cache::LocalImageServicesFromAppConfig(config).source_images),
      overlaybd_install_root_(core::fs::Join(config.deps_path, "overlaybd")),
      overlaybd_convert_global_config_(config.ResolvedOverlaybdConvertGlobalConfigPath()),
      overlaybd_oci_converter_id_(config.ResolvedOverlaybdOciConverterId()),
      regctl_binary_(config.ResolvedRegctlBinary()),
      default_image_(config.image.resolver.default_image),
      search_registries_(config.image.resolver.search_registries),
      allowed_registries_(config.image.resolver.allowed_registries),
      try_referrers_overlaybd_prefixes_(config.image.resolver.try_referrers_overlaybd_prefixes),
      convert_standard_oci_(config.image.resolver.convert_standard_oci) {}

DefaultImageResolver& DefaultImageResolver::WithRepository(
    std::shared_ptr<snapshot::repository::SnapshotRepository> repository) {
    repository_ = std::move(repository);
    return *this;
}

OverlaybdConversionEnv DefaultImageResolver::ConversionEnv(
    const core::Optional<std::string>& registry_config) const {
    OverlaybdConversionEnv env;
    env.install_root = overlaybd_install_root_;
    env.global_config = overlaybd_convert_global_config_;
    env.converter_id = overlaybd_oci_converter_id_;
    env.regctl_binary = regctl_binary_;
    env.registry_config = registry_config;
    return env;
}

core::Expected<ResolvedBlockImage, std::string> DefaultImageResolver::ResolveBuildkit(
    const BuildkitContent& content, const std::string& digest) {
    if (!convert_standard_oci_) {
        return core::make_unexpected(std::string("standard OCI conversion is disabled"));
    }
    const core::Expected<std::string, std::string> arch = DetectArch();
    if (!arch.ok()) return core::make_unexpected(arch.error());
    const core::Expected<std::pair<FetchedManifest, ImageResolutionMetadata>, std::string>
        fetched_and_metadata = FetchContentManifest(content, digest, arch.value());
    if (!fetched_and_metadata.ok()) return core::make_unexpected(fetched_and_metadata.error());
    const FetchedManifest& fetched = fetched_and_metadata.value().first;
    const ImageResolutionMetadata& metadata = fetched_and_metadata.value().second;

    core::Expected<std::unique_ptr<cache::SourceImageEntry>, std::string> source =
        store_->Open(fetched.manifest_digest, core::Optional<std::string>());
    if (!source.ok()) return core::make_unexpected(source.error());
    const core::Expected<cache::CachedImageConfig, std::string> cached =
        source.value()->CachedConfig();
    if (!cached.ok()) return core::make_unexpected(cached.error());
    if (cached.value().kind == cache::CachedImageConfig::Kind::Found) {
        return ResolvedFromCachedConfig(fetched.manifest_digest,
                                        cached.value().image_config_path, metadata);
    }
    core::Expected<std::shared_ptr<ImageConversion>, std::string> conversion =
        source.value()->BeginConversion();
    if (!conversion.ok()) return core::make_unexpected(conversion.error());
    const core::Expected<ResolvedImage, ImageError> image = ConvertContentImage(
        content, fetched, ConversionEnv(core::Optional<std::string>()), conversion.value().get());
    if (!image.ok()) return core::make_unexpected(image.error().ToString());
    std::shared_ptr<ImageConversion> handed_over;
    handed_over.swap(conversion.value());
    const core::Expected<std::string, std::string> path = source.value()->PublishConfig(
        OverlaybdImageConfigJson(image.value()), metadata, std::move(handed_over));
    if (!path.ok()) return core::make_unexpected(path.error());
    return ResolvedFromCachedConfig(fetched.manifest_digest, path.value(), metadata);
}

core::Expected<core::Optional<ResolvedBlockImage>, ImageError> DefaultImageResolver::ResolveTools(
    const std::string& image_ref) const {
    DefaultImageResolver self(*this);
    // System dependencies use their configured release source, independently
    // of the admission policy and conversion settings for user images.
    const core::Expected<std::string, std::string> arch = DetectArch();
    if (!arch.ok()) return Other<core::Optional<ResolvedBlockImage> >(arch.error());
    core::Expected<FetchedManifest, ImageError> fetched = FetchOciManifest(
        self.regctl_binary_, image_ref, arch.value(), core::Optional<std::string>());
    if (!fetched.ok()) return core::make_unexpected(fetched.error());
    if (fetched.value().format != ImageFormat::OverlaybdNative) {
        const core::Expected<ImageResolutionMetadata, ImageError> metadata =
            FetchOciImageConfigMetadata(self.regctl_binary_, fetched.value().selected_image_ref,
                                        fetched.value().ConfigDigest(),
                                        core::Optional<std::string>());
        if (!metadata.ok()) return core::make_unexpected(metadata.error());
        const std::map<std::string, std::string>& labels = metadata.value().base_context.labels;
        const std::map<std::string, std::string>::const_iterator format =
            labels.find("io.agentenv.tools-drive.format");
        if (format == labels.end()) return core::Optional<ResolvedBlockImage>();
        if (format->second != "oci-rootfs-v1") {
            return core::make_unexpected(
                UnsupportedImage("unsupported tools image format '" + format->second + "'"));
        }
        // v1 is part of the immutable tools release contract (64 GiB layers),
        // independently of runtime upgrades and the conversion policy for
        // user images.
        const core::Optional<std::string> deps = core::fs::Parent(self.overlaybd_install_root_);
        if (!deps.has_value()) {
            return Other<core::Optional<ResolvedBlockImage> >("overlaybd dependency root");
        }
        const core::Expected<std::string, std::string> install_root =
            setup::overlaybd::EnsureToolsConverterV1(*deps);
        if (!install_root.ok()) {
            return Other<core::Optional<ResolvedBlockImage> >(install_root.error());
        }
        self.overlaybd_install_root_ = install_root.value();
        self.overlaybd_oci_converter_id_ = "tools-oci-rootfs-v1:overlaybd-v1.0.18-aenv.1";
        self.convert_standard_oci_ = true;
        self.try_referrers_overlaybd_prefixes_.clear();
        fetched.value().repository_scope = std::string("tools-oci-rootfs-v1");
    }
    const core::Expected<ResolvedBlockImage, ImageError> resolved = self.ResolveFetchedManifest(
        image_ref, arch.value(), fetched.value(), core::Optional<std::string>());
    if (!resolved.ok()) return core::make_unexpected(resolved.error());
    return core::Optional<ResolvedBlockImage>(resolved.value());
}

core::Expected<std::string, std::string> DefaultImageResolver::PublishImage(
    const ResolvedBlockImage& image, const StoreLayersFn& store_layers) {
    if (!repository_) {
        return core::make_unexpected(std::string("image repository is not configured"));
    }
    // Capacity eviction takes this same source-image lock. Keep the local
    // converted layers rooted until the repository has committed them.
    core::Expected<std::unique_ptr<cache::SourceImageEntry>, std::string> source =
        store_->Open(image.image_ref, core::Optional<std::string>());
    if (!source.ok()) return core::make_unexpected(source.error());
    const core::Expected<std::shared_ptr<ImageConversion>, std::string> source_lock =
        source.value()->BeginConversion();
    if (!source_lock.ok()) return core::make_unexpected(source_lock.error());
    if (!image.raw_config.has_value()) {
        return core::make_unexpected(std::string("built image configuration is missing"));
    }
    const snapshot::repository::RepositoryResult<std::vector<snapshot::OverlaybdLayerRef> >
        layers = store_layers(*repository_, image.overlaybd_config_path);
    if (!layers.ok()) return core::make_unexpected(layers.error().ToString());
    const core::Expected<std::string, std::string> architecture = HostOciArch();
    if (!architecture.ok()) return core::make_unexpected(architecture.error());
    const PublishedImage description =
        PublishedImage::New(architecture.value(), layers.value(), *image.raw_config);
    const snapshot::repository::RepositoryResult<std::string> digest =
        PutImage(*repository_, description);
    if (!digest.ok()) return core::make_unexpected(digest.error().ToString());
    return digest.value();
}

core::Expected<ResolvedBlockImage, ImageError> DefaultImageResolver::ResolvePublished(
    const std::string& digest) {
    const core::Expected<core::Unit, std::string> valid = ValidateDigest(digest);
    if (!valid.ok()) return core::make_unexpected(InvalidReference(valid.error()));
    if (!repository_) return Other<ResolvedBlockImage>("image repository is not configured");
    const snapshot::repository::RepositoryResult<core::Optional<PublishedImage> > found =
        GetImage(*repository_, digest);
    if (!found.ok()) return Other<ResolvedBlockImage>(found.error().ToString());
    if (!found.value().has_value()) {
        return core::make_unexpected(
            NotFound("published OverlayBD image '" + digest + "' was not found"));
    }
    const PublishedImage& image = *found.value();
    const core::Expected<std::string, std::string> architecture = HostOciArch();
    if (!architecture.ok()) return Other<ResolvedBlockImage>(architecture.error());
    if (image.architecture != architecture.value()) {
        return core::make_unexpected(UnsupportedImage("image requires " + image.architecture));
    }
    core::Json wrapped = core::Json(core::JsonObject());
    wrapped["config"] = image.config;
    const core::Expected<ImageResolutionMetadata, std::string> metadata =
        ParseOciImageConfig(wrapped.ToString());
    if (!metadata.ok()) return Other<ResolvedBlockImage>(metadata.error());

    core::Expected<std::unique_ptr<cache::SourceImageEntry>, std::string> source =
        store_->Open(digest, core::Optional<std::string>(std::string("published-overlaybd-v1")));
    if (!source.ok()) return Other<ResolvedBlockImage>(source.error());
    const core::Expected<cache::CachedImageConfig, std::string> cached =
        source.value()->CachedConfig();
    if (!cached.ok()) return Other<ResolvedBlockImage>(cached.error());
    if (cached.value().kind == cache::CachedImageConfig::Kind::Found) {
        return ResolvedFromCachedConfig(digest, cached.value().image_config_path,
                                        metadata.value());
    }
    core::Expected<std::shared_ptr<ImageConversion>, std::string> conversion =
        source.value()->BeginConversion();
    if (!conversion.ok()) return Other<ResolvedBlockImage>(conversion.error());
    const core::Expected<std::string, std::string> work_dir =
        core::fs::CreateTempDir("aenv-published-image-");
    if (!work_dir.ok()) {
        return Other<ResolvedBlockImage>("materialize published image: " + work_dir.error());
    }
    const ScopedTempDir work(work_dir.value());
    const snapshot::repository::RepositoryResult<std::string> path =
        snapshot::repository::MaterializeVolumeBacking(*repository_, image.layers,
                                                       core::fs::Join(work.path(), "image.json"));
    if (!path.ok()) return Other<ResolvedBlockImage>(path.error().ToString());
    const core::Expected<std::string, std::string> bytes = core::fs::ReadToString(path.value());
    if (!bytes.ok()) return Other<ResolvedBlockImage>(bytes.error());
    const core::Expected<core::Json, core::AnyError> config = core::Json::Parse(bytes.value());
    if (!config.ok()) return Other<ResolvedBlockImage>(config.error().chain());
    std::shared_ptr<ImageConversion> handed_over;
    handed_over.swap(conversion.value());
    const core::Expected<std::string, std::string> published =
        source.value()->PublishConfig(config.value(), metadata.value(), std::move(handed_over));
    if (!published.ok()) return Other<ResolvedBlockImage>(published.error());
    return ResolvedFromCachedConfig(digest, published.value(), metadata.value());
}

core::Expected<ResolvedBlockImage, ImageError> DefaultImageResolver::ResolveTag(
    const std::string& reference) {
    const core::Expected<snapshot::ImageTag, std::string> tag =
        snapshot::ImageTag::Parse(reference);
    if (!tag.ok()) return core::make_unexpected(InvalidReference(tag.error()));
    if (!repository_) return Other<ResolvedBlockImage>("image repository is not configured");
    const snapshot::repository::RepositoryResult<core::Optional<std::string> > digest =
        GetImageTag(*repository_, tag.value());
    if (!digest.ok()) return Other<ResolvedBlockImage>(digest.error().ToString());
    if (!digest.value().has_value()) {
        return core::make_unexpected(
            NotFound("image tag '" + tag.value().ToString() + "' was not found"));
    }
    AGENTENV_DEBUG("resolved image tag: tag=" << tag.value().ToString()
                                              << " digest=" << *digest.value());
    const core::Expected<ResolvedBlockImage, ImageError> resolved =
        ResolvePublished(*digest.value());
    if (!resolved.ok()) {
        return core::make_unexpected(
            resolved.error().Context("image tag '" + tag.value().ToString() + "'"));
    }
    return resolved;
}

core::Expected<ResolvedBlockImage, ImageError> DefaultImageResolver::Resolve(
    const std::string& image_ref) {
    return ResolveWithRegistryAuth(image_ref, NULL);
}

core::Expected<ResolvedBlockImage, ImageError> DefaultImageResolver::ResolveWithRegistryAuth(
    const std::string& image_ref, const RegistryAuth* auth) {
    // Bare digests identify repository-owned OverlayBD descriptions, not OCI
    // manifests or node-local conversion cache entries.
    const std::string trimmed = core::TrimUnicodeWhitespace(image_ref);
    const std::string tag_prefix(kImageTagPrefix);
    if (trimmed.compare(0, tag_prefix.size(), tag_prefix) == 0) {
        if (auth) {
            return core::make_unexpected(InvalidReference(
                "fromImageRegistry cannot be used with AgentENV published images"));
        }
        return ResolveTag(trimmed.substr(tag_prefix.size()));
    }
    if (trimmed.compare(0, 7, "sha256:") == 0) {
        if (auth) {
            return core::make_unexpected(InvalidReference(
                "fromImageRegistry cannot be used with AgentENV published images"));
        }
        return ResolvePublished(trimmed);
    }
    const core::Expected<std::vector<std::string>, ImageError> candidates =
        ImageRefCandidates(image_ref, search_registries_, allowed_registries_);
    if (!candidates.ok()) return core::make_unexpected(candidates.error());
    if (auth && candidates.value().size() != 1) {
        return core::make_unexpected(InvalidReference(
            "fromImageRegistry requires an unambiguous image registry; use a fully-qualified "
            "fromImage"));
    }
    // Keeps the regctl config file alive for the whole resolve.
    core::Optional<RegistryConfig> registry_config;
    if (auth) {
        const core::Optional<std::string> host = RegistryHostOf(candidates.value()[0]);
        if (!host.has_value()) return Other<ResolvedBlockImage>("image registry host");
        const core::Expected<RegistryConfig, std::string> config = auth->Config(*host);
        if (!config.ok()) return Other<ResolvedBlockImage>(config.error());
        registry_config = config.value();
    }
    const core::Optional<std::string> config_path =
        registry_config.has_value() ? core::Optional<std::string>(registry_config->Path())
                                    : core::Optional<std::string>();
    const core::Expected<std::string, std::string> arch = DetectArch();
    if (!arch.ok()) return Other<ResolvedBlockImage>(arch.error());
    std::vector<std::string> manifest_failures;
    // Track whether every candidate failed specifically because the registry
    // returned HTTP 404. If so, the overall failure is a user error (the image
    // does not exist) rather than a server error, and a typed `NotFound` lets
    // the API layer map it to a 4xx.
    bool all_not_found = true;

    for (std::size_t i = 0; i < candidates.value().size(); ++i) {
        const std::string& candidate = candidates.value()[i];
        // The manifest fetch is the only "wrong registry, try the next one"
        // stage. Once a manifest is fetched, any later failure is fatal and
        // aborts the whole resolve.
        observability::MetricGuard metric =
            observability::MetricGuard::Stage(kImageResolveStageDuration, "manifest_fetch");
        core::Expected<FetchedManifest, ImageError> fetched =
            FetchOciManifest(regctl_binary_, candidate, arch.value(), config_path);
        metric.Finish(fetched.ok());
        if (!fetched.ok()) {
            const ImageError& err = fetched.error();
            if (err.kind == ImageError::kNotFound) {
                AGENTENV_WARN("image resolver candidate does not exist; trying next candidate: "
                              "image="
                              << candidate << " error=" << err.reason);
                manifest_failures.push_back(candidate + ": " + err.reason);
                continue;
            }
            // The manifest was fetched but describes an image AgentENV cannot
            // run (e.g. overlaybd turbo-OCI). The image exists, so trying
            // other registries is pointless.
            if (err.kind == ImageError::kUnsupportedImage) {
                return core::make_unexpected(err.Context("resolve image '" + candidate + "'"));
            }
            AGENTENV_WARN("image resolver candidate manifest fetch failed; trying next "
                          "candidate: image="
                          << candidate << " error=" << err.ToString());
            all_not_found = false;
            manifest_failures.push_back(candidate + ": " + err.ToString());
            continue;
        }
        return ResolveFetchedManifest(candidate, arch.value(), fetched.value(), config_path);
    }

    std::string joined;
    for (std::size_t i = 0; i < manifest_failures.size(); ++i) {
        if (i) joined += "; ";
        joined += manifest_failures[i];
    }
    const std::string message = "resolve image '" + trimmed +
                                "' to overlaybd: all registry candidates failed during manifest "
                                "fetch: " +
                                joined;
    if (all_not_found) return core::make_unexpected(NotFound(message));
    return Other<ResolvedBlockImage>(message);
}

core::Expected<ResolvedBlockImage, ImageError> DefaultImageResolver::ResolveFetchedManifest(
    const std::string& image_ref, const std::string& arch, FetchedManifest fetched,
    const core::Optional<std::string>& registry_config) {
    const std::string source_image_ref = fetched.selected_image_ref;
    // Config metadata comes from the source image even when layer resolution
    // is redirected to an overlaybd referrer below.
    const std::string source_config_digest = fetched.ConfigDigest();
    std::string overlaybd_image_ref = source_image_ref;
    if (ShouldTryOverlaybdReferrers(source_image_ref, try_referrers_overlaybd_prefixes_) &&
        fetched.format != ImageFormat::OverlaybdNative) {
        const core::Expected<core::Optional<std::pair<FetchedManifest, std::string> >,
                             std::string>
            referrer = TryResolveOverlaybdReferrer(regctl_binary_, source_image_ref, arch,
                                                   registry_config);
        if (!referrer.ok()) {
            AGENTENV_WARN("failed to use overlaybd OCI referrer; continuing with source image: "
                          "image="
                          << image_ref << " subject=" << source_image_ref
                          << " error=" << referrer.error());
        } else if (referrer.value().has_value()) {
            overlaybd_image_ref = referrer.value()->first.selected_image_ref;
            AGENTENV_INFO("using overlaybd-native OCI referrer instead of source image: image="
                          << image_ref << " subject=" << source_image_ref
                          << " overlaybd_referrer=" << overlaybd_image_ref
                          << " artifact_type=" << referrer.value()->second);
            fetched = referrer.value()->first;
        } else {
            AGENTENV_TRACE("no overlaybd-native OCI referrer found; continuing with source "
                           "image: image="
                           << image_ref << " subject=" << source_image_ref);
        }
    }
    const core::Expected<core::Unit, ImageError> auth_ok =
        ValidateRegistryAuthForFormat(fetched.format, registry_config.has_value());
    if (!auth_ok.ok()) return core::make_unexpected(auth_ok.error());
    if (fetched.format == ImageFormat::StandardOci && !convert_standard_oci_) {
        return core::make_unexpected(InvalidReference(
            "image '" + image_ref +
            "' is standard OCI, but AgentENV standard OCI to overlaybd conversion is disabled "
            "by image.resolver.convert_standard_oci=false; publish an overlaybd-native image "
            "or enable conversion"));
    }
    const std::string manifest_digest = fetched.manifest_digest;
    const core::Optional<std::string> repository_scope = fetched.repository_scope;
    core::Expected<std::unique_ptr<cache::SourceImageEntry>, std::string> source =
        store_->Open(manifest_digest, repository_scope);
    if (!source.ok()) return Other<ResolvedBlockImage>(source.error());

    const core::Expected<cache::CachedImageConfig, std::string> cached =
        source.value()->CachedConfig();
    if (!cached.ok()) return Other<ResolvedBlockImage>(cached.error());
    if (cached.value().kind == cache::CachedImageConfig::Kind::Found) {
        CountResolveCache("hit", fetched.format, RegistryLabel(source_image_ref));
        if (cached.value().metadata.has_value()) {
            return ResolvedFromCachedConfig(source_image_ref, cached.value().image_config_path,
                                            *cached.value().metadata);
        }
        const core::Expected<ImageResolutionMetadata, ImageError> metadata =
            FetchOciImageConfigMetadata(regctl_binary_, source_image_ref, source_config_digest,
                                        registry_config);
        if (!metadata.ok()) {
            return core::make_unexpected(metadata.error().Context(
                "fetch image metadata for '" + source_image_ref + "'"));
        }
        const core::Expected<std::string, std::string> written =
            source.value()->WriteMetadata(metadata.value());
        if (!written.ok()) return Other<ResolvedBlockImage>(written.error());
        return ResolvedFromCachedConfig(source_image_ref, cached.value().image_config_path,
                                        metadata.value());
    }
    CountResolveCache("miss", fetched.format, RegistryLabel(source_image_ref));

    core::Expected<std::shared_ptr<ImageConversion>, std::string> conversion =
        source.value()->BeginConversion();
    if (!conversion.ok()) return Other<ResolvedBlockImage>(conversion.error());

    observability::MetricGuard config_metric =
        observability::MetricGuard::Stage(kImageResolveStageDuration, "config_fetch");
    core::Expected<ImageResolutionMetadata, ImageError> image_config_metadata =
        FetchOciImageConfigMetadata(regctl_binary_, source_image_ref, source_config_digest,
                                    registry_config);
    config_metric.Finish(image_config_metadata.ok());
    if (!image_config_metadata.ok()) {
        return core::make_unexpected(image_config_metadata.error().Context(
            "fetch image metadata for '" + source_image_ref + "'"));
    }

    observability::MetricGuard convert_metric =
        observability::MetricGuard::Stage(kImageResolveStageDuration, "layer_convert");
    const core::Expected<ResolvedImage, ImageError> resolved = ConvertFetchedOciImageToOverlaybd(
        overlaybd_image_ref, fetched, ConversionEnv(registry_config), conversion.value().get(),
        arch);
    convert_metric.Finish(resolved.ok());
    if (!resolved.ok()) {
        return core::make_unexpected(
            resolved.error().Context("resolve image '" + image_ref + "' to overlaybd"));
    }
    std::shared_ptr<ImageConversion> handed_over;
    handed_over.swap(conversion.value());
    const core::Expected<std::string, std::string> image_config_path =
        source.value()->PublishConfig(OverlaybdImageConfigJson(resolved.value()),
                                      image_config_metadata.value(), std::move(handed_over));
    if (!image_config_path.ok()) return Other<ResolvedBlockImage>(image_config_path.error());

    AGENTENV_INFO("image resolved to overlaybd config: image="
                  << image_ref << " source_image=" << source_image_ref
                  << " resolved_image=" << overlaybd_image_ref
                  << " manifest_digest=" << manifest_digest << " repository_scope="
                  << (repository_scope.has_value() ? *repository_scope : std::string("None"))
                  << " config=" << image_config_path.value());

    return ResolvedFromCachedConfig(source_image_ref, image_config_path.value(),
                                    image_config_metadata.value());
}

}  // namespace image
}  // namespace agentenv
