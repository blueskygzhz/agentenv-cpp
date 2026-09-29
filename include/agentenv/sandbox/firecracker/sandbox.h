// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs — the FirecrackerBackend (a Backend
// impl that drives a Firecracker microVM). Kept under the firecracker/ subdir to
// mirror the Rust module layout 1:1.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_SANDBOX_H_
#define AGENTENV_SANDBOX_FIRECRACKER_SANDBOX_H_

#include <memory>
#include <string>

#include "agentenv/sandbox/backend.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Configuration to launch a Firecracker process.
struct Config {
    std::string firecracker_bin = "/usr/bin/firecracker";
    std::string jailer_bin      = "/usr/bin/jailer";
    std::string chroot_base_dir = "/var/lib/agentenv/vm";
    bool        use_jailer      = true;
};

class FirecrackerBackend final : public Backend {
 public:
    explicit FirecrackerBackend(Config cfg);
    ~FirecrackerBackend() override;

    core::Expected<Handle, core::AnyError>
        Boot(LaunchPlan plan) override;
    core::Expected<core::Unit, core::AnyError>
        Shutdown(core::SandboxId id) override;
    core::Expected<core::Unit, core::AnyError>
        Pause(core::SandboxId id) override;
    core::Expected<core::Unit, core::AnyError>
        Resume(core::SandboxId id) override;
    core::Expected<std::string, core::AnyError>
        Snapshot(core::SandboxId id, const std::string& out_dir) override;
    core::Expected<Handle, core::AnyError>
        Restore(LaunchPlan plan, const std::string& snapshot_dir) override;
    core::Expected<ExecResult, core::AnyError>
        Exec(core::SandboxId id, ExecSpec spec) override;

 private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_SANDBOX_H_
