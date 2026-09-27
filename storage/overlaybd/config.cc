// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/config.rs
#include "agentenv/storage/overlaybd/config.h"

#include <sstream>

#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace {

/// `#[serde(default)]` on every field means a missing key keeps the default and
/// a wrong-typed key is a hard error. These helpers reproduce that split.
const core::Json* Field(const core::Json& object, const char* key) {
    if (object.kind() != core::Json::Kind::Object) return NULL;
    const core::JsonObject& members = object.as_object();
    const core::JsonObject::const_iterator found = members.find(key);
    return found == members.end() ? NULL : &found->second;
}

core::Expected<core::Unit, std::string> ReadString(const core::Json& object, const char* key,
                                                   std::string* out) {
    const core::Json* field = Field(object, key);
    if (field == NULL || field->kind() == core::Json::Kind::Null) return core::Unit();
    if (field->kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("field '") + key + "' must be a string");
    }
    *out = field->as_string();
    return core::Unit();
}

core::Expected<core::Unit, std::string> ReadUint(const core::Json& object, const char* key,
                                                 uint64_t* out) {
    const core::Json* field = Field(object, key);
    if (field == NULL || field->kind() == core::Json::Kind::Null) return core::Unit();
    if (field->kind() != core::Json::Kind::Int) {
        return core::make_unexpected(std::string("field '") + key +
                                     "' must be a non-negative integer");
    }
    const int64_t value = field->as_int();
    if (value < 0) {
        return core::make_unexpected(std::string("field '") + key +
                                     "' must be a non-negative integer");
    }
    *out = static_cast<uint64_t>(value);
    return core::Unit();
}

core::Expected<core::Unit, std::string> ReadBool(const core::Json& object, const char* key,
                                                 bool* out) {
    const core::Json* field = Field(object, key);
    if (field == NULL || field->kind() == core::Json::Kind::Null) return core::Unit();
    if (field->kind() != core::Json::Kind::Bool) {
        return core::make_unexpected(std::string("field '") + key + "' must be a boolean");
    }
    *out = field->as_bool();
    return core::Unit();
}

core::Expected<LayerConfig, std::string> ParseLayerConfig(const core::Json& object) {
    if (object.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("each entry of 'lowers' must be an object"));
    }
    LayerConfig layer;
    core::Expected<core::Unit, std::string> step = ReadString(object, "gzipIndex", &layer.gzip_index);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadString(object, "file", &layer.file);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadString(object, "targetFile", &layer.target_file);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadString(object, "dir", &layer.dir);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadString(object, "repoBlobUrl", &layer.repo_blob_url);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadString(object, "digest", &layer.digest);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadString(object, "uuid", &layer.uuid);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadString(object, "targetDigest", &layer.target_digest);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadUint(object, "size", &layer.size);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    return layer;
}

core::Expected<UpperConfig, std::string> ParseUpperConfig(const core::Json& object) {
    UpperConfig upper;
    if (object.kind() == core::Json::Kind::Null) return upper;
    if (object.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("field 'upper' must be an object"));
    }
    const core::Json* mode = Field(object, "mode");
    if (mode != NULL && mode->kind() != core::Json::Kind::Null) {
        if (mode->kind() != core::Json::Kind::String) {
            return core::make_unexpected(std::string("field 'upper.mode' must be a string"));
        }
        const core::Expected<UpperMode, std::string> parsed = UpperModeParse(mode->as_string());
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        upper.has_mode = true;
        upper.mode = parsed.value();
    }
    core::Expected<core::Unit, std::string> step = ReadString(object, "index", &upper.index);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadString(object, "data", &upper.data);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadString(object, "target", &upper.target);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadString(object, "gzipIndex", &upper.gzip_index);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    return upper;
}

}  // namespace

core::Expected<bool, std::string> ValidateUpperConfig(const UpperConfig& upper) {
    const bool has_upper_data = !upper.data.empty();
    const bool has_upper_index = !upper.index.empty();
    if (has_upper_data != has_upper_index) {
        return core::make_unexpected(
            std::string("upper config must set both data and index, or neither"));
    }
    return has_upper_data;
}

const char* UpperModeToString(UpperMode mode) {
    // Rust `#[serde(rename_all = "camelCase")]`.
    switch (mode) {
        case UpperMode::Sparse:
            return "sparse";
        case UpperMode::LogStructured:
            return "logStructured";
        case UpperMode::HybridLogStructured:
            return "hybridLogStructured";
    }
    return "logStructured";
}

core::Expected<UpperMode, std::string> UpperModeParse(const std::string& raw) {
    if (raw == "sparse") return UpperMode::Sparse;
    if (raw == "logStructured") return UpperMode::LogStructured;
    if (raw == "hybridLogStructured") return UpperMode::HybridLogStructured;
    return core::make_unexpected(std::string("unsupported upper mode \"") + raw + "\"");
}

