// SPDX-License-Identifier: MIT
// Rust: src/image/metadata.rs
#include "agentenv/image/metadata.h"

namespace agentenv {
namespace image {

namespace {

/// The raw member, null included. serde's `#[serde(default)]` only covers a
/// *missing* key: an explicit `null` deserializes to `None` for an `Option`
/// field but is an "invalid type: null" error for a `Vec`, `HashMap` or
/// nested struct. Callers pick which of the two rules applies.
const core::Json* RawField(const core::Json& object, const std::string& key) {
    if (object.kind() != core::Json::Kind::Object) return 0;
    const core::JsonObject& map = object.as_object();
    const core::JsonObject::const_iterator found = map.find(key);
    if (found == map.end()) return 0;
    return &found->second;
}

/// An `Option<_>` member: null and missing are both `None`.
const core::Json* OptionField(const core::Json& object, const std::string& key) {
    const core::Json* raw = RawField(object, key);
    if (raw == 0 || raw->kind() == core::Json::Kind::Null) return 0;
    return raw;
}

core::Json StringsToJson(const std::vector<std::string>& values) {
    core::JsonArray array;
    for (std::size_t i = 0; i < values.size(); ++i) {
        array.push_back(core::Json(values[i]));
    }
    return core::Json(array);
}

core::Json MapToJson(const std::map<std::string, std::string>& values) {
    core::JsonObject object;
    for (std::map<std::string, std::string>::const_iterator it = values.begin();
         it != values.end(); ++it) {
        object.insert(std::make_pair(it->first, core::Json(it->second)));
    }
    return core::Json(object);
}

core::Expected<std::vector<std::string>, std::string> ParseStrings(const core::Json& value,
                                                                   const std::string& field) {
    std::vector<std::string> out;
    if (value.kind() != core::Json::Kind::Array) {
        return core::make_unexpected("field '" + field + "' must be an array");
    }
    const core::JsonArray& array = value.as_array();
    for (std::size_t i = 0; i < array.size(); ++i) {
        if (array[i].kind() != core::Json::Kind::String) {
            return core::make_unexpected("field '" + field + "' must contain only strings");
        }
        out.push_back(array[i].as_string());
    }
    return out;
}

core::Expected<std::map<std::string, std::string>, std::string> ParseMap(const core::Json& value,
                                                                         const std::string& field) {
    std::map<std::string, std::string> out;
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected("field '" + field + "' must be an object");
    }
    const core::JsonObject& object = value.as_object();
    for (core::JsonObject::const_iterator it = object.begin(); it != object.end(); ++it) {
        if (it->second.kind() != core::Json::Kind::String) {
            return core::make_unexpected("field '" + field + "' must map to strings");
        }
        out.insert(std::make_pair(it->first, it->second.as_string()));
    }
    return out;
}

}  // namespace

bool ImageBaseContext::operator==(const ImageBaseContext& other) const {
    if (env_vars != other.env_vars) return false;
    if (workdir.has_value() != other.workdir.has_value()) return false;
    if (workdir.has_value() && *workdir != *other.workdir) return false;
    if (user.has_value() != other.user.has_value()) return false;
    if (user.has_value() && *user != *other.user) return false;
    if (exposed_ports != other.exposed_ports) return false;
    if (entrypoint.has_value() != other.entrypoint.has_value()) return false;
    if (entrypoint.has_value() && *entrypoint != *other.entrypoint) return false;
    if (cmd.has_value() != other.cmd.has_value()) return false;
    if (cmd.has_value() && *cmd != *other.cmd) return false;
    if (volumes != other.volumes) return false;
    return labels == other.labels;
}

bool ImageResolutionMetadata::operator==(const ImageResolutionMetadata& other) const {
    if (base_context != other.base_context) return false;
    if (raw_config.has_value() != other.raw_config.has_value()) return false;
    if (!raw_config.has_value()) return true;
    // Compared by serialized form: `core::Json` has no `operator==`, and the
    // document is only ever used as an opaque pass-through, so textual
    // identity is the right notion of equality for it.
    return raw_config->ToString() == other.raw_config->ToString();
}

core::Optional<std::string> NormalizeOptionalString(const core::Optional<std::string>& value) {
    if (!value.has_value()) return core::Optional<std::string>();
    const std::string& raw = *value;
    // Rust `str::trim` over the ASCII range (`char::is_whitespace` includes
    // vertical tab and form feed).
    const char* const kSpace = " \t\n\r\v\f";
    const std::size_t begin = raw.find_first_not_of(kSpace);
    if (begin == std::string::npos) return core::Optional<std::string>();
    const std::size_t end = raw.find_last_not_of(kSpace);
    const std::string trimmed = raw.substr(begin, end - begin + 1);
    if (trimmed.empty()) return core::Optional<std::string>();
    return core::Optional<std::string>(trimmed);
}

ImageBaseContext MakeImageBaseContext(const std::map<std::string, std::string>& env_vars,
                                      const core::Optional<std::string>& workdir,
                                      const core::Optional<std::string>& user,
                                      const std::vector<std::string>& exposed_ports,
                                      const core::Optional<std::vector<std::string> >& entrypoint,
                                      const core::Optional<std::vector<std::string> >& cmd,
                                      const std::vector<std::string>& volumes,
                                      const std::map<std::string, std::string>& labels) {
    ImageBaseContext context;
    context.env_vars = env_vars;
    // Only these two are normalized. `entrypoint`/`cmd` are argv arrays where
    // a whitespace argument can be meaningful, and `labels` values are opaque.
    context.workdir = NormalizeOptionalString(workdir);
    context.user = NormalizeOptionalString(user);
    context.exposed_ports = exposed_ports;
    context.entrypoint = entrypoint;
    context.cmd = cmd;
    context.volumes = volumes;
    context.labels = labels;
    return context;
}

std::map<std::string, std::string> EnvVarsFromEntries(const std::vector<std::string>& entries) {
    std::map<std::string, std::string> out;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string& entry = entries[i];
        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        // An empty key is dropped, not inserted: `execve` rejects an
        // environment containing a nameless variable outright, so one bad
        // entry would take down every other variable with it.
        if (eq == 0) continue;
        // Assignment, not `insert`: later entries must win.
        out[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    return out;
}

// Neither struct carries `#[serde(rename_all = ...)]`, so the wire names are
// the Rust field names verbatim (snake_case). The sidecar written by
// `ImageCacheStore::write_metadata` is read back by both implementations, so a
// camelCase key here would be silently ignored by `#[serde(default)]` on the
// Rust side and the image would lose its env/workdir/user.
core::Json ImageBaseContextToJson(const ImageBaseContext& context) {
    core::JsonObject object;
    core::Json out(object);
    // `env_vars` has no `skip_serializing_if`, so it is always present --
    // including as `{}`.
    out["env_vars"] = MapToJson(context.env_vars);
    if (context.workdir.has_value()) out["workdir"] = core::Json(*context.workdir);
    if (context.user.has_value()) out["user"] = core::Json(*context.user);
    if (!context.exposed_ports.empty()) {
        out["exposed_ports"] = StringsToJson(context.exposed_ports);
    }
    if (context.entrypoint.has_value()) out["entrypoint"] = StringsToJson(*context.entrypoint);
    if (context.cmd.has_value()) out["cmd"] = StringsToJson(*context.cmd);
    if (!context.volumes.empty()) out["volumes"] = StringsToJson(context.volumes);
    if (!context.labels.empty()) out["labels"] = MapToJson(context.labels);
    return out;
}

core::Json ImageResolutionMetadataToJson(const ImageResolutionMetadata& metadata) {
    core::JsonObject object;
    core::Json out(object);
    out["base_context"] = ImageBaseContextToJson(metadata.base_context);
    if (metadata.raw_config.has_value()) out["raw_config"] = *metadata.raw_config;
    return out;
}

namespace {

/// A defaulted non-`Option` member (`#[serde(default)] Vec<String>`): missing
/// keeps the default, null is a type error.
core::Expected<core::Unit, std::string> ReadStrings(const core::Json& object,
                                                    const std::string& key,
                                                    std::vector<std::string>* out) {
    const core::Json* raw = RawField(object, key);
    if (raw == 0) return core::Unit();
    const core::Expected<std::vector<std::string>, std::string> parsed = ParseStrings(*raw, key);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());
    *out = parsed.value();
    return core::Unit();
}

core::Expected<core::Unit, std::string> ReadMap(const core::Json& object, const std::string& key,
                                                std::map<std::string, std::string>* out) {
    const core::Json* raw = RawField(object, key);
    if (raw == 0) return core::Unit();
    const core::Expected<std::map<std::string, std::string>, std::string> parsed =
        ParseMap(*raw, key);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());
    *out = parsed.value();
    return core::Unit();
}

core::Expected<core::Unit, std::string> ReadOptionalString(const core::Json& object,
                                                           const std::string& key,
                                                           core::Optional<std::string>* out) {
    const core::Json* field = OptionField(object, key);
    if (field == 0) return core::Unit();
    if (field->kind() != core::Json::Kind::String) {
        return core::make_unexpected("field '" + key + "' must be a string");
    }
    *out = core::Optional<std::string>(field->as_string());
    return core::Unit();
}

core::Expected<core::Unit, std::string> ReadOptionalStrings(
    const core::Json& object, const std::string& key,
    core::Optional<std::vector<std::string> >* out) {
    const core::Json* field = OptionField(object, key);
    if (field == 0) return core::Unit();
    const core::Expected<std::vector<std::string>, std::string> parsed = ParseStrings(*field, key);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());
    *out = core::Optional<std::vector<std::string> >(parsed.value());
    return core::Unit();
}

}  // namespace

