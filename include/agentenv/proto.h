// SPDX-License-Identifier: MIT
// Rust: src/proto.rs — thin gRPC wire types re-exported into the crate root.
#ifndef AGENTENV_PROTO_H_
#define AGENTENV_PROTO_H_

#include <cstdint>
#include <string>
#include <vector>

namespace agentenv {
namespace proto {

/// Scheduler <-> Node RPC: node-side heartbeat payload.
/// Rust: scheduler.proto message `Heartbeat`.
struct Heartbeat {
    std::string node_id;
    uint32_t    running_sandboxes = 0;
    uint32_t    free_cpu_units    = 0;
    uint64_t    free_memory_bytes = 0;
};

/// Scheduler -> Node RPC: create-sandbox command.
/// Rust: scheduler.proto message `CreateSandboxCommand`.
struct CreateSandboxCommand {
    std::string sandbox_id;
    std::string template_id;
    std::vector<std::string> env_vars;
};

}  // namespace proto
}  // namespace agentenv
#endif  // AGENTENV_PROTO_H_
