// SPDX-License-Identifier: MIT
// Rust: src/snapshot/types/value.rs + src/snapshot/types/snapshot.rs
#include "agentenv/snapshot/types.h"

#include <cctype>
#include <sstream>

#include "agentenv/shell-util/shell.h"

namespace agentenv {
namespace snapshot {

core::Expected<SnapshotAlias, std::string> SnapshotAlias::Parse(const std::string& input) {
    if (input.empty()) {
        return core::make_unexpected(std::string("snapshot alias cannot be empty"));
    }
    for (std::size_t i = 0; i < input.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                  (c >= '0' && c <= '9');
        if (!alphanumeric && c != '-' && c != '_') {
            std::ostringstream oss;
            oss << "snapshot alias only supports ASCII letters, digits, hyphens, and "
                   "underscores, got: "
                << input;
            return core::make_unexpected(oss.str());
        }
    }
    return SnapshotAlias(input);
}

std::ostream& operator<<(std::ostream& os, const SnapshotAlias& alias) {
    return os << alias.ToString();
}

bool CommandContext::operator==(const CommandContext& o) const {
    if (env_vars != o.env_vars || workdir != o.workdir || exposed_ports != o.exposed_ports ||
        volumes != o.volumes || labels != o.labels) {
        return false;
    }
    if (user.has_value() != o.user.has_value()) return false;
    if (user.has_value() && *user != *o.user) return false;
    // An absent entrypoint and an empty one are different to OCI, so the
    // presence flags are compared before the contents.
    if (entrypoint.has_value() != o.entrypoint.has_value()) return false;
    if (entrypoint.has_value() && *entrypoint != *o.entrypoint) return false;
    if (cmd.has_value() != o.cmd.has_value()) return false;
    if (cmd.has_value() && *cmd != *o.cmd) return false;
    return true;
}

TemplateBuildErrorReason TemplateBuildErrorReason::New(const std::string& message) {
    TemplateBuildErrorReason reason;
    reason.message = message;
    return reason;
}

TemplateBuildErrorReason TemplateBuildErrorReason::WithStep(const std::string& message,
                                                            const std::string& step) {
    TemplateBuildErrorReason reason;
    reason.message = message;
    reason.step = step;
    return reason;
}

bool TemplateBuildErrorReason::operator==(const TemplateBuildErrorReason& o) const {
    if (message != o.message) return false;
    if (step.has_value() != o.step.has_value()) return false;
    return !step.has_value() || *step == *o.step;
}

std::ostream& operator<<(std::ostream& os, const TemplateBuildErrorReason& reason) {
    return os << reason.ToString();
}

core::Json TemplateBuildErrorReason::ToJson() const {
    // Always the structured form; the bare-string shape is read-only legacy.
    core::JsonObject object;
    object["message"] = core::Json(message);
    if (step.has_value()) object["step"] = core::Json(*step);
    return core::Json(object);
}

core::Expected<TemplateBuildErrorReason, std::string> TemplateBuildErrorReason::FromJson(
    const core::Json& json) {
    // Rust's `#[serde(untagged)]` tries the structured variant first and
    // falls back to a bare string, which is how records written before the
    // `step` field stay readable.
    if (json.kind() == core::Json::Kind::String) {
        return TemplateBuildErrorReason::New(json.as_string());
    }
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(
            std::string("build error reason must be an object or a string"));
    }

    const core::JsonObject& fields = json.as_object();
    const core::JsonObject::const_iterator message = fields.find("message");
    if (message == fields.end() || message->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("missing field `message`"));
    }

    TemplateBuildErrorReason reason = TemplateBuildErrorReason::New(message->second.as_string());
    const core::JsonObject::const_iterator step = fields.find("step");
    if (step != fields.end() && step->second.kind() == core::Json::Kind::String) {
        reason.step = core::Optional<std::string>(step->second.as_string());
    }
    return reason;
}

// ---- CommandContext -------------------------------------------------------

std::string NormalizeWorkdir(const std::string& workdir) {
    // Rust `workdir.trim().is_empty()`: a whitespace-only value is as useless
    // as an empty one, and `cd ""` would fail at boot.
    std::size_t begin = 0;
    while (begin < workdir.size() &&
           std::isspace(static_cast<unsigned char>(workdir[begin])) != 0) {
        ++begin;
    }
    return begin == workdir.size() ? std::string("/") : workdir;
}

