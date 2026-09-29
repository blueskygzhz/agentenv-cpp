// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/metrics.rs
#ifndef AGENTENV_ORCHESTRATOR_METRICS_H_
#define AGENTENV_ORCHESTRATOR_METRICS_H_

#include <atomic>
#include <cstdint>

#include "agentenv/orchestrator/types.h"
#include "agentenv/sandbox/types.h"

namespace agentenv {
namespace orchestrator {

/// Rust struct `OrchestratorMetrics`.
struct OrchestratorMetrics {
    uint64_t create_successes = 0;
    uint64_t create_fails     = 0;
    uint32_t running_sandbox_count  = 0;
    uint32_t starting_sandbox_count = 0;
    uint32_t allocated_cpu          = 0;
    uint64_t allocated_memory_bytes = 0;
    uint32_t paused_sandbox_count   = 0;
    uint32_t paused_allocated_cpu   = 0;
    uint64_t paused_allocated_memory_bytes = 0;

    bool operator==(const OrchestratorMetrics& o) const {
        return create_successes == o.create_successes &&
               create_fails == o.create_fails &&
               running_sandbox_count == o.running_sandbox_count &&
               starting_sandbox_count == o.starting_sandbox_count &&
               allocated_cpu == o.allocated_cpu &&
               allocated_memory_bytes == o.allocated_memory_bytes &&
               paused_sandbox_count == o.paused_sandbox_count &&
               paused_allocated_cpu == o.paused_allocated_cpu &&
               paused_allocated_memory_bytes == o.paused_allocated_memory_bytes;
    }
    bool operator!=(const OrchestratorMetrics& o) const { return !(*this == o); }
};

/// Rust struct `OrchestratorCounters`.
class OrchestratorCounters {
 public:
    OrchestratorCounters() : create_successes_(0), create_fails_(0) {}
    /// Rust `record_create_success`.
    void RecordCreateSuccess(uint64_t n) {
        create_successes_.fetch_add(n, std::memory_order_relaxed);
    }
    /// Rust `record_create_fail`.
    void RecordCreateFail(uint64_t n) {
        create_fails_.fetch_add(n, std::memory_order_relaxed);
    }
    uint64_t CreateSuccesses() const {
        return create_successes_.load(std::memory_order_relaxed);
    }
    uint64_t CreateFails() const {
        return create_fails_.load(std::memory_order_relaxed);
    }

 private:
    std::atomic<uint64_t> create_successes_;
    std::atomic<uint64_t> create_fails_;
};

/// Rust struct `SandboxContribution` — runtime resource contribution of a
/// single sandbox in a given state.
///
/// Matches the comment-documented semantics in metrics.rs exactly:
/// - `running_sandbox_count`: Running | Pausing | Snapshotting | Forking | Killing
/// - `starting_sandbox_count`: Creating | Resuming
/// - allocated_* counts every state except Paused
/// - paused_* counts only Paused
struct SandboxContribution {
    uint32_t running_sandbox_count  = 0;
    uint32_t starting_sandbox_count = 0;
    uint32_t allocated_cpu          = 0;
    uint64_t allocated_memory_bytes = 0;
    uint32_t paused_sandbox_count   = 0;
    uint32_t paused_allocated_cpu   = 0;
    uint64_t paused_allocated_memory_bytes = 0;

    /// Rust `SandboxContribution::new(state, resources)`.
    static SandboxContribution New(SandboxState state,
                                   const sandbox::SandboxResources& resources);
};

/// Rust fn `aggregate_resource_metrics`.
void AggregateResourceMetrics(OrchestratorMetrics* metrics,
                              const SandboxContribution& contribution);

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_METRICS_H_
