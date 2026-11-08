// SPDX-License-Identifier: MIT
// Rust: src/api/impls/*.rs
#include "agentenv/api/impls.h"
#include <sstream>
#include <unordered_map>

#include "agentenv/api/sandbox_params.h"
#include "agentenv/api/volumes.h"
#include "agentenv/core/identity.h"

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

/// Rust struct `PreparedVolumeMounts`.
struct PreparedVolumeMounts {
    /// The temporary lease owner, empty when there was nothing to reserve.
    /// Upstream's `Option<String>`: an empty owner means `finish_volume_
    /// reservation` has nothing to re-point and returns immediately.
    std::string owner;
    std::vector<sandbox::ExtraDrive> drives;
    std::map<std::string, std::string> mounts;
    std::vector<std::string> volume_ids;
};

/// Rust `prepare_volume_mounts`.
///
/// The lease has to be taken before the sandbox exists, because resolving a
/// mount is what proves the volume is free -- but there is no sandbox id yet
/// to lease it to. A synthetic `pending-<uuid>` owner holds it for the
/// duration of the launch and is swapped for the real id afterwards by
/// `FinishVolumeReservation`. Without the placeholder, two concurrent creates
/// naming one exclusive volume would both resolve successfully.
core::Expected<PreparedVolumeMounts, ApiError> PrepareVolumeMounts(
    volume::VolumeManager* manager, const std::map<std::string, std::string>& mounts) {
    PreparedVolumeMounts prepared;
    if (mounts.empty()) return prepared;

    const std::string owner = "pending-" + core::Uuid::GenV7().ToSimpleString();
    const core::Expected<ResolvedVolumeMounts, VolumeApiError> resolved =
        ResolveVolumeMounts(manager, mounts, owner);
    if (!resolved.ok()) return core::make_unexpected(resolved.error());

    prepared.owner  = owner;
    prepared.drives = resolved.value().drives;
    prepared.mounts = resolved.value().normalized_mounts;
    for (std::map<std::string, std::string>::const_iterator it =
             prepared.mounts.begin();
         it != prepared.mounts.end(); ++it) {
        prepared.volume_ids.push_back(it->second);
    }
    return prepared;
}

/// Rust `finish_volume_reservation` — re-points the placeholder lease at the
/// real sandbox id, or releases it when `new_owner` is absent.
core::Expected<core::Unit, volume::VolumeError> FinishVolumeReservation(
    volume::VolumeManager* manager, const std::string& owner,
    const core::Optional<std::string>& new_owner,
    const std::vector<std::string>& volume_ids) {
    // No placeholder was taken, so there is nothing to hand over. Checked
    // first because `manager` is legitimately null on that path.
    if (owner.empty()) return core::Unit();
    return manager->ReplaceOwnerFor(owner, new_owner, volume_ids);
}

/// Rust `ApiImpl::bad_request_for_repository_build_error`, folded together
/// with the `unwrap_or_else(|| error(500, ...))` arm every caller pairs it
/// with.
///
/// Only three variants are the caller's fault: a malformed request, an alias
/// someone else holds, and a digest that did not match what was uploaded.
/// Everything else -- a missing artifact, an unsupported backend, a transport
/// failure -- is this node's problem, and a 400 would tell the client to stop
/// retrying something that a retry might well fix.
///
/// Note `AliasConflict` maps to 400 and not 409, which is upstream's choice:
/// the generated response for this route has only a 400 and a 500 variant, so
/// that is the status a client written against upstream can ever observe here.
ApiError RepositoryPublishError(const snapshot::repository::RepositoryError& err) {
    switch (err.kind) {
        case snapshot::repository::RepositoryErrorKind::InvalidRequest:
        case snapshot::repository::RepositoryErrorKind::AliasConflict:
        case snapshot::repository::RepositoryErrorKind::IntegrityMismatch:
            return ApiError::Make(400, err.ToString());
        default:
            return ApiError::Make(500, err.ToString());
    }
}

