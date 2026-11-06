// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/sandbox_metrics.rs — the retention cache and the
// concurrency rule for the sampling fan-out.
#ifndef AGENTENV_ORCHESTRATOR_SANDBOX_METRICS_H_
#define AGENTENV_ORCHESTRATOR_SANDBOX_METRICS_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/metrics.h"

namespace agentenv {
namespace orchestrator {

/// Rust `METRICS_PARALLELISM_FACTOR`.
const std::size_t kMetricsParallelismFactor = 5;

/// Rust `METRICS_REQUEST_TIMEOUT` — 200 ms per guest call.
///
/// Deliberately short: a sample is worth far less than the sampling loop
/// staying on schedule, and one wedged guest must not stall the sweep for
/// every other sandbox on the node.
const int64_t kMetricsRequestTimeoutMs = 200;

/// Rust `metrics_concurrency` — `running_sandboxes.div_ceil(5)`.
///
/// Scales with the node's load rather than being a constant, so a node with
/// three sandboxes opens one guest connection at a time while a busy one
/// still finishes its sweep inside the interval. Rounds **up** so that a
/// non-zero sandbox count never yields a concurrency of zero, which in
/// upstream's `for_each_concurrent` would mean *unbounded* rather than none.
std::size_t MetricsConcurrency(std::size_t running_sandboxes);

/// Rust `struct SandboxMetrics` — per-sandbox sample history.
///
/// Named `...Cache` here because `sandbox::SandboxMetric` (the sample) is
/// already in scope in callers and the plural/singular pair is too easy to
/// misread in C++ where there is no module prefix to tell them apart.
class SandboxMetricsCache {
 public:
    /// Rust `prune` — drops samples older than `retention` and then forgets
    /// any sandbox left with none.
    ///
    /// `now_monotonic_ns` and the stored `collected_at` are on the **host's
    /// monotonic clock**, deliberately not on the sample's own timestamp:
    /// retention is about how long *this process* has held the sample, and
    /// measuring it against a guest's wall clock would let a guest with a fast
    /// clock have its history evicted instantly, or one with a slow clock keep
    /// its samples forever.
    ///
    /// Dropping the empty entry matters: without it the map accumulates one
    /// key per sandbox ever seen, which on a long-lived node is a leak that
    /// grows with lifetime rather than with load.
    void Prune(int64_t now_monotonic_ns, int64_t retention_ns);

    /// Rust `insert`.
    void Insert(const core::SandboxId& id, uint64_t generation, int64_t collected_at_monotonic_ns,
                const sandbox::SandboxMetric& metric);

    /// Rust `has_generation` — whether the newest sample for `id` came from
    /// this incarnation.
    ///
    /// Used to skip an event-triggered sample when the periodic sweep already
    /// took one for the same generation, so a burst of create/resume events
    /// does not multiply guest calls.
    bool HasGeneration(const core::SandboxId& id, uint64_t generation) const;

    /// Rust `history` — every retained sample in `[start, end]`, oldest first.
    ///
    /// The bounds are the *guest's* seconds-since-epoch, because that is what
    /// the client asked about — the opposite clock from the one `Prune` uses,
    /// and the reason both are stored.
    std::vector<sandbox::SandboxMetric> History(const core::SandboxId& id,
                                                const core::Optional<int64_t>& start_unix_secs,
                                                const core::Optional<int64_t>& end_unix_secs) const;

    /// Rust `latest` — the newest sample, but only if it belongs to
    /// `generation`.
    ///
    /// The generation check is the point: after a fork or a resume the sandbox
    /// is a different incarnation behind the same id, and reporting the
    /// previous one's last sample as current would show an operator stale
    /// numbers with no indication they are stale. Returning nothing is the
    /// honest answer until the new incarnation has been sampled.
    core::Optional<sandbox::SandboxMetric> Latest(const core::SandboxId& id,
                                                  uint64_t generation) const;

    /// Test accessor: how many sandboxes still have retained samples.
    std::size_t TrackedSandboxesForTest() const { return samples_.size(); }

 private:
    /// Rust `struct Sample`.
    struct Sample {
        uint64_t               generation = 0;
        int64_t                collected_at_monotonic_ns = 0;
        sandbox::SandboxMetric metric;
    };

    /// A deque because retention evicts from the front while sampling appends
    /// to the back; a vector would make every prune an O(n) shift of the
    /// entire history.
    std::map<core::SandboxId, std::deque<Sample> > samples_;
};

}  // namespace orchestrator
}  // namespace agentenv

#endif  // AGENTENV_ORCHESTRATOR_SANDBOX_METRICS_H_
