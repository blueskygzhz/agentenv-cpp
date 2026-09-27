// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/metrics.rs
#ifndef AGENTENV_ORCHESTRATOR_METRICS_H_
#define AGENTENV_ORCHESTRATOR_METRICS_H_

#include <atomic>
#include <cstdint>

#include "agentenv/orchestrator/store.h"
#include "agentenv/orchestrator/types.h"

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
};

/// Rust struct `OrchestratorCounters`.
class OrchestratorCounters {
 public:
    OrchestratorCounters() : create_successes_(0), create_fails_(0) {}
    void RecordCreateSuccess(uint64_t n) {
        create_successes_.fetch_add(n, std::memory_order_relaxed);
    }
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

/// Rust struct `SandboxContribution`.
struct SandboxContribution {
    uint32_t running_sandbox_count  = 0;
    uint32_t starting_sandbox_count = 0;
    uint32_t allocated_cpu          = 0;
    uint64_t allocated_memory_bytes = 0;
    uint32_t paused_sandbox_count   = 0;
    uint32_t paused_allocated_cpu   = 0;
    uint64_t paused_allocated_memory_bytes = 0;

    static SandboxContribution FromMeta(const SandboxMetadata& m);
};

/// Rust fn `aggregate_resource_metrics`.
void AggregateResourceMetrics(OrchestratorMetrics* metrics,
                              const SandboxContribution& c);

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_METRICS_H_
