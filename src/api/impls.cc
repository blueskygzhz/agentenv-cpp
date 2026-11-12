// SPDX-License-Identifier: MIT
// Rust: src/api/impls/*.rs
#include "agentenv/api/impls.h"
// `QueryBuildLogs` reverses the entry list for a backward page.
#include <algorithm>
#include <sstream>
#include <unordered_map>

#include "agentenv/api/sandbox_params.h"
#include "agentenv/api/volumes.h"
#include "agentenv/core/identity.h"
// `CreateTemplateBuild` commits the waiting record through
// `record_catalog::Create` — Rust's `SnapshotManager::create`.
#include "agentenv/snapshot/repository/record_catalog.h"

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

/// Rust `cleanup_volume_ids`.
///
/// Deletes, where `FinishVolumeReservation(.., None, ..)` only *releases*, and
/// the two are not interchangeable. These volumes were created by this request
/// a moment ago to hold a capture's filesystems; nothing else refers to them,
/// so a launch that fails has to remove them or every retry leaks another set
/// of records and backing files that no sandbox will ever claim. The volumes
/// behind `PreparedVolumeMounts::volume_ids` are the opposite case -- they
/// existed before the request and belong to the caller, so they are released
/// and must not be deleted.
///
/// Every failure is swallowed, as upstream's `let _ =` does: this runs on a
/// path that is already returning an error, and the caller needs to hear about
/// that one rather than about a cleanup that also went wrong.
void CleanupVolumeIds(volume::VolumeManager* manager,
                      const std::vector<std::string>& volume_ids) {
    if (manager == NULL) return;
    for (std::size_t i = 0; i < volume_ids.size(); ++i) {
        const core::Expected<core::Unit, volume::VolumeError> removed =
            manager->Remove(volume_ids[i]);
        if (!removed.ok()) {
            AGENTENV_WARN("failed to clean up restored volume volume_id="
                          << volume_ids[i] << " error=" << removed.error().ToString());
        }
    }
}

/// Rust `restore_snapshot_volume_mounts`.
///
/// What a resume does with the volumes its capture was holding. A captured
/// snapshot records one `SnapshotVolume` per mounted volume -- mount path,
/// mode, size and the published backing layers -- and this turns each back
/// into a live volume record that the launch can mount.
///
/// It has to *create* rather than look up, because the original volume is not
/// what comes back. The capture froze a point in time, and the volume it was
/// taken from has kept running: it may have been written to, deleted, or
/// already be leased to another sandbox. Resuming into it would either fail on
/// the lease or silently resume a sandbox onto a filesystem that has moved on
/// since the capture. So each restore is a fresh record over the captured
/// layers, named `volume-restore-<uuid>` -- and because every resume of one
/// snapshot makes its own, a snapshot can be resumed any number of times
/// concurrently without the copies interfering.
///
/// Returns the mount map for `PrepareVolumeMounts` plus the ids it created, so
/// the caller can delete them if anything downstream fails.
core::Expected<std::pair<std::map<std::string, std::string>, std::vector<std::string> >,
               ApiError>
RestoreSnapshotVolumeMounts(volume::VolumeManager* manager,
                            const snapshot::CommittedSnapshot& committed) {
    std::map<std::string, std::string> mounts;
    std::vector<std::string>           volume_ids;
    if (committed.volume_snapshots.empty()) {
        return std::make_pair(mounts, volume_ids);
    }
    if (manager == NULL) {
        return core::make_unexpected(ApiError::Make(
            501, "restoring a snapshot's volume mounts requires a volume manager"));
    }

    for (std::size_t i = 0; i < committed.volume_snapshots.size(); ++i) {
        const snapshot::SnapshotVolume& captured = committed.volume_snapshots[i];

        // Re-normalised rather than trusted, even though the path was already
        // normalised when the capture was taken: the record is durable and may
        // have been written by another node or an older build, and this path
        // becomes a guest mount point.
        const core::Expected<std::string, std::string> mount_path =
            sandbox::NormalizeMountPath(captured.mount_path);
        if (!mount_path.ok()) {
            CleanupVolumeIds(manager, volume_ids);
            return core::make_unexpected(ApiError::Make(400, mount_path.error()));
        }

        const std::string name = "volume-restore-" + core::Uuid::GenV7().ToSimpleString();
        const volume::VolumeResult<volume::VolumeRecord> child = manager->CreateFromSnapshot(
            name, captured.mode, captured.size_mb, captured.layers);
        if (!child.ok()) {
            // Unwound before returning, so a snapshot whose third volume
            // cannot be restored does not leave the first two behind.
            CleanupVolumeIds(manager, volume_ids);
            return core::make_unexpected(VolumeErrorResponse(child.error()));
        }
        volume_ids.push_back(child.value().id);
        mounts[mount_path.value()] = child.value().id;
    }
    return std::make_pair(mounts, volume_ids);
}

