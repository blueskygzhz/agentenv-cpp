// SPDX-License-Identifier: MIT
// Rust: src/observability/model.rs — the projected node-observability view.
#ifndef AGENTENV_OBSERVABILITY_MODEL_H_
#define AGENTENV_OBSERVABILITY_MODEL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/observability/host.h"

namespace agentenv {
namespace observability {

/// Rust struct `MachineInfo`.
struct MachineInfo {
    std::string cpu_family;
    std::string cpu_model;
    std::string cpu_model_name;
    std::string cpu_architecture;
    /// Rust `Option<String>` — populated out-of-band by the reporter from
    /// `cpu-template-helper`, not by `detect_machine_info`.
    core::Optional<std::string> cpu_config_json;
};

/// Rust struct `NodeMetricsSnapshot` — orchestrator counters joined with the
/// latest sampled host snapshot.
struct NodeMetricsSnapshot {
    uint32_t allocated_cpu          = 0;
    uint64_t allocated_memory_bytes = 0;
    uint32_t cpu_percent            = 0;
    uint32_t cpu_count              = 0;
    uint64_t memory_used_bytes      = 0;
    uint64_t memory_total_bytes     = 0;
    std::vector<DiskMetric> disks;
    /// Rust: CPU reservation of all paused sandboxes on the node, summed.
    uint32_t paused_allocated_cpu          = 0;
    /// Rust: memory reservation of all paused sandboxes on the node, summed.
    uint64_t paused_allocated_memory_bytes = 0;
};

/// Rust struct `NodeSnapshot` — request-time snapshot for the admin/node APIs.
struct NodeSnapshot {
    std::string version;
    std::string commit;
    std::string node_id;
    std::string service_instance_id;
    core::Uuid  cluster_id;
    MachineInfo machine_info;
    uint32_t    sandbox_count = 0;
    std::vector<core::SandboxId> sandbox_ids;
    NodeMetricsSnapshot metrics;
    uint64_t create_successes = 0;
    uint64_t create_fails     = 0;
    uint32_t sandbox_starting_count = 0;
    /// Rust: number of sandboxes currently in the Paused state on this node.
    uint32_t paused_sandbox_count  = 0;
};

}  // namespace observability
}  // namespace agentenv
#endif  // AGENTENV_OBSERVABILITY_MODEL_H_
