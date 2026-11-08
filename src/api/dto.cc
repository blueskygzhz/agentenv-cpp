// SPDX-License-Identifier: MIT
#include "agentenv/api/dto.h"

// `SnapshotInfoResp` reports its timestamps as RFC3339, as upstream's
// `DateTime<Utc>` does.
#include "agentenv/core/time.h"

namespace agentenv {
namespace api {

// A single boilerplate factored so every DTO is one to_json + one from_json.
static core::Expected<std::string, core::AnyError>
    GetString(const core::Json& j, const std::string& k) {
    const auto& v = j[k];
    if (v.kind() != core::Json::Kind::String)
        return core::make_unexpected(core::err("missing field: " + k));
    return v.as_string();
}
static int64_t GetIntOr(const core::Json& j, const std::string& k, int64_t d) {
    const auto& v = j[k];
    return v.kind() == core::Json::Kind::Int ? v.as_int() : d;
}

/// Rust `Option<Vec<models::SandboxVolumeMount>>`, shared by `NewSandbox` and
/// `NewColdSandbox` because the schema shares it. Factored out when the cold
/// body arrived: two copies of a parser whose whole point is the
/// absent-versus-empty distinction is how the two stop agreeing about it.
///
/// Returns an unset optional for an absent key and an error for a present
/// value of the wrong shape -- never an empty vector for either, since that
/// is the one meaning the caller must be able to state explicitly.
static core::Expected<core::Optional<std::vector<SandboxVolumeMountModel> >, core::AnyError>
    ParseVolumeMounts(const core::Json& j) {
    const auto& mounts = j["volumeMounts"];
    if (mounts.kind() == core::Json::Kind::Null) {
        return core::Optional<std::vector<SandboxVolumeMountModel> >();
    }
    if (mounts.kind() != core::Json::Kind::Array) {
        return core::make_unexpected(
            core::err("invalid field: volumeMounts must be an array"));
    }
    std::vector<SandboxVolumeMountModel> parsed;
    for (const auto& e : mounts.as_array()) {
        if (e.kind() != core::Json::Kind::Object) {
            return core::make_unexpected(
                core::err("invalid field: volumeMounts entries must be objects"));
        }
        // Both halves are non-optional `String` upstream, so a mount missing
        // either is a body that fails to deserialize -- not a mount at the
        // empty path, which would then be normalized into something the
        // caller never asked for.
        auto name = GetString(e, "name");
        if (!name.ok()) {
            return core::make_unexpected(
                core::err("invalid field: volumeMounts entry missing name"));
        }
        auto path = GetString(e, "path");
        if (!path.ok()) {
            return core::make_unexpected(
                core::err("invalid field: volumeMounts entry missing path"));
        }
        SandboxVolumeMountModel mount;
        mount.name = name.value();
        mount.path = path.value();
        parsed.push_back(mount);
    }
    return core::Optional<std::vector<SandboxVolumeMountModel> >(parsed);
}

static void WriteVolumeMounts(
    core::Json* j, const core::Optional<std::vector<SandboxVolumeMountModel> >& mounts) {
    if (!mounts.has_value()) return;
    core::JsonArray out;
    for (std::size_t i = 0; i < mounts->size(); ++i) {
        core::Json m;
        m["name"] = core::Json((*mounts)[i].name);
        m["path"] = core::Json((*mounts)[i].path);
        out.emplace_back(std::move(m));
    }
    (*j)["volumeMounts"] = core::Json(std::move(out));
}

/// A `uint32` resource field. Absent stays absent: `ColdStartResources` reads
/// the node's configured default for an unset cpu/memory and lets the
/// orchestrator size the disk from the resolved rootfs, so substituting a
/// zero here would turn "omitted" into "explicitly zero" and be rejected.
static core::Expected<core::Optional<uint32_t>, core::AnyError>
    ParseResourceField(const core::Json& j, const std::string& key) {
    const auto& v = j[key];
    if (v.kind() == core::Json::Kind::Null) return core::Optional<uint32_t>();
    if (v.kind() != core::Json::Kind::Int || v.as_int() < 0) {
        return core::make_unexpected(
            core::err("invalid field: " + key + " must be a non-negative integer"));
    }
    return core::Optional<uint32_t>(static_cast<uint32_t>(v.as_int()));
}

/// Rust `Option<bool>`. A present non-boolean is a body that does not
/// deserialize; it must not silently read as the field's default, because for
/// `autoPause` that default is the opposite of what a caller writing
/// `"autoPause": "false"` meant.
static core::Expected<core::Optional<bool>, core::AnyError>
    ParseOptionalBool(const core::Json& j, const std::string& key) {
    const auto& v = j[key];
    if (v.kind() == core::Json::Kind::Null) return core::Optional<bool>();
    if (v.kind() != core::Json::Kind::Bool) {
        return core::make_unexpected(
            core::err("invalid field: " + key + " must be a boolean"));
    }
    return core::Optional<bool>(v.as_bool());
}

// -------- CreateSandboxReq --------
core::Json CreateSandboxReq::ToJson() const {
    core::Json j;
    j["template_id"]  = core::Json(template_id);
    j["timeout_ms"]   = core::Json(timeout_ms);
    core::JsonArray arr;
    for (const auto& e : env_vars) arr.emplace_back(core::Json(e));
    j["env_vars"] = core::Json(std::move(arr));
    if (secure) j["secure"] = core::Json(true);
    WriteVolumeMounts(&j, volume_mounts);
    return j;
}

core::Expected<CreateSandboxReq, core::AnyError>
CreateSandboxReq::FromJson(const core::Json& j) {
    CreateSandboxReq r;
    auto tid = GetString(j, "template_id");
    if (!tid.ok()) return core::make_unexpected(std::move(tid.error()));
    r.template_id = tid.value();
    r.timeout_ms = GetIntOr(j, "timeout_ms", 300000);
    const auto& envs = j["env_vars"];
    if (envs.kind() == core::Json::Kind::Array) {
        for (const auto& e : envs.as_array()) {
            if (e.kind() == core::Json::Kind::String) r.env_vars.push_back(e.as_string());
        }
    }
    // Rust `secure: Option<bool>`: a present non-boolean is a body that does
    // not deserialize, which must not quietly become "not secure".
    const auto& secure = j["secure"];
    if (secure.kind() == core::Json::Kind::Bool) {
        r.secure = secure.as_bool();
    } else if (secure.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(core::err("invalid field: secure must be a boolean"));
    }
    // Rust `volume_mounts: Option<Vec<models::SandboxVolumeMount>>`. Absent is
    // distinct from `[]` (see the field comment), which is why the parser
    // returns an optional rather than a vector.
    auto mounts = ParseVolumeMounts(j);
    if (!mounts.ok()) return core::make_unexpected(std::move(mounts.error()));
    r.volume_mounts = mounts.value();
    return r;
}

// -------- CreateColdSandboxReq --------
core::Json CreateColdSandboxReq::ToJson() const {
    core::Json j;
    j["image"] = core::Json(image);
    if (resources.cpu_count.has_value()) {
        j["cpuCount"] = core::Json(static_cast<int64_t>(*resources.cpu_count));
    }
    if (resources.memory_mb.has_value()) {
        j["memoryMB"] = core::Json(static_cast<int64_t>(*resources.memory_mb));
    }
    if (resources.disk_size_mb.has_value()) {
        j["diskSizeMB"] = core::Json(static_cast<int64_t>(*resources.disk_size_mb));
    }
    if (!env_vars.empty()) {
        core::JsonArray arr;
        for (const auto& e : env_vars) arr.emplace_back(core::Json(e));
        j["envVars"] = core::Json(std::move(arr));
    }
    if (timeout_secs.has_value()) j["timeout"] = core::Json(*timeout_secs);
    if (secure) j["secure"] = core::Json(true);
    if (auto_pause.has_value()) j["autoPause"] = core::Json(*auto_pause);
    if (allow_internet_access.has_value()) {
        j["allowInternetAccess"] = core::Json(*allow_internet_access);
    }
    if (!metadata.empty()) {
        core::Json meta;
        for (std::map<std::string, std::string>::const_iterator it = metadata.begin();
             it != metadata.end(); ++it) {
            meta[it->first] = core::Json(it->second);
        }
        j["metadata"] = std::move(meta);
    }
    WriteVolumeMounts(&j, volume_mounts);
    if (extra_boot_args.has_value()) {
        j["extraBootArgs"] = core::Json(*extra_boot_args);
    }
    return j;
}

core::Expected<CreateColdSandboxReq, core::AnyError>
CreateColdSandboxReq::FromJson(const core::Json& j) {
    CreateColdSandboxReq r;
    // The only required field. Upstream's `image: String` is not an `Option`,
    // so a body without it does not deserialize at all -- it must not become
    // a resolve of the empty reference, which the registry would answer for
    // with a confusing 404 instead of this clear 400.
    auto image = GetString(j, "image");
    if (!image.ok()) return core::make_unexpected(std::move(image.error()));
    if (image.value().empty()) {
        return core::make_unexpected(core::err("invalid field: image must not be empty"));
    }
    r.image = image.value();

    auto cpu = ParseResourceField(j, "cpuCount");
    if (!cpu.ok()) return core::make_unexpected(std::move(cpu.error()));
    r.resources.cpu_count = cpu.value();
    auto mem = ParseResourceField(j, "memoryMB");
    if (!mem.ok()) return core::make_unexpected(std::move(mem.error()));
    r.resources.memory_mb = mem.value();
    auto disk = ParseResourceField(j, "diskSizeMB");
    if (!disk.ok()) return core::make_unexpected(std::move(disk.error()));
    r.resources.disk_size_mb = disk.value();

    const auto& envs = j["envVars"];
    if (envs.kind() == core::Json::Kind::Array) {
        for (const auto& e : envs.as_array()) {
            if (e.kind() == core::Json::Kind::String) r.env_vars.push_back(e.as_string());
        }
    } else if (envs.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(core::err("invalid field: envVars must be an array"));
    }

    // Seconds, as `duration_from_secs` reads them -- not the milliseconds
    // `NewSandbox` uses. Absent means the node default, so there is no
    // sentinel: zero is a legal explicit value.
    const auto& timeout = j["timeout"];
    if (timeout.kind() == core::Json::Kind::Int) {
        if (timeout.as_int() < 0) {
            return core::make_unexpected(
                core::err("invalid field: timeout must not be negative"));
        }
        r.timeout_secs = core::Optional<int64_t>(timeout.as_int());
    } else if (timeout.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(core::err("invalid field: timeout must be an integer"));
    }

    auto secure = ParseOptionalBool(j, "secure");
    if (!secure.ok()) return core::make_unexpected(std::move(secure.error()));
    // Rust `secure: body.secure == Some(true)`.
    r.secure = secure.value().has_value() && *secure.value();

    auto auto_pause = ParseOptionalBool(j, "autoPause");
    if (!auto_pause.ok()) return core::make_unexpected(std::move(auto_pause.error()));
    r.auto_pause = auto_pause.value();

    auto internet = ParseOptionalBool(j, "allowInternetAccess");
    if (!internet.ok()) return core::make_unexpected(std::move(internet.error()));
    r.allow_internet_access = internet.value();

    const auto& meta = j["metadata"];
    if (meta.kind() == core::Json::Kind::Object) {
        for (const auto& entry : meta.as_object()) {
            if (entry.second.kind() != core::Json::Kind::String) {
                return core::make_unexpected(
                    core::err("invalid field: metadata values must be strings"));
            }
            r.metadata[entry.first] = entry.second.as_string();
        }
    } else if (meta.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(core::err("invalid field: metadata must be an object"));
    }

    auto mounts = ParseVolumeMounts(j);
    if (!mounts.ok()) return core::make_unexpected(std::move(mounts.error()));
    r.volume_mounts = mounts.value();

    const auto& boot = j["extraBootArgs"];
    if (boot.kind() == core::Json::Kind::String) {
        r.extra_boot_args = core::Optional<std::string>(boot.as_string());
    } else if (boot.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(
            core::err("invalid field: extraBootArgs must be a string"));
    }
    return r;
}

// -------- CreateSandboxResp --------
core::Json CreateSandboxResp::ToJson() const {
    core::Json j;
    j["sandbox_id"]    = core::Json(sandbox_id);
    j["template_id"]   = core::Json(template_id);
    j["state"]         = core::Json(state);
    j["created_at_ms"] = core::Json(created_at_ms);
    // E2B field names, as Rust's `models::Sandbox` serializes them.
    if (envd_access_token.has_value()) {
        j["envdAccessToken"] = core::Json(*envd_access_token);
    }
    j["trafficAccessToken"] = traffic_access_token.has_value()
                                  ? core::Json(*traffic_access_token)
                                  : core::Json();
    return j;
}
core::Expected<CreateSandboxResp, core::AnyError>
CreateSandboxResp::FromJson(const core::Json& j) {
    CreateSandboxResp r;
    auto sid = GetString(j, "sandbox_id");
    if (!sid.ok()) return core::make_unexpected(std::move(sid.error()));
    r.sandbox_id = sid.value();
    r.template_id   = j["template_id"].kind() == core::Json::Kind::String
                        ? j["template_id"].as_string() : "";
    r.state         = j["state"].kind() == core::Json::Kind::String
                        ? j["state"].as_string() : "";
    r.created_at_ms = GetIntOr(j, "created_at_ms", 0);
    if (j["envdAccessToken"].kind() == core::Json::Kind::String) {
        r.envd_access_token = core::Optional<std::string>(j["envdAccessToken"].as_string());
    }
    if (j["trafficAccessToken"].kind() == core::Json::Kind::String) {
        r.traffic_access_token =
            core::Optional<std::string>(j["trafficAccessToken"].as_string());
    }
    return r;
}

// -------- GetSandboxResp --------
core::Json GetSandboxResp::ToJson() const {
    core::Json j;
    j["sandbox_id"]    = core::Json(sandbox_id);
    j["state"]         = core::Json(state);
    j["template_id"]   = core::Json(template_id);
    j["created_at_ms"] = core::Json(created_at_ms);
    j["started_at_ms"] = core::Json(started_at_ms);
    if (envd_access_token.has_value()) {
        j["envdAccessToken"] = core::Json(*envd_access_token);
    }
    return j;
}
core::Expected<GetSandboxResp, core::AnyError>
GetSandboxResp::FromJson(const core::Json& j) {
    GetSandboxResp r;
    auto sid = GetString(j, "sandbox_id");
    if (!sid.ok()) return core::make_unexpected(std::move(sid.error()));
    r.sandbox_id = sid.value();
    r.state         = j["state"].kind() == core::Json::Kind::String
                        ? j["state"].as_string() : "";
    r.template_id   = j["template_id"].kind() == core::Json::Kind::String
                        ? j["template_id"].as_string() : "";
    r.created_at_ms = GetIntOr(j, "created_at_ms", 0);
    r.started_at_ms = GetIntOr(j, "started_at_ms", 0);
    return r;
}

// -------- ListSandboxesResp --------
core::Json ListSandboxesResp::ToJson() const {
    core::JsonArray arr;
    for (const auto& s : sandboxes) arr.emplace_back(s.ToJson());
    core::Json j;
    j["sandboxes"] = core::Json(std::move(arr));
    return j;
}
core::Expected<ListSandboxesResp, core::AnyError>
ListSandboxesResp::FromJson(const core::Json& j) {
    ListSandboxesResp r;
    if (j["sandboxes"].kind() == core::Json::Kind::Array) {
        for (const auto& e : j["sandboxes"].as_array()) {
            auto s = GetSandboxResp::FromJson(e);
            if (!s.ok()) return core::make_unexpected(std::move(s.error()));
            r.sandboxes.push_back(s.value());
        }
    }
    return r;
}

// -------- CreateSnapshotReq --------
core::Json CreateSnapshotReq::ToJson() const {
    core::Json j;
    // Omitted when absent rather than sent as null or "": upstream's `name` is
    // an `Option<String>`, and an empty string is a *malformed alias* rather
    // than "no alias", so the two must not collapse on the wire.
    if (name.has_value()) j["name"] = core::Json(*name);
    return j;
}
core::Expected<CreateSnapshotReq, core::AnyError>
CreateSnapshotReq::FromJson(const core::Json& j) {
    CreateSnapshotReq r;
    const core::Json& name = j["name"];
    if (name.kind() == core::Json::Kind::String) {
        r.name = core::Optional<std::string>(name.as_string());
    }
    return r;
}

// -------- SnapshotInfoResp --------
core::Json SnapshotInfoResp::ToJson() const {
    core::Json j;
    j["snapshotID"] = core::Json(snapshot_id);
    core::JsonArray names_json;
    for (std::size_t i = 0; i < names.size(); ++i) {
        names_json.emplace_back(core::Json(names[i]));
    }
    j["names"] = core::Json(names_json);
    j["cpuCount"]   = core::Json(static_cast<int64_t>(cpu_count));
    j["memoryMB"]   = core::Json(static_cast<int64_t>(memory_mb));
    j["diskSizeMB"] = core::Json(static_cast<int64_t>(disk_size_mb));
    // RFC3339, as upstream's `DateTime<Utc>` serializes. The record stores
    // milliseconds, so the value is widened to nanos for the formatter rather
    // than emitted as a number -- a client parsing a timestamp must not have
    // to know which of the two shapes this field is in.
    j["createdAt"] = core::Json(
        core::FormatRfc3339Nanos(core::SystemTime(created_at_unix_ms * 1000000)));
    j["updatedAt"] = core::Json(
        core::FormatRfc3339Nanos(core::SystemTime(updated_at_unix_ms * 1000000)));
    if (image_ref.has_value()) j["imageRef"] = core::Json(*image_ref);
    return j;
}
core::Expected<SnapshotInfoResp, core::AnyError>
SnapshotInfoResp::FromJson(const core::Json& j) {
    SnapshotInfoResp r;
    auto id = GetString(j, "snapshotID");
    if (!id.ok()) return core::make_unexpected(std::move(id.error()));
    r.snapshot_id = id.value();

    const core::Json& names = j["names"];
    if (names.kind() == core::Json::Kind::Array) {
        const core::JsonArray& arr = names.as_array();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (arr[i].kind() == core::Json::Kind::String) {
                r.names.push_back(arr[i].as_string());
            }
        }
    }
    r.cpu_count    = static_cast<uint32_t>(GetIntOr(j, "cpuCount", 0));
    r.memory_mb    = static_cast<uint64_t>(GetIntOr(j, "memoryMB", 0));
    r.disk_size_mb = static_cast<uint64_t>(GetIntOr(j, "diskSizeMB", 0));

