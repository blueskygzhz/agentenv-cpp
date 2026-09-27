// SPDX-License-Identifier: MIT
// Rust: src/api/impls/*.rs
#include "agentenv/api/impls.h"

namespace agentenv {
namespace api {

core::Expected<CreateSandboxResp, core::AnyError>
ApiImpl::CreateSandbox(const CreateSandboxReq& req) {
    orchestrator::CreateOptions opts;
    opts.template_id = req.template_id;
    opts.env_vars    = req.env_vars;
    opts.timeout_ms  = req.timeout_ms;
    auto fut = svc_->Create(std::move(opts));
    auto r = fut.get();
    if (!r.ok()) return core::make_unexpected(std::move(r.error()));
    CreateSandboxResp resp;
    resp.sandbox_id  = r.value().id.ToString();
    resp.template_id = r.value().template_id;
    resp.state       = orchestrator::PhaseName(r.value().phase);
    resp.created_at_ms = r.value().created_at_ms;
    return resp;
}

core::Expected<core::Unit, core::AnyError>
ApiImpl::DeleteSandbox(const std::string& sandbox_id) {
    core::Uuid u;
    if (!core::Uuid::Parse(sandbox_id, &u)) {
        return core::make_unexpected(core::AnyError("invalid sandbox id"));
    }
    auto fut = svc_->Stop(core::SandboxId(u));
    auto r = fut.get();
    if (!r.ok()) return core::make_unexpected(std::move(r.error()));
    return core::Unit{};
}

core::Expected<GetSandboxResp, core::AnyError>
ApiImpl::GetSandbox(const std::string& sandbox_id) {
    core::Uuid u;
    if (!core::Uuid::Parse(sandbox_id, &u)) {
        return core::make_unexpected(core::AnyError("invalid sandbox id"));
    }
    auto r = svc_->Get(core::SandboxId(u));
    if (!r.ok()) return core::make_unexpected(std::move(r.error()));
    GetSandboxResp resp;
    resp.sandbox_id  = r.value().id.ToString();
    resp.state       = orchestrator::PhaseName(r.value().phase);
    resp.template_id = r.value().template_id;
    resp.created_at_ms = r.value().created_at_ms;
    resp.started_at_ms = r.value().started_at_ms;
    return resp;
}

core::Expected<ListSandboxesResp, core::AnyError>
ApiImpl::ListSandboxes() {
    auto r = svc_->List();
    if (!r.ok()) return core::make_unexpected(std::move(r.error()));
    ListSandboxesResp resp;
    for (const orchestrator::Sandbox& s : r.value()) {
        GetSandboxResp g;
        g.sandbox_id  = s.id.ToString();
        g.state       = orchestrator::PhaseName(s.phase);
        g.template_id = s.template_id;
        g.created_at_ms = s.created_at_ms;
        g.started_at_ms = s.started_at_ms;
        resp.sandboxes.push_back(g);
    }
    return resp;
}

core::Expected<ExecResp, core::AnyError>
ApiImpl::Exec(const std::string& sandbox_id, const ExecReq& req) {
    core::Uuid u;
    if (!core::Uuid::Parse(sandbox_id, &u)) {
        return core::make_unexpected(core::AnyError("invalid sandbox id"));
    }
    sandbox::ExecSpec spec;
    spec.cmd = req.cmd;
    spec.env_vars = req.env_vars;
    spec.timeout_sec = req.timeout_sec;
    auto fut = svc_->Exec(core::SandboxId(u), std::move(spec));
    auto r = fut.get();
    if (!r.ok()) return core::make_unexpected(std::move(r.error()));
    ExecResp resp;
    resp.exit_code = r.value().exit_code;
    resp.stdout_output = r.value().stdout_output;
    resp.stderr_output = r.value().stderr_output;
    return resp;
}

core::Expected<core::Unit, core::AnyError>
ApiImpl::CreateSnapshot(const std::string& /*sandbox_id*/) {
    // TODO: bridge to snapshot::Manager
    return core::Unit{};
}

core::Expected<core::Unit, core::AnyError>
ApiImpl::ListTemplates() {
    // TODO: bridge to template::Engine
    return core::Unit{};
}

}  // namespace api
}  // namespace agentenv
