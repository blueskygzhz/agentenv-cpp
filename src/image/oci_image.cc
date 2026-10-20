// SPDX-License-Identifier: MIT
// Rust: src/image/oci_image.rs — the pure-logic subset.
#include "agentenv/image/oci_image.h"

#include <cstring>
#include <sstream>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/image/commit_index.h"

namespace agentenv {
namespace image {

const uint32_t kRegctlRetryAttempts = 5;
const int64_t kRegctlRetryBaseDelayMs = 500;
const std::size_t kMaxIndexResolutionDepth = 4;
const uint64_t kLayerVirtualSizeGib = 64;

const char* const kOverlaybdTurboArtifactType =
    "application/vnd.containerd.overlaybd.turbo.v1+json";
const char* const kOverlaybdTurboAnnotationPrefix =
    "containerd.io/snapshot/overlaybd/turbo-oci/";
const char* const kOverlaybdVersionAnnotation = "containerd.io/snapshot/overlaybd/version";
const char* const kOverlaybdBlobDigestAnnotation =
    "containerd.io/snapshot/overlaybd/blob-digest";

namespace {

typedef std::map<std::string, std::string> StringMap;

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// Rust `Value::get`, without the null-means-absent rewrite.
///
/// Distinct from the `Field` helper in `metadata.cc` on purpose: there, a
/// `null` stands for an unset `Option` and is reported as absent. Here the
/// question is whether the *key* exists, because `raw_config` mirrors
/// `doc.get_mut("config")`, which yields `Some(Null)` for `"config": null`.
const core::Json* Member(const core::Json& object, const std::string& key) {
    if (object.kind() != core::Json::Kind::Object) return 0;
    const core::JsonObject& map = object.as_object();
    const core::JsonObject::const_iterator found = map.find(key);
    return found == map.end() ? 0 : &found->second;
}

/// The serde-`Option` reading: a missing key and an explicit `null` both mean
/// "take the default".
const core::Json* OptionalMember(const core::Json& object, const std::string& key) {
    const core::Json* found = Member(object, key);
    if (found == 0 || found->kind() == core::Json::Kind::Null) return 0;
    return found;
}

core::Expected<std::vector<std::string>, std::string> ReadStringArray(const core::Json& value,
                                                                      const char* field) {
    if (value.kind() != core::Json::Kind::Array) {
        return core::make_unexpected(std::string("OCI image config field '") + field +
                                     "' must be an array of strings");
    }
    std::vector<std::string> out;
    const core::JsonArray& array = value.as_array();
    for (std::size_t i = 0; i < array.size(); ++i) {
        if (array[i].kind() != core::Json::Kind::String) {
            return core::make_unexpected(std::string("OCI image config field '") + field +
                                         "' must contain only strings");
        }
        out.push_back(array[i].as_string());
    }
    return out;
}

/// The keys of an OCI set-shaped field (`ExposedPorts`, `Volumes`).
///
/// Values are ignored, matching Rust's `HashMap<String, serde_json::Value>`:
/// the spec writes `{}` for every entry and a publisher that puts something
/// else there must not make the whole config unreadable.
///
/// The keys come back sorted, because `std::map` is ordered where Rust's
/// `HashMap` is not. Rust's own tests sort before comparing, so sorted output
/// satisfies the same contract -- and it additionally makes the serialized
/// metadata byte-stable, which the Rust side does not guarantee.
core::Expected<std::vector<std::string>, std::string> ReadSetKeys(const core::Json& value,
                                                                  const char* field) {
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("OCI image config field '") + field +
                                     "' must be an object");
    }
    std::vector<std::string> out;
    const core::JsonObject& object = value.as_object();
    for (core::JsonObject::const_iterator it = object.begin(); it != object.end(); ++it) {
        out.push_back(it->first);
    }
    return out;
}

core::Expected<StringMap, std::string> ReadStringMap(const core::Json& value, const char* field) {
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("OCI image config field '") + field +
                                     "' must be an object");
    }
    StringMap out;
    const core::JsonObject& object = value.as_object();
    for (core::JsonObject::const_iterator it = object.begin(); it != object.end(); ++it) {
        if (it->second.kind() != core::Json::Kind::String) {
            return core::make_unexpected(std::string("OCI image config field '") + field +
                                         "' must map to strings");
        }
        out.insert(std::make_pair(it->first, it->second.as_string()));
    }
    return out;
}

