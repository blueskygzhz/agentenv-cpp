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

bool DownloadConfig::operator==(const DownloadConfig& o) const {
    return enable == o.enable && delay == o.delay && delay_extra == o.delay_extra &&
           max_mbps == o.max_mbps && try_cnt == o.try_cnt && block_size == o.block_size &&
           concurrency == o.concurrency && max_inflight_blocks == o.max_inflight_blocks &&
           max_concurrent_files == o.max_concurrent_files;
}

core::Json DownloadConfigToJson(const DownloadConfig& config) {
    core::JsonObject out;
    out["enable"] = core::Json(config.enable);
    out["delay"] = core::Json(static_cast<int64_t>(config.delay));
    out["delayExtra"] = core::Json(static_cast<int64_t>(config.delay_extra));
    // Rust `#[serde(rename = "maxMBps")]`, not the camelCase default.
    out["maxMBps"] = core::Json(static_cast<int64_t>(config.max_mbps));
    out["tryCnt"] = core::Json(static_cast<int64_t>(config.try_cnt));
    out["blockSize"] = core::Json(static_cast<int64_t>(config.block_size));
    out["concurrency"] = core::Json(static_cast<int64_t>(config.concurrency));
    out["maxInflightBlocks"] = core::Json(static_cast<int64_t>(config.max_inflight_blocks));
    out["maxConcurrentFiles"] = core::Json(static_cast<int64_t>(config.max_concurrent_files));
    return core::Json(out);
}

core::Expected<DownloadConfig, std::string> ParseDownloadConfig(const core::Json& value) {
    DownloadConfig config;  // starts at Rust's non-zero defaults
    if (value.kind() == core::Json::Kind::Null) return config;
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("field 'download' must be an object"));
    }

    core::Expected<core::Unit, std::string> step = ReadBool(value, "enable", &config.enable);
    if (!step.ok()) return core::make_unexpected(step.take_error());

    // The integer fields are read through a uint64 helper and then narrowed,
    // so a negative or oversized value is rejected rather than wrapped.
    uint64_t scratch = 0;
    struct IntField {
        const char* key;
        int32_t* target;
    };
    const IntField int_fields[] = {
        {"delay", &config.delay},
        {"delayExtra", &config.delay_extra},
        {"maxMBps", &config.max_mbps},
        {"tryCnt", &config.try_cnt},
    };
    for (std::size_t i = 0; i < sizeof(int_fields) / sizeof(int_fields[0]); ++i) {
        scratch = static_cast<uint64_t>(*int_fields[i].target);
        step = ReadUint(value, int_fields[i].key, &scratch);
        if (!step.ok()) return core::make_unexpected(step.take_error());
        if (scratch > 0x7FFFFFFFu) {
            return core::make_unexpected(std::string("field '") + int_fields[i].key +
                                         "' is out of range");
        }
        *int_fields[i].target = static_cast<int32_t>(scratch);
    }

    scratch = config.block_size;
    step = ReadUint(value, "blockSize", &scratch);
    if (!step.ok()) return core::make_unexpected(step.take_error());
    if (scratch > 0xFFFFFFFFu) {
        return core::make_unexpected(std::string("field 'blockSize' is out of range"));
    }
    config.block_size = static_cast<uint32_t>(scratch);

    struct SizeField {
        const char* key;
        std::size_t* target;
    };
    const SizeField size_fields[] = {
        {"concurrency", &config.concurrency},
        {"maxInflightBlocks", &config.max_inflight_blocks},
        {"maxConcurrentFiles", &config.max_concurrent_files},
    };
    for (std::size_t i = 0; i < sizeof(size_fields) / sizeof(size_fields[0]); ++i) {
        scratch = static_cast<uint64_t>(*size_fields[i].target);
        step = ReadUint(value, size_fields[i].key, &scratch);
        if (!step.ok()) return core::make_unexpected(step.take_error());
        *size_fields[i].target = static_cast<std::size_t>(scratch);
    }
    return config;
}

core::Expected<UpperMode, std::string> UpperModeParse(const std::string& raw) {
    if (raw == "sparse") return UpperMode::Sparse;
    if (raw == "logStructured") return UpperMode::LogStructured;
    if (raw == "hybridLogStructured") return UpperMode::HybridLogStructured;
    return core::make_unexpected(std::string("unsupported upper mode \"") + raw + "\"");
}

