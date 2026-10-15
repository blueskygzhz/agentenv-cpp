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
    // Rust `started_at(created_at)` — the record carries no separate start
    // instant, and the API's `started_at` is derived from `created_at` there
    // too. Reported rather than left zero, which a client reads as "never".
    g.started_at_ms = m.created_at_ms;
    return g;
}

/// Rust `ApiImpl::internal_error` — 500 with the source message.
ApiError InternalError(const std::string& message) {
    return ApiError::Make(500, message);
}

}  // namespace

ApiError SandboxNotFound(const std::string& id) {
    return ApiError::Make(404, "sandbox " + id + " not found");
}

ApiError ToApiError(const orchestrator::OrchestratorError& err) {
    switch (err.kind) {
        case orchestrator::OrchestratorErrorKind::ShuttingDown:
            // 503, not 500: the node is going away on purpose, so the client
            // should retry elsewhere rather than treat this as a defect.
            return ApiError::Make(503, "orchestrator is shutting down");
        case orchestrator::OrchestratorErrorKind::SandboxNotFound:
            return SandboxNotFound(err.sandbox_id.ToString());
        case orchestrator::OrchestratorErrorKind::InvalidSandboxState:
            // 400: the sandbox exists but the request does not apply to the
            // state it is in. Retrying verbatim will fail the same way.
            return ApiError::Make(400, err.Message());
        case orchestrator::OrchestratorErrorKind::SandboxOperationConflict:
            // 409: the same operation is already running. Unlike the 400
            // above, waiting and retrying is the right response.
            return ApiError::Make(409, err.Message());
        case orchestrator::OrchestratorErrorKind::SandboxOperationFailed:
            return ApiError::Make(500, err.Message());
        default:
            // Rust's `other => internal_error(&other)` arm.
            return InternalError(err.Message());
    }
}

core::Expected<core::SandboxId, ApiError>
ApiImpl::ParseId(const std::string& text) const {
    const core::Expected<core::SandboxId, std::string> parsed =
        core::SandboxId::Parse(text);
    if (!parsed.ok()) return core::make_unexpected(SandboxNotFound(text));
    return parsed.value();
}

core::Expected<CreateSandboxResp, ApiError>
ApiImpl::CreateSandbox(const CreateSandboxReq& req) {
    orchestrator::CreateSandboxRequest request;
    // Rust resolves `template_id` to a snapshot through the template store
    // before launching. There is no template store in this port, so the id is
    // carried straight through as the launch source's snapshot id; a caller
    // naming a template that does not exist is rejected by the launch rather
    // than silently given an empty sandbox.
    request.source.snapshot_id = req.template_id;
    if (req.timeout_ms > 0) {
        request.timeout_ms = core::Optional<int64_t>(req.timeout_ms);
    }
    for (std::size_t i = 0; i < req.env_vars.size(); ++i) {
        // `KEY=VALUE`, as the DTO carries them. A pair without `=` is dropped
        // rather than stored under an empty key, which the guest would export
        // as a nameless variable.
        const std::string& kv = req.env_vars[i];
        const std::size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        request.env_vars[kv.substr(0, eq)] = kv.substr(eq + 1);
    }

    const orchestrator::OrchestratorResult<orchestrator::SandboxMetadata> r =
        svc_->CreateSandbox(request);
    if (!r.ok()) return core::make_unexpected(ToApiError(r.error()));

    const orchestrator::SandboxMetadata& meta = r.value();
    CreateSandboxResp resp;
    resp.sandbox_id    = meta.id.ToString();
    resp.template_id   = meta.template_id;
    resp.state         = orchestrator::SandboxStateName(meta.state);
    resp.created_at_ms = meta.created_at_ms;
    return resp;
}

core::Expected<core::Unit, ApiError>
ApiImpl::DeleteSandbox(const std::string& sandbox_id) {
    const core::Expected<core::SandboxId, ApiError> id = ParseId(sandbox_id);
    if (!id.ok()) return core::make_unexpected(id.error());

    const orchestrator::OrchestratorResult<core::Unit> r =
        svc_->DeleteSandbox(id.value());
    if (!r.ok()) return core::make_unexpected(ToApiError(r.error()));
    return core::Unit{};
}