core::Expected<core::Optional<std::string>, std::string> ReadOptionalString(
    const core::Json& value, const char* field) {
    if (value.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("OCI image config field '") + field +
                                     "' must be a string");
    }
    return core::Optional<std::string>(value.as_string());
}

/// Rust `leaf.find('@').or_else(|| leaf.find(':'))`.
///
/// Note the asymmetry with `SplitNameSuffix` in `reference.h`: that one splits
/// at the *tag* when a reference carries both, dropping it in favour of the
/// digest, whereas this one splits at the `@` and so leaves `repo:tag` as the
/// repository. The two behaviours are both as Rust has them and are not
/// interchangeable -- `reference.rs` is building a reference to pull, while
/// this is recovering a repository path for a registry URL. Unifying them
/// would change one of the two call sites silently.
std::string StripLeafSuffix(const std::string& leaf) {
    std::size_t cut = leaf.find('@');
    if (cut == std::string::npos) cut = leaf.find(':');
    if (cut == std::string::npos) return leaf;
    return leaf.substr(0, cut);
}

std::string JoinWith(const std::vector<std::string>& values, const std::string& separator) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += separator;
        out += values[i];
    }
    return out;
}

/// `{:?}` of a `BTreeMap<String, String>`, as it appears in the classification
/// error messages.
///
/// Rendered here rather than omitted because the annotation set is the whole
/// evidence for rejecting an image: without it the operator is told the layer
/// is turbo-OCI but not which annotation said so.
std::string DebugAnnotations(const StringMap& annotations) {
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (StringMap::const_iterator it = annotations.begin(); it != annotations.end(); ++it) {
        if (!first) out << ", ";
        first = false;
        out << "\"" << it->first << "\": \"" << it->second << "\"";
    }
    out << "}";
    return out.str();
}

bool IsAsciiHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

char ToLowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreAsciiCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) return false;
    }
    return true;
}

}  // namespace

const char* ImageFormatToString(ImageFormat format) {
    switch (format) {
        case ImageFormat::OverlaybdNative:
            return "overlaybd_native";
        case ImageFormat::StandardOci:
        default:
            return "standard_oci";
    }
}

bool IsOverlaybdNativeMediaType(const std::string& media_type) {
    // Matched at component boundaries (`.`/`/`/`+` prefixes) rather than as a
    // bare substring, so an unrelated mediaType that merely contains the word
    // is not misclassified as a sealed overlaybd lower.
    static const char* const kTokens[] = {
        ".overlaybd", "/overlaybd", "+overlaybd", ".zfile", "/zfile", "+zfile",
    };
    for (std::size_t i = 0; i < sizeof(kTokens) / sizeof(kTokens[0]); ++i) {
        if (Contains(media_type, kTokens[i])) return true;
    }
    return false;
}

bool IsStandardTarMediaType(const std::string& media_type) {
    return media_type == "application/vnd.oci.image.layer.v1.tar" ||
           media_type == "application/vnd.oci.image.layer.v1.tar+gzip" ||
           media_type == "application/vnd.oci.image.layer.v1.tar+zstd" ||
           media_type == "application/vnd.docker.image.rootfs.diff.tar" ||
           media_type == "application/vnd.docker.image.rootfs.diff.tar.gzip";
}

