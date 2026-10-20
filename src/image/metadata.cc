// SPDX-License-Identifier: MIT
// Rust: src/image/metadata.rs
#include "agentenv/image/metadata.h"

namespace agentenv {
namespace image {

namespace {

const core::Json* Field(const core::Json& object, const std::string& key) {
    if (object.kind() != core::Json::Kind::Object) return 0;
    const core::JsonObject& map = object.as_object();
    const core::JsonObject::const_iterator found = map.find(key);
    if (found == map.end()) return 0;
    // Null is reported as absent, matching serde's `#[serde(default)]` on an
    // `Option`: a producer that writes an explicit null means "unset", and
    // treating it as a present value of the wrong kind would reject a document
    // serde accepts.
    if (found->second.kind() == core::Json::Kind::Null) return 0;
    return &found->second;
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
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && (raw[begin] == ' ' || raw[begin] == '\t' || raw[begin] == '\n' ||
                           raw[begin] == '\r')) {
        ++begin;
    }
    while (end > begin && (raw[end - 1] == ' ' || raw[end - 1] == '\t' || raw[end - 1] == '\n' ||
                           raw[end - 1] == '\r')) {
        --end;
    }
    const std::string trimmed = raw.substr(begin, end - begin);
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

core::Json ImageBaseContextToJson(const ImageBaseContext& context) {
    core::JsonObject object;
    core::Json out(object);
    // `env_vars` has no `skip_serializing_if`, so it is always present --
    // including as `{}`.
    out["envVars"] = MapToJson(context.env_vars);
    if (context.workdir.has_value()) out["workdir"] = core::Json(*context.workdir);
    if (context.user.has_value()) out["user"] = core::Json(*context.user);
    if (!context.exposed_ports.empty()) out["exposedPorts"] = StringsToJson(context.exposed_ports);
    if (context.entrypoint.has_value()) out["entrypoint"] = StringsToJson(*context.entrypoint);
    if (context.cmd.has_value()) out["cmd"] = StringsToJson(*context.cmd);
    if (!context.volumes.empty()) out["volumes"] = StringsToJson(context.volumes);
    if (!context.labels.empty()) out["labels"] = MapToJson(context.labels);
    return out;
}

core::Json ImageResolutionMetadataToJson(const ImageResolutionMetadata& metadata) {
    core::JsonObject object;
    core::Json out(object);
    out["baseContext"] = ImageBaseContextToJson(metadata.base_context);
    if (metadata.raw_config.has_value()) out["rawConfig"] = *metadata.raw_config;
    return out;
}

core::Expected<ImageBaseContext, std::string> ParseImageBaseContext(const core::Json& value) {
    ImageBaseContext context;
    // An absent object is the default context, matching `#[serde(default)]` on
    // the containing field rather than being an error.
    if (value.kind() == core::Json::Kind::Null) return context;
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("image base context must be an object"));
    }

    if (const core::Json* env_vars = Field(value, "envVars")) {
        const core::Expected<std::map<std::string, std::string>, std::string> parsed =
            ParseMap(*env_vars, "envVars");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        context.env_vars = parsed.value();
    }
    if (const core::Json* workdir = Field(value, "workdir")) {
        if (workdir->kind() != core::Json::Kind::String) {
            return core::make_unexpected(std::string("field 'workdir' must be a string"));
        }
        context.workdir = core::Optional<std::string>(workdir->as_string());
    }
    if (const core::Json* user = Field(value, "user")) {
        if (user->kind() != core::Json::Kind::String) {
            return core::make_unexpected(std::string("field 'user' must be a string"));
        }
        context.user = core::Optional<std::string>(user->as_string());
    }
    if (const core::Json* ports = Field(value, "exposedPorts")) {
        const core::Expected<std::vector<std::string>, std::string> parsed =
            ParseStrings(*ports, "exposedPorts");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        context.exposed_ports = parsed.value();
    }
    if (const core::Json* entrypoint = Field(value, "entrypoint")) {
        const core::Expected<std::vector<std::string>, std::string> parsed =
            ParseStrings(*entrypoint, "entrypoint");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        context.entrypoint = core::Optional<std::vector<std::string> >(parsed.value());
    }
    if (const core::Json* cmd = Field(value, "cmd")) {
        const core::Expected<std::vector<std::string>, std::string> parsed =
            ParseStrings(*cmd, "cmd");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        context.cmd = core::Optional<std::vector<std::string> >(parsed.value());
    }
    if (const core::Json* volumes = Field(value, "volumes")) {
        const core::Expected<std::vector<std::string>, std::string> parsed =
            ParseStrings(*volumes, "volumes");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        context.volumes = parsed.value();
    }
    if (const core::Json* labels = Field(value, "labels")) {
        const core::Expected<std::map<std::string, std::string>, std::string> parsed =
            ParseMap(*labels, "labels");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        context.labels = parsed.value();
    }
    return context;
}

core::Expected<ImageResolutionMetadata, std::string> ParseImageResolutionMetadata(
    const core::Json& value) {
    ImageResolutionMetadata metadata;
    if (value.kind() == core::Json::Kind::Null) return metadata;
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("image resolution metadata must be an object"));
    }

    const core::Json* base = Field(value, "baseContext");
    const core::Expected<ImageBaseContext, std::string> parsed =
        ParseImageBaseContext(base != 0 ? *base : core::Json());
    if (!parsed.ok()) return core::make_unexpected(parsed.error());
    metadata.base_context = parsed.value();

    // Stored without inspection: this is the pass-through document, and
    // validating its shape here would reject configs the consumer understands
    // and this port does not.
    if (const core::Json* raw_config = Field(value, "rawConfig")) {
        metadata.raw_config = core::Optional<core::Json>(*raw_config);
    }
    return metadata;
}

}  // namespace image
}  // namespace agentenv
