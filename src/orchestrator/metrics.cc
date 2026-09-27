// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/metrics.rs
#include "agentenv/orchestrator/metrics.h"

namespace agentenv {
namespace orchestrator {

static inline uint32_t sat_add_u32(uint32_t a, uint32_t b) {
    uint64_t v = static_cast<uint64_t>(a) + b;
    if (v > 0xFFFFFFFFu) {
        return 0xFFFFFFFFu;
    }
    return static_cast<uint32_t>(v);
}
static inline uint64_t sat_add_u64(uint64_t a, uint64_t b) {
    uint64_t v = a + b;
    if (v < a) return static_cast<uint64_t>(-1);
    return v;
}

SandboxContribution SandboxContribution::FromMeta(const SandboxMetadata& m) {
    SandboxContribution c;
    const bool is_paused = false;  // Rust: matches Paused variant (skeleton)
    const bool counts_running = (m.state == LifecyclePhase::Running ||
                                 m.state == LifecyclePhase::Snapshotting ||
                                 m.state == LifecyclePhase::Stopping);
    const bool counts_starting = (m.state == LifecyclePhase::Created ||
                                  m.state == LifecyclePhase::Reserved ||
                                  m.state == LifecyclePhase::Booting);
    const uint64_t memory_bytes =
        static_cast<uint64_t>(m.memory_mib) * 1024ULL * 1024ULL;
    c.running_sandbox_count  = counts_running ? 1u : 0u;
    c.starting_sandbox_count = counts_starting ? 1u : 0u;
    c.allocated_cpu          = is_paused ? 0u : m.cpu_count;
    c.allocated_memory_bytes = is_paused ? 0u : memory_bytes;
    c.paused_sandbox_count   = is_paused ? 1u : 0u;
    c.paused_allocated_cpu   = is_paused ? m.cpu_count : 0u;
    c.paused_allocated_memory_bytes = is_paused ? memory_bytes : 0u;
    return c;
}

void AggregateResourceMetrics(OrchestratorMetrics* m, const SandboxContribution& c) {
    m->running_sandbox_count  = sat_add_u32(m->running_sandbox_count, c.running_sandbox_count);
    m->starting_sandbox_count = sat_add_u32(m->starting_sandbox_count, c.starting_sandbox_count);
    m->allocated_cpu          = sat_add_u32(m->allocated_cpu, c.allocated_cpu);
    m->allocated_memory_bytes = sat_add_u64(m->allocated_memory_bytes, c.allocated_memory_bytes);
    m->paused_sandbox_count   = sat_add_u32(m->paused_sandbox_count, c.paused_sandbox_count);
    m->paused_allocated_cpu   = sat_add_u32(m->paused_allocated_cpu, c.paused_allocated_cpu);
    m->paused_allocated_memory_bytes =
        sat_add_u64(m->paused_allocated_memory_bytes, c.paused_allocated_memory_bytes);
}

}  // namespace orchestrator
}  // namespace agentenv