/// Rust passes `capture.metadata.custom_extension_params.clone()` straight
/// into the publish metadata, because both sides are the same
/// `serde_json::Map` there. This port stores the sandbox's copy as the
/// caller's original *text* (`custom_extension::Params::json_bytes`) and the
/// snapshot's as a parsed `core::Json`, so the hand-off needs a parse.
///
/// Mirrors `custom_extension::ParamsJson`, the other place that crosses the
/// same boundary: an empty value becomes an empty object, and a non-empty one
/// must parse to a JSON object.
///
/// A malformed non-empty value is an error rather than being dropped. These
/// params are forwarded to the extension's lifecycle hooks, so a snapshot
/// published without them would launch sandboxes that behave differently from
/// the one that was captured -- and nothing would say why.
core::Expected<core::Optional<core::Json>, ApiError> PublishedCustomExtensionParams(
    const core::Optional<sandbox::custom_extension::Params>& params) {
    if (!params.has_value()) return core::Optional<core::Json>();
    if (sandbox::custom_extension::ParamsIsEmpty(params)) {
        return core::Optional<core::Json>(core::Json(core::JsonObject()));
    }
    const core::Expected<core::Json, core::AnyError> parsed =
        core::Json::Parse(params->json_bytes);
    if (!parsed.ok()) {
        return core::make_unexpected(InternalError(
            "stored custom extension params are not valid JSON: " + parsed.error().chain()));
    }
    if (parsed.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(
            InternalError("stored custom extension params must be a JSON object"));
    }
    return core::Optional<core::Json>(parsed.value());
}