LayerClass ClassifyLayer(const OciLayerDescriptor& layer) {
    // Turbo detection comes first, before the mediaType is even looked at:
    // turbo layers also carry `blob-digest == layer digest`, so the
    // self-referential rule below would classify them as native and they would
    // then fail at runtime with a confusing short read from the registryfs
    // zfile probe instead of being rejected here.
    bool is_turbo = false;
    const std::string turbo_prefix(kOverlaybdTurboAnnotationPrefix);
    for (StringMap::const_iterator it = layer.annotations.begin();
         it != layer.annotations.end(); ++it) {
        if (StartsWith(it->first, turbo_prefix)) {
            is_turbo = true;
            break;
        }
    }
    if (!is_turbo) {
        const StringMap::const_iterator version =
            layer.annotations.find(kOverlaybdVersionAnnotation);
        if (version != layer.annotations.end() && Contains(version->second, "turbo")) {
            is_turbo = true;
        }
    }
    if (is_turbo) return LayerClass::OverlaybdTurbo;

    // A native mediaType wins next: the blob is directly a sealed overlaybd
    // lower even if the publisher also emitted a legacy overlaybd annotation.
    if (IsOverlaybdNativeMediaType(layer.media_type)) return LayerClass::OverlaybdNative;

    if (!IsStandardTarMediaType(layer.media_type)) return LayerClass::Unknown;

    // A `blob-digest` annotation marks an accelerated-container-image
    // publication, and two conventions coexist:
    //
    //  * mediaType-lying native -- the wire blob IS the overlaybd lower and the
    //    tar mediaType is there only for OCI-client compatibility. Publishers
    //    set `blob-digest` to the layer's own digest to say "these bytes are
    //    the overlaybd content". registryfs_v2 fetches the layer digest and
    //    gets overlaybd bytes unchanged, so this is usable as-is.
    //  * tar-wrapped -- the wire blob is a tar archive with an overlaybd blob
    //    inside, and `blob-digest` names the *inner* blob. That still needs
    //    local extraction, so it stays rejected.
    const StringMap::const_iterator annotated =
        layer.annotations.find(kOverlaybdBlobDigestAnnotation);
    if (annotated != layer.annotations.end()) {
        if (annotated->second == layer.digest) return LayerClass::OverlaybdNative;
        return LayerClass::OverlaybdTarWrapped;
    }

    return LayerClass::StandardTar;
}

core::Expected<ImageFormat, ImageError> ClassifyManifest(const OciManifest& manifest) {
    // `kOther`, not `kUnsupportedImage`: a layerless manifest is not a shape a
    // publisher chose, it is a manifest that should not exist, so this is a
    // 5xx and not advice to the caller.
    if (manifest.layers.empty()) {
        return core::make_unexpected(OtherError("OCI manifest has no layers"));
    }

    const std::string turbo_suffix =
        "); turbo-OCI layers carry only index metadata and are not supported by AgentENV";

    // Manifest-level turbo detection: the artifactType alone is authoritative,
    // before any layer is inspected.
    if (manifest.artifact_type.has_value() &&
        *manifest.artifact_type == kOverlaybdTurboArtifactType) {
        return core::make_unexpected(UnsupportedImage(
            std::string("image is an overlaybd turbo-OCI image (artifactType=") +
            kOverlaybdTurboArtifactType + turbo_suffix));
    }

    std::vector<LayerClass> classes;
    classes.reserve(manifest.layers.size());
    for (std::size_t i = 0; i < manifest.layers.size(); ++i) {
        classes.push_back(ClassifyLayer(manifest.layers[i]));
    }

    for (std::size_t i = 0; i < classes.size(); ++i) {
        const OciLayerDescriptor& layer = manifest.layers[i];
        std::ostringstream reason;
        if (classes[i] == LayerClass::OverlaybdTurbo) {
            reason << "image is an overlaybd turbo-OCI image (layer " << i << " (" << layer.digest
                   << ") carries turbo-OCI annotations: " << DebugAnnotations(layer.annotations)
                   << turbo_suffix;
            return core::make_unexpected(UnsupportedImage(reason.str()));
        }
        if (classes[i] == LayerClass::OverlaybdTarWrapped) {
            reason << "layer " << i << " (" << layer.digest
                   << ") is a tar-wrapped overlaybd layer (mediaType=" << layer.media_type
                   << ", annotations=" << DebugAnnotations(layer.annotations)
                   << "); this shape is not yet supported";
            return core::make_unexpected(UnsupportedImage(reason.str()));
        }
        if (classes[i] == LayerClass::Unknown) {
            reason << "layer " << i << " (" << layer.digest
                   << ") has an unsupported mediaType: " << layer.media_type;
            return core::make_unexpected(UnsupportedImage(reason.str()));
        }
    }

    // Phrased as "all of one kind" rather than "not both kinds". The two are
    // equivalent today, because every other class returned above, but only
    // this form stays correct if a class is added later: an unhandled new
    // class would fall through to the mixed-image rejection instead of being
    // silently accepted as standard OCI.
    bool all_standard = true;
    bool all_overlaybd = true;
    for (std::size_t i = 0; i < classes.size(); ++i) {
        if (classes[i] != LayerClass::StandardTar) all_standard = false;
        if (classes[i] != LayerClass::OverlaybdNative) all_overlaybd = false;
    }

    if (all_standard) return ImageFormat::StandardOci;
    if (all_overlaybd) return ImageFormat::OverlaybdNative;
    return core::make_unexpected(UnsupportedImage(
        "image has a mix of standard OCI and overlaybd-native layers; only homogeneous "
        "images are supported"));
}