bool ImageConfig::operator==(const ImageConfig& other) const {
    if (repo_blob_url != other.repo_blob_url || result_file != other.result_file ||
        has_download_override != other.has_download_override ||
        (has_download_override && download_override != other.download_override) ||
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
    // Rust `#[serde(rename = "download", skip_serializing_if = "Option::is_none")]`:
    // the key is `download`, and it is omitted entirely when unset.
    if (config.has_download_override) {
        root["download"] = DownloadConfigToJson(config.download_override);
    }
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
    const core::Json* download = Field(root, "download");
    if (download != NULL && download->kind() != core::Json::Kind::Null) {
        const core::Expected<DownloadConfig, std::string> parsed_download =
            ParseDownloadConfig(*download);
        if (!parsed_download.ok()) return core::make_unexpected(parsed_download.error());
        config.has_download_override = true;
        config.download_override = parsed_download.value();
    }

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


// ---------------------------------------------------------------------------
// Image config path resolution and validation
// Rust: config.rs :: {lexically_normalize_path, resolve_image_config_local_paths,
//                     validate_image_config}
// ---------------------------------------------------------------------------

std::string LexicallyNormalizePath(const std::string& path) {
    if (path.empty()) return path;
    const bool absolute = path[0] == '/';

    std::vector<std::string> parts;
    std::string::size_type i = 0;
    while (i < path.size()) {
        std::string::size_type slash = path.find('/', i);
        if (slash == std::string::npos) slash = path.size();
        const std::string part = path.substr(i, slash - i);
        if (part == ".") {
            // Dropped, matching Rust's `Component::CurDir => {}`.
        } else if (part == "..") {
            // Rust's `normalized.pop()`: popping an empty buffer is a no-op,
            // so a relative path cannot climb above its own start and an
            // absolute one cannot climb above the root.
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty()) {
            parts.push_back(part);
        }
        i = slash == path.size() ? slash : slash + 1;
    }

    std::string out;
    if (absolute) out.push_back('/');
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k > 0) out.push_back('/');
        out += parts[k];
    }
    // A path that normalizes away entirely. Rust's `PathBuf` would be empty
    // here too, so the field is left empty rather than becoming ".".
    return out;
}

namespace {

/// Rust `resolve_path_field`.
void ResolvePathField(const std::string& base_dir, std::string* value) {
    if (value->empty()) return;
    // Not a path. A URL's `//` would otherwise be mangled by normalization,
    // and rebasing it against a directory is meaningless.
    if (value->find("://") != std::string::npos) return;

    std::string resolved;
    if ((*value)[0] == '/') {
        // Already absolute: normalized but not rebased.
        resolved = *value;
    } else if (base_dir.empty()) {
        resolved = *value;
    } else if (base_dir[base_dir.size() - 1] == '/') {
        resolved = base_dir + *value;
    } else {
        resolved = base_dir + "/" + *value;
    }
    *value = LexicallyNormalizePath(resolved);
}

}  // namespace

void ResolveImageConfigLocalPaths(const std::string& image_config_path, ImageConfig* cfg) {
    if (cfg == 0) return;

    // Rust's `parent().unwrap_or(Path::new("."))`: a bare filename has no
    // parent, and its siblings live in the current directory.
    const core::Optional<std::string> parent = core::fs::Parent(image_config_path);
    const std::string base_dir =
        parent.has_value() && !parent->empty() ? *parent : std::string(".");

    for (size_t i = 0; i < cfg->lowers.size(); ++i) {
        LayerConfig& layer = cfg->lowers[i];
        ResolvePathField(base_dir, &layer.gzip_index);
        ResolvePathField(base_dir, &layer.file);
        ResolvePathField(base_dir, &layer.target_file);
        ResolvePathField(base_dir, &layer.dir);
    }

    ResolvePathField(base_dir, &cfg->upper.index);
    ResolvePathField(base_dir, &cfg->upper.data);
    ResolvePathField(base_dir, &cfg->upper.target);
    ResolvePathField(base_dir, &cfg->upper.gzip_index);
    ResolvePathField(base_dir, &cfg->result_file);
    ResolvePathField(base_dir, &cfg->record_trace_path);
}

namespace {
/// Forward declaration. The definition sits further down, inside the
/// anonymous namespace that holds the rest of the parser helpers, next to
/// `ValidateDownloadSchedulerCaps` -- the other half of the same rule set.
core::Expected<core::Unit, std::string> ValidateDownloadChunkKnobsImpl(
    const DownloadConfig& cfg, const char* field);
}  // namespace

// Exported so `ValidateImageConfig` and `ValidateGlobalConfig` apply the
// same per-layer rules to an image override and to the global config.
core::Expected<core::Unit, std::string> ValidateDownloadChunkKnobs(const DownloadConfig& cfg,
                                                                     const std::string& field) {
    return ValidateDownloadChunkKnobsImpl(cfg, field.c_str());
}

