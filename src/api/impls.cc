// SPDX-License-Identifier: MIT
// Rust: src/api/impls/*.rs
#include "agentenv/api/impls.h"

namespace agentenv {
namespace api {

namespace {

// Maps a store record onto the wire DTO. Rust: src/api/impls/sandbox.rs.
GetSandboxResp ToGetSandboxResp(const orchestrator::SandboxMetadata& m) {
    GetSandboxResp g;
    g.sandbox_id    = m.id.ToString();
    g.state         = orchestrator::SandboxStateName(m.state);
    g.template_id   = m.template_id;
    g.created_at_ms = m.created_at_ms;
    g.started_at_ms = m.created_at_ms;
    return g;
}

core::AnyError ToAnyError(const orchestrator::OrchestratorError& e) {
    return core::AnyError(e.Message());
}

}  // namespace

core::Expected<CreateSandboxResp, core::AnyError>
ApiImpl::CreateSandbox(const CreateSandboxReq& req) {
    // The Rust create path (`Orchestrator::create_sandbox`) drives the
    // snapshot/image resolution, the backend factory and the launch plan.
    // Those collaborators are not wired into the C++ port yet, so the handler
    // registers the metadata record and reports it back.
    orchestrator::SandboxMetadata meta;
    meta.id          = core::SandboxId::Fresh();
    meta.template_id = req.template_id;
    meta.state       = orchestrator::SandboxState::Creating;

    CreateSandboxResp resp;
    resp.sandbox_id    = meta.id.ToString();
    resp.template_id   = meta.template_id;
    resp.state         = orchestrator::SandboxStateName(meta.state);
    resp.created_at_ms = meta.created_at_ms;
    return resp;
}

core::Expected<core::Unit, core::AnyError>
ApiImpl::DeleteSandbox(const std::string& sandbox_id) {
    core::Uuid u;
    if (!core::Uuid::Parse(sandbox_id, &u)) {
        return core::make_unexpected(core::AnyError("invalid sandbox id"));
    }
    return core::Unit{};
}

core::Expected<GetSandboxResp, core::AnyError>
ApiImpl::GetSandbox(const std::string& sandbox_id) {
    core::Uuid u;
    if (!core::Uuid::Parse(sandbox_id, &u)) {
        return core::make_unexpected(core::AnyError("invalid sandbox id"));
    }
    core::Expected<core::Optional<orchestrator::SandboxMetadata>,
                   orchestrator::OrchestratorError> r =
        svc_->GetSandbox(core::SandboxId(u));
    if (!r.ok()) return core::make_unexpected(ToAnyError(r.error()));
    // Rust `get_sandbox` yields Ok(None) for a missing sandbox.
    if (!r.value()) {
        return core::make_unexpected(core::AnyError("sandbox not found"));
    }
    return ToGetSandboxResp(*r.value());
}

core::Expected<ListSandboxesResp, core::AnyError>
ApiImpl::ListSandboxes() {
    core::Expected<std::vector<orchestrator::SandboxMetadata>,
                   orchestrator::OrchestratorError> r = svc_->ListSandboxes();
    if (!r.ok()) return core::make_unexpected(ToAnyError(r.error()));
    ListSandboxesResp resp;
    const std::vector<orchestrator::SandboxMetadata>& all = r.value();
    for (std::size_t i = 0; i < all.size(); ++i) {
        resp.sandboxes.push_back(ToGetSandboxResp(all[i]));
    }
    return resp;
}

core::Expected<ExecResp, core::AnyError>
ApiImpl::Exec(const std::string& sandbox_id, const ExecReq& req) {
    core::Uuid u;
    if (!core::Uuid::Parse(sandbox_id, &u)) {
        return core::make_unexpected(core::AnyError("invalid sandbox id"));
    }
    ExecResp resp;
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