bool ImageConfig::operator==(const ImageConfig& other) const {
    if (repo_blob_url != other.repo_blob_url || result_file != other.result_file ||
        acceleration_layer != other.acceleration_layer ||
        record_trace_path != other.record_trace_path) {
        return false;
    }
    if (upper.has_mode != other.upper.has_mode) return false;
    if (upper.has_mode && upper.mode != other.upper.mode) return false;
    if (upper.index != other.upper.index || upper.data != other.upper.data ||
        upper.target != other.upper.target || upper.gzip_index != other.upper.gzip_index) {
        return false;
    }
    if (lowers.size() != other.lowers.size()) return false;
    for (std::size_t i = 0; i < lowers.size(); ++i) {
        const LayerConfig& a = lowers[i];
        const LayerConfig& b = other.lowers[i];
        if (a.gzip_index != b.gzip_index || a.file != b.file || a.target_file != b.target_file ||
            a.dir != b.dir || a.repo_blob_url != b.repo_blob_url || a.digest != b.digest ||
            a.uuid != b.uuid || a.target_digest != b.target_digest || a.size != b.size) {
            return false;
        }
    }
    return true;
}

std::string ImageConfigToJson(const ImageConfig& config) {
    core::JsonObject root;
    // `repoBlobUrl` has no `skip_serializing_if` on ImageConfig, but `lowers[].`
    // does; see below.
    root["repoBlobUrl"] = core::Json(config.repo_blob_url);

    core::JsonArray lowers;
    for (std::size_t i = 0; i < config.lowers.size(); ++i) {
        const LayerConfig& layer = config.lowers[i];
        core::JsonObject entry;
        entry["gzipIndex"] = core::Json(layer.gzip_index);
        entry["file"] = core::Json(layer.file);
        entry["targetFile"] = core::Json(layer.target_file);
        entry["dir"] = core::Json(layer.dir);
        // Rust: `#[serde(skip_serializing_if = "String::is_empty")]`.
        if (!layer.repo_blob_url.empty()) {
            entry["repoBlobUrl"] = core::Json(layer.repo_blob_url);
        }
        entry["digest"] = core::Json(layer.digest);
        entry["uuid"] = core::Json(layer.uuid);
        entry["targetDigest"] = core::Json(layer.target_digest);
        entry["size"] = core::Json(static_cast<int64_t>(layer.size));
        lowers.push_back(core::Json(entry));
    }
    root["lowers"] = core::Json(lowers);

    core::JsonObject upper;
    // Rust: `#[serde(skip_serializing_if = "Option::is_none")]`.
    if (config.upper.has_mode) {
        upper["mode"] = core::Json(std::string(UpperModeToString(config.upper.mode)));
    }
    upper["index"] = core::Json(config.upper.index);
    upper["data"] = core::Json(config.upper.data);
    upper["target"] = core::Json(config.upper.target);
    upper["gzipIndex"] = core::Json(config.upper.gzip_index);
    root["upper"] = core::Json(upper);

    root["resultFile"] = core::Json(config.result_file);
    root["accelerationLayer"] = core::Json(config.acceleration_layer);
    root["recordTracePath"] = core::Json(config.record_trace_path);

    return core::Json(root).ToString();
}

core::Expected<ImageConfig, std::string> ParseImageConfig(const std::string& text) {
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(text);
    if (!parsed.ok()) {
        return core::make_unexpected(std::string("invalid image config JSON: ") +
                                     parsed.error().chain());
    }
    const core::Json& root = parsed.value();
    if (root.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("image config must be a JSON object"));
    }

    ImageConfig config;
    core::Expected<core::Unit, std::string> step =
        ReadString(root, "repoBlobUrl", &config.repo_blob_url);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadString(root, "resultFile", &config.result_file);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadString(root, "recordTracePath", &config.record_trace_path);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    step = ReadBool(root, "accelerationLayer", &config.acceleration_layer);
    if (!step.ok()) return core::make_unexpected(step.take_error());

    const core::Json* lowers = Field(root, "lowers");
    if (lowers != NULL && lowers->kind() != core::Json::Kind::Null) {
        if (lowers->kind() != core::Json::Kind::Array) {
            return core::make_unexpected(std::string("field 'lowers' must be an array"));
        }
        const core::JsonArray& entries = lowers->as_array();
        if (entries.size() > kMaxLayerCnt) {
            std::ostringstream oss;
            oss << "image config has " << entries.size() << " lowers, exceeding the maximum of "
                << kMaxLayerCnt;
            return core::make_unexpected(oss.str());
        }
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const core::Expected<LayerConfig, std::string> layer = ParseLayerConfig(entries[i]);
            if (!layer.ok()) return core::make_unexpected(layer.error());
            config.lowers.push_back(layer.value());
        }
    }

    const core::Json* upper = Field(root, "upper");
    if (upper != NULL) {
        const core::Expected<UpperConfig, std::string> parsed_upper = ParseUpperConfig(*upper);
        if (!parsed_upper.ok()) return core::make_unexpected(parsed_upper.error());
        config.upper = parsed_upper.value();
    }

    return config;
}

core::Expected<ImageConfig, std::string> LoadImageConfig(const std::string& path) {
    const core::Expected<std::string, std::string> text = core::fs::ReadToString(path);
    if (!text.ok()) return core::make_unexpected(text.error());
    return ParseImageConfig(text.value());
}

core::Expected<bool, std::string> SaveImageConfig(const std::string& path,
                                                  const ImageConfig& config) {
    const core::Optional<std::string> parent = core::fs::Parent(path);
    if (parent.has_value()) {
        const core::Expected<core::Unit, std::string> created = core::fs::CreateDirAll(*parent);
        if (!created.ok()) return core::make_unexpected(created.error());
    }
    const core::Expected<core::Unit, std::string> written =
        core::fs::Write(path, ImageConfigToJson(config));
    if (!written.ok()) return core::make_unexpected(written.error());
    return true;
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
