// SPDX-License-Identifier: MIT
// Go: services/scheduler/internal/filter.go
#include "services/scheduler/internal.h"

namespace agentenv {
namespace services {
namespace scheduler {

static bool within_limit(const RichNode& n, const NodeResourceLimit& limit) {
    const NodeSnapshot& s = n.snapshot;

    if (limit.has_max_sandbox_count && s.sandbox_count > limit.max_sandbox_count)
        return false;
    if (limit.has_max_sandbox_starting_count &&
        s.sandbox_starting_count > limit.max_sandbox_starting_count)
        return false;
    if (limit.has_max_cpu_used_percent && s.cpu_percent > limit.max_cpu_used_percent)
        return false;
    if (limit.has_max_cpu_allocated_percent && s.cpu_count > 0) {
        uint32_t allocated_percent = s.allocated_cpu * 100 / s.cpu_count;
        if (allocated_percent > limit.max_cpu_allocated_percent) return false;
    }
    if (limit.has_max_memory_used_percent && s.memory_total_bytes > 0) {
        uint32_t used_percent =
            static_cast<uint32_t>(s.memory_used_bytes * 100 / s.memory_total_bytes);
        if (used_percent > limit.max_memory_used_percent) return false;
    }
    if (limit.has_max_memory_allocated_percent && s.memory_total_bytes > 0) {
        uint32_t allocated_percent =
            static_cast<uint32_t>(s.allocated_memory_bytes * 100 / s.memory_total_bytes);
        if (allocated_percent > limit.max_memory_allocated_percent) return false;
    }

    // "Including paused" ceilings.
    if (limit.has_max_sandbox_count_incl_paused) {
        uint32_t total = s.sandbox_count + s.paused_sandbox_count;
        if (total > limit.max_sandbox_count_incl_paused) return false;
    }
    if (limit.has_max_alloc_cpu_incl_paused) {
        uint32_t total = s.allocated_cpu + s.paused_allocated_cpu;
        if (total > limit.max_alloc_cpu_incl_paused) return false;
    }
    if (limit.has_max_alloc_mem_incl_paused) {
        uint64_t total = s.allocated_memory_bytes + s.paused_allocated_memory_bytes;
        if (total > limit.max_alloc_mem_incl_paused) return false;
    }
    return true;
}

std::vector<RichNode> FilterByResourceLimit(const std::vector<RichNode>& nodes,
                                            const NodeResourceLimit* limit) {
    if (limit == nullptr) return nodes;
    std::vector<RichNode> result;
    result.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        const RichNode& n = nodes[i];
        if (!n.has_snapshot) {
            // No heartbeat yet — cannot evaluate; keep the node.
            result.push_back(n);
            continue;
        }
        if (!within_limit(n, *limit)) continue;
        result.push_back(n);
    }
    return result;
}

}  // namespace scheduler
}  // namespace services
}  // namespace agentenv