    // Parsed back from RFC3339, matching what `ToJson` writes. A failure
    // leaves the field at zero rather than failing the whole parse: the
    // timestamps are informational, while `snapshotID` above is not.
    const core::Json& created = j["createdAt"];
    if (created.kind() == core::Json::Kind::String) {
        const core::Expected<core::SystemTime, std::string> t =
            core::ParseRfc3339(created.as_string());
        if (t.ok()) r.created_at_unix_ms = t.value().unix_nanos / 1000000;
    }
    const core::Json& updated = j["updatedAt"];
    if (updated.kind() == core::Json::Kind::String) {
        const core::Expected<core::SystemTime, std::string> t =
            core::ParseRfc3339(updated.as_string());
        if (t.ok()) r.updated_at_unix_ms = t.value().unix_nanos / 1000000;
    }
    const core::Json& image_ref_json = j["imageRef"];
    if (image_ref_json.kind() == core::Json::Kind::String) {
        r.image_ref = core::Optional<std::string>(image_ref_json.as_string());
    }
    return r;
}

// -------- ExecReq / ExecResp --------
core::Json ExecReq::ToJson() const {
    core::Json j;
    core::JsonArray a;
    for (const auto& x : cmd) a.emplace_back(core::Json(x));
    j["cmd"] = core::Json(std::move(a));
    core::JsonArray b;
    for (const auto& x : env_vars) b.emplace_back(core::Json(x));
    j["env_vars"] = core::Json(std::move(b));
    j["timeout_sec"] = core::Json(static_cast<int64_t>(timeout_sec));
    return j;
}
core::Expected<ExecReq, core::AnyError> ExecReq::FromJson(const core::Json& j) {
    ExecReq r;
    if (j["cmd"].kind() != core::Json::Kind::Array)
        return core::make_unexpected(core::err("missing field: cmd"));
    for (const auto& e : j["cmd"].as_array()) {
        if (e.kind() == core::Json::Kind::String) r.cmd.push_back(e.as_string());
    }
    if (j["env_vars"].kind() == core::Json::Kind::Array) {
        for (const auto& e : j["env_vars"].as_array()) {
            if (e.kind() == core::Json::Kind::String) r.env_vars.push_back(e.as_string());
        }
    }
    r.timeout_sec = static_cast<int32_t>(GetIntOr(j, "timeout_sec", 60));
    return r;
}

core::Json ExecResp::ToJson() const {
    core::Json j;
    j["exit_code"]     = core::Json(static_cast<int64_t>(exit_code));
    j["stdout_output"] = core::Json(stdout_output);
    j["stderr_output"] = core::Json(stderr_output);
    return j;
}
core::Expected<ExecResp, core::AnyError> ExecResp::FromJson(const core::Json& j) {
    ExecResp r;
    r.exit_code     = static_cast<int32_t>(GetIntOr(j, "exit_code", 0));
    r.stdout_output = j["stdout_output"].kind() == core::Json::Kind::String
                          ? j["stdout_output"].as_string() : "";
    r.stderr_output = j["stderr_output"].kind() == core::Json::Kind::String
                          ? j["stderr_output"].as_string() : "";
    return r;
}

core::Json ErrorResp::ToJson() const {
    core::Json j;
    j["code"] = core::Json(code);
    j["message"] = core::Json(message);
    return j;
}

ApiError ApiError::Make(int code, const std::string& message) {
    ApiError error;
    error.code    = code;
    error.message = message;
    return error;
}

core::Json ApiError::ToJson() const {
    core::Json j;
    j["code"]    = core::Json(static_cast<int64_t>(code));
    j["message"] = core::Json(message);
    return j;
}


// -------- ForkSandboxReq --------
//
// Both fields are omitted when unset rather than written as a sentinel: Rust
// has `skip_serializing_if = "Option::is_none"` on each, and an explicit 0
// would mean something different in both cases -- a `count` of zero is invalid
// and a `timeout` of zero is "expire immediately", not "inherit".
core::Json ForkSandboxReq::ToJson() const {
    core::Json j;
    if (count.has_value()) j["count"] = core::Json(static_cast<int64_t>(*count));
    if (timeout_secs.has_value()) {
        j["timeout"] = core::Json(static_cast<int64_t>(*timeout_secs));
    }
    return j;
}
core::Expected<ForkSandboxReq, core::AnyError>
ForkSandboxReq::FromJson(const core::Json& j) {
    ForkSandboxReq r;
    // Absence is the documented default for both, so a missing field is not an
    // error. The range check on `count` belongs to the handler, which is where
    // Rust validates it and where a 400 can be returned.
    const core::Json& c = j["count"];
    if (c.kind() == core::Json::Kind::Int && c.as_int() >= 0) {
        r.count = core::Optional<uint32_t>(static_cast<uint32_t>(c.as_int()));
    }
    const core::Json& t = j["timeout"];
    if (t.kind() == core::Json::Kind::Int && t.as_int() >= 0) {
        r.timeout_secs = core::Optional<uint32_t>(static_cast<uint32_t>(t.as_int()));
    }
    return r;
}

// -------- ForkSandboxResultEntry --------
core::Json ForkSandboxResultEntry::ToJson() const {
    core::Json j;
    // Exactly one side is written. Emitting both keys with one null would make
    // a caller checking `if (error)` see a present-but-empty object.
    if (sandbox.has_value()) j["sandbox"] = sandbox->ToJson();
    if (error.has_value()) j["error"] = error->ToJson();
    return j;
}

// -------- ForkSandboxResp --------
core::Json ForkSandboxResp::ToJson() const {
    // A bare array, matching Rust's `Vec<SandboxForkResult>` response body.
    core::JsonArray arr;
    for (std::size_t i = 0; i < results.size(); ++i) {
        arr.push_back(results[i].ToJson());
    }
    return core::Json(arr);
}

// -------- NewVolumeReq --------
core::Json NewVolumeReq::ToJson() const {
    core::Json j;
    j["name"] = core::Json(name);
    // Omitted when unset: upstream's optional fields carry
    // `skip_serializing_if = "Option::is_none"`, and a written-out default
    // would be indistinguishable from a caller that asked for it explicitly.
    if (mode.has_value()) j["mode"] = core::Json(*mode);
    if (size_mb.has_value()) j["sizeMb"] = core::Json(static_cast<int64_t>(*size_mb));
    if (image.has_value()) j["image"] = core::Json(*image);
    if (from_volume.has_value()) j["fromVolume"] = core::Json(*from_volume);
    return j;
}

core::Expected<NewVolumeReq, core::AnyError> NewVolumeReq::FromJson(const core::Json& j) {
    NewVolumeReq r;
    // `name` is the one required field; the rest are `Option`s upstream, so a
    // missing key is not an error. A present key of the wrong type is: silently
    // dropping a malformed `sizeMb` would create a 64 GiB volume for a caller
    // who asked for something else entirely.
    const core::Json& name_field = j["name"];
    if (name_field.kind() != core::Json::Kind::String) {
        return core::make_unexpected(core::AnyError(std::string("volume name is required")));
    }
    r.name = name_field.as_string();

    const core::Json& mode_field = j["mode"];
    if (mode_field.kind() == core::Json::Kind::String) {
        r.mode = core::Optional<std::string>(mode_field.as_string());
    } else if (mode_field.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(core::AnyError(std::string("invalid type for `mode`")));
    }

    const core::Json& size_field = j["sizeMb"];
    if (size_field.kind() == core::Json::Kind::Int) {
        if (size_field.as_int() < 0) {
            return core::make_unexpected(core::AnyError(std::string("`sizeMb` must not be negative")));
        }
        r.size_mb = core::Optional<uint64_t>(static_cast<uint64_t>(size_field.as_int()));
    } else if (size_field.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(core::AnyError(std::string("invalid type for `sizeMb`")));
    }

    const core::Json& image_field = j["image"];
    if (image_field.kind() == core::Json::Kind::String) {
        r.image = core::Optional<std::string>(image_field.as_string());
    } else if (image_field.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(core::AnyError(std::string("invalid type for `image`")));
    }

    const core::Json& from_field = j["fromVolume"];
    if (from_field.kind() == core::Json::Kind::String) {
        r.from_volume = core::Optional<std::string>(from_field.as_string());
    } else if (from_field.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(core::AnyError(std::string("invalid type for `fromVolume`")));
    }
    return r;
}

}  // namespace api
}  // namespace agentenv