core::Expected<core::Unit, std::string> ValidateImageConfig(const ImageConfig& cfg) {
    if (cfg.has_download_override) {
        // Only the chunk knobs. The scheduler caps are fixed when the backend
        // is created, so validating them here would reject a value that is
        // then ignored anyway.
        const core::Expected<core::Unit, std::string> knobs =
            ValidateDownloadChunkKnobs(cfg.download_override, "download override concurrency");
        if (!knobs.ok()) return knobs;
    }

    if (cfg.lowers.size() > kMaxLayerCnt) {
        std::ostringstream o;
        o << "too many lowers: " << cfg.lowers.size() << ", max " << kMaxLayerCnt;
        return core::make_unexpected(o.str());
    }

    const core::Expected<bool, std::string> writable = ValidateUpperConfig(cfg.upper);
    if (!writable.ok()) return core::make_unexpected(writable.error());

    if (cfg.lowers.empty() && !writable.value()) {
        // Nothing to read and nothing to write: there is no device to build.
        return core::make_unexpected(
            std::string("image config has no lowers and no writable upper"));
    }

    for (size_t i = 0; i < cfg.lowers.size(); ++i) {
        const LayerConfig& layer = cfg.lowers[i];
        if (layer.file.empty() && layer.digest.empty() && layer.dir.empty()) {
            return core::make_unexpected(
                std::string("each lower must provide at least one of file/dir/digest"));
        }
        // A lower with a digest but no local file is fetched from the
        // registry, which needs a base URL to fetch it from.
        const bool is_remote = layer.file.empty() && !layer.digest.empty();
        if (is_remote && layer.EffectiveRepoBlobUrl(cfg.repo_blob_url).empty()) {
            return core::make_unexpected(
                std::string("repoBlobUrl is required for remote lower layers"));
        }
    }

    // Recording a trace rewrites the upper as it runs, and an acceleration
    // layer is itself a recorded trace being replayed; enabling both would
    // have the replay recording over its own input.
    if (cfg.acceleration_layer && !cfg.record_trace_path.empty()) {
        return core::make_unexpected(
            std::string("accelerationLayer and recordTracePath cannot be enabled together"));
    }
    return core::Unit();
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


// ---------------------------------------------------------------------------
// Rust: config.rs — GlobalConfig
// ---------------------------------------------------------------------------

const char* const kOverlaybdUserAgent = "0.1.0";

namespace {

/// `ReadUint` narrowed to u32. Separate rather than a cast at the call site:
/// the JSON value is i64, and a silent truncation would turn an out-of-range
/// cache size into a plausible small one.
core::Expected<core::Unit, std::string> ReadU32(const core::Json& object, const char* key,
                                                   uint32_t* out) {
    const core::Json* field = Field(object, key);
    if (field == NULL || field->kind() == core::Json::Kind::Null) return core::Unit();
    uint64_t wide = 0;
    const core::Expected<core::Unit, std::string> step = ReadUint(object, key, &wide);
    if (!step.ok()) return step;
    if (wide > 0xFFFFFFFFULL) {
        return core::make_unexpected(std::string("field '") + key + "' exceeds 32 bits");
    }
    *out = static_cast<uint32_t>(wide);
    return core::Unit();
}

core::Expected<core::Unit, std::string> ReadSizeT(const core::Json& object, const char* key,
                                                     std::size_t* out) {
    const core::Json* field = Field(object, key);
    if (field == NULL || field->kind() == core::Json::Kind::Null) return core::Unit();
    uint64_t wide = 0;
    const core::Expected<core::Unit, std::string> step = ReadUint(object, key, &wide);
    if (!step.ok()) return step;
    *out = static_cast<std::size_t>(wide);
    return core::Unit();
}

/// Signed, so a negative value is carried through rather than refused: Rust's
/// `log_rotate_num` and `port` are `i32`, and it is `NormalizeCompatFields`
/// that repairs a nonsensical rotate count, not the parser.
core::Expected<core::Unit, std::string> ReadI32(const core::Json& object, const char* key,
                                                   int32_t* out) {
    const core::Json* field = Field(object, key);
    if (field == NULL || field->kind() == core::Json::Kind::Null) return core::Unit();
    if (field->kind() != core::Json::Kind::Int) {
        return core::make_unexpected(std::string("field '") + key + "' must be an integer");
    }
    *out = static_cast<int32_t>(field->as_int());
    return core::Unit();
}

/// Rust's `str::trim` — only used where Rust itself trims before comparing.
std::string Trimmed(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\n' ||
            value[begin] == '\r')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\n' || value[end - 1] == '\r')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

core::Expected<P2PConfig, std::string> ParseP2PConfig(const core::Json& o) {
    P2PConfig c;
    core::Expected<core::Unit, std::string> s = ReadBool(o, "enable", &c.enable);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "address", &c.address);
    if (!s.ok()) return core::make_unexpected(s.error());
    return c;
}

core::Expected<GzipCacheConfig, std::string> ParseGzipCacheConfig(const core::Json& o) {
    GzipCacheConfig c;
    core::Expected<core::Unit, std::string> s = ReadBool(o, "enable", &c.enable);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "cacheDir", &c.cache_dir);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadU32(o, "cacheSizeGB", &c.cache_size_gb);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadU32(o, "refillSize", &c.refill_size);
    if (!s.ok()) return core::make_unexpected(s.error());
    return c;
}

