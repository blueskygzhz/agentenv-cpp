// SPDX-License-Identifier: MIT
// Rust: src/sandbox/mod.rs — the sandbox lifecycle types.
#ifndef AGENTENV_SANDBOX_TYPES_H_
#define AGENTENV_SANDBOX_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/identity.h"

namespace agentenv {
namespace sandbox {

/// Rust struct `crate::types::SandboxResources` (src/types/resources.rs).
/// Kept in the sandbox namespace (aggregated) since it is consumed by both
/// the sandbox backends and the orchestrator lifecycle events.
struct SandboxResources {
    uint32_t cpu_count     = 1;
    uint32_t memory_mib    = 128;
    uint32_t disk_size_mib = 1024;

    bool operator==(const SandboxResources& o) const {
        return cpu_count == o.cpu_count && memory_mib == o.memory_mib &&
               disk_size_mib == o.disk_size_mib;
    }
    bool operator!=(const SandboxResources& o) const { return !(*this == o); }
};

/// What the orchestrator hands the backend to boot a sandbox.
struct LaunchPlan {
    core::SandboxId  sandbox_id;
    std::string      template_id;
    std::string      rootfs_path;        // where the OCI unpack lives
    std::string      kernel_path;
    std::string      kernel_cmdline;
    int32_t          vcpus = 1;
    int64_t          mem_bytes = 512 * 1024 * 1024;
    std::vector<std::string> env_vars;
};

/// Runtime handle exposed by the backend.
struct Handle {
    core::SandboxId sandbox_id;
    std::string     backend_kind;   // "firecracker" | "mock"
    std::string     control_endpoint;  // e.g. unix socket / IP:port
    int64_t         boot_ms = 0;
};

/// Exec spec sent to the guest agent (envd) through the backend.
struct ExecSpec {
    std::vector<std::string> cmd;
    std::vector<std::string> env_vars;
    int32_t timeout_sec = 60;
};

struct ExecResult {
    int32_t     exit_code = 0;
    std::string stdout_output;
    std::string stderr_output;
};

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_TYPES_H_
