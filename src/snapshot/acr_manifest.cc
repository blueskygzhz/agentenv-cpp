// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/common/acr/manifest.rs
#include "agentenv/snapshot/acr_manifest.h"

#include <algorithm>
#include <sstream>

#include "agentenv/core/digest.h"

namespace agentenv {
namespace snapshot {

const char* const kOciImageManifestMediaType =
    "application/vnd.oci.image.manifest.v1+json";
const char* const kOciImageConfigMediaType = "application/vnd.oci.image.config.v1+json";
const char* const kOciTarLayerMediaType = "application/vnd.oci.image.layer.v1.tar";

const char* const kOverlaybdBlobDigestAnnotation =
    "containerd.io/snapshot/overlaybd/blob-digest";
const char* const kOverlaybdBlobSizeAnnotation =
    "containerd.io/snapshot/overlaybd/blob-size";
const char* const kSnapshotTagAnnotation = "io.agentenv.snapshot.tag";

namespace {

// `serde` serialises a *struct* in field-declaration order, while a
// `serde_json::Value::Object` (a BTreeMap) comes out key-sorted. `core::Json`
// is backed by `std::map`, so it reproduces the second case but not the
// first. These blobs are hashed and the digest is the image's identity, so
// the struct-shaped parts are written field by field here rather than through
// a `JsonObject`; getting this wrong would silently produce a different
// digest from the Rust implementation for identical input.

void WriteJsonString(std::ostringstream& out, const std::string& value) {
    out << core::Json(value).ToString();
}

/// One `"key":<value>` pair, with the comma handled by the caller.
void WriteField(std::ostringstream& out, bool& first, const std::string& key,
                const std::string& serialized_value) {
    if (!first) out << ",";
    first = false;
    WriteJsonString(out, key);
    out << ":" << serialized_value;
}

std::string SerializeAnnotations(const std::map<std::string, std::string>& annotations) {
    // A BTreeMap on the Rust side; `std::map` iterates in the same order.
    core::JsonObject object;
    for (std::map<std::string, std::string>::const_iterator it = annotations.begin();
         it != annotations.end(); ++it) {
        object[it->first] = core::Json(it->second);
    }
    return core::Json(object).ToString();
}

/// Rust `OciDescriptor` field order: mediaType, digest, size, annotations.
/// `annotations` is skipped when empty (`skip_serializing_if`).
std::string SerializeDescriptor(const OciDescriptor& descriptor) {
    std::ostringstream out;
    out << "{";
    bool first = true;
    WriteField(out, first, "mediaType", core::Json(descriptor.media_type).ToString());
    WriteField(out, first, "digest", core::Json(descriptor.digest).ToString());
    WriteField(out, first, "size",
               core::Json(static_cast<int64_t>(descriptor.size)).ToString());
    if (!descriptor.annotations.empty()) {
        WriteField(out, first, "annotations", SerializeAnnotations(descriptor.annotations));
    }
    out << "}";
    return out.str();
}

/// Rust `OciConfig` field order: created, architecture, os, config, rootfs,
/// history; `MinimalRootfs` is `type`, `diff_ids`.
std::string SerializeOciConfig(const std::string& architecture, const core::Json& config) {
    std::ostringstream out;
    out << "{";
    bool first = true;
    // A fixed epoch timestamp, so the blob is reproducible across runs.
    WriteField(out, first, "created", core::Json("1970-01-01T00:00:00Z").ToString());
    WriteField(out, first, "architecture", core::Json(architecture).ToString());
    WriteField(out, first, "os", core::Json("linux").ToString());
    WriteField(out, first, "config", config.ToString());
    // OverlayBD snapshot layers are not ordinary uncompressed OCI tar diffs,
    // so `diff_ids` stays empty on purpose.
    WriteField(out, first, "rootfs", std::string("{\"type\":\"layers\",\"diff_ids\":[]}"));
    WriteField(out, first, "history", std::string("[]"));
    out << "}";
    return out.str();
}

/// Rust `oci_config_blob`.
OciConfigBlob BuildConfigBlob(const std::string& architecture, const core::Json& config) {
    OciConfigBlob blob;
    blob.bytes = SerializeOciConfig(architecture, config);
    blob.digest = core::Sha256Digest(blob.bytes);
    blob.size = static_cast<uint64_t>(blob.bytes.size());
    return blob;
}

/// Rust `set_optional` — inserts when present, removes when absent, so a
/// field inherited from the source image is cleared rather than left behind.
void SetOptional(core::JsonObject& config, const std::string& key,
                 const core::Optional<core::Json>& value) {
    if (value.has_value()) {
        config[key] = *value;
    } else {
        config.erase(key);
    }
}

/// Rust `empty_object_map` — OCI renders port and volume sets as objects with
/// empty values.
core::Json EmptyObjectMap(const std::vector<std::string>& values) {
    core::JsonObject object;
    for (std::size_t i = 0; i < values.size(); ++i) {
        object[values[i]] = core::Json(core::JsonObject());
    }
    return core::Json(object);
}

core::Json StringArray(const std::vector<std::string>& values) {
    core::JsonArray array;
    for (std::size_t i = 0; i < values.size(); ++i) {
        array.push_back(core::Json(values[i]));
    }
    return core::Json(array);
}

}  // namespace

// ---- OciDescriptor --------------------------------------------------------

OciDescriptor OciDescriptor::OverlaybdLayer(const std::string& digest, uint64_t size) {
    OciDescriptor descriptor;
    descriptor.media_type = kOciTarLayerMediaType;
    descriptor.digest = digest;
    descriptor.size = size;
    std::ostringstream size_text;
    size_text << size;
    descriptor.annotations[kOverlaybdBlobDigestAnnotation] = digest;
    descriptor.annotations[kOverlaybdBlobSizeAnnotation] = size_text.str();
    return descriptor;
}

OciDescriptor OciDescriptor::Config(const std::string& digest, uint64_t size) {
    OciDescriptor descriptor;
    descriptor.media_type = kOciImageConfigMediaType;
    descriptor.digest = digest;
    descriptor.size = size;
    return descriptor;
}

core::Json OciDescriptor::ToJson() const {
    core::JsonObject object;
    object["mediaType"] = core::Json(media_type);
    object["digest"] = core::Json(digest);
    object["size"] = core::Json(static_cast<int64_t>(size));
    if (!annotations.empty()) {
        core::JsonObject values;
        for (std::map<std::string, std::string>::const_iterator it = annotations.begin();
             it != annotations.end(); ++it) {
            values[it->first] = core::Json(it->second);
        }
        object["annotations"] = core::Json(values);
    }
    return core::Json(object);
}

core::Expected<OciDescriptor, std::string> OciDescriptor::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("OCI descriptor must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    OciDescriptor descriptor;

    const core::JsonObject::const_iterator media_type = fields.find("mediaType");
    if (media_type == fields.end() ||
        media_type->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("missing field `mediaType`"));
    }
    descriptor.media_type = media_type->second.as_string();

