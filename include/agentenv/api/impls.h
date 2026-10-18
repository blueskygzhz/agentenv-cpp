// SPDX-License-Identifier: MIT
// Rust: src/api/impls/*.rs — server-side handler bundle.
#ifndef AGENTENV_API_IMPLS_H_
#define AGENTENV_API_IMPLS_H_

#include <memory>
#include <string>

#include "agentenv/api/dto.h"
#include "agentenv/core/expected.h"
#include "agentenv/orchestrator/service.h"

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
    /// Deviation: Rust additionally forks the source's volumes, giving each
    /// child its own writable clone (`prepare_volume_fork_specs`). That needs a
    /// `VolumeManager`, which this handler bundle does not hold, so a source
    /// with volume mounts is refused rather than forked without them -- sharing
    /// one exclusive volume between parent and child would let two VMs write
    /// the same image.
    core::Expected<ForkSandboxResp, ApiError>
        ForkSandbox(const std::string& sandbox_id, const ForkSandboxReq& req);

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

    std::shared_ptr<orchestrator::Orchestrator> svc_;
};

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_IMPLS_H_
