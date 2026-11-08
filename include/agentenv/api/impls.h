// SPDX-License-Identifier: MIT
// Rust: src/api/impls/*.rs — server-side handler bundle.
#ifndef AGENTENV_API_IMPLS_H_
#define AGENTENV_API_IMPLS_H_

#include <memory>
#include <string>

#include "agentenv/api/dto.h"
#include "agentenv/core/expected.h"
#include "agentenv/image/layers.h"
#include "agentenv/orchestrator/service.h"
// `CreateSnapshot` publishes through `snapshot::Publish`, which takes a
// repository reference and a `SnapshotVolume` list.
#include "agentenv/snapshot/publish.h"
#include "agentenv/volume.h"

namespace agentenv {
namespace api {

/// Rust `impl From<OrchestratorError> for models::Error` (impls/sandbox.rs).
///
/// The status is part of the contract, not a detail: a client retries a 503,
/// gives up on a 400 and backs off on a 409. Collapsing these to one code
/// would make every failure look alike to the caller.
ApiError ToApiError(const orchestrator::OrchestratorError& err);

/// Rust `sandbox_not_found` — 404 with the id in the message.
ApiError SandboxNotFound(const std::string& id);

/// Rust struct `ApiImpl` — glues API DTOs to orchestrator/snapshot/template.
///
/// Every handler reports failure as an `ApiError` carrying the status Rust
/// assigns it. A handler that cannot do its job returns an error rather than
/// an empty success: a caller told its snapshot was taken, or its sandbox
/// deleted, discovers otherwise on another node at another time, and by then
/// the record is simply corrupt.
class ApiImpl {
 public:
    explicit ApiImpl(std::shared_ptr<orchestrator::Orchestrator> svc)
        : svc_(std::move(svc)) {}

    /// With a volume manager, which the fork handler needs to give each child
    /// its own clone of the source's exclusive volumes, and which every
    /// `/volumes*` handler needs outright.
    ///
    /// Separate from the one-argument form rather than defaulted so a caller
    /// that has a manager cannot forget to pass it: the handler's behaviour
    /// differs, and silently getting the volume-less form would refuse forks
    /// the node is perfectly able to serve.
    ///
    /// `image_resolver` is optional because only `CreateVolume`'s `image`
    /// source needs it. Upstream's `ApiImpl` holds it unconditionally; here a
    /// request naming an image without one is refused rather than served from
    /// a resolver this bundle does not have.
    ///
    /// `repository` is optional on the same terms: only `CreateSnapshot` reads
    /// it, and it refuses rather than pretending to publish. Defaulted rather
    /// than split into another overload because, unlike the volume manager,
    /// its absence changes nothing about any *other* handler's behaviour.
    ///
    /// `runtime_resolver` is what turns `CreateSandbox`'s `templateID` into a
    /// launchable snapshot. Optional because only the posix-fs backend can
    /// supply one (see `SnapshotBackend` in
    /// `snapshot/repository/backends.h`); without it `CreateSandbox` cannot
    /// take the snapshot path at all and says so.
    ApiImpl(std::shared_ptr<orchestrator::Orchestrator> svc,
            std::shared_ptr<volume::VolumeManager> volumes,
            std::shared_ptr<image::ImageResolver> image_resolver =
                std::shared_ptr<image::ImageResolver>(),
            std::shared_ptr<snapshot::repository::SnapshotRepository> repository =
                std::shared_ptr<snapshot::repository::SnapshotRepository>(),
            std::shared_ptr<snapshot::SnapshotRuntimeResolver> runtime_resolver =
                std::shared_ptr<snapshot::SnapshotRuntimeResolver>())
        : svc_(std::move(svc)),
          volumes_(std::move(volumes)),
          image_resolver_(std::move(image_resolver)),
          repository_(std::move(repository)),
          runtime_resolver_(std::move(runtime_resolver)) {}