/// The tail both create handlers share, from the launch onwards.
///
/// A free function because `PreparedVolumeMounts` is file-local: declaring it
/// as a member would have meant hoisting that struct into the public header
/// for no other reason. Rust duplicates this block between `sandboxes_post`
/// and `sandboxes_cold_post`; sharing it here is a deliberate deviation,
/// because the duplicated part is the volume-lease handover and two copies of
/// *that* is how one of them stops releasing on failure.
///
/// `restored_volume_ids` are the volumes `RestoreSnapshotVolumeMounts` created
/// for this launch. They are threaded in rather than cleaned up by the caller
/// because every failure below has to remove them, and a caller that forgot
/// one would leak a volume on exactly the path nobody tests by hand.
core::Expected<CreateSandboxResp, ApiError> FinishCreate(
    orchestrator::Orchestrator* svc, volume::VolumeManager* volumes,
    const orchestrator::CreateSandboxRequest& request,
    const PreparedVolumeMounts& prepared,
    const std::vector<std::string>& restored_volume_ids = std::vector<std::string>()) {
    const orchestrator::OrchestratorResult<orchestrator::SandboxMetadata> r =
        svc->CreateSandbox(request);
    if (!r.ok()) {
        // The launch never happened, so the placeholder lease has to go --
        // these volumes already existed and belong to the caller, so they are
        // released, not deleted.
        FinishVolumeReservation(volumes, prepared.owner,
                                core::Optional<std::string>(), prepared.volume_ids);
        // The restored ones are the other case: this request made them, so a
        // launch that never happened leaves nothing that should keep them.
        CleanupVolumeIds(volumes, restored_volume_ids);
        return core::make_unexpected(ToApiError(r.error()));
    }

    const orchestrator::SandboxMetadata& meta = r.value();

    // Handing the lease to the sandbox that now holds the volumes. If this
    // fails the sandbox is running with volumes still leased to a placeholder
    // nothing will ever release, so the sandbox is torn down and the lease
    // dropped rather than left in that state.
    const core::Expected<core::Unit, volume::VolumeError> handover =
        FinishVolumeReservation(volumes, prepared.owner,
                                core::Optional<std::string>(meta.id.ToString()),
                                prepared.volume_ids);
    if (!handover.ok()) {
        const std::string message =
            "failed to finalize volume reservation: " + handover.error().ToString();
        svc->DeleteSandbox(meta.id);
        FinishVolumeReservation(volumes, prepared.owner,
                                core::Optional<std::string>(), prepared.volume_ids);
        // After the teardown, not before: until the sandbox is gone it is
        // still mounting these, and deleting a volume out from under a running
        // guest is the failure this ordering avoids.
        CleanupVolumeIds(volumes, restored_volume_ids);
        return core::make_unexpected(ApiError::Make(500, message));
    }

    CreateSandboxResp resp;
    resp.sandbox_id    = meta.id.ToString();
    resp.template_id   = meta.template_id;
    resp.state         = orchestrator::SandboxStateName(meta.state);
    resp.created_at_ms = meta.created_at_ms;
    // Rust `sandbox_model`.
    const core::Optional<sandbox::EnvdAccessToken> envd = svc->GetEnvdAccessToken(meta);
    if (envd.has_value()) {
        resp.envd_access_token = core::Optional<std::string>(envd->Expose());
    }
    if (!meta.network_policy.allow_public_traffic) {
        resp.traffic_access_token = core::Optional<std::string>(svc->TrafficAccessToken(meta.id));
    }
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

/// Rust `params_map_to_model` — the stored params as the response model.
///
/// Absent params become an empty object, not `null`: upstream's return type is
/// a `HashMap`, so "this sandbox has no params" and "this sandbox has no
/// params set" are the same `{}` to a client. This port stores the params as
/// their original JSON *text*, so the absent case is the only one that needs
/// manufacturing; a stored value is handed back as it was received.
core::Json CustomExtensionParamsModel(
    const core::Optional<sandbox::custom_extension::Params>& params) {
    if (!params.has_value() || params->json_bytes.empty()) {
        return core::Json(core::JsonObject());
    }
    const core::Expected<core::Json, core::AnyError> parsed =
        core::Json::Parse(params->json_bytes);
    // Unparseable stored text cannot be reported as params, and cannot be
    // reported as *absent* either -- that would tell the caller the sandbox
    // has none when it has something nobody can read. An empty object is the
    // one honest answer that is also the documented shape; the text itself is
    // only ever written by this process from a body it already parsed, so
    // reaching here means local corruption rather than a bad request.
    if (!parsed.ok() || parsed.value().kind() != core::Json::Kind::Object) {
        return core::Json(core::JsonObject());
    }
    return parsed.value();
}

/// The status mapping for `custom_extension_params_patch`, which is this
/// route's own rather than `ToApiError`'s.
///
/// Two of the differences matter and neither is cosmetic:
///
///   * a non-running sandbox is **409**, where `ToApiError` says 400. The
///     request is well-formed and will succeed once the sandbox is running
///     again, so a 400 would tell the client to stop retrying something a
///     resume fixes.
///   * a failed hook splits on *who* failed. The extension answering 4xx is
///     the extension rejecting this patch — the caller's problem, **400**.
///     The extension answering 5xx, or not being configured at all, is this
///     deployment's problem, **500**. `ToApiError` calls the whole variant 500
///     and would hide a rejected patch behind a server error the client
///     retries forever.
///
/// The 5xx arm reports the hook's own message rather than the orchestrator's
/// wrapped chain, matching upstream's `Self::error(500, hook_err.to_string())`.
ApiError PatchParamsApiError(const orchestrator::OrchestratorError& err) {
    switch (err.kind) {
        case orchestrator::OrchestratorErrorKind::SandboxNotFound:
            return SandboxNotFound(err.sandbox_id.ToString());
        case orchestrator::OrchestratorErrorKind::InvalidSandboxState:
            return ApiError::Make(
                409, std::string("sandbox custom extension params cannot be patched from ") +
                         orchestrator::SandboxStateName(err.state) + " state");
        case orchestrator::OrchestratorErrorKind::SandboxOperationConflict:
            return ApiError::Make(409, err.Message());
        case orchestrator::OrchestratorErrorKind::SandboxOperationFailed: {
            // Rust `hook_status_error(&err).filter(|e| e.is_server_error())`.
            // Only a *server* error takes the 500 arm; a 4xx and a chain with
            // no hook error at all both fall through to 400.
            const sandbox::custom_extension::HookStatusError* hook =
                sandbox::custom_extension::FindHookStatusError(err.source);
            if (hook != NULL && hook->IsServerError()) {
                return ApiError::Make(500, hook->ToString());
            }
            return ApiError::Make(400, err.Message());
        }
        default:
            return ToApiError(err);
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

    // Rust `network_policy_from_create(body.allow_internet_access,
    // body.network.as_ref())`. This used to be skipped entirely on the
    // snapshot path, leaving `request.network_policy` default-constructed --
    // so `allow_internet_access` and the whole `network` config were accepted
    // by no field and honoured by no code, and every restored sandbox got
    // whatever the default policy happens to be rather than the one asked for.
    const core::Expected<sandbox::network::SandboxNetworkPolicy, std::string> policy =
        NetworkPolicyFromCreate(req.allow_internet_access,
                                req.network.has_value() ? &*req.network : NULL);
    if (!policy.ok()) {
        return core::make_unexpected(ApiError::Make(400, policy.error()));
    }
    request.network_policy = policy.value();

    // Rust `params_model_to_map` + `validate_custom_extension_params`.
    const core::Expected<core::Optional<sandbox::custom_extension::Params>, std::string>
        params = ParamsModelToMap(req.custom_extension_params);
    if (!params.ok()) return core::make_unexpected(ApiError::Make(400, params.error()));
    const core::Expected<core::Unit, std::string> params_ok =
        ValidateCustomExtensionParams(params.value());
    if (!params_ok.ok()) {
        return core::make_unexpected(ApiError::Make(400, params_ok.error()));
    }
    // Rust's snapshot path passes the request's value straight through; the
    // *inheritance* from the capture happens in `create_sandbox_inner`, which
    // reads `source.snapshot_custom_extension_params` when this is unset.
    request.custom_extension_params = params.value();

    // Rust `match body.auto_pause { Some(false) => Delete, _ => Pause }`.
    request.timeout_action = req.auto_pause.has_value() && !*req.auto_pause
                                 ? orchestrator::SandboxTimeoutAction::Delete
                                 : orchestrator::SandboxTimeoutAction::Pause;
    // Rust `body.auto_resume.as_ref().is_some_and(|cfg| cfg.enabled)`. The
    // orchestrator stores this on the record (`service.cc:1257`) and the proxy
    // reads it back to decide whether a paused sandbox may be woken by an
    // incoming request (`service.cc:514`) -- a chain that was fully wired and
    // permanently false, because no request could ask for it.
    request.auto_resume = req.auto_resume.has_value() && *req.auto_resume;
    // Rust `user_metadata: body.metadata.clone()`.
    for (std::map<std::string, std::string>::const_iterator it = req.metadata.begin();
         it != req.metadata.end(); ++it) {
        request.user_metadata[it->first] = it->second;
    }

    // Rust `duration_from_secs(body.timeout)` — **seconds**, and absent means
    // the node's configured default rather than any value spelled here.
    if (req.timeout_secs.has_value()) {
        request.timeout_ms = core::Optional<int64_t>(*req.timeout_secs * 1000);
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
    // Rust `check_snapshot_attached_drives`, which was absent along with the
    // `attachedDrives` field it guards. Same presence-not-emptiness rule, and
    // a disjunction: omitting the field restores both the captured drives and
    // the captured volumes, so an explicit list contradicts either.
    const core::Expected<core::Unit, std::string> drives_ok =
        CheckSnapshotAttachedDrives(committed, req.attached_drives.has_value());
    if (!drives_ok.ok()) {
        return core::make_unexpected(ApiError::Make(400, drives_ok.error()));
    }

    const core::Expected<core::Optional<std::map<std::string, std::string> >, ApiError>
        requested = VolumeMountsFromModel(
            req.volume_mounts.has_value() ? *req.volume_mounts
                                          : std::vector<SandboxVolumeMountModel>());
    if (!requested.ok()) return core::make_unexpected(requested.error());

    // Rust `attached_drives.len() + volume_mounts.len()`. Both halves are now
    // counted; this used to count only the volumes, because the request could
    // not carry drives, and the note saying so stood where this does.
    const std::size_t attached_count =
        req.attached_drives.has_value() ? req.attached_drives->size() : 0;
    const std::size_t requested_drive_count =
        attached_count + (requested.value().has_value() ? requested.value()->size() : 0);
    const std::size_t free_slots = runnable.value().manifest.FreeDriveSlots();
    if (requested_drive_count > free_slots) {
        std::ostringstream message;
        message << "this snapshot has " << free_slots
                << " free drive slots, but " << requested_drive_count
                << " attached drives and volumes were requested";
        return core::make_unexpected(ApiError::Make(400, message.str()));
    }

    // Rust `resolve_attached_drives(attached_drives, image_resolver)`, then
    // `.with_kind(ExtraDriveKind::LaunchSlot)`.
    //
    // The kind is not carried: this port's `ExtraDrive` has upstream's legacy
    // `bool volume` rather than the three-state `ExtraDriveKind`. That is
    // sound *here* because the only production reader of the distinction,
    // `PrepareResumeDrives` (`sandbox_backend.cc:586`), asks
    // `!IsVolume()` where upstream asks `kind() == LaunchSlot` -- and the two
    // differ only for a drive that is both new to this launch and `Physical`,
    // which the resume path cannot produce: a capture's physical drives come
    // from its manifest and are never new. Recorded rather than silently
    // relied upon, since a future `Physical` drive new to a resume would be
    // prepared `Fresh` and lose its contents.
    std::vector<sandbox::ExtraDrive> attached_drives;
    if (attached_count > 0) {
        if (!image_resolver_) {
            return core::make_unexpected(ApiError::Make(
                501, "attaching drives from images requires an image resolver"));
        }
        const core::Expected<std::vector<ResolvedAttachedDriveSpec>, AttachedDriveError>
            resolved = ResolveAttachedDrives(*req.attached_drives, image_resolver_.get());
        if (!resolved.ok()) {
            // Rust `client_or_server_response`: the error already carries the
            // status, so a bad mount path stays 400 and a registry fault 500.
            return core::make_unexpected(
                ApiError::Make(resolved.error().status, resolved.error().message));
        }
        for (std::size_t i = 0; i < resolved.value().size(); ++i) {
            attached_drives.push_back(resolved.value()[i].drive);
        }
    }

    // Rust `check_attached_drive_overlap`, which was absent for the same
    // reason the field was. Compared against the *requested* mounts, before
    // any volume is leased: a 400 after `PrepareVolumeMounts` would leave
    // placeholder leases behind for a request that was never going to launch.
    if (requested.value().has_value()) {
        const core::Expected<core::Unit, std::string> overlap =
            CheckAttachedDriveOverlap(attached_drives, *requested.value());
        if (!overlap.ok()) {
            return core::make_unexpected(ApiError::Make(400, overlap.error()));
        }
    }

    // Rust: `body.volume_mounts.is_none() && !volume_snapshots.is_empty()`.
    //
    // The note that stood here said the restore was not ported and the flag
    // was therefore left false. The restore is ported now (below), so the flag
    // is set for the case it describes.
    //
    // It is load-bearing on exactly this path. The flag tells
    // `InitialGuestDriveMountsForSnapshotLaunch` that the drives were bound
    // *before* the snapshot was loaded, so there is nothing to patch
    // afterwards -- which is right for a restore, because the captured config
    // already names a drive per captured volume and the guest resumed with
    // those mount points live. Leaving it false sends the launch down the
    // patch path, where it counts this launch's drives back from the end of
    // the config's drive list and rebinds that many -- so the restored volumes
    // would be written over the slots the capture was already using, in
    // whatever order a `std::map` iterated them.
    const bool extra_drives_in_snapshot =
        !replaces_mounts && !committed.volume_snapshots.empty();

    // Rust `restore_snapshot_volume_mounts`, the else-branch of the mounts
    // decision. This is what brings a capture's filesystems back.
    //
    // Without it the resume used to succeed with no volumes at all: a 201,
    // a running sandbox, and every path the guest had mounted now empty. That
    // is the shape of failure worth naming -- the client has no way to tell it
    // from a capture that never had volumes, and the guest sees directories
    // rather than an error.
    //
    // The branch is reached whenever the request omits `volumeMounts`, and the
    // two branches cannot both apply: the `check_snapshot_volume_replacement`
    // check above has already refused a request that names mounts for a
    // capture that recorded some. When the capture recorded none this returns
    // empty, which is why it needs no guard of its own.
    std::map<std::string, std::string> effective_mounts =
        requested.value().has_value() ? *requested.value()
                                      : std::map<std::string, std::string>();
    std::vector<std::string> restored_volume_ids;
    if (!replaces_mounts) {
        const core::Expected<
            std::pair<std::map<std::string, std::string>, std::vector<std::string> >, ApiError>
            restored = RestoreSnapshotVolumeMounts(volumes_.get(), committed);
        if (!restored.ok()) return core::make_unexpected(restored.error());
        effective_mounts   = restored.value().first;
        restored_volume_ids = restored.value().second;
    }

    const core::Expected<PreparedVolumeMounts, ApiError> prepared =
        PrepareVolumeMounts(volumes_.get(), effective_mounts);
    // Returned unchanged: the error already carries the status upstream maps
    // it to (409 for a volume already leased, 400 for a bad mount path, 500
    // for a storage fault), so there is nothing left to classify.
    if (!prepared.ok()) {
        // The restore already created records by this point, and this request
        // is over, so they have to go -- otherwise a resume that fails to
        // lease leaves a full copy of the capture's volumes behind every time
        // it is retried.
        CleanupVolumeIds(volumes_.get(), restored_volume_ids);
        return core::make_unexpected(prepared.error());
    }

    // Rust: `let mut extra_drives = attached_drives; extra_drives.extend(
    // volume_drives);`. The order is load-bearing, not cosmetic -- upstream's
    // own comment is "attached drives take the first free slots, before
    // volumes" -- because the reserved slots are consumed positionally and a
    // volume landing on the slot an attached drive expected would rebind the
    // guest's mounts to the wrong backing devices.
    request.extra_drives = attached_drives;
    for (std::size_t i = 0; i < prepared.value().drives.size(); ++i) {
        request.extra_drives.push_back(prepared.value().drives[i]);
    }
    for (std::map<std::string, std::string>::const_iterator it =
             prepared.value().mounts.begin();
         it != prepared.value().mounts.end(); ++it) {
        request.volume_mounts[it->first] = it->second;
    }
    request.extra_drives_in_snapshot = extra_drives_in_snapshot;

    return FinishCreate(svc_.get(), volumes_.get(), request, prepared.value(),
                        restored_volume_ids);
}

core::Expected<CreateSandboxResp, ApiError>
ApiImpl::CreateColdSandbox(const CreateColdSandboxReq& req) {
    // Rust `sandboxes_cold_post`.
    if (!image_resolver_) {
        return core::make_unexpected(ApiError::Make(
            501, "creating a sandbox from an image requires an image resolver"));
    }
    // Rust `image_resolver.resolve(&body.image)`. This is the step that turns
    // `python:3.11-slim` into an overlaybd `image.json` on this node -- the
    // thing `POST /sandboxes` cannot do, because its body has no image to
    // resolve.
    const core::Expected<image::ResolvedBlockImage, image::ImageError> resolved =
        image_resolver_->Resolve(req.image);
    if (!resolved.ok()) {
        // Rust: `is_user_error()` decides 400 against 500. A bad reference or
        // a missing tag is the caller's; a registry fault is this node's, and
        // a caller must be able to tell which one to retry.
        const int code = resolved.error().IsUserError() ? 400 : 500;
        return core::make_unexpected(ApiError::Make(
            code, "resolve sandbox rootfs image '" + req.image + "': " +
                      resolved.error().reason));
    }

    // Rust `cold_start_resources(body)`, which was already ported and tested
    // and had no caller: this route is the only one that reads these fields.
    const core::Expected<sandbox::SandboxResources, ApiError> resources =
        ColdStartResources(req.resources);
    if (!resources.ok()) return core::make_unexpected(resources.error());

    // Rust `resolve_attached_drives(body.attached_drives.as_deref()
    // .unwrap_or_default(), image_resolver.as_ref())`. Placed here, between
    // the resources check and the network policy, because upstream's order is
    // the error precedence a caller sees: a body with both a bad drive and a
    // bad network must answer about the drive.
    //
    // No presence test and no emptiness test: an absent list resolves to an
    // empty vector, and this route has no snapshot whose captured drives an
    // explicit `[]` could be refusing. The resolver is already known non-NULL
    // from this function's own guard, so `ResolveAttachedDrives` cannot reach
    // its "no image resolver configured" branch from here.
    std::vector<ResolvedAttachedDriveSpec> resolved_attached;
    if (req.attached_drives.has_value() && !req.attached_drives->empty()) {
        const core::Expected<std::vector<ResolvedAttachedDriveSpec>, AttachedDriveError>
            drives = ResolveAttachedDrives(*req.attached_drives, image_resolver_.get());
        if (!drives.ok()) {
            // Rust `client_or_server_response`: the status travels with the
            // error, so a reserved mount path stays 400 and a registry fault
            // stays 500.
            return core::make_unexpected(
                ApiError::Make(drives.error().status, drives.error().message));
        }
        resolved_attached = drives.value();
    }

    // Rust `network_policy_from_create(body.allow_internet_access,
    // body.network.as_ref())`.
    const core::Expected<sandbox::network::SandboxNetworkPolicy, std::string> policy =
        NetworkPolicyFromCreate(req.allow_internet_access,
                                req.network.has_value() ? &*req.network : NULL);
    if (!policy.ok()) {
        return core::make_unexpected(ApiError::Make(400, policy.error()));
    }

    // Rust `params_model_to_map` + `validate_custom_extension_params`.
    const core::Expected<core::Optional<sandbox::custom_extension::Params>, std::string>
        params = ParamsModelToMap(req.custom_extension_params);
    if (!params.ok()) return core::make_unexpected(ApiError::Make(400, params.error()));
    const core::Expected<core::Unit, std::string> params_ok =
        ValidateCustomExtensionParams(params.value());
    if (!params_ok.ok()) {
        return core::make_unexpected(ApiError::Make(400, params_ok.error()));
    }

    orchestrator::CreateSandboxRequest request;
    // Rust `SandboxLaunchSource::Image { .. }`.
    request.source = orchestrator::SandboxLaunchSource::FromImage(
        resolved.value().image_ref, resolved.value().overlaybd_config_path);
    // Rust `resolved_rootfs.base_context.into()` — `From<ImageBaseContext> for
    // CommandContext`. The fields line up one to one, including the two
    // optionals whose absent-versus-empty distinction OCI inheritance depends
    // on, so this is a copy and not a reinterpretation. `FromEnvAndWorkdir`
    // rather than aggregate assignment because it normalises a blank workdir
    // to `/`, which an image config may well carry.
    const image::ImageBaseContext& base = resolved.value().base_context;
    request.source.context =
        snapshot::CommandContext::FromEnvAndWorkdir(base.env_vars, base.workdir);
    request.source.context.WithUser(base.user)
        .WithExposedPorts(base.exposed_ports)
        .WithEntrypoint(base.entrypoint)
        .WithCmd(base.cmd)
        .WithVolumes(base.volumes)
        .WithLabels(base.labels);
    // Rust `resources: Some(resources)` — the Image variant's optional, where
    // unset would mean "the node's configured defaults". Always set here,
    // because `ColdStartResources` has already applied those defaults to
    // whichever fields the body omitted.
    request.source.image_resources =
        core::Optional<sandbox::SandboxResources>(resources.value());
    request.source.extra_boot_args = req.extra_boot_args;
    // Rust `build_image_configs(&resolved_rootfs, &resolved_attached)`. The
    // rootfs entry first: an unset `drive_id` and a `/` mount path are what
    // `ImageConfigs::RootfsConfig` matches on, and a snapshot taken from this
    // sandbox publishes that object as its own `Env`/`Entrypoint`. Getting
    // either wrong would make the capture inherit a volume's config.
    //
    // Skipped entirely when the image carried no OCI config, as upstream's
    // `if let Some(config)` does -- an entry holding JSON `null` would read as
    // a rootfs config that explicitly clears every inherited field.
    if (resolved.value().raw_config.has_value()) {
        request.source.image_configs.Add(core::Optional<std::string>(), "/",
                                         *resolved.value().raw_config);
    }
    // Then one entry per attached drive that carried a config, keyed by its
    // drive id and *resolved* mount path. Both are required to keep these from
    // being mistaken for the rootfs entry by `RootfsConfig`, which is why the
    // drive id is set even though the mount path alone would already differ.
    for (std::size_t i = 0; i < resolved_attached.size(); ++i) {
        const ResolvedAttachedDriveSpec& spec = resolved_attached[i];
        if (!spec.raw_config.has_value()) continue;
        request.source.image_configs.Add(
            core::Optional<std::string>(spec.drive.drive_id), spec.drive.mount_path,
            *spec.raw_config);
    }

    request.timeout_action = req.auto_pause.has_value() && !*req.auto_pause
                                 ? orchestrator::SandboxTimeoutAction::Delete
                                 : orchestrator::SandboxTimeoutAction::Pause;
    // Rust `body.auto_resume.as_ref().is_some_and(|cfg| cfg.enabled)`.
    request.auto_resume = req.auto_resume.has_value() && *req.auto_resume;
    request.custom_extension_params = params.value();
    // Rust `duration_from_secs(body.timeout)` — seconds, as in `NewSandbox`.
    // Both bodies spell this field `timeout` and both mean seconds; an earlier
    // note here claimed `NewSandbox` carried milliseconds, which was wrong.
    if (req.timeout_secs.has_value()) {
        request.timeout_ms = core::Optional<int64_t>(*req.timeout_secs * 1000);
    }
    for (std::size_t i = 0; i < req.env_vars.size(); ++i) {
        // `KEY=VALUE`, as in `CreateSandbox`. A pair without `=` is dropped
        // rather than stored under an empty key.
        const std::string& kv = req.env_vars[i];
        const std::size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        request.env_vars[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    for (std::map<std::string, std::string>::const_iterator it = req.metadata.begin();
         it != req.metadata.end(); ++it) {
        request.user_metadata[it->first] = it->second;
    }
    request.network_policy = policy.value();
    request.secure         = req.secure;

    const core::Expected<core::Optional<std::map<std::string, std::string> >, ApiError>
        requested = VolumeMountsFromModel(
            req.volume_mounts.has_value() ? *req.volume_mounts
                                          : std::vector<SandboxVolumeMountModel>());
    if (!requested.ok()) return core::make_unexpected(requested.error());

    const core::Expected<PreparedVolumeMounts, ApiError> prepared = PrepareVolumeMounts(
        volumes_.get(),
        requested.value().has_value() ? *requested.value()
                                      : std::map<std::string, std::string>());
    if (!prepared.ok()) return core::make_unexpected(prepared.error());

    // On the *source*, matching Rust's `SandboxLaunchSource::Image {
    // extra_drives }`. Note what this is **not**: a correctness requirement.
    // `Orchestrator::CreateSandbox`'s fresh branch concatenates
    // `source.extra_drives` with `request.extra_drives` before building the
    // spec, so either field reaches the guest. A mutation that moved these to
    // `request.extra_drives` survived its test, which is how that was
    // established rather than assumed.
    //
    // Kept on the source because the concatenation is ordered -- the image's
    // own drives first, then launch-time ones -- and that order is what
    // upstream's `extra_drives.extend(volume_drives)` produces.
    //
    // Rust `let mut extra_drives = resolved_attached.into_iter().map(|r|
    // r.drive).collect(); extra_drives.extend(volume_drives);`. Attached
    // drives lead, volumes follow: slots are consumed positionally, so the
    // reverse order would hand the guest a volume where it mounts a drive.
    // Unlike the snapshot route the kind is not overridden to `LaunchSlot`
    // here either -- upstream leaves these `Physical` on a cold boot, which is
    // what this port's `volume == false` already means.
    request.source.extra_drives.clear();
    for (std::size_t i = 0; i < resolved_attached.size(); ++i) {
        request.source.extra_drives.push_back(resolved_attached[i].drive);
    }
    for (std::size_t i = 0; i < prepared.value().drives.size(); ++i) {
        request.source.extra_drives.push_back(prepared.value().drives[i]);
    }
    for (std::map<std::string, std::string>::const_iterator it =
             prepared.value().mounts.begin();
         it != prepared.value().mounts.end(); ++it) {
        request.volume_mounts[it->first] = it->second;
    }

    return FinishCreate(svc_.get(), volumes_.get(), request, prepared.value());
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

core::Expected<core::Json, ApiError>
ApiImpl::GetSandboxCustomExtensionParams(const std::string& sandbox_id) {
    const core::Expected<core::SandboxId, ApiError> id = ParseId(sandbox_id);
    if (!id.ok()) return core::make_unexpected(id.error());

    const orchestrator::OrchestratorResult<core::Optional<orchestrator::SandboxMetadata> >
        got = svc_->GetSandbox(id.value());
    if (!got.ok()) return core::make_unexpected(ToApiError(got.error()));
    if (!got.value().has_value()) {
        return core::make_unexpected(SandboxNotFound(sandbox_id));
    }
    return CustomExtensionParamsModel(got.value()->custom_extension_params);
}

core::Expected<core::Json, ApiError>
ApiImpl::PatchSandboxCustomExtensionParams(const std::string& sandbox_id,
                                           const core::Json& patch) {
    const core::Expected<core::SandboxId, ApiError> id = ParseId(sandbox_id);
    if (!id.ok()) return core::make_unexpected(id.error());

    // Rust `params_model_to_map(body)`: the body is the patch map itself, not
    // a wrapper object. An empty object is a legal no-op patch -- the hook
    // still runs, and may still rewrite the params.
    if (patch.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(
            ApiError::Make(400, "customExtensionParams body must be a JSON object"));
    }
    sandbox::custom_extension::Params params;
    params.json_bytes = patch.ToString();

    const orchestrator::OrchestratorResult<
        core::Optional<sandbox::custom_extension::Params> >
        patched = svc_->PatchSandboxCustomExtensionParams(id.value(), params);
    if (!patched.ok()) {
        return core::make_unexpected(PatchParamsApiError(patched.error()));
    }
    return CustomExtensionParamsModel(patched.value());
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

// Rust `v3_templates_post`.
core::Expected<TemplateRequestResponseV3, ApiError>
ApiImpl::CreateTemplateBuild(const TemplateBuildRequestV3& body) {
    // Rust's first branch: `image_only == Some(true)` goes to
    // `create_image_build`, which builds a reusable *image* from a Dockerfile
    // and never produces a template record.
    //
    // Refused rather than quietly built as a template. The two differ in what
    // the caller gets back and in what the tags end up pointing at, so
    // ignoring the flag would answer 202 and hand over the one artifact the
    // request explicitly asked not to have. 501: the node is correctly
    // configured and simply cannot do this, so retrying will not help.
    if (body.image_only.has_value() && *body.image_only) {
        return core::make_unexpected(ApiError::Make(
            501, "imageOnly builds are not available in this build "
                 "(needs the Dockerfile image-build pipeline: ImageBuildRecord, "
                 "the buildkit worker and its log tunnel)"));
    }

    // Rust's `let Some(name) = body.name.clone() else { 400 }`. Optional in
    // the schema, required by this branch: the name is both the alias the
    // record claims and the only handle the caller is given back.
    if (!body.name.has_value() || body.name->empty()) {
        return core::make_unexpected(ApiError::Make(400, "template name must be provided"));
    }
    const std::string& name = *body.name;

    // The repository is demanded before the id is minted: `Create` is what
    // makes the reservation real, and a 501 here must not look different from
    // any other unconfigured-node refusal.
    const core::Expected<snapshot::repository::SnapshotRepository*, ApiError> repository =
        RequireRepository();
    if (!repository.ok()) return core::make_unexpected(repository.error());

    // Rust `SnapshotId::generate()`. The caller does not choose the id, which
    // is why the V2 start route takes it back from this response.
    const core::SnapshotId id = core::SnapshotId::Fresh();

    const core::Expected<snapshot::SnapshotRecord, ApiError> record =
        TemplateBuildRecordFromV3Request(body, id, name);
    if (!record.ok()) return core::make_unexpected(record.error());

    // Rust `self.snapshot_manager.create(record)`. This is the commit that
    // was missing: `TemplateBuildRecordFromV3Request` built the waiting
    // record and nothing stored it, so the name was never actually claimed
    // and `TryStartBuild` had no record to find.
    const snapshot::repository::RepositoryResult<snapshot::SnapshotRecord> created =
        snapshot::repository::record_catalog::Create(*repository.value(), record.value());
    if (!created.ok()) {
        // Rust `bad_request_for_repository_build_error(..).unwrap_or_else(..
        // repository_error(..))` followed by `client_or_server_response`.
        // `AliasConflict` lands on 400 here rather than the 409 that
        // `repository_error` alone would give -- on this route a taken
        // template name is bad input, and it is the response a client sees
        // when it races another request for the same name.
        return core::make_unexpected(RepositoryPublishError(created.error()));
    }

    TemplateRequestResponseV3 response;
    // Both from the *stored* record, not from the local `id`: `Create` is the
    // authority on what was written, and reporting the id we hoped for would
    // hide any divergence.
    response.template_id = created.value().id.ToString();
    response.build_id    = created.value().id.ToString();
    response.is_public   = true;
    response.names.push_back(name);
    // Rust passes `Vec::new()` for tags even when the request carried some:
    // `TemplateBuildRecordFromV3Request` refuses a non-empty `tags` list, so
    // by here there are none to echo.
    response.aliases.push_back(name);
    return response;
}

// Rust `v2_templates_template_id_builds_build_id_post`.
core::Expected<core::Unit, ApiError>
ApiImpl::StartTemplateBuild(const std::string& template_id, const std::string& build_id,
                            const TemplateBuildStartRequest& body) {
    // Rust's first check, verbatim down to the wording. AgentENV keeps one
    // build per template, so the two path segments are the same id -- which is
    // also why `CreateTemplateBuild` reports it twice.
    if (template_id != build_id) {
        return core::make_unexpected(ApiError::Make(
            400, "templateID and buildID must match in AgentENV compatibility mode"));
    }

    const core::Expected<core::SnapshotId, std::string> parsed_build_id =
        core::SnapshotId::Parse(build_id);
    if (!parsed_build_id.ok()) {
        return core::make_unexpected(ApiError::Make(400, "invalid buildID: " + build_id));
    }

    const core::Expected<snapshot::repository::SnapshotRepository*, ApiError> repository =
        RequireRepository();
    if (!repository.ok()) return core::make_unexpected(repository.error());

    // Rust `snapshot_manager.get(&template_id)`: by id *or* alias, which is
    // why the next check exists at all.
    const snapshot::repository::RepositoryResult<core::Optional<snapshot::SnapshotRecord> >
        found = snapshot::repository::record_catalog::Get(*repository.value(), template_id);
    if (!found.ok()) {
        return core::make_unexpected(InternalError(
            "load template '" + template_id + "' through repository: " +
            found.error().ToString()));
    }
    if (!found.value().has_value()) {
        return core::make_unexpected(
            ApiError::Make(404, "template " + template_id + " not found"));
    }
    const snapshot::SnapshotRecord& pending = *found.value();

    // The lookup above accepts an alias, so `templateID` may name a record
    // whose id is not `buildID`. Starting it anyway would run this build
    // against someone else's reservation.
    if (pending.id != parsed_build_id.value()) {
        return core::make_unexpected(ApiError::Make(
            400, "templateID must reference the same build record as buildID"));
    }

    // Rust `template_build_start_base_source(body)` — `fromImage` and
    // `fromTemplate` are mutually exclusive, and neither means the configured
    // default image.
    const core::Expected<TemplateBuildStartBaseSource, ApiError> base_source =
        TemplateBuildStartBaseSourceFrom(body);
    if (!base_source.ok()) return core::make_unexpected(base_source.error());

    // Rust `template_registry_auth(body)`. The validation is upstream's; what
    // differs is the outcome, because `RegistryAuth` is not wired into the
    // build path here. Refused rather than ignored: pulling anonymously
    // either fails with an authentication error the caller already supplied
    // credentials for, or silently resolves a *different, public* image of
    // the same name -- and then publishes it as the template.
    if (body.from_image_registry.has_value()) {
        const core::Optional<std::string> from_image =
            body.from_image.has_value() && !body.from_image->empty()
                ? body.from_image
                : core::Optional<std::string>();
        if (!from_image.has_value() ||
            (body.from_template.has_value() && !body.from_template->empty())) {
            // Upstream's own 400, checked before the capability refusal so a
            // malformed request is reported as malformed.
            return core::make_unexpected(ApiError::Make(
                400, "fromImageRegistry requires fromImage and cannot be used with "
                     "fromTemplate"));
        }
        return core::make_unexpected(ApiError::Make(
            501, "fromImageRegistry is not available in this build "
                 "(RegistryAuth is ported but not wired into the template build path)"));
    }

    // Rust `template_build_spec_from_start_request(body, record.alias,
    // record.resources)`. The resources come from the *reservation*, not from
    // this request: `POST /v3/templates` is where the machine shape was
    // chosen, and this body has no fields for it.
    const core::Expected<tpl::TemplateBuildSpec, ApiError> spec =
        TemplateBuildSpecFromStartRequest(body, pending.alias, pending.resources);
    if (!spec.ok()) return core::make_unexpected(spec.error());

    // Everything the request could get wrong has now been checked, and
    // nothing has been mutated. `TryStartBuild` would be next.
    //
    // It is deliberately *not* called. It moves the record from `waiting` to
    // `building`, and only a `waiting` record can be claimed -- so claiming
    // and then failing to build would leave this template permanently
    // unstartable, dead even once the executor lands. Leaving it `waiting`
    // keeps the reservation usable by the identical request later.
    //
    // 501 because the node is correctly configured and cannot do this; a
    // retry will not help, and the operator needs to see which piece is
    // absent rather than a generic fault.
    return core::make_unexpected(ApiError::Make(
        501, "starting a template build is not available in this build "
             "(the request is valid and the reservation is intact, but the build "
             "executor -- which boots a Firecracker sandbox and drives "
             "start/capture/stop -- is not ported; the record is left waiting so "
             "this request will work unchanged once it is)"));
}

// Rust `ApiImpl::build_record`.
core::Expected<snapshot::SnapshotRecord, ApiError>
ApiImpl::BuildRecordFor(const std::string& template_id, const std::string& build_id) {
    // 404, not the 400 the V2 start route gives the same mismatch. Upstream
    // splits them deliberately: this is a sub-resource address, and a build
    // that is not under that template is not there.
    if (template_id != build_id) {
        return core::make_unexpected(ApiError::Make(404, "build not found for template"));
    }

    // Parsed, and then *not* used to look the record up -- the lookup below
    // takes the raw string, which also accepts an alias. The parse is here
    // only to make a malformed id a 400 before anything is read, which is
    // upstream's order. The consequence is upstream's too: a template cannot
    // be addressed by name on this route, because a name is not a `SnapshotId`
    // and is refused before the alias lookup could resolve it.
    if (!core::SnapshotId::Parse(build_id).ok()) {
        return core::make_unexpected(ApiError::Make(400, "invalid buildID"));
    }

    const core::Expected<snapshot::repository::SnapshotRepository*, ApiError> repository =
        RequireRepository();
    if (!repository.ok()) return core::make_unexpected(repository.error());

    const snapshot::repository::RepositoryResult<core::Optional<snapshot::SnapshotRecord> >
        found = snapshot::repository::record_catalog::Get(*repository.value(), build_id);
    if (!found.ok()) {
        return core::make_unexpected(InternalError(
            "load template build '" + build_id + "' through repository: " +
            found.error().ToString()));
    }
    // Upstream's next step is `image_build_record::get`, for an imageOnly
    // build that has no snapshot record. `imageOnly` is refused by
    // `CreateTemplateBuild`, so no such record can exist and the fallback
    // would return this same 404.
    if (!found.value().has_value()) {
        return core::make_unexpected(ApiError::Make(404, "template build not found"));
    }
    // A sandbox-sourced snapshot is addressable by id but is not a build, and
    // reporting one here would describe a captured sandbox as a template build
    // that is permanently `Ready`.
    if (!found.value()->source.is_template()) {
        return core::make_unexpected(ApiError::Make(404, "template build not found"));
    }
    return *found.value();
}

// Rust `ApiImpl::query_build_logs`.
core::Expected<std::vector<tpl::BuildLogEntry>, ApiError>
ApiImpl::QueryBuildLogs(const core::SnapshotId& id, const BuildLogQuery& query) {
    // Rust's own ceiling, enforced here rather than clamped: a caller asking
    // for 500 entries and silently getting 100 cannot tell that from a build
    // whose log is 100 lines long.
    if (query.limit > 100) {
        return core::make_unexpected(ApiError::Make(400, "limit must be between 0 and 100"));
    }
    // Checked before anything is read, as upstream does -- `limit=0` is a
    // valid request for nothing, not a request to be refused.
    if (query.limit == 0) return std::vector<tpl::BuildLogEntry>();

    std::vector<tpl::BuildLogEntry> entries;
    // Upstream reads the live buffer first and falls through to the document.
    // There is no live buffer here, so `source=temporary` is the one case that
    // reads nothing: answering with the persisted document instead would hand
    // back a *previous* build's log to a client that asked for the running
    // one's.
    if (!(query.source.has_value() && *query.source == LogsSource::Temporary)) {
        const core::Expected<snapshot::repository::SnapshotRepository*, ApiError> repository =
            RequireRepository();
        if (!repository.ok()) return core::make_unexpected(repository.error());
        const snapshot::repository::RepositoryResult<std::vector<tpl::BuildLogEntry> > read =
            tpl::ReadBuildLogs(*repository.value(), id);
        if (!read.ok()) {
            return core::make_unexpected(InternalError(
                "read build logs for '" + id.ToString() + "': " + read.error().ToString()));
        }
        entries = read.value();
    }

    // Reversed *before* filtering, so `cursor` and `offset` both count from
    // the end a backward page starts at.
    if (query.backward) std::reverse(entries.begin(), entries.end());

    std::vector<tpl::BuildLogEntry> out;
    std::size_t skipped = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const tpl::BuildLogEntry& entry = entries[i];
        // A *minimum* severity: `level=warn` returns warnings and errors.
        // `LogRecordLevel`'s order is what this relies on, which is why the
        // enum documents that its four variants must stay ascending.
        if (query.level.has_value() && entry.level < *query.level) continue;
        if (query.cursor.has_value()) {
            // Inclusive, and in milliseconds: the stored timestamp has
            // nanosecond resolution, so it is truncated to the cursor's unit
            // rather than the cursor scaled up -- otherwise an entry in the
            // same millisecond as the cursor would fall on the wrong side of
            // an inclusive bound.
            const int64_t entry_ms = entry.timestamp.unix_nanos / 1000000;
            if (query.backward ? entry_ms > *query.cursor : entry_ms < *query.cursor) {
                continue;
            }
        }
        // Rust's `.skip(offset)`, which comes after the filter -- the
        // parameter is documented as an index into the *filtered* list, so a
        // client paging with a level filter gets consecutive pages.
        if (skipped < query.offset) {
            ++skipped;
            continue;
        }
        out.push_back(entry);
        if (out.size() >= query.limit) break;
    }
    return out;
}

// Rust `ApiImpl::query_build_reason_logs`.
core::Expected<std::vector<tpl::BuildLogEntry>, ApiError>
ApiImpl::QueryBuildReasonLogs(const core::SnapshotId& id, const std::string& step) {
    const core::Expected<snapshot::repository::SnapshotRepository*, ApiError> repository =
        RequireRepository();
    if (!repository.ok()) return core::make_unexpected(repository.error());
    const snapshot::repository::RepositoryResult<std::vector<tpl::BuildLogEntry> > read =
        tpl::ReadBuildLogs(*repository.value(), id);
    if (!read.ok()) {
        return core::make_unexpected(InternalError(
            "read build logs for '" + id.ToString() + "': " + read.error().ToString()));
    }

    std::vector<tpl::BuildLogEntry> out;
    for (std::size_t i = 0; i < read.value().size() && out.size() < 100; ++i) {
        const tpl::BuildLogEntry& entry = read.value()[i];
        // Bound to the failing step and to `warn`-and-above. Both halves
        // matter: without the step filter this would be the whole log of a
        // build that mostly succeeded, and without the level filter the
        // failing step's own informational output would bury its error.
        if (!entry.step.has_value() || *entry.step != step) continue;
        if (entry.level < core::LogRecordLevel::Warn) continue;
        out.push_back(entry);
    }
    return out;
}

// Rust `templates_template_id_builds_build_id_status_get`.
core::Expected<TemplateBuildInfoModel, ApiError>
ApiImpl::GetTemplateBuildStatus(const std::string& template_id, const std::string& build_id,
                                const BuildLogQuery& logs) {
    const core::Expected<snapshot::SnapshotRecord, ApiError> record =
        BuildRecordFor(template_id, build_id);
    if (!record.ok()) return core::make_unexpected(record.error());

    TemplateBuildInfoModel info = TemplateBuildInfoModel::FromRecord(record.value());

    // Upstream passes `cursor = None`, `backward = false` and `source = None`
    // literally: this route's schema has no spelling for any of the three.
    // Pinned here rather than left to the caller so the handler's answer does
    // not depend on a parameter the route cannot carry.
    BuildLogQuery page = logs;
    page.cursor   = core::Optional<int64_t>();
    page.backward = false;
    page.source   = core::Optional<LogsSource>();
    const core::Expected<std::vector<tpl::BuildLogEntry>, ApiError> entries =
        QueryBuildLogs(record.value().id, page);
    if (!entries.ok()) return core::make_unexpected(entries.error());
    info.log_entries = entries.value();

    // The reason's own logs, fetched only when the failure names a step.
    // This is what makes the route worth having over `GET /templates`: the
    // list reports *that* a build failed, and this reports the output of the
    // step that failed it.
    if (info.reason.has_value() && info.reason->step.has_value()) {
        const core::Expected<std::vector<tpl::BuildLogEntry>, ApiError> reason_logs =
            QueryBuildReasonLogs(record.value().id, *info.reason->step);
        if (!reason_logs.ok()) return core::make_unexpected(reason_logs.error());
        info.reason->log_entries =
            core::Optional<std::vector<tpl::BuildLogEntry> >(reason_logs.value());
    }
    return info;
}

// Rust `templates_template_id_builds_build_id_logs_get`.
core::Expected<std::vector<tpl::BuildLogEntry>, ApiError>
ApiImpl::GetTemplateBuildLogs(const std::string& template_id, const std::string& build_id,
                              const BuildLogQuery& logs) {
    // The record lookup runs even though only its id is used: it is what
    // turns a request for a build nobody reserved into a 404 instead of an
    // empty log, which a client cannot tell from a build that has not
    // written anything yet.
    const core::Expected<snapshot::SnapshotRecord, ApiError> record =
        BuildRecordFor(template_id, build_id);
    if (!record.ok()) return core::make_unexpected(record.error());
    return QueryBuildLogs(record.value().id, logs);
}

// Rust `templates_get`.
core::Expected<std::vector<TemplateModel>, ApiError>
ApiImpl::ListTemplates() {
    // The 501 this used to return blamed "the template store", which was the
    // wrong diagnosis: there is no separate template store either upstream or
    // here -- templates *are* snapshot records, `record_catalog::List` is
    // ported and tested, and this class already holds the repository that
    // `CreateSnapshot` and `CreateTemplateBuild` publish through. What was
    // missing was only the call.
    const core::Expected<snapshot::repository::SnapshotRepository*, ApiError> repository =
        RequireRepository();
    if (!repository.ok()) return core::make_unexpected(repository.error());

    // Rust `SnapshotListFilter::templates()`. Not `MatchesAll()`: a
    // sandbox-sourced record is a snapshot, not a template, and listing it
    // here would offer `POST /sandboxes {"templateID": ...}` callers an entry
    // that was never built as a template.
    const snapshot::repository::RepositoryResult<std::vector<snapshot::SnapshotRecord> >
        records = snapshot::repository::record_catalog::List(
            *repository.value(), snapshot::repository::SnapshotListFilter::Templates());
    if (!records.ok()) {
        // 500 for every failure, which is both upstream's shape and the
        // truthful one here: `TemplatesGetResponse` declares no 4xx variant
        // besides the auth one, and `List` can only fail by failing to read
        // or decode the catalog -- there is no request to be at fault.
        return core::make_unexpected(InternalError(
            "list template snapshots through repository: " + records.error().ToString()));
    }

    std::vector<TemplateModel> out;
    out.reserve(records.value().size());
    for (std::size_t i = 0; i < records.value().size(); ++i) {
        out.push_back(TemplateModel::FromRecord(records.value()[i]));
    }
    return out;
}

}  // namespace api
}  // namespace agentenv
