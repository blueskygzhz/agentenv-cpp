// SPDX-License-Identifier: MIT
// Rust: src/image/mod.rs — `PublishedImage`.
#include "agentenv/image/published_image.h"

#include "agentenv/core/digest.h"
#include "agentenv/image/buildkit.h"

namespace agentenv {
namespace image {

const char* const kImageTagPrefix = "aenv/";

namespace {

const char* const kFields[] = {"schemaVersion", "os", "architecture", "layers", "config"};
const std::size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);
const std::size_t kMaxLayers = 1024;

std::string ExpectedFieldList() {
    std::string out;
    for (std::size_t i = 0; i < kFieldCount; ++i) {
        if (i != 0) out += ", ";
        out += std::string("`") + kFields[i] + "`";
    }
    return out;
}

core::Expected<core::Unit, std::string> Ensure(bool condition, const char* message) {
    if (!condition) return core::make_unexpected(std::string(message));
    return core::Unit();
}

}  // namespace

PublishedImage PublishedImage::New(const std::string& architecture,
                                   const std::vector<snapshot::OverlaybdLayerRef>& layers,
                                   const core::Json& config) {
    PublishedImage out;
    out.schema_version = 1;
    out.os = "linux";
    out.architecture = architecture;
    out.layers = layers;
    out.config = config;
    return out;
}

core::Json PublishedImage::ToJson() const {
    core::JsonObject object;
    object["schemaVersion"] = core::Json(static_cast<int64_t>(schema_version));
    object["os"] = core::Json(os);
    object["architecture"] = core::Json(architecture);
    object["layers"] = snapshot::LayerRefsToJson(layers);
    object["config"] = config;
    return core::Json(object);
}

core::Expected<PublishedImage, std::string> PublishedImage::FromJson(const core::Json& value) {
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(
            std::string("invalid type: expected struct PublishedImage"));
    }
    const core::JsonObject& fields = value.as_object();
    // `deny_unknown_fields`: checked first, as serde reports the unknown key
    // when it meets it, before noticing anything missing.
    for (core::JsonObject::const_iterator it = fields.begin(); it != fields.end(); ++it) {
        bool known = false;
        for (std::size_t i = 0; i < kFieldCount && !known; ++i) known = it->first == kFields[i];
        if (!known) {
            return core::make_unexpected("unknown field `" + it->first + "`, expected one of " +
                                         ExpectedFieldList());
        }
    }
    for (std::size_t i = 0; i < kFieldCount; ++i) {
        if (fields.find(kFields[i]) == fields.end()) {
            return core::make_unexpected(std::string("missing field `") + kFields[i] + "`");
        }
    }

    PublishedImage out;
    const core::Json& schema = fields.find("schemaVersion")->second;
    if (schema.kind() != core::Json::Kind::Int || schema.as_int() < 0 ||
        schema.as_int() > static_cast<int64_t>(0xFFFFFFFFu)) {
        return core::make_unexpected(std::string("invalid type: field `schemaVersion`, expected u32"));
    }
    out.schema_version = static_cast<uint32_t>(schema.as_int());

    const core::Json& os = fields.find("os")->second;
    if (os.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("invalid type: field `os`, expected a string"));
    }
    out.os = os.as_string();

    const core::Json& architecture = fields.find("architecture")->second;
    if (architecture.kind() != core::Json::Kind::String) {
        return core::make_unexpected(
            std::string("invalid type: field `architecture`, expected a string"));
    }
    out.architecture = architecture.as_string();

    const core::Expected<std::vector<snapshot::OverlaybdLayerRef>, std::string> layers =
        snapshot::LayerRefsFromJson(fields.find("layers")->second);
    if (!layers.ok()) return core::make_unexpected(layers.error());
    out.layers = layers.value();

    // `serde_json::Value`: any JSON, null included; shape is `validate`'s job.
    out.config = fields.find("config")->second;
    return out;
}

core::Expected<core::Unit, std::string> PublishedImage::Validate() const {
    core::Expected<core::Unit, std::string> check =
        Ensure(schema_version == 1, "unsupported OverlayBD image description version");
    if (!check.ok()) return check;
    check = Ensure(os == "linux", "unsupported image operating system");
    if (!check.ok()) return check;
    check = Ensure(architecture == "amd64" || architecture == "arm64",
                   "unsupported image architecture");
    if (!check.ok()) return check;
    check = Ensure(config.kind() == core::Json::Kind::Object,
                   "image runtime configuration must be an object");
    if (!check.ok()) return check;
    check = Ensure(!layers.empty() && layers.size() <= kMaxLayers, "invalid image layer count");
    if (!check.ok()) return check;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (!layers[i].is_managed()) {
            return core::make_unexpected(
                std::string("published images must own their layers in the repository"));
        }
        check = ValidateDigest(layers[i].managed.digest);
        if (!check.ok()) return check;
        check = Ensure(layers[i].managed.size > 0, "empty image layer");
        if (!check.ok()) return check;
    }
    return core::Unit();
}

core::Expected<std::pair<std::string, std::string>, std::string> PublishedImage::Encode() const {
    const core::Expected<core::Unit, std::string> valid = Validate();
    if (!valid.ok()) return core::make_unexpected(valid.error());
    const std::string bytes = ToJson().ToString();
    return std::make_pair(core::Sha256Digest(bytes), bytes);
}

core::Expected<PublishedImage, std::string> PublishedImage::Decode(const std::string& digest,
                                                                   const std::string& bytes) {
    const core::Expected<core::Unit, std::string> digest_ok = ValidateDigest(digest);
    if (!digest_ok.ok()) return core::make_unexpected(digest_ok.error());
    if (core::Sha256Digest(bytes) != digest) {
        return core::make_unexpected(std::string("OverlayBD image description digest mismatch"));
    }
    const core::Expected<core::Json, core::AnyError> value = core::Json::Parse(bytes);
    if (!value.ok()) {
        return core::make_unexpected("decode OverlayBD image description: " +
                                     value.error().chain());
    }
    const core::Expected<PublishedImage, std::string> image = FromJson(value.value());
    if (!image.ok()) {
        return core::make_unexpected("decode OverlayBD image description: " + image.error());
    }
    const core::Expected<core::Unit, std::string> valid = image.value().Validate();
    if (!valid.ok()) return core::make_unexpected(valid.error());
    return image.value();
}

}  // namespace image
}  // namespace agentenv