core::Expected<GetSandboxResp, ApiError>
ApiImpl::GetSandbox(const std::string& sandbox_id) {
    const core::Expected<core::SandboxId, ApiError> id = ParseId(sandbox_id);
    if (!id.ok()) return core::make_unexpected(id.error());

    const orchestrator::OrchestratorResult<
        core::Optional<orchestrator::SandboxMetadata> > r =
        svc_->GetSandbox(id.value());
    if (!r.ok()) return core::make_unexpected(ToApiError(r.error()));
    // Rust `get_sandbox` yields Ok(None) for a missing sandbox, which the
    // handler turns into the same 404 a bad id gets.
    if (!r.value()) return core::make_unexpected(SandboxNotFound(sandbox_id));
    return ToGetSandboxResp(*r.value());
}

core::Expected<ListSandboxesResp, ApiError>
ApiImpl::ListSandboxes() {
    const orchestrator::OrchestratorResult<
        std::vector<orchestrator::SandboxMetadata> > r = svc_->ListSandboxes();
    if (!r.ok()) return core::make_unexpected(ToApiError(r.error()));

    ListSandboxesResp resp;
    const std::vector<orchestrator::SandboxMetadata>& all = r.value();
    for (std::size_t i = 0; i < all.size(); ++i) {
        // Rust excludes server-owned template builders from the public list.
        // They are real sandboxes in the store, so without this a client sees
        // machines it never created and cannot act on.
        if (all[i].template_builder) continue;
        resp.sandboxes.push_back(ToGetSandboxResp(all[i]));
    }
    return resp;
}

core::Expected<core::Unit, ApiError>
ApiImpl::PauseSandbox(const std::string& sandbox_id) {
    const core::Expected<core::SandboxId, ApiError> id = ParseId(sandbox_id);
    if (!id.ok()) return core::make_unexpected(id.error());

    const orchestrator::OrchestratorResult<core::Unit> r =
        svc_->PauseSandbox(id.value());
    if (!r.ok()) return core::make_unexpected(ToApiError(r.error()));
    return core::Unit{};
}

core::Expected<GetSandboxResp, ApiError>
ApiImpl::ResumeSandbox(const std::string& sandbox_id) {
    const core::Expected<core::SandboxId, ApiError> id = ParseId(sandbox_id);
    if (!id.ok()) return core::make_unexpected(id.error());

    // `UseExisting`: a resume with no timeout in the request keeps whatever
    // the sandbox was created with. Defaulting to `None` here would silently
    // turn a sandbox that expires into one that never does.
    const orchestrator::OrchestratorResult<orchestrator::SandboxMetadata> r =
        svc_->ResumeSandbox(id.value(), orchestrator::NewTimeout::UseExisting());
    if (!r.ok()) return core::make_unexpected(ToApiError(r.error()));
    return ToGetSandboxResp(r.value());
}

core::Expected<ExecResp, ApiError>
ApiImpl::Exec(const std::string& sandbox_id, const ExecReq& /*req*/) {
    const core::Expected<core::SandboxId, ApiError> id = ParseId(sandbox_id);
    if (!id.ok()) return core::make_unexpected(id.error());

    // Rust proxies this to envd inside the guest. There is no envd client in
    // this port, so there is no way to run the command.
    //
    // Reported as 501 rather than an empty success: an exit code of 0 with no
    // output is indistinguishable from a command that ran and printed
    // nothing, so a caller would conclude its work had been done.
    return core::make_unexpected(ApiError::Make(
        501, "exec is not available in this build (needs the envd guest client)"));
}

core::Expected<core::Unit, ApiError>
ApiImpl::CreateSnapshot(const std::string& sandbox_id) {
    const core::Expected<core::SandboxId, ApiError> id = ParseId(sandbox_id);
    if (!id.ok()) return core::make_unexpected(id.error());

    // Rust publishes the capture into the snapshot repository and records it
    // under a new snapshot id. The repository side is not ported, so there is
    // nowhere to publish to.
    //
    // This one is the most dangerous to fake: a caller told the snapshot
    // succeeded will later launch from a snapshot id that was never written,
    // and the failure then appears on another node as a corrupt record.
    return core::make_unexpected(ApiError::Make(
        501, "snapshot publication is not available in this build "
             "(needs the snapshot repository)"));
}

core::Expected<core::Unit, ApiError>
ApiImpl::ListTemplates() {
    // Rust reads these from the template store, which is not ported. An empty
    // list would be a wrong answer rather than a missing one: a client cannot
    // tell "this node has no templates" from "this node cannot tell you".
    return core::make_unexpected(ApiError::Make(
        501, "template listing is not available in this build "
             "(needs the template store)"));
}

}  // namespace api
}  // namespace agentenv