core::Expected<std::size_t, std::string> SelectManifestForPlatform(
    const std::vector<OciManifestDescriptor>& manifests, const std::string& arch,
    const std::string& os) {
    if (manifests.size() == 1) return static_cast<std::size_t>(0);

    for (std::size_t i = 0; i < manifests.size(); ++i) {
        const core::Optional<OciPlatform>& platform = manifests[i].platform;
        if (!platform.has_value()) continue;
        // Both halves must be present and match. A descriptor that names only
        // the architecture is not treated as a wildcard for the OS: a linux
        // host must not be handed a windows manifest.
        if (!platform->architecture.has_value() || !platform->os.has_value()) continue;
        if (*platform->architecture == arch && *platform->os == os) return i;
    }

    // The available list is part of the message, not a debug aid: "no manifest
    // matches linux/riscv64" alone does not tell the operator whether the
    // image is single-arch, built for another OS, or published without
    // platform blocks at all.
    std::vector<std::string> available;
    for (std::size_t i = 0; i < manifests.size(); ++i) {
        const core::Optional<OciPlatform>& platform = manifests[i].platform;
        if (!platform.has_value()) {
            available.push_back("no-platform");
            continue;
        }
        const std::string entry_os = platform->os.has_value() ? *platform->os : std::string("?");
        const std::string entry_arch =
            platform->architecture.has_value() ? *platform->architecture : std::string("?");
        available.push_back(entry_os + "/" + entry_arch);
    }
    return core::make_unexpected("no OCI manifest entry matches " + os + "/" + arch +
                                 "; available: [" + JoinWith(available, ", ") + "]");
}

core::Expected<ImageResolutionMetadata, std::string> ParseOciImageConfig(
    const std::string& raw) {
    const core::Expected<core::Json, core::AnyError> document = core::Json::Parse(raw);
    if (!document.ok()) {
        return core::make_unexpected(std::string("parse OCI image config JSON"));
    }

    // Read twice in Rust (once typed, once as a `Value`); here the single
    // parsed document serves both, which cannot drift the way two parses can.
    const core::Json* raw_config = Member(document.value(), "config");
    const core::Json* config = OptionalMember(document.value(), "config");

    core::Json empty;
    const core::Json& fields = config != 0 ? *config : empty;
    if (config != 0 && fields.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("OCI image config field 'config' must be "
                                                 "an object"));
    }

    std::vector<std::string> env_entries;
    if (const core::Json* env = OptionalMember(fields, "Env")) {
        const core::Expected<std::vector<std::string>, std::string> parsed =
            ReadStringArray(*env, "Env");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        env_entries = parsed.value();
    }

    core::Optional<std::string> workdir;
    if (const core::Json* value = OptionalMember(fields, "WorkingDir")) {
        const core::Expected<core::Optional<std::string>, std::string> parsed =
            ReadOptionalString(*value, "WorkingDir");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        workdir = parsed.value();
    }

    core::Optional<std::string> user;
    if (const core::Json* value = OptionalMember(fields, "User")) {
        const core::Expected<core::Optional<std::string>, std::string> parsed =
            ReadOptionalString(*value, "User");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        user = parsed.value();
    }

    std::vector<std::string> exposed_ports;
    if (const core::Json* value = OptionalMember(fields, "ExposedPorts")) {
        const core::Expected<std::vector<std::string>, std::string> parsed =
            ReadSetKeys(*value, "ExposedPorts");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        exposed_ports = parsed.value();
    }

    // Left absent when the key is missing, rather than defaulted to an empty
    // vector: OCI inheritance reads an explicit `[]` as "clear the inherited
    // entrypoint" and a missing key as "keep it", and collapsing the two here
    // would start the container with no entrypoint at all.
    core::Optional<std::vector<std::string> > entrypoint;
    if (const core::Json* value = OptionalMember(fields, "Entrypoint")) {
        const core::Expected<std::vector<std::string>, std::string> parsed =
            ReadStringArray(*value, "Entrypoint");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        entrypoint = core::Optional<std::vector<std::string> >(parsed.value());
    }

    core::Optional<std::vector<std::string> > cmd;
    if (const core::Json* value = OptionalMember(fields, "Cmd")) {
        const core::Expected<std::vector<std::string>, std::string> parsed =
            ReadStringArray(*value, "Cmd");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        cmd = core::Optional<std::vector<std::string> >(parsed.value());
    }

    std::vector<std::string> volumes;
    if (const core::Json* value = OptionalMember(fields, "Volumes")) {
        const core::Expected<std::vector<std::string>, std::string> parsed =
            ReadSetKeys(*value, "Volumes");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        volumes = parsed.value();
    }

    StringMap labels;
    if (const core::Json* value = OptionalMember(fields, "Labels")) {
        const core::Expected<StringMap, std::string> parsed = ReadStringMap(*value, "Labels");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        labels = parsed.value();
    }

    ImageResolutionMetadata metadata;
    metadata.base_context =
        MakeImageBaseContext(EnvVarsFromEntries(env_entries), workdir, user, exposed_ports,
                             entrypoint, cmd, volumes, labels);
    // Present whenever the key is, including for `"config": null`. Keyed on
    // presence rather than content because this is a verbatim pass-through and
    // "the publisher wrote a null config" is itself information a consumer may
    // act on.
    if (raw_config != 0) metadata.raw_config = core::Optional<core::Json>(*raw_config);
    return metadata;
}

