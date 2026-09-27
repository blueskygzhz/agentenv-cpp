// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/service.rs — THE semantic core of AgentENV.
//
// It's a state machine over `Sandbox` records with a pluggable `Backend` and
// `Persister`. All public methods are synchronous by returning std::future<T>;
// internally the work is scheduled on `core::Executor`.
#ifndef AGENTENV_ORCHESTRATOR_SERVICE_H_
#define AGENTENV_ORCHESTRATOR_SERVICE_H_

#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agentenv/core/error.h"
#include "agentenv/core/executor.h"
#include "agentenv/core/expected.h"
#include "agentenv/orchestrator/persistence.h"
#include "agentenv/orchestrator/types.h"
#include "agentenv/sandbox/backend.h"

namespace agentenv {
namespace orchestrator {

/// Options passed to Service::Create.
struct CreateOptions {
    std::string template_id;
    std::vector<std::string> env_vars;
    int64_t timeout_ms = 300000;
};

class Service {
 public:
    Service(std::shared_ptr<sandbox::Backend> backend,
            std::shared_ptr<Persister> persister,
            core::Executor* executor = &core::GlobalExecutor());

    /// State transition APIs (mirrors Rust `impl Service`).
    ///
    /// Each returns a future that resolves once the transition is durable.
    std::future<core::Expected<Sandbox, core::AnyError>>
        Create(CreateOptions opts);

    std::future<core::Expected<core::Unit, core::AnyError>>
        Stop(core::SandboxId id);

    core::Expected<Sandbox, core::AnyError>
        Get(core::SandboxId id) const;

    core::Expected<std::vector<Sandbox>, core::AnyError>
        List() const;

    std::future<core::Expected<sandbox::ExecResult, core::AnyError>>
        Exec(core::SandboxId id, sandbox::ExecSpec spec);

 private:
    void Transition_(Sandbox* s, LifecyclePhase to);
    core::Expected<Sandbox, core::AnyError> DoCreate_(CreateOptions opts);
    core::Expected<core::Unit, core::AnyError> DoStop_(core::SandboxId id);

    std::shared_ptr<sandbox::Backend> backend_;
    std::shared_ptr<Persister>        persister_;
    core::Executor*                   executor_;

    // Guards persister writes.
    std::mutex mu_;
};

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_SERVICE_H_