core::Expected<OssConfig, std::string> ParseOssConfig(const core::Json& o) {
    OssConfig c;
    core::Expected<core::Unit, std::string> s = ReadBool(o, "enable", &c.enable);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "accessKeyId", &c.access_key_id);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "secretAccessKey", &c.secret_access_key);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "securityToken", &c.security_token);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "credentialProcess", &c.credential_process);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "defaultRegion", &c.default_region);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "defaultEndpoint", &c.default_endpoint);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "defaultAddressingStyle", &c.default_addressing_style);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadUint(o, "timeoutSecs", &c.timeout_secs);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadU32(o, "retryCount", &c.retry_count);
    if (!s.ok()) return core::make_unexpected(s.error());
    return c;
}

core::Expected<ExporterConfig, std::string> ParseExporterConfig(const core::Json& o) {
    ExporterConfig c;
    core::Expected<core::Unit, std::string> s = ReadBool(o, "enable", &c.enable);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "uriPrefix", &c.uri_prefix);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadI32(o, "port", &c.port);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadUint(o, "updateInterval", &c.update_interval);
    if (!s.ok()) return core::make_unexpected(s.error());
    return c;
}

core::Expected<CredentialConfig, std::string> ParseCredentialConfig(const core::Json& o) {
    CredentialConfig c;
    core::Expected<core::Unit, std::string> s = ReadString(o, "mode", &c.mode);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "path", &c.path);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadI32(o, "timeout", &c.timeout);
    if (!s.ok()) return core::make_unexpected(s.error());
    return c;
}

core::Expected<CacheConfig, std::string> ParseCacheConfig(const core::Json& o) {
    CacheConfig c;
    core::Expected<core::Unit, std::string> s = ReadString(o, "cacheType", &c.cache_type);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "cacheDir", &c.cache_dir);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadU32(o, "cacheSizeGB", &c.cache_size_gb);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadU32(o, "refillSize", &c.refill_size);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadUint(o, "openEvictIdleSecs", &c.open_evict_idle_secs);
    if (!s.ok()) return core::make_unexpected(s.error());
    return c;
}

core::Expected<LogConfig, std::string> ParseLogConfig(const core::Json& o) {
    LogConfig c;
    core::Expected<core::Unit, std::string> s = ReadU32(o, "logLevel", &c.log_level);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "logPath", &c.log_path);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadU32(o, "logSizeMB", &c.log_size_mb);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadI32(o, "logRotateNum", &c.log_rotate_num);
    if (!s.ok()) return core::make_unexpected(s.error());
    return c;
}

core::Expected<PrefetchConfig, std::string> ParsePrefetchConfig(const core::Json& o) {
    PrefetchConfig c;
    const core::Expected<core::Unit, std::string> s = ReadI32(o, "concurrency", &c.concurrency);
    if (!s.ok()) return core::make_unexpected(s.error());
    return c;
}

core::Expected<CertConfig, std::string> ParseCertConfig(const core::Json& o) {
    CertConfig c;
    core::Expected<core::Unit, std::string> s = ReadString(o, "certFile", &c.cert_file);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(o, "keyFile", &c.key_file);
    if (!s.ok()) return core::make_unexpected(s.error());
    return c;
}

/// Rust `validate_download_chunk_knobs` — the per-layer knobs.
///
/// File-local; `ValidateDownloadChunkKnobs` below is the exported wrapper.
core::Expected<core::Unit, std::string> ValidateDownloadChunkKnobsImpl(
    const DownloadConfig& cfg, const char* field) {
    const uint64_t kMaxBlockSize = 64ULL * 1024ULL * 1024ULL;
    const std::size_t kMaxConcurrency = 16;
    const uint64_t kMaxLayerScratchBytes = 256ULL * 1024ULL * 1024ULL;

    if (cfg.concurrency == 0) {
        std::ostringstream o;
        o << field << " must be > 0";
        return core::make_unexpected(o.str());
    }
    if (cfg.concurrency > kMaxConcurrency) {
        std::ostringstream o;
        o << "download.concurrency must be <= " << kMaxConcurrency << " (in " << field << ")";
        return core::make_unexpected(o.str());
    }
    if (cfg.block_size == 0) {
        std::ostringstream o;
        o << "download.blockSize must be > 0 (in " << field << ")";
        return core::make_unexpected(o.str());
    }
    if (static_cast<uint64_t>(cfg.block_size) > kMaxBlockSize) {
        std::ostringstream o;
        o << "download.blockSize must be <= " << kMaxBlockSize << " bytes (in " << field << ")";
        return core::make_unexpected(o.str());
    }
    // The per-layer scratch budget: one layer's background download holds
    // `blockSize * concurrency` bytes at once.
    if (static_cast<uint64_t>(cfg.block_size) * static_cast<uint64_t>(cfg.concurrency) >
        kMaxLayerScratchBytes) {
        std::ostringstream o;
        o << "download.blockSize * concurrency must be <= " << kMaxLayerScratchBytes
          << " bytes (in " << field << ")";
        return core::make_unexpected(o.str());
    }
    return core::Unit();
}

