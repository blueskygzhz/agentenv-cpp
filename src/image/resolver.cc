// SPDX-License-Identifier: MIT
// Rust: src/image/resolver.rs — free functions.
#include "agentenv/image/resolver.h"

#include <sys/utsname.h>

#include <cstdint>

#include "agentenv/core/logging.h"
#include "agentenv/core/process.h"
#include "agentenv/image/reference.h"

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

/// Length of the Unicode `White_Space` character encoded at `s[i]` (Rust
/// `char::is_whitespace`), or 0. `s` is valid UTF-8 (it went through
/// `FromUtf8Lossy`).
std::size_t WhitespaceAt(const std::string& s, std::size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == ' ' || (c >= 0x09 && c <= 0x0d)) return 1;
    if (c < 0x80) return 0;
    std::size_t len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    if (i + len > s.size()) return 0;
    uint32_t cp = len == 2 ? (c & 0x1f) : len == 3 ? (c & 0x0f) : (c & 0x07);
    for (std::size_t k = 1; k < len; ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
    }
    const bool ws = cp == 0x85 || cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) ||
                    cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f ||
                    cp == 0x3000;
    return ws ? len : 0;
}

/// Rust `str::trim`.
std::string TrimUnicode(const std::string& s) {
    std::size_t begin = 0;
    for (std::size_t n; begin < s.size() && (n = WhitespaceAt(s, begin)) != 0;) begin += n;
    // Scan forward to find where trailing whitespace starts: UTF-8 cannot be
    // decoded backwards without re-synchronizing anyway.
    std::size_t end = begin;
    for (std::size_t i = begin; i < s.size();) {
        const std::size_t n = WhitespaceAt(s, i);
        if (n != 0) {
            i += n;
            continue;
        }
        const unsigned char c = static_cast<unsigned char>(s[i]);
        i += c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        end = i < s.size() ? i : s.size();
    }
    return s.substr(begin, end - begin);
}

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
            TrimUnicode(core::FromUtf8Lossy(output.value().stderr_text)));
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

}  // namespace image
}  // namespace agentenv