    const core::JsonObject::const_iterator digest = fields.find("digest");
    if (digest == fields.end() || digest->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("missing field `digest`"));
    }
    descriptor.digest = digest->second.as_string();

    const core::JsonObject::const_iterator size = fields.find("size");
    if (size == fields.end() || size->second.kind() != core::Json::Kind::Int) {
        return core::make_unexpected(std::string("missing field `size`"));
    }
    if (size->second.as_int() < 0) {
        return core::make_unexpected(std::string("field `size` must not be negative"));
    }
    descriptor.size = static_cast<uint64_t>(size->second.as_int());

    // `#[serde(default)]`: an absent annotations map is empty, not an error.
    const core::JsonObject::const_iterator annotations = fields.find("annotations");
    if (annotations != fields.end() &&
        annotations->second.kind() == core::Json::Kind::Object) {
        const core::JsonObject& values = annotations->second.as_object();
        for (core::JsonObject::const_iterator it = values.begin(); it != values.end(); ++it) {
            if (it->second.kind() != core::Json::Kind::String) {
                return core::make_unexpected(
                    std::string("annotation values must be strings"));
            }
            descriptor.annotations[it->first] = it->second.as_string();
        }
    }

    return descriptor;
}

bool OciDescriptor::operator==(const OciDescriptor& o) const {
    return media_type == o.media_type && digest == o.digest && size == o.size &&
           annotations == o.annotations;
}

// ---- config construction --------------------------------------------------

core::Json CanonicalizeJson(const core::Json& value) {
    if (value.kind() == core::Json::Kind::Array) {
        core::JsonArray out;
        const core::JsonArray& values = value.as_array();
        for (std::size_t i = 0; i < values.size(); ++i) {
            out.push_back(CanonicalizeJson(values[i]));
        }
        return core::Json(out);
    }
    if (value.kind() == core::Json::Kind::Object) {
        // `core::JsonObject` is a `std::map`, so the keys are already sorted;
        // the recursion is what still matters, for nested objects.
        core::JsonObject out;
        const core::JsonObject& values = value.as_object();
        for (core::JsonObject::const_iterator it = values.begin(); it != values.end(); ++it) {
            out[it->first] = CanonicalizeJson(it->second);
        }
        return core::Json(out);
    }
    return value;
}