/// Rust `validate_download_scheduler_caps` — the process-wide caps.
///
/// Only checked for the global config: a per-image override may carry these
/// fields for serialization fidelity, but the scheduler is sized once at
/// backend creation and never resized.
core::Expected<core::Unit, std::string> ValidateDownloadSchedulerCaps(
    const DownloadConfig& cfg, const char* field) {
    const std::size_t kMaxInflightBlocks = 128;
    const std::size_t kMaxConcurrentFiles = 128;
    const uint64_t kMaxGlobalScratchBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

    if (cfg.max_inflight_blocks == 0) {
        std::ostringstream o;
        o << "download.maxInflightBlocks must be > 0 (in " << field << ")";
        return core::make_unexpected(o.str());
    }
    if (cfg.max_inflight_blocks > kMaxInflightBlocks) {
        std::ostringstream o;
        o << "download.maxInflightBlocks must be <= " << kMaxInflightBlocks << " (in " << field
          << ")";
        return core::make_unexpected(o.str());
    }
    if (cfg.max_concurrent_files == 0) {
        std::ostringstream o;
        o << "download.maxConcurrentFiles must be > 0 (in " << field << ")";
        return core::make_unexpected(o.str());
    }
    if (cfg.max_concurrent_files > kMaxConcurrentFiles) {
        std::ostringstream o;
        o << "download.maxConcurrentFiles must be <= " << kMaxConcurrentFiles << " (in "
          << field << ")";
        return core::make_unexpected(o.str());
    }
    // Overflow is checked, not assumed: Rust uses `checked_mul` here, and a
    // wrapped product would land back inside the budget and pass.
    if (cfg.max_inflight_blocks != 0 &&
        static_cast<uint64_t>(cfg.block_size) >
            (~static_cast<uint64_t>(0)) / static_cast<uint64_t>(cfg.max_inflight_blocks)) {
        return core::make_unexpected(std::string("download scratch budget overflow"));
    }
    const uint64_t global_scratch =
        static_cast<uint64_t>(cfg.block_size) * static_cast<uint64_t>(cfg.max_inflight_blocks);
    if (global_scratch > kMaxGlobalScratchBytes) {
        std::ostringstream o;
        o << "download.blockSize * maxInflightBlocks must be <= " << kMaxGlobalScratchBytes
          << " bytes (in " << field << ")";
        return core::make_unexpected(o.str());
    }
    return core::Unit();
}

}  // namespace

void GlobalConfig::NormalizeCompatFields() {
    // Each fold is conditional on the *new* field being unset, so a config
    // that sets both keeps the new value rather than having it overwritten.
    if (cache_config.cache_type.empty()) {
        cache_config.cache_type = cache_type.empty() ? std::string("file") : cache_type;
        cache_config.cache_dir = registry_cache_dir;
        cache_config.cache_size_gb = registry_cache_size_gb;
    }
    // Reachable only when the legacy field was empty too, which is why this
    // is a second check rather than an else.
    if (cache_config.cache_type.empty()) cache_config.cache_type = "file";

    if (credential_config.mode.empty() && !credential_file_path.empty()) {
        credential_config.mode = "file";
        credential_config.path = credential_file_path;
    }

    if (log_config.log_path.empty()) {
        log_config.log_level = log_level;
        log_config.log_path = log_path;
        // Repaired rather than refused: zero or negative means the legacy
        // block simply carried no value for them.
        if (log_config.log_size_mb == 0) log_config.log_size_mb = 10;
        if (log_config.log_rotate_num <= 0) log_config.log_rotate_num = 3;
    }

    if (user_agent.empty()) user_agent = kOverlaybdUserAgent;
}

