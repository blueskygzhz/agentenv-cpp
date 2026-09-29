// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/metrics.rs
#include "agentenv/orchestrator/metrics.h"

namespace agentenv {
namespace orchestrator {

namespace {

static inline uint32_t sat_add_u32(uint32_t a, uint32_t b) {
    uint64_t v = static_cast<uint64_t>(a) + b;
    return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(v);
}
static inline uint64_t sat_add_u64(uint64_t a, uint64_t b) {
    uint64_t v = a + b;
    return v < a ? static_cast<uint64_t>(-1) : v;
}

}  // namespace

// Rust `SandboxContribution::new(state, resources)`.
//
// running_sandbox_count: Running | Pausing | Snapshotting | Forking | Killing
// starting_sandbox_count: Creating | Resuming
// allocated_*: every state except Paused
// paused_*: Paused only
SandboxContribution SandboxContribution::New(
    SandboxState state,
    const sandbox::SandboxResources& resources) {
    const bool is_paused =
        (state == SandboxState::Paused);
    const bool counts_as_running =
        (state == SandboxState::Running   ||
         state == SandboxState::Pausing   ||
         state == SandboxState::Snapshotting ||
         state == SandboxState::Forking   ||
         state == SandboxState::Killing);
    const bool counts_as_starting =
        (state == SandboxState::Creating  ||
         state == SandboxState::Resuming);
    const uint64_t memory_bytes =
        static_cast<uint64_t>(resources.memory_mib) * 1024ULL * 1024ULL;

    SandboxContribution c;
    c.running_sandbox_count  = counts_as_running  ? 1u : 0u;
    c.starting_sandbox_count = counts_as_starting ? 1u : 0u;
    c.allocated_cpu          = is_paused ? 0u : resources.cpu_count;
    c.allocated_memory_bytes = is_paused ? 0u : memory_bytes;
    c.paused_sandbox_count   = is_paused ? 1u : 0u;
    c.paused_allocated_cpu   = is_paused ? resources.cpu_count : 0u;
    c.paused_allocated_memory_bytes = is_paused ? memory_bytes : 0u;
    return c;
}

void AggregateResourceMetrics(OrchestratorMetrics* m,
                              const SandboxContribution& c) {
    m->running_sandbox_count  = sat_add_u32(m->running_sandbox_count,  c.running_sandbox_count);
    m->starting_sandbox_count = sat_add_u32(m->starting_sandbox_count, c.starting_sandbox_count);
    m->allocated_cpu          = sat_add_u32(m->allocated_cpu,          c.allocated_cpu);
    m->allocated_memory_bytes = sat_add_u64(m->allocated_memory_bytes, c.allocated_memory_bytes);
    m->paused_sandbox_count   = sat_add_u32(m->paused_sandbox_count,   c.paused_sandbox_count);
    m->paused_allocated_cpu   = sat_add_u32(m->paused_allocated_cpu,   c.paused_allocated_cpu);
    m->paused_allocated_memory_bytes =
        sat_add_u64(m->paused_allocated_memory_bytes, c.paused_allocated_memory_bytes);
}

}  // namespace orchestrator
}  // namespace agentenv