/// Rust `From<SnapshotRecord> for models::SnapshotInfo` (impls/snapshots.rs).
SnapshotInfoResp ToSnapshotInfoResp(const snapshot::SnapshotRecord& record) {
    SnapshotInfoResp resp;
    resp.snapshot_id = record.id.ToString();
    // Rust `vec![alias.to_string()]` or `vec![]`: a snapshot carries at most
    // one alias, and the schema reports it as a list.
    if (record.alias.has_value()) resp.names.push_back(record.alias->ToString());
    resp.cpu_count    = record.resources.cpu_count;
    resp.memory_mb    = record.resources.memory_mib;
    resp.disk_size_mb = record.resources.disk_size_mib;
    resp.created_at_unix_ms = record.created_at_unix_ms;
    resp.updated_at_unix_ms = record.updated_at_unix_ms;
    resp.image_ref = record.PublishedRootfsImageRef();
    return resp;
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

core::Expected<snapshot::RunnableSnapshot, ApiError>
ApiImpl::LoadRunnableSnapshot(const std::string& id_or_alias) {
    if (!runtime_resolver_) {
        // 501, not 500: a node with no resolver is correctly configured and
        // simply cannot serve this; retrying will not help, and the operator
        // needs to see which capability is absent rather than a generic fault.
        return core::make_unexpected(ApiError::Make(
            501,
            "this node cannot launch from a snapshot: no snapshot runtime resolver "
            "(only the posix_fs repository backend provides one)"));
    }
    const core::Expected<snapshot::repository::SnapshotRepository*, ApiError> repository =
        RequireRepository();
    if (!repository.ok()) return core::make_unexpected(repository.error());

    // Rust `SnapshotManager::get`, which is `repository.get(id_or_alias)` --
    // the repository resolves an alias itself, so no separate alias lookup.
    const snapshot::repository::RepositoryResult<core::Optional<snapshot::SnapshotRecord> >
        record = repository.value()->Get(id_or_alias);
    if (!record.ok()) {
        return core::make_unexpected(InternalError(
            "load committed snapshot '" + id_or_alias + "' through repository: " +
            record.error().ToString()));
    }
    if (!record.value().has_value()) {
        // Upstream's `Ok(None)` arm, verbatim down to the wording.
        return core::make_unexpected(
            ApiError::Make(400, "template " + id_or_alias + " not found"));
    }

    // Rust `SnapshotManager::resolve_runnable`. This is the call the whole
    // chain existed for and that had no production caller: it materialises
    // the node-local `image.json` for rootfs, memory and every attached
    // drive, and hands back the lease that keeps them from being reclaimed.
    const snapshot::repository::RepositoryResult<snapshot::RunnableSnapshot> runnable =
        runtime_resolver_->Resolve(*record.value());
    if (!runnable.ok()) {
        return core::make_unexpected(InternalError(
            "resolve committed snapshot into runnable runtime paths: " +
            runnable.error().ToString()));
    }
    return runnable.value();
}

core::Expected<CreateSandboxResp, ApiError>
ApiImpl::CreateSandbox(const CreateSandboxReq& req) {
    orchestrator::CreateSandboxRequest request;
    // Rust `create_sandbox_from_snapshot`: `template_id` names a record in the
    // repository, which is resolved into node-local paths *before* the launch
    // source is built. There is no template store in this port, so the id is
    // looked up as a snapshot id or alias directly -- upstream's template
    // store is a second catalog in front of the same repository, and skipping
    // it changes which names resolve, not what a resolved one launches.
    //
    // This used to assign `request.source.snapshot_id = req.template_id` and
    // stop there, leaving `kind` at its `Kind::Image` default. The snapshot
    // branch in `Orchestrator::CreateSandbox` was therefore unreachable, and
    // every create fell through to the fresh path with an empty
    // `image_config_path` -- a rootfs ublk device that cannot be built.
    const core::Expected<snapshot::RunnableSnapshot, ApiError> runnable =
        LoadRunnableSnapshot(req.template_id);
    if (!runnable.ok()) return core::make_unexpected(runnable.error());

    const core::Expected<orchestrator::SandboxLaunchSource, std::string> source =
        orchestrator::SandboxLaunchSource::FromRunnableSnapshot(runnable.value());
    if (!source.ok()) {
        // Not reachable through a resolver, which refuses an uncommitted
        // record first; reported as 500 because if it ever is, the repository
        // handed out a record the resolver should have rejected.
        return core::make_unexpected(InternalError(source.error()));
    }
    request.source = source.value();

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
    // Rust `secure: body.secure == Some(true)`. Dropping it would launch a
    // sandbox whose envd port accepts any caller.
    request.secure = req.secure;

    // Rust `sandboxes_post`, volume section.
    //
    // The two checks that read the source snapshot are now performed, because
    // the snapshot is finally resolved above. The note that used to stand here
    // said they were "deliberately absent" since "this port has no snapshot
    // repository" -- which was already wrong when it was written (the
    // repository is threaded into this bundle for `CreateSnapshot`) and is
    // doubly wrong now.
    //
    // `check_attached_drive_overlap` stays absent, and for the reason that
    // does still hold: it compares volume mounts against the request's
    // `attachedDrives`, a field `CreateSandboxReq` does not carry. See the
    // `/sandboxes-cold` note in `server.cc` for why that field is missing
    // from *this* request upstream too.
    // `has_value()`, not `!empty()`, and this is the distinction the Optional
    // in `CreateSandboxReq::volume_mounts` exists for: upstream passes
    // `body.volume_mounts.is_some()`. A present-but-empty list is an explicit
    // "this sandbox has no volumes", which still contradicts a capture that
    // recorded some; only an absent list means "restore them".
    const bool replaces_mounts = req.volume_mounts.has_value();
    const snapshot::CommittedSnapshot& committed = *runnable.value().record.committed;
    // Rust `check_snapshot_volume_replacement`. A restore brings the captured
    // volumes back with it; honouring `volumeMounts` as well would put the
    // guest's own filesystems and the requested ones on the same slots.
    if (replaces_mounts && !committed.volume_snapshots.empty()) {
        return core::make_unexpected(ApiError::Make(
            400, "this snapshot has volume mounts, which cannot be replaced; "
                 "omit volumeMounts to restore them"));
    }

    const core::Expected<core::Optional<std::map<std::string, std::string> >, ApiError>
        requested = VolumeMountsFromModel(
            req.volume_mounts.has_value() ? *req.volume_mounts
                                          : std::vector<SandboxVolumeMountModel>());
    if (!requested.ok()) return core::make_unexpected(requested.error());

    // Rust's free-slot check. Upstream counts `attached_drives.len() +
    // volume_mounts.len()`; this request carries no attached drives, so the
    // volume mounts are the whole of it.
    const std::size_t requested_drive_count =
        requested.value().has_value() ? requested.value()->size() : 0;
    const std::size_t free_slots = runnable.value().manifest.FreeDriveSlots();
    if (requested_drive_count > free_slots) {
        std::ostringstream message;
        message << "this snapshot has " << free_slots
                << " free drive slots, but " << requested_drive_count
                << " attached drives and volumes were requested";
        return core::make_unexpected(ApiError::Make(400, message.str()));
    }

    // `extra_drives_in_snapshot` stays false, and this is the one part of the
    // old note that survives. Upstream sets it when the request omits
    // `volumeMounts` *and* the capture recorded volumes -- but it sets it
    // beside `restore_snapshot_volume_mounts`, which recreates a volume record
    // per captured volume and puts the resulting drives in `extra_drives`. That
    // restore is not ported. The flag only describes how `extra_drives` are
    // bound, so setting it with an empty list would assert something about
    // drives that are not there. Left false rather than set speculatively; the
    // missing behaviour is the restore, not the flag.

    const core::Expected<PreparedVolumeMounts, ApiError> prepared = PrepareVolumeMounts(
        volumes_.get(),
        requested.value().has_value() ? *requested.value()
                                      : std::map<std::string, std::string>());
    // Returned unchanged: the error already carries the status upstream maps
    // it to (409 for a volume already leased, 400 for a bad mount path, 500
    // for a storage fault), so there is nothing left to classify.
    if (!prepared.ok()) return core::make_unexpected(prepared.error());

    request.extra_drives = prepared.value().drives;
    for (std::map<std::string, std::string>::const_iterator it =
             prepared.value().mounts.begin();
         it != prepared.value().mounts.end(); ++it) {
        request.volume_mounts[it->first] = it->second;
    }

    return FinishCreate(request, prepared.value());
}

core::Expected<CreateSandboxResp, ApiError>
ApiImpl::FinishCreate(const orchestrator::CreateSandboxRequest& request,
                      const PreparedVolumeMounts& prepared) {
    const orchestrator::OrchestratorResult<orchestrator::SandboxMetadata> r =
        svc_->CreateSandbox(request);
    if (!r.ok()) {
        // The launch never happened, so the placeholder lease has to go --
        // these volumes already existed and belong to the caller, so they are
        // released, not deleted.
        FinishVolumeReservation(volumes_.get(), prepared.owner,
                                core::Optional<std::string>(), prepared.volume_ids);
        return core::make_unexpected(ToApiError(r.error()));
    }

    const orchestrator::SandboxMetadata& meta = r.value();

    // Handing the lease to the sandbox that now holds the volumes. If this
    // fails the sandbox is running with volumes still leased to a placeholder
    // nothing will ever release, so the sandbox is torn down and the lease
    // dropped rather than left in that state.
    const core::Expected<core::Unit, volume::VolumeError> handover =
        FinishVolumeReservation(volumes_.get(), prepared.owner,
                                core::Optional<std::string>(meta.id.ToString()),
                                prepared.volume_ids);
    if (!handover.ok()) {
        const std::string message =
            "failed to finalize volume reservation: " + handover.error().ToString();
        svc_->DeleteSandbox(meta.id);
        FinishVolumeReservation(volumes_.get(), prepared.owner,
                                core::Optional<std::string>(), prepared.volume_ids);
        return core::make_unexpected(ApiError::Make(500, message));
    }

    CreateSandboxResp resp;
    resp.sandbox_id    = meta.id.ToString();
    resp.template_id   = meta.template_id;
    resp.state         = orchestrator::SandboxStateName(meta.state);
    resp.created_at_ms = meta.created_at_ms;
    // Rust `sandbox_model`.
    const core::Optional<sandbox::EnvdAccessToken> envd = svc_->GetEnvdAccessToken(meta);
    if (envd.has_value()) {
        resp.envd_access_token = core::Optional<std::string>(envd->Expose());
    }
    if (!meta.network_policy.allow_public_traffic) {
        resp.traffic_access_token = core::Optional<std::string>(svc_->TrafficAccessToken(meta.id));
    }
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
    GetSandboxResp resp = ToGetSandboxResp(*r.value());
    // Rust `SandboxDetail::envd_access_token`.
    const core::Optional<sandbox::EnvdAccessToken> envd = svc_->GetEnvdAccessToken(*r.value());
    if (envd.has_value()) {
        resp.envd_access_token = core::Optional<std::string>(envd->Expose());
    }
    return resp;
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

core::Expected<snapshot::repository::SnapshotRepository*, ApiError>
ApiImpl::RequireRepository() const {
    if (!repository_) {
        // Upstream's `snapshot_manager` is non-optional, so there is no
        // upstream message to copy. Refused rather than answered with a
        // fabricated id: a caller told its snapshot was taken will later
        // launch from a snapshot that was never written, and the failure then
        // surfaces on another node as a corrupt record.
        return core::make_unexpected(
            ApiError::Make(500, "no snapshot repository configured"));
    }
    return repository_.get();
}

core::Expected<std::vector<snapshot::SnapshotVolume>, ApiError>
ApiImpl::SnapshotSandboxVolumes(const orchestrator::SandboxMetadata& metadata) {
    std::vector<snapshot::SnapshotVolume> snapshots;
    if (metadata.volume_mounts.empty()) return snapshots;

    // The manager is only demanded once there is something to ask it for. A
    // sandbox with no mounts publishes fine without one, and refusing it
    // would make every snapshot on a volume-less node fail for a reason that
    // does not apply to it.
    const core::Expected<volume::VolumeManager*, ApiError> manager = RequireVolumes();
    if (!manager.ok()) return core::make_unexpected(manager.error());

    // Upstream iterates a `HashMap`, so its order is arbitrary and nothing
    // can depend on it. Sorted by mount path here, which is within that
    // freedom and makes the published record reproducible for the same
    // sandbox -- an arbitrary order would make two captures of one sandbox
    // differ in a field that carries no ordering meaning.
    const std::map<std::string, std::string> mounts(metadata.volume_mounts.begin(),
                                                    metadata.volume_mounts.end());
    snapshots.reserve(mounts.size());
    for (std::map<std::string, std::string>::const_iterator it = mounts.begin();
         it != mounts.end(); ++it) {
        const volume::VolumeResult<volume::VolumeRecord> state =
            manager.value()->SnapshotVolumeState(it->second);
        if (!state.ok()) {
            return core::make_unexpected(VolumeErrorResponse(state.error()));
        }
        snapshot::SnapshotVolume entry;
        entry.mount_path = it->first;
        entry.mode       = state.value().mode;
        entry.size_mb    = state.value().size_mb;
        entry.layers     = state.value().backing_layers;
        snapshots.push_back(entry);
    }
    return snapshots;
}

core::Expected<SnapshotInfoResp, ApiError>
ApiImpl::CreateSnapshot(const std::string& sandbox_id, const CreateSnapshotReq& req) {
    const core::Expected<core::SandboxId, ApiError> id = ParseId(sandbox_id);
    if (!id.ok()) return core::make_unexpected(id.error());

    // Demanded before the capture rather than after it. A capture pauses the
    // guest and writes its memory and rootfs out in full, and the only reason
    // to pay that is to publish the result; discovering afterwards that there
    // is nowhere to publish would have cost the sandbox a pause and left the
    // files on disk with nothing naming them.
    const core::Expected<snapshot::repository::SnapshotRepository*, ApiError> repository =
        RequireRepository();
    if (!repository.ok()) return core::make_unexpected(repository.error());

    // Rust parses the alias before capturing too, for the same reason.
    core::Optional<snapshot::SnapshotAlias> alias;
    if (req.name.has_value()) {
        const core::Expected<snapshot::SnapshotAlias, std::string> parsed =
            snapshot::SnapshotAlias::Parse(*req.name);
        if (!parsed.ok()) {
            // Rust `format!("invalid snapshot alias: {}", err)`.
            return core::make_unexpected(
                ApiError::Make(400, "invalid snapshot alias: " + parsed.error()));
        }
        alias = core::Optional<snapshot::SnapshotAlias>(parsed.value());
    }

    // 1) Capture. The orchestrator claims `Snapshotting`, drives the backend
    // and puts the sandbox back the way it found it.
    const orchestrator::OrchestratorResult<orchestrator::SnapshotCaptureResult> capture =
        svc_->CaptureSnapshot(id.value());
    if (!capture.ok()) {
        // `ToApiError` already produces the statuses upstream assigns this
        // route: 404 for a missing sandbox and 400 for a state a snapshot
        // cannot be taken from. Only the wording differs, and it comes from
        // the orchestrator error rather than being re-spelled here.
        return core::make_unexpected(ToApiError(capture.error()));
    }
    const orchestrator::SandboxMetadata& metadata = capture.value().metadata;

    // 2) The mounted volumes, as of the capture.
    const core::Expected<std::vector<snapshot::SnapshotVolume>, ApiError> volume_snapshots =
        SnapshotSandboxVolumes(metadata);
    if (!volume_snapshots.ok()) {
        // Rust: `match error.code { 500 => 500, _ => 400 }`. A volume
        // conflict is a 409 in `VolumeErrorResponse`, but this route's
        // generated response upstream has only the two variants, so 400 is
        // the status a client can observe here.
        const ApiError& error = volume_snapshots.error();
        return core::make_unexpected(error.code == 500 ? error
                                                       : ApiError::Make(400, error.message));
    }

    const core::Expected<core::Optional<core::Json>, ApiError> extension_params =
        PublishedCustomExtensionParams(metadata.custom_extension_params);
    if (!extension_params.ok()) {
        return core::make_unexpected(extension_params.error());
    }

    // 3) Publish: store the files and layers, then commit the record.
    snapshot::SnapshotPublishMetadata publish;
    publish.id    = core::SnapshotId::Fresh();
    publish.alias = alias;
    // The user metadata is provenance only; it never affects a launch.
    //
    // Upstream's field is an `Option<HashMap>` on both sides and is cloned
    // straight across. This port's `SandboxMetadata` holds a plain map, so
    // "absent" and "empty" have already collapsed by the time the capture
    // hands it over -- an empty one is published as absent, which is the
    // shape upstream writes for a sandbox that never had any.
    core::Optional<std::map<std::string, std::string> > user_metadata;
    if (!metadata.user_metadata.empty()) {
        user_metadata = core::Optional<std::map<std::string, std::string> >(
            std::map<std::string, std::string>(metadata.user_metadata.begin(),
                                               metadata.user_metadata.end()));
    }
    publish.source = snapshot::SnapshotPublishSource::Sandbox(metadata.id.ToString(),
                                                              user_metadata);
    publish.context             = metadata.context;
    publish.startup             = metadata.startup;
    publish.resources           = metadata.resources;
    publish.runtime_versions    = metadata.runtime_versions;
    publish.virtualization_mode = metadata.virtualization_mode;
    publish.image_configs       = metadata.image_configs;
    publish.volume_snapshots    = volume_snapshots.value();
    publish.custom_extension_params = extension_params.value();

    const snapshot::repository::RepositoryResult<snapshot::SnapshotPublication> published =
        snapshot::Publish(*repository.value(), publish,
                          capture.value().captured_snapshot.manifest);
    if (!published.ok()) {
        return core::make_unexpected(RepositoryPublishError(published.error()));
    }
    return ToSnapshotInfoResp(published.value().record);
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