core::Expected<core::Unit, std::string> ValidateGlobalConfig(const GlobalConfig& cfg) {
    if (cfg.io_engine > 2) {
        std::ostringstream o;
        o << "unknown ioEngine " << cfg.io_engine;
        return core::make_unexpected(o.str());
    }

    core::Expected<core::Unit, std::string> step =
        ValidateDownloadChunkKnobs(cfg.download, "download.concurrency");
    if (!step.ok()) return step;
    step = ValidateDownloadSchedulerCaps(cfg.download, "download");
    if (!step.ok()) return step;

    if (cfg.cache_config.cache_type != "file" && cfg.cache_config.cache_type != "ocf" &&
        cfg.cache_config.cache_type != "download") {
        return core::make_unexpected("unknown cache type: " + cfg.cache_config.cache_type);
    }

    // The file cache does its work on the calling thread; the threaded path
    // belongs to the other two.
    if (cfg.enable_thread && cfg.cache_config.cache_type == "file") {
        return core::make_unexpected(
            std::string("enableThread=true is invalid when cache type is file"));
    }

    // An empty mode is valid and means "no credentials"; a set one must name
    // where they come from.
    if (!cfg.credential_config.mode.empty()) {
        if (cfg.credential_config.mode != "file" && cfg.credential_config.mode != "http") {
            return core::make_unexpected("invalid credential mode " +
                                         cfg.credential_config.mode);
        }
        if (cfg.credential_config.path.empty()) {
            return core::make_unexpected(
                std::string("credentialConfig.path cannot be empty when mode is set"));
        }
    }

    if (cfg.p2p_config.enable && cfg.p2p_config.address.empty()) {
        return core::make_unexpected(
            std::string("p2pConfig.address cannot be empty when p2p is enabled"));
    }

    if (cfg.registry_fs_version != "v1" && cfg.registry_fs_version != "v2") {
        return core::make_unexpected("unsupported registryFsVersion " +
                                     cfg.registry_fs_version);
    }

    // Only when OSS is on: an unused, half-filled block is not an error.
    if (cfg.oss_config.enable) {
        if (cfg.oss_config.default_endpoint.empty()) {
            return core::make_unexpected(
                std::string("ossConfig.defaultEndpoint cannot be empty when oss is enabled"));
        }
        if (Trimmed(cfg.oss_config.default_region).empty()) {
            return core::make_unexpected(
                std::string("ossConfig.defaultRegion cannot be empty when oss is enabled"));
        }
        // Both or neither: one without the other cannot sign a request.
        if (cfg.oss_config.access_key_id.empty() != cfg.oss_config.secret_access_key.empty()) {
            return core::make_unexpected(std::string(
                "ossConfig.accessKeyId and ossConfig.secretAccessKey must be set together"));
        }
        // An external credential process and inline keys are two sources for
        // the same thing; accepting both would leave which one wins up to the
        // implementation.
        if (!Trimmed(cfg.oss_config.credential_process).empty() &&
            !(cfg.oss_config.access_key_id.empty() &&
              cfg.oss_config.secret_access_key.empty() &&
              cfg.oss_config.security_token.empty())) {
            return core::make_unexpected(
                std::string("ossConfig.credentialProcess cannot be combined with "
                            "accessKeyId/secretAccessKey/securityToken"));
        }
        // A session token augments a key pair, it does not replace one.
        if (!cfg.oss_config.security_token.empty() &&
            (cfg.oss_config.access_key_id.empty() ||
             cfg.oss_config.secret_access_key.empty())) {
            return core::make_unexpected(std::string(
                "ossConfig.securityToken requires accessKeyId and secretAccessKey"));
        }
        const std::string style = Trimmed(cfg.oss_config.default_addressing_style);
        if (!style.empty() && style != "virtual" && style != "path") {
            return core::make_unexpected(
                "ossConfig.defaultAddressingStyle must be 'virtual', 'path', or empty for "
                "auto-detection, got '" +
                cfg.oss_config.default_addressing_style + "'");
        }
    }

    return core::Unit();
}