#define AGENTENV_METADATA_TRY(expr)                                     \
    do {                                                                \
        const core::Expected<core::Unit, std::string> step_ = (expr);   \
        if (!step_.ok()) return core::make_unexpected(step_.error());   \
    } while (0)

core::Expected<ImageBaseContext, std::string> ParseImageBaseContext(const core::Json& value) {
    ImageBaseContext context;
    // A struct deserializes only from an object; serde rejects `null` here
    // even when the containing field is `#[serde(default)]`.
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("image base context must be an object"));
    }
    AGENTENV_METADATA_TRY(ReadMap(value, "env_vars", &context.env_vars));
    AGENTENV_METADATA_TRY(ReadOptionalString(value, "workdir", &context.workdir));
    AGENTENV_METADATA_TRY(ReadOptionalString(value, "user", &context.user));
    AGENTENV_METADATA_TRY(ReadStrings(value, "exposed_ports", &context.exposed_ports));
    AGENTENV_METADATA_TRY(ReadOptionalStrings(value, "entrypoint", &context.entrypoint));
    AGENTENV_METADATA_TRY(ReadOptionalStrings(value, "cmd", &context.cmd));
    AGENTENV_METADATA_TRY(ReadStrings(value, "volumes", &context.volumes));
    AGENTENV_METADATA_TRY(ReadMap(value, "labels", &context.labels));
    return context;
}

#undef AGENTENV_METADATA_TRY

core::Expected<ImageResolutionMetadata, std::string> ParseImageResolutionMetadata(
    const core::Json& value) {
    ImageResolutionMetadata metadata;
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("image resolution metadata must be an object"));
    }

    // `#[serde(default)] base_context`: a missing key is the default context;
    // a present one (null included) must parse as an object.
    if (const core::Json* base = RawField(value, "base_context")) {
        const core::Expected<ImageBaseContext, std::string> parsed = ParseImageBaseContext(*base);
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        metadata.base_context = parsed.value();
    }

    // Stored without inspection: this is the pass-through document, and
    // validating its shape here would reject configs the consumer understands
    // and this port does not. `Option<Value>` maps null to `None`.
    if (const core::Json* raw_config = OptionField(value, "raw_config")) {
        metadata.raw_config = core::Optional<core::Json>(*raw_config);
    }
    return metadata;
}

}  // namespace image
}  // namespace agentenv