core::Json MergedRuntimeConfig(const CommandContext& context,
                               const core::Optional<core::Json>& raw_config) {
    core::JsonObject config;
    if (raw_config.has_value() && raw_config->kind() == core::Json::Kind::Object) {
        config = raw_config->as_object();
    }

    // Env is sorted so that the same environment always hashes identically,
    // regardless of the order the map was built in.
    std::vector<std::string> env;
    for (std::map<std::string, std::string>::const_iterator it = context.env_vars.begin();
         it != context.env_vars.end(); ++it) {
        env.push_back(it->first + "=" + it->second);
    }
    std::sort(env.begin(), env.end());
    config["Env"] = StringArray(env);
    config["WorkingDir"] = core::Json(context.workdir);

    SetOptional(config, "User",
                context.user.has_value()
                    ? core::Optional<core::Json>(core::Json(*context.user))
                    : core::Optional<core::Json>());
    SetOptional(config, "Entrypoint",
                context.entrypoint.has_value()
                    ? core::Optional<core::Json>(StringArray(*context.entrypoint))
                    : core::Optional<core::Json>());
    SetOptional(config, "Cmd",
                context.cmd.has_value()
                    ? core::Optional<core::Json>(StringArray(*context.cmd))
                    : core::Optional<core::Json>());
    SetOptional(config, "ExposedPorts",
                context.exposed_ports.empty()
                    ? core::Optional<core::Json>()
                    : core::Optional<core::Json>(EmptyObjectMap(context.exposed_ports)));
    SetOptional(config, "Volumes",
                context.volumes.empty()
                    ? core::Optional<core::Json>()
                    : core::Optional<core::Json>(EmptyObjectMap(context.volumes)));

    if (context.labels.empty()) {
        config.erase("Labels");
    } else {
        core::JsonObject labels;
        for (std::map<std::string, std::string>::const_iterator it = context.labels.begin();
             it != context.labels.end(); ++it) {
            labels[it->first] = core::Json(it->second);
        }
        config["Labels"] = core::Json(labels);
    }

    return CanonicalizeJson(core::Json(config));
}

std::string HostArchitectureForOci() {
    // Rust matches on `std::env::consts::ARCH`; the preprocessor is the
    // equivalent compile-time source here.
#if defined(__x86_64__)
    return "amd64";
#elif defined(__aarch64__)
    return "arm64";
#elif defined(__arm__)
    return "arm";
#elif defined(__riscv) && __riscv_xlen == 64
    return "riscv64";
#else
    return "unknown";
#endif
}

OciConfigBlob SnapshotOciConfigBlob(const std::string& architecture,
                                    const CommandContext& context,
                                    const core::Optional<core::Json>& raw_config) {
    return BuildConfigBlob(architecture, MergedRuntimeConfig(context, raw_config));
}

OciConfigBlob MinimalOciConfigBlob(const std::string& architecture) {
    core::JsonObject config;
    config["Env"] = core::Json(core::JsonArray());
    config["WorkingDir"] = core::Json("");
    return BuildConfigBlob(architecture, core::Json(config));
}

std::string BuildOciImageManifest(const OciDescriptor& config,
                                  const std::vector<OciDescriptor>& layers,
                                  const std::string& publication_tag) {
    // Rust `OciManifest` field order: schemaVersion, mediaType, config,
    // layers, annotations.
    std::ostringstream out;
    out << "{";
    bool first = true;
    WriteField(out, first, "schemaVersion", std::string("2"));
    WriteField(out, first, "mediaType", core::Json(kOciImageManifestMediaType).ToString());
    WriteField(out, first, "config", SerializeDescriptor(config));

    std::ostringstream layer_array;
    layer_array << "[";
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (i > 0) layer_array << ",";
        layer_array << SerializeDescriptor(layers[i]);
    }
    layer_array << "]";
    WriteField(out, first, "layers", layer_array.str());

    std::map<std::string, std::string> annotations;
    annotations[kSnapshotTagAnnotation] = publication_tag;
    WriteField(out, first, "annotations", SerializeAnnotations(annotations));
    out << "}";
    return out.str();
}

}  // namespace snapshot
}  // namespace agentenv