core::Expected<GlobalConfig, std::string> ParseGlobalConfig(const std::string& text) {
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(text);
    if (!parsed.ok()) {
        return core::make_unexpected(std::string("invalid global config JSON: ") +
                                     parsed.error().chain());
    }
    const core::Json& root = parsed.value();
    if (root.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("global config must be a JSON object"));
    }

    GlobalConfig cfg;
    core::Expected<core::Unit, std::string> s =
        ReadString(root, "registryCacheDir", &cfg.registry_cache_dir);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(root, "credentialFilePath", &cfg.credential_file_path);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadU32(root, "registryCacheSizeGB", &cfg.registry_cache_size_gb);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadU32(root, "ioEngine", &cfg.io_engine);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(root, "cacheType", &cfg.cache_type);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadU32(root, "logLevel", &cfg.log_level);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(root, "logPath", &cfg.log_path);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadBool(root, "enableAudit", &cfg.enable_audit);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadBool(root, "enableThread", &cfg.enable_thread);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(root, "auditPath", &cfg.audit_path);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(root, "registryFsVersion", &cfg.registry_fs_version);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadString(root, "userAgent", &cfg.user_agent);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadSizeT(root, "nrIoRings", &cfg.nr_io_rings);
    if (!s.ok()) return core::make_unexpected(s.error());
    s = ReadSizeT(root, "remoteIoWorkers", &cfg.remote_io_workers);
    if (!s.ok()) return core::make_unexpected(s.error());

    // Each nested block keeps its defaults when the key is absent or null,
    // matching `#[serde(default)]` on the field. A present non-object is an
    // error rather than ignored: it means the config meant something by it.
    const core::Json* field = NULL;

    field = Field(root, "download");
    if (field != NULL && field->kind() != core::Json::Kind::Null) {
        const core::Expected<DownloadConfig, std::string> v = ParseDownloadConfig(*field);
        if (!v.ok()) return core::make_unexpected(v.error());
        cfg.download = v.value();
    }

    field = Field(root, "p2pConfig");
    if (field != NULL && field->kind() != core::Json::Kind::Null) {
        if (field->kind() != core::Json::Kind::Object) {
            return core::make_unexpected(std::string("field 'p2pConfig' must be an object"));
        }
        const core::Expected<P2PConfig, std::string> v = ParseP2PConfig(*field);
        if (!v.ok()) return core::make_unexpected(v.error());
        cfg.p2p_config = v.value();
    }

    field = Field(root, "exporterConfig");
    if (field != NULL && field->kind() != core::Json::Kind::Null) {
        if (field->kind() != core::Json::Kind::Object) {
            return core::make_unexpected(
                std::string("field 'exporterConfig' must be an object"));
        }
        const core::Expected<ExporterConfig, std::string> v = ParseExporterConfig(*field);
        if (!v.ok()) return core::make_unexpected(v.error());
        cfg.exporter_config = v.value();
    }

    field = Field(root, "cacheConfig");
    if (field != NULL && field->kind() != core::Json::Kind::Null) {
        if (field->kind() != core::Json::Kind::Object) {
            return core::make_unexpected(std::string("field 'cacheConfig' must be an object"));
        }
        const core::Expected<CacheConfig, std::string> v = ParseCacheConfig(*field);
        if (!v.ok()) return core::make_unexpected(v.error());
        cfg.cache_config = v.value();
    }

    field = Field(root, "gzipCacheConfig");
    if (field != NULL && field->kind() != core::Json::Kind::Null) {
        if (field->kind() != core::Json::Kind::Object) {
            return core::make_unexpected(
                std::string("field 'gzipCacheConfig' must be an object"));
        }
        const core::Expected<GzipCacheConfig, std::string> v = ParseGzipCacheConfig(*field);
        if (!v.ok()) return core::make_unexpected(v.error());
        cfg.gzip_cache_config = v.value();
    }

    field = Field(root, "ossConfig");
    if (field != NULL && field->kind() != core::Json::Kind::Null) {
        if (field->kind() != core::Json::Kind::Object) {
            return core::make_unexpected(std::string("field 'ossConfig' must be an object"));
        }
        const core::Expected<OssConfig, std::string> v = ParseOssConfig(*field);
        if (!v.ok()) return core::make_unexpected(v.error());
        cfg.oss_config = v.value();
    }

    field = Field(root, "logConfig");
    if (field != NULL && field->kind() != core::Json::Kind::Null) {
        if (field->kind() != core::Json::Kind::Object) {
            return core::make_unexpected(std::string("field 'logConfig' must be an object"));
        }
        const core::Expected<LogConfig, std::string> v = ParseLogConfig(*field);
        if (!v.ok()) return core::make_unexpected(v.error());
        cfg.log_config = v.value();
    }

    field = Field(root, "prefetchConfig");
    if (field != NULL && field->kind() != core::Json::Kind::Null) {
        if (field->kind() != core::Json::Kind::Object) {
            return core::make_unexpected(
                std::string("field 'prefetchConfig' must be an object"));
        }
        const core::Expected<PrefetchConfig, std::string> v = ParsePrefetchConfig(*field);
        if (!v.ok()) return core::make_unexpected(v.error());
        cfg.prefetch_config = v.value();
    }

    field = Field(root, "credentialConfig");
    if (field != NULL && field->kind() != core::Json::Kind::Null) {
        if (field->kind() != core::Json::Kind::Object) {
            return core::make_unexpected(
                std::string("field 'credentialConfig' must be an object"));
        }
        const core::Expected<CredentialConfig, std::string> v = ParseCredentialConfig(*field);
        if (!v.ok()) return core::make_unexpected(v.error());
        cfg.credential_config = v.value();
    }

    field = Field(root, "certConfig");
    if (field != NULL && field->kind() != core::Json::Kind::Null) {
        if (field->kind() != core::Json::Kind::Object) {
            return core::make_unexpected(std::string("field 'certConfig' must be an object"));
        }
        const core::Expected<CertConfig, std::string> v = ParseCertConfig(*field);
        if (!v.ok()) return core::make_unexpected(v.error());
        cfg.cert_config = v.value();
    }

    return cfg;
}

core::Expected<GlobalConfig, std::string> LoadGlobalConfig(const std::string& path) {
    const core::Expected<std::string, std::string> text = core::fs::ReadToString(path);
    if (!text.ok()) {
        return core::make_unexpected("reading global config file: " + text.error());
    }
    const core::Expected<GlobalConfig, std::string> parsed = ParseGlobalConfig(text.value());
    if (!parsed.ok()) return parsed;

    // Normalize before validating, not after: the legacy fold is what
    // supplies `cacheConfig.cacheType`, and validation requires it to name a
    // known cache. Validating first would reject every legacy config.
    GlobalConfig cfg = parsed.value();
    cfg.NormalizeCompatFields();
    const core::Expected<core::Unit, std::string> valid = ValidateGlobalConfig(cfg);
    if (!valid.ok()) return core::make_unexpected(valid.error());
    return cfg;
}

