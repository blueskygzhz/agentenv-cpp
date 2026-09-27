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

/// Rust struct `ApiImpl` — glues API DTOs to orchestrator/snapshot/template.
class ApiImpl {
 public:
    explicit ApiImpl(std::shared_ptr<orchestrator::Service> svc)
        : svc_(std::move(svc)) {}

    // Sandbox handlers (Rust: src/api/impls/sandbox.rs).
    core::Expected<CreateSandboxResp, core::AnyError>
        CreateSandbox(const CreateSandboxReq& req);
    core::Expected<core::Unit, core::AnyError>
        DeleteSandbox(const std::string& sandbox_id);
    core::Expected<GetSandboxResp, core::AnyError>
        GetSandbox(const std::string& sandbox_id);
    core::Expected<ListSandboxesResp, core::AnyError>
        ListSandboxes();
    core::Expected<ExecResp, core::AnyError>
        Exec(const std::string& sandbox_id, const ExecReq& req);

    // Snapshot handlers (Rust: src/api/impls/snapshot.rs).
    core::Expected<core::Unit, core::AnyError>
        CreateSnapshot(const std::string& sandbox_id);

    // Template handlers (Rust: src/api/impls/template.rs).
    core::Expected<core::Unit, core::AnyError>
        ListTemplates();

 private:
    std::shared_ptr<orchestrator::Service> svc_;
};

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_IMPLS_H_
