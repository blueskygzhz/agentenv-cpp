// SPDX-License-Identifier: MIT
#include "agentenv/api/dto.h"

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

// -------- CreateSandboxReq --------
core::Json CreateSandboxReq::ToJson() const {
    core::Json j;
    j["template_id"]  = core::Json(template_id);
    j["timeout_ms"]   = core::Json(timeout_ms);
    core::JsonArray arr;
    for (const auto& e : env_vars) arr.emplace_back(core::Json(e));
    j["env_vars"] = core::Json(std::move(arr));
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
    return r;
}

// -------- CreateSandboxResp --------
core::Json CreateSandboxResp::ToJson() const {
    core::Json j;
    j["sandbox_id"]    = core::Json(sandbox_id);
    j["template_id"]   = core::Json(template_id);
    j["state"]         = core::Json(state);
    j["created_at_ms"] = core::Json(created_at_ms);
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

}  // namespace api
}  // namespace agentenv