    // Sandbox handlers (Rust: src/api/impls/sandbox.rs).
    core::Expected<CreateSandboxResp, ApiError>
        CreateSandbox(const CreateSandboxReq& req);
    /// Rust `sandboxes_cold_post` — `POST /sandboxes-cold`.
    ///
    /// The route that takes an OCI reference. `POST /sandboxes` cannot: its
    /// `NewSandbox` schema carries only `templateID`, and its own description
    /// sends callers here. So the two are not alternatives -- one launches
    /// from a published snapshot, the other pulls an image and boots it cold,
    /// and only this one needs the image resolver.
    core::Expected<CreateSandboxResp, ApiError>
        CreateColdSandbox(const CreateColdSandboxReq& req);
    core::Expected<core::Unit, ApiError>
        DeleteSandbox(const std::string& sandbox_id);
    core::Expected<GetSandboxResp, ApiError>
        GetSandbox(const std::string& sandbox_id);
    core::Expected<ListSandboxesResp, ApiError>
        ListSandboxes();
    core::Expected<ExecResp, ApiError>
        Exec(const std::string& sandbox_id, const ExecReq& req);

    /// Rust `pause_sandbox` / `resume_sandbox` handlers.
    core::Expected<core::Unit, ApiError>
        PauseSandbox(const std::string& sandbox_id);
    core::Expected<GetSandboxResp, ApiError>
        ResumeSandbox(const std::string& sandbox_id);

    /// Rust `sandboxes_sandbox_id_fork_post`.
    ///
    /// Returns 201 with one entry per child even when every child failed: the
    /// children are independent, and a single status could not say that two of
    /// three booted. Only a failure of the *fork itself* -- a missing or
    /// non-running source -- comes back as an `ApiError`.
    ///
    /// A source holding volume mounts has each of its **exclusive** volumes
    /// cloned per child, and its read-only ones shared. Without a
    /// `VolumeManager` such a source is refused rather than forked without its
    /// volumes: handing one exclusive volume to parent and child would put two
    /// live VMs on one image.
    core::Expected<ForkSandboxResp, ApiError>
        ForkSandbox(const std::string& sandbox_id, const ForkSandboxReq& req);

    // Volume handlers (Rust: src/api/impls/volumes.rs).

    /// Rust `volumes_get`. An absent `limit` means `kDefaultVolumePageSize`.
    ///
    /// The continuation token comes back beside the records rather than inside
    /// them, because upstream returns it as the `x-next-token` header.
    core::Expected<VolumePageResp, ApiError>
        ListVolumes(const core::Optional<std::string>& next_token,
                    const core::Optional<std::size_t>& limit);

    /// Rust `volumes_post`. Returns the projected record for a 201.
    core::Expected<core::Json, ApiError> CreateVolume(const NewVolumeReq& req);

    /// Rust `volumes_volume_id_get`.
    core::Expected<core::Json, ApiError> GetVolume(const std::string& volume_id);

    /// Rust `volumes_volume_id_delete`.
    core::Expected<core::Unit, ApiError> DeleteVolume(const std::string& volume_id);

    /// Rust `sandboxes_sandbox_id_snapshots_post` (impls/sandbox.rs) — capture
    /// the running sandbox, then publish it as a snapshot.
    ///
    /// The three steps, and why the order is not free:
    ///
    ///   1. **Capture.** `Orchestrator::CaptureSnapshot` pauses the guest,
    ///      writes the artifacts and resumes it, handing back the manifest
    ///      that describes them.
    ///   2. **Snapshot the mounted volumes.** Their sealed layers are read
    ///      from the volume catalog and recorded in the publication, so a
    ///      restore gets the volumes as of the capture. Done *after* the
    ///      capture, as upstream does: the guest's volume writes are flushed
    ///      as part of the capture, so reading the layers first would record
    ///      a state older than the rootfs the snapshot restores.
    ///   3. **Publish.** Stores the files and layers, then commits the
    ///      record. The record is what names the snapshot, so nothing before
    ///      this point is visible to any other node.
    core::Expected<SnapshotInfoResp, ApiError>
        CreateSnapshot(const std::string& sandbox_id, const CreateSnapshotReq& req);

