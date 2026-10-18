// SPDX-License-Identifier: MIT
// Rust: src/api/impls/*.rs
#include "agentenv/api/impls.h"
#include <unordered_map>

#include "agentenv/api/volumes.h"

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

// Rust `sandboxes_sandbox_id_fork_post`.
core::Expected<ForkSandboxResp, ApiError>
ApiImpl::ForkSandbox(const std::string& sandbox_id, const ForkSandboxReq& req) {
    const core::Expected<core::SandboxId, ApiError> id = ParseId(sandbox_id);
    if (!id.ok()) return core::make_unexpected(id.error());

    // Fetched before validating `count` so a missing sandbox is a 404 rather
    // than a 400 about the body, matching Rust's ordering. The source is also
    // needed to see whether it has volume mounts.
    const orchestrator::OrchestratorResult<
        core::Optional<orchestrator::SandboxMetadata> > source =
        svc_->GetSandbox(id.value());
    if (!source.ok()) return core::make_unexpected(ToApiError(source.error()));
    if (!source.value()) return core::make_unexpected(SandboxNotFound(sandbox_id));

    // Rust: `body.count.unwrap_or(1)` then `(1..=100).contains`.
    const uint32_t count = req.count.has_value() ? *req.count : 1;
    if (count < 1 || count > 100) {
        return core::make_unexpected(
            ApiError::Make(400, "count must be between 1 and 100"));
    }

    const bool has_volumes = !source.value()->volume_mounts.empty();

    // Refused rather than forked without them. Each of the source's exclusive
    // volumes has to be cloned per child, and with no manager there is nothing
    // to clone with -- proceeding would hand parent and child the same volume
    // id, which is two live VMs writing one image.
    if (has_volumes && !volumes_) {
        return core::make_unexpected(ApiError::Make(
            501, "forking a sandbox with volume mounts requires a volume manager"));
    }

    ForkVolumeChildren child_volume_ids;
    std::vector<orchestrator::SandboxForkChildSpec> child_specs;

    if (has_volumes) {
        // The children resume from the parent's capture, so the parent's
        // volume state has to be on disk before that capture is taken --
        // otherwise a child reads a volume mid-write.
        const orchestrator::OrchestratorResult<core::Unit> snapshotted =
            svc_->SnapshotVolumeMounts(id.value());
        if (!snapshotted.ok()) {
            return core::make_unexpected(ToApiError(snapshotted.error()));
        }

        // The clones below are made from the parent's *committed* layers, so a
        // node whose local backing is stale has to catch up first. Skipping
        // this would clone from a backing that no longer matches the record.
        std::vector<std::string> source_volume_ids;
        for (std::unordered_map<std::string, std::string>::const_iterator it =
                 source.value()->volume_mounts.begin();
             it != source.value()->volume_mounts.end(); ++it) {
            source_volume_ids.push_back(it->second);
        }
        const core::Expected<core::Unit, volume::VolumeError> recovered =
            volumes_->RecoverBackings(sandbox_id, source_volume_ids);
        if (!recovered.ok()) {
            return core::make_unexpected(ApiError::Make(
                500, std::string("failed to recover local volume backing before fork: ") +
                         recovered.error().ToString()));
        }

        const core::Expected<PreparedVolumeForkSpecs, VolumeApiError> prepared =
            PrepareVolumeForkSpecs(volumes_.get(), *source.value(), count);
        if (!prepared.ok()) return core::make_unexpected(prepared.error());
        child_specs = prepared.value().specs;
        child_volume_ids = prepared.value().children;
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            orchestrator::SandboxForkChildSpec spec;
            spec.sandbox_id = core::SandboxId::Fresh();
            child_specs.push_back(spec);
        }
    }

    // Seconds on the wire, milliseconds internally. An absent timeout inherits
    // the source's, which is why this is not defaulted to a number.
    const orchestrator::NewTimeout timeout =
        req.timeout_secs.has_value()
            ? orchestrator::NewTimeout::Set(static_cast<int64_t>(*req.timeout_secs) * 1000)
            : orchestrator::NewTimeout::UseExisting();

    const orchestrator::OrchestratorResult<
        std::vector<orchestrator::SandboxForkOutcome> > outcomes =
        svc_->ForkSandboxWithSpecs(id.value(), child_specs, timeout);
    if (!outcomes.ok()) {
        // The fork never ran, so every clone made for it is orphaned: no
        // sandbox exists to hold the lease or ever release it.
        CleanupForkVolumeChildren(volumes_.get(), child_volume_ids);
        // A failure of the fork itself, not of a child.
        //
        // `InvalidSandboxState` is overridden to 409 here. The generic mapping
        // gives it 400, which is right for a request that cannot ever apply --
        // but a sandbox that is merely paused or still creating will be
        // forkable shortly, and 400 tells the client to stop trying. Rust
        // makes the same exception for this one handler, with this wording.
        if (outcomes.error().kind ==
            orchestrator::OrchestratorErrorKind::InvalidSandboxState) {
            return core::make_unexpected(ApiError::Make(
                409, std::string("sandbox cannot be forked from ") +
                         orchestrator::SandboxStateName(outcomes.error().state) +
                         " state"));
        }
        return core::make_unexpected(ToApiError(outcomes.error()));
    }

    // Only the children that failed have their clones removed. A successful
    // child owns its clone now and will release it when it is deleted, so
    // tidying those up too would pull the volume out from under a running
    // sandbox.
    if (!child_volume_ids.empty()) {
        ForkVolumeChildren failed = child_volume_ids;
        for (std::size_t i = 0; i < outcomes.value().size(); ++i) {
            if (outcomes.value()[i].ok()) {
                failed.erase(outcomes.value()[i].value().id.ToString());
            }
        }
        CleanupForkVolumeChildren(volumes_.get(), failed);
    }

    ForkSandboxResp resp;
    for (std::size_t i = 0; i < outcomes.value().size(); ++i) {
        ForkSandboxResultEntry entry;
        if (outcomes.value()[i].ok()) {
            entry.sandbox = core::Optional<GetSandboxResp>(
                ToGetSandboxResp(outcomes.value()[i].value()));
        } else {
            entry.error = core::Optional<ApiError>(
                ToApiError(outcomes.value()[i].error()));
        }
        resp.results.push_back(entry);
    }
    return resp;
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