CommandContext CommandContext::New(const std::map<std::string, std::string>& env_vars,
                                   const std::string& workdir) {
    CommandContext context;
    context.env_vars = env_vars;
    context.workdir = NormalizeWorkdir(workdir);
    return context;
}

CommandContext CommandContext::FromEnvAndWorkdir(
    const std::map<std::string, std::string>& env_vars,
    const core::Optional<std::string>& workdir) {
    // Rust `workdir.unwrap_or_default()` yields "", which `New` normalises.
    return New(env_vars, workdir.has_value() ? *workdir : std::string());
}

CommandContext& CommandContext::WithEnvVar(const std::string& key, const std::string& value) {
    env_vars[key] = value;
    return *this;
}

CommandContext& CommandContext::WithEnvOverrides(
    const std::map<std::string, std::string>& overrides) {
    // Rust `extend`: an existing key is replaced, not kept.
    for (std::map<std::string, std::string>::const_iterator it = overrides.begin();
         it != overrides.end(); ++it) {
        env_vars[it->first] = it->second;
    }
    return *this;
}

CommandContext& CommandContext::WithWorkdir(const std::string& value) {
    workdir = NormalizeWorkdir(value);
    return *this;
}

CommandContext& CommandContext::WithUser(const core::Optional<std::string>& value) {
    user = value;
    return *this;
}

CommandContext& CommandContext::WithExposedPorts(const std::vector<std::string>& ports) {
    exposed_ports = ports;
    return *this;
}

CommandContext& CommandContext::WithEntrypoint(
    const core::Optional<std::vector<std::string> >& value) {
    entrypoint = value;
    return *this;
}

CommandContext& CommandContext::WithCmd(const core::Optional<std::vector<std::string> >& value) {
    cmd = value;
    return *this;
}

CommandContext& CommandContext::WithVolumes(const std::vector<std::string>& value) {
    volumes = value;
    return *this;
}

CommandContext& CommandContext::WithLabels(const std::map<std::string, std::string>& value) {
    labels = value;
    return *this;
}

core::Optional<std::string> CommandContext::EffectiveStartCmd() const {
    std::vector<std::string> parts;
    if (entrypoint.has_value()) {
        parts.insert(parts.end(), entrypoint->begin(), entrypoint->end());
    }
    if (cmd.has_value()) {
        parts.insert(parts.end(), cmd->begin(), cmd->end());
    }
    if (parts.empty()) return core::Optional<std::string>();

    // Each argument is quoted separately: the result goes to `bash -lc`, so
    // an unquoted space or metacharacter would split or reinterpret it.
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) joined += " ";
        joined += shellutil::ShellQuote(parts[i]);
    }
    return core::Optional<std::string>(joined);
}

core::Json CommandContext::ToJson() const {
    core::JsonObject object;

    core::JsonObject env;
    for (std::map<std::string, std::string>::const_iterator it = env_vars.begin();
         it != env_vars.end(); ++it) {
        env[it->first] = core::Json(it->second);
    }
    object["env_vars"] = core::Json(env);
    object["workdir"] = core::Json(workdir);

    // The remaining fields all carry `skip_serializing_if`, so an absent or
    // empty value is omitted rather than written as null / [].
    if (user.has_value()) object["user"] = core::Json(*user);
    if (!exposed_ports.empty()) {
        core::JsonArray ports;
        for (std::size_t i = 0; i < exposed_ports.size(); ++i) {
            ports.push_back(core::Json(exposed_ports[i]));
        }
        object["exposed_ports"] = core::Json(ports);
    }
    if (entrypoint.has_value()) {
        core::JsonArray values;
        for (std::size_t i = 0; i < entrypoint->size(); ++i) {
            values.push_back(core::Json((*entrypoint)[i]));
        }
        object["entrypoint"] = core::Json(values);
    }
    if (cmd.has_value()) {
        core::JsonArray values;
        for (std::size_t i = 0; i < cmd->size(); ++i) {
            values.push_back(core::Json((*cmd)[i]));
        }
        object["cmd"] = core::Json(values);
    }
    if (!volumes.empty()) {
        core::JsonArray values;
        for (std::size_t i = 0; i < volumes.size(); ++i) {
            values.push_back(core::Json(volumes[i]));
        }
        object["volumes"] = core::Json(values);
    }
    if (!labels.empty()) {
        core::JsonObject values;
        for (std::map<std::string, std::string>::const_iterator it = labels.begin();
             it != labels.end(); ++it) {
            values[it->first] = core::Json(it->second);
        }
        object["labels"] = core::Json(values);
    }
    return core::Json(object);
}