    // Template handlers (Rust: src/api/impls/template.rs).
    core::Expected<core::Unit, ApiError>
        ListTemplates();

 private:
    /// Parses an id and maps a bad one onto Rust's 404. Rust takes the id
    /// through a typed path parameter, so a malformed one never reaches the
    /// handler and is indistinguishable from a missing sandbox to the caller.
    core::Expected<core::SandboxId, ApiError> ParseId(const std::string& text) const;

    /// Rust reads `self.volume_manager` directly; it is non-optional there.
    /// Here it may be absent, so every volume handler starts by demanding it
    /// and reports the same 500 when it is missing.
    core::Expected<volume::VolumeManager*, ApiError> RequireVolumes() const;

    /// The same demand for the repository, which only `CreateSnapshot` needs.
    core::Expected<snapshot::repository::SnapshotRepository*, ApiError>
        RequireRepository() const;

    /// Rust `self.snapshot_manager.load_runnable(&body.template_id)` —
    /// `SnapshotManager::get` followed by `resolve_runnable`.
    ///
    /// Inlined rather than ported as a `SnapshotManager` type: that struct's
    /// other eleven methods are thin forwards to the repository, and the two
    /// this port needs are the pair that actually composes. Everything else it
    /// owns (publish, P2P fan-out) is already reachable as the free functions
    /// in `snapshot/publish.h`.
    ///
    /// Three distinct outcomes, and they must stay distinct:
    ///
    ///   * no resolver on this node -> 501. The node cannot launch from a
    ///     snapshot at all, which is not the caller's fault and will not
    ///     change on retry.
    ///   * no such record -> 400 "template <id> not found", matching
    ///     upstream's `Ok(None)` arm. Deliberately not 404: upstream treats a
    ///     bad `templateID` as a malformed request body, because the path
    ///     itself (`/sandboxes`) does exist.
    ///   * repository or resolve failure -> 500.
    core::Expected<snapshot::RunnableSnapshot, ApiError>
        LoadRunnableSnapshot(const std::string& id_or_alias);

    /// Rust `snapshot_sandbox_volumes` — the mounted volumes' sealed layers,
    /// as of the capture.
    ///
    /// A sandbox with no mounts yields an empty list, which is a successful
    /// answer rather than a reason to skip the publication.
    core::Expected<std::vector<snapshot::SnapshotVolume>, ApiError>
        SnapshotSandboxVolumes(const orchestrator::SandboxMetadata& metadata);

    std::shared_ptr<orchestrator::Orchestrator> svc_;
    /// Null when this bundle was built without one. The fork handler needs it
    /// for a source that has volume mounts, and `/volumes*` needs it always.
    std::shared_ptr<volume::VolumeManager> volumes_;
    /// Null unless supplied. Only `CreateVolume`'s `image` source reads it.
    std::shared_ptr<image::ImageResolver> image_resolver_;
    /// Null unless supplied. Only `CreateSnapshot` reads it, and it refuses
    /// rather than publishing nowhere when it is absent.
    ///
    /// Upstream reaches this through a `SnapshotManager` that owns both the
    /// repository and the catalog. There is no such type here -- `Publish` is
    /// a free function over `SnapshotRepository&` -- so the repository is held
    /// directly.
    std::shared_ptr<snapshot::repository::SnapshotRepository> repository_;
    /// Null unless supplied, and null on every OSS node. `CreateSandbox`
    /// resolves `templateID` through it; without one that handler cannot
    /// produce a `Kind::Snapshot` launch source and reports 501 rather than
    /// falling through to a fresh boot with an empty rootfs config, which is
    /// what it used to do.
    std::shared_ptr<snapshot::SnapshotRuntimeResolver> runtime_resolver_;
};

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_IMPLS_H_
