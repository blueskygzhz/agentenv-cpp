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
    ApiImpl(std::shared_ptr<orchestrator::Orchestrator> svc,
            std::shared_ptr<volume::VolumeManager> volumes,
            std::shared_ptr<image::ImageResolver> image_resolver =
                std::shared_ptr<image::ImageResolver>())
        : svc_(std::move(svc)),
          volumes_(std::move(volumes)),
          image_resolver_(std::move(image_resolver)) {}

    // Sandbox handlers (Rust: src/api/impls/sandbox.rs).
    core::Expected<CreateSandboxResp, ApiError>
        CreateSandbox(const CreateSandboxReq& req);
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

    // Snapshot handlers (Rust: src/api/impls/snapshot.rs).
    core::Expected<core::Unit, ApiError>
        CreateSnapshot(const std::string& sandbox_id);

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

    std::shared_ptr<orchestrator::Orchestrator> svc_;
    /// Null when this bundle was built without one. The fork handler needs it
    /// for a source that has volume mounts, and `/volumes*` needs it always.
    std::shared_ptr<volume::VolumeManager> volumes_;
    /// Null unless supplied. Only `CreateVolume`'s `image` source reads it.
    std::shared_ptr<image::ImageResolver> image_resolver_;
};

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_IMPLS_H_