namespace {

core::Expected<std::vector<std::string>, std::string> StringArray(const core::Json& json,
                                                                  const std::string& field) {
    if (json.kind() != core::Json::Kind::Array) {
        return core::make_unexpected(std::string("field `") + field + "` must be an array");
    }
    std::vector<std::string> values;
    const core::JsonArray& array = json.as_array();
    for (std::size_t i = 0; i < array.size(); ++i) {
        if (array[i].kind() != core::Json::Kind::String) {
            return core::make_unexpected(std::string("field `") + field +
                                         "` must contain strings");
        }
        values.push_back(array[i].as_string());
    }
    return values;
}

core::Expected<std::map<std::string, std::string>, std::string> StringMap(
    const core::Json& json, const std::string& field) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("field `") + field + "` must be an object");
    }
    std::map<std::string, std::string> values;
    const core::JsonObject& object = json.as_object();
    for (core::JsonObject::const_iterator it = object.begin(); it != object.end(); ++it) {
        if (it->second.kind() != core::Json::Kind::String) {
            return core::make_unexpected(std::string("field `") + field +
                                         "` must contain strings");
        }
        values[it->first] = it->second.as_string();
    }
    return values;
}

}  // namespace

core::Expected<CommandContext, std::string> CommandContext::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("command context must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    CommandContext context;

    const core::JsonObject::const_iterator env = fields.find("env_vars");
    if (env == fields.end()) {
        return core::make_unexpected(std::string("missing field `env_vars`"));
    }
    const core::Expected<std::map<std::string, std::string>, std::string> parsed_env =
        StringMap(env->second, "env_vars");
    if (!parsed_env.ok()) return core::make_unexpected(parsed_env.error());
    context.env_vars = parsed_env.value();

    const core::JsonObject::const_iterator workdir = fields.find("workdir");
    if (workdir == fields.end() || workdir->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("missing field `workdir`"));
    }
    context.workdir = workdir->second.as_string();

    const core::JsonObject::const_iterator user = fields.find("user");
    if (user != fields.end() && user->second.kind() == core::Json::Kind::String) {
        context.user = core::Optional<std::string>(user->second.as_string());
    }

    const core::JsonObject::const_iterator ports = fields.find("exposed_ports");
    if (ports != fields.end()) {
        const core::Expected<std::vector<std::string>, std::string> parsed =
            StringArray(ports->second, "exposed_ports");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        context.exposed_ports = parsed.value();
    }

    const core::JsonObject::const_iterator entrypoint = fields.find("entrypoint");
    if (entrypoint != fields.end() && entrypoint->second.kind() != core::Json::Kind::Null) {
        const core::Expected<std::vector<std::string>, std::string> parsed =
            StringArray(entrypoint->second, "entrypoint");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        // Present-but-empty is meaningful, so this stays an engaged optional.
        context.entrypoint = core::Optional<std::vector<std::string> >(parsed.value());
    }

    const core::JsonObject::const_iterator cmd = fields.find("cmd");
    if (cmd != fields.end() && cmd->second.kind() != core::Json::Kind::Null) {
        const core::Expected<std::vector<std::string>, std::string> parsed =
            StringArray(cmd->second, "cmd");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        context.cmd = core::Optional<std::vector<std::string> >(parsed.value());
    }

    const core::JsonObject::const_iterator volumes = fields.find("volumes");
    if (volumes != fields.end()) {
        const core::Expected<std::vector<std::string>, std::string> parsed =
            StringArray(volumes->second, "volumes");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        context.volumes = parsed.value();
    }

    const core::JsonObject::const_iterator labels = fields.find("labels");
    if (labels != fields.end()) {
        const core::Expected<std::map<std::string, std::string>, std::string> parsed =
            StringMap(labels->second, "labels");
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        context.labels = parsed.value();
    }

    return context;
}

}  // namespace snapshot
}  // namespace agentenv
