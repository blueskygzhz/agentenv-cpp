// SPDX-License-Identifier: MIT
// Rust: src/sandbox/backend.rs — the pluggable backend trait.
#ifndef AGENTENV_SANDBOX_BACKEND_H_
#define AGENTENV_SANDBOX_BACKEND_H_

#include <future>
#include <memory>
#include <string>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"
#include "agentenv/sandbox/types.h"

namespace agentenv {
namespace sandbox {

/// The core polymorphic contract. Every backend produces `std::future<Expected<T,E>>`
/// to model Rust `async fn -> Result<T,E>`.
class Backend {
 public:
    virtual ~Backend() = default;

    /// Create and boot a sandbox. Returns when the guest agent (envd) is reachable.
    virtual std::future<core::Expected<Handle, core::AnyError>>
        Boot(LaunchPlan plan) = 0;

    /// Stop and reclaim the sandbox.
    virtual std::future<core::Expected<core::Unit, core::AnyError>>
        Shutdown(core::SandboxId id) = 0;

    /// Pause / resume the microVM (for snapshotting).
    virtual std::future<core::Expected<core::Unit, core::AnyError>>
        Pause(core::SandboxId id) = 0;
    virtual std::future<core::Expected<core::Unit, core::AnyError>>
        Resume(core::SandboxId id) = 0;

    /// Snapshot / restore.
    virtual std::future<core::Expected<std::string, core::AnyError>>
        Snapshot(core::SandboxId id, const std::string& out_dir) = 0;
    virtual std::future<core::Expected<Handle, core::AnyError>>
        Restore(LaunchPlan plan, const std::string& snapshot_dir) = 0;

    /// Execute a command in the guest.
    virtual std::future<core::Expected<ExecResult, core::AnyError>>
        Exec(core::SandboxId id, ExecSpec spec) = 0;
};

/// Factory dispatched by config: "mock" | "firecracker".
std::unique_ptr<Backend> MakeBackend(const std::string& kind);

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_BACKEND_H_