std::string GlobalConfigToJson(const GlobalConfig& config) {
    core::JsonObject root;
    root["registryCacheDir"] = core::Json(config.registry_cache_dir);
    root["credentialFilePath"] = core::Json(config.credential_file_path);
    // `registryCacheSizeGB`, `cacheSizeGB` and `logSizeMB` keep their explicit
    // renames: camelCase alone would spell them `...Gb` / `...Mb`.
    root["registryCacheSizeGB"] =
        core::Json(static_cast<int64_t>(config.registry_cache_size_gb));
    root["ioEngine"] = core::Json(static_cast<int64_t>(config.io_engine));
    root["cacheType"] = core::Json(config.cache_type);
    root["logLevel"] = core::Json(static_cast<int64_t>(config.log_level));
    root["logPath"] = core::Json(config.log_path);
    root["download"] = DownloadConfigToJson(config.download);
    root["enableAudit"] = core::Json(config.enable_audit);
    root["enableThread"] = core::Json(config.enable_thread);

    core::JsonObject p2p;
    p2p["enable"] = core::Json(config.p2p_config.enable);
    p2p["address"] = core::Json(config.p2p_config.address);
    root["p2pConfig"] = core::Json(p2p);

    core::JsonObject exporter;
    exporter["enable"] = core::Json(config.exporter_config.enable);
    exporter["uriPrefix"] = core::Json(config.exporter_config.uri_prefix);
    exporter["port"] = core::Json(static_cast<int64_t>(config.exporter_config.port));
    exporter["updateInterval"] =
        core::Json(static_cast<int64_t>(config.exporter_config.update_interval));
    root["exporterConfig"] = core::Json(exporter);

    root["auditPath"] = core::Json(config.audit_path);
    root["registryFsVersion"] = core::Json(config.registry_fs_version);

    core::JsonObject cache;
    cache["cacheType"] = core::Json(config.cache_config.cache_type);
    cache["cacheDir"] = core::Json(config.cache_config.cache_dir);
    cache["cacheSizeGB"] = core::Json(static_cast<int64_t>(config.cache_config.cache_size_gb));
    cache["refillSize"] = core::Json(static_cast<int64_t>(config.cache_config.refill_size));
    cache["openEvictIdleSecs"] =
        core::Json(static_cast<int64_t>(config.cache_config.open_evict_idle_secs));
    root["cacheConfig"] = core::Json(cache);

    core::JsonObject gzip;
    gzip["enable"] = core::Json(config.gzip_cache_config.enable);
    gzip["cacheDir"] = core::Json(config.gzip_cache_config.cache_dir);
    gzip["cacheSizeGB"] =
        core::Json(static_cast<int64_t>(config.gzip_cache_config.cache_size_gb));
    gzip["refillSize"] = core::Json(static_cast<int64_t>(config.gzip_cache_config.refill_size));
    root["gzipCacheConfig"] = core::Json(gzip);

    core::JsonObject oss;
    oss["enable"] = core::Json(config.oss_config.enable);
    oss["accessKeyId"] = core::Json(config.oss_config.access_key_id);
    oss["secretAccessKey"] = core::Json(config.oss_config.secret_access_key);
    oss["securityToken"] = core::Json(config.oss_config.security_token);
    oss["credentialProcess"] = core::Json(config.oss_config.credential_process);
    oss["defaultRegion"] = core::Json(config.oss_config.default_region);
    oss["defaultEndpoint"] = core::Json(config.oss_config.default_endpoint);
    oss["defaultAddressingStyle"] = core::Json(config.oss_config.default_addressing_style);
    oss["timeoutSecs"] = core::Json(static_cast<int64_t>(config.oss_config.timeout_secs));
    oss["retryCount"] = core::Json(static_cast<int64_t>(config.oss_config.retry_count));
    root["ossConfig"] = core::Json(oss);

    core::JsonObject log;
    log["logLevel"] = core::Json(static_cast<int64_t>(config.log_config.log_level));
    log["logPath"] = core::Json(config.log_config.log_path);
    log["logSizeMB"] = core::Json(static_cast<int64_t>(config.log_config.log_size_mb));
    log["logRotateNum"] = core::Json(static_cast<int64_t>(config.log_config.log_rotate_num));
    root["logConfig"] = core::Json(log);

    core::JsonObject prefetch;
    prefetch["concurrency"] =
        core::Json(static_cast<int64_t>(config.prefetch_config.concurrency));
    root["prefetchConfig"] = core::Json(prefetch);

    core::JsonObject cert;
    cert["certFile"] = core::Json(config.cert_config.cert_file);
    cert["keyFile"] = core::Json(config.cert_config.key_file);
    root["certConfig"] = core::Json(cert);

    root["userAgent"] = core::Json(config.user_agent);

    core::JsonObject cred;
    cred["mode"] = core::Json(config.credential_config.mode);
    cred["path"] = core::Json(config.credential_config.path);
    cred["timeout"] = core::Json(static_cast<int64_t>(config.credential_config.timeout));
    root["credentialConfig"] = core::Json(cred);

    root["nrIoRings"] = core::Json(static_cast<int64_t>(config.nr_io_rings));
    root["remoteIoWorkers"] = core::Json(static_cast<int64_t>(config.remote_io_workers));

    return core::Json(root).ToString();
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