core::Expected<core::Unit, std::string> VerifyBlobDigest(const std::string& expected_digest,
                                                         const void* data, std::size_t len) {
    const std::string prefix = "sha256:";
    if (!StartsWith(expected_digest, prefix)) {
        return core::make_unexpected("unsupported digest algorithm (expected sha256): " +
                                     expected_digest);
    }
    const std::string expected_hex = expected_digest.substr(prefix.size());
    const std::string actual_hex = core::Sha256Hex(data, len);
    // Case-insensitive: the OCI grammar allows uppercase hex, and rejecting a
    // correct blob because the manifest spelled the digest in capitals would
    // be indistinguishable from a tampered one.
    if (!EqualsIgnoreAsciiCase(actual_hex, expected_hex)) {
        return core::make_unexpected("blob digest mismatch: manifest declares " + expected_digest +
                                     ", fetched bytes hash to sha256:" + actual_hex);
    }
    return core::Unit();
}

core::Expected<ParsedImageRef, std::string> ParseImageRef(const std::string& image_ref) {
    const std::size_t slash = image_ref.find('/');
    if (slash == std::string::npos) {
        return core::make_unexpected("image reference must include a registry host: " + image_ref);
    }
    const std::string host_part = image_ref.substr(0, slash);
    const std::string rest = image_ref.substr(slash + 1);

    // The same host test as `IsRegistryHost`, kept inline because this one
    // guards the low-level registry URL builder: a hostless reference reaching
    // it would be turned into a URL against whatever the default is.
    const bool looks_like_host = Contains(host_part, ".") || Contains(host_part, ":") ||
                                 host_part == "localhost";
    if (!looks_like_host) {
        return core::make_unexpected(
            "image reference must be fully qualified with a registry host "
            "(e.g. `docker.io/library/debian:tag`), got: " + image_ref);
    }

    // Stripped from the leaf, not the whole reference, so a host port colon
    // survives.
    const std::size_t last_slash_pos = rest.rfind('/');
    const std::size_t last_slash = last_slash_pos == std::string::npos ? 0 : last_slash_pos + 1;
    const std::string repo_head = rest.substr(0, last_slash);
    const std::string leaf_stripped = StripLeafSuffix(rest.substr(last_slash));
    if (leaf_stripped.empty()) {
        return core::make_unexpected("image reference has an empty repository component: " +
                                     image_ref);
    }

    ParsedImageRef out;
    // Both Docker Hub spellings map to the host that actually serves `/v2/`.
    out.host = (host_part == "docker.io" || host_part == "index.docker.io")
                   ? std::string("registry-1.docker.io")
                   : host_part;
    out.repository = repo_head + leaf_stripped;
    return out;
}

std::string RepositoryCacheKey(const std::string& host, const std::string& repository) {
    // No length cap: the value is a cache key that must stay collision-free,
    // and truncating it would make two repositories share one entry.
    return SanitizeFilenameComponent(host + "/" + repository, std::string::npos);
}

core::Expected<std::string, std::string> ImageRefWithDigest(const std::string& image_ref,
                                                            const std::string& digest) {
    const std::size_t last_slash = image_ref.rfind('/');
    if (last_slash == std::string::npos) {
        return core::make_unexpected("image reference is missing host/repository separator: " +
                                     image_ref);
    }
    const std::string head = image_ref.substr(0, last_slash + 1);
    const std::string leaf_stripped = StripLeafSuffix(image_ref.substr(last_slash + 1));
    return head + leaf_stripped + "@" + digest;
}

core::Expected<std::string, std::string> ImageRefRepository(const std::string& image_ref) {
    const std::size_t last_slash = image_ref.rfind('/');
    if (last_slash == std::string::npos) {
        return core::make_unexpected("image reference is missing host/repository separator: " +
                                     image_ref);
    }
    const std::string head = image_ref.substr(0, last_slash + 1);
    const std::string leaf_stripped = StripLeafSuffix(image_ref.substr(last_slash + 1));
    if (leaf_stripped.empty()) {
        return core::make_unexpected("image reference has an empty repository component: " +
                                     image_ref);
    }
    return head + leaf_stripped;
}

core::Expected<std::string, std::string> HostArchToOci(const std::string& host_arch) {
    if (host_arch == "x86_64" || host_arch == "amd64") return std::string("amd64");
    if (host_arch == "aarch64" || host_arch == "arm64") return std::string("arm64");
    return core::make_unexpected("unsupported architecture for OCI pull: " + host_arch);
}

bool IsValidDigestAlgorithm(const std::string& algo) {
    if (algo.empty()) return false;
    // Alphanumeric components joined by single `.`/`_`/`+`/`-` separators.
    // Anything else is rejected, which is what keeps path separators, `..`,
    // and other traversal payloads out of the value before it is joined.
    std::size_t part_begin = 0;
    for (std::size_t i = 0; i <= algo.size(); ++i) {
        const bool at_end = i == algo.size();
        const bool separator =
            !at_end && (algo[i] == '.' || algo[i] == '_' || algo[i] == '+' || algo[i] == '-');
        if (!at_end && !separator) continue;
        if (i == part_begin) return false;  // empty component, e.g. "sha256..x"
        for (std::size_t j = part_begin; j < i; ++j) {
            const char c = algo[j];
            const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            if (!allowed) return false;
        }
        part_begin = i + 1;
    }
    return true;
}

core::Expected<std::string, std::string> BlobPathForDigest(const std::string& layout_dir,
                                                           const std::string& digest) {
    const std::size_t colon = digest.find(':');
    if (colon == std::string::npos) {
        return core::make_unexpected("malformed OCI digest: " + digest);
    }
    const std::string algo = digest.substr(0, colon);
    const std::string hex = digest.substr(colon + 1);

    bool hex_ok = !hex.empty();
    for (std::size_t i = 0; hex_ok && i < hex.size(); ++i) {
        if (!IsAsciiHexDigit(hex[i])) hex_ok = false;
    }
    if (!IsValidDigestAlgorithm(algo) || !hex_ok) {
        return core::make_unexpected(
            "refusing OCI digest with an unsafe algorithm or hex component: " + digest);
    }

    const std::string blob_root = core::fs::Join(layout_dir, "blobs");
    const std::string blob_path = core::fs::Join(core::fs::Join(blob_root, algo), hex);
    // Redundant given the grammar check above, and kept anyway: it is the
    // invariant the caller actually depends on, so it should fail here rather
    // than be re-derived by a future reader of the grammar.
    if (!StartsWith(blob_path, blob_root)) {
        return core::make_unexpected("OCI blob path for digest " + digest +
                                     " escapes the layout directory");
    }
    return blob_path;
}

core::Uuid UuidFromLayerDigest(const std::string& digest) {
    // Hashed over the digest *string*, not the digest bytes: the input is the
    // textual reference as it appears in the manifest, so two manifests that
    // spell the same hash differently intentionally yield different UUIDs
    // rather than silently aliasing.
    const std::array<uint8_t, 32> hash = core::Sha256Bytes(digest.data(), digest.size());
    uint8_t bytes[16];
    std::memcpy(bytes, hash.data(), sizeof(bytes));
    // `Builder::from_custom_bytes`: version nibble 8 ("custom") and the RFC
    // 4122 variant. Stamped so a reader can tell the value was derived rather
    // than randomly generated -- which matters because a v4 here would be
    // indistinguishable while being unreproducible.
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x80);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
    return core::Uuid(bytes);
}

bool RegctlStderrIsNotFound(const std::string& stderr_text) {
    return Contains(stderr_text, "[http 404]");
}

}  // namespace image
}  // namespace agentenv
