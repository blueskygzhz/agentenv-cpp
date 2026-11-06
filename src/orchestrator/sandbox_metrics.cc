// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/sandbox_metrics.rs
#include "agentenv/orchestrator/sandbox_metrics.h"

#include <algorithm>

namespace agentenv {
namespace orchestrator {

std::size_t MetricsConcurrency(std::size_t running_sandboxes) {
    // `div_ceil`, written as the safe form: `(n + 4) / 5` would overflow for a
    // count within four of `size_t`'s maximum. That cannot happen with real
    // sandbox counts, but the ceiling is one line either way and the
    // overflowing spelling is the one that silently returns a small number.
    std::size_t whole = running_sandboxes / kMetricsParallelismFactor;
    if (running_sandboxes % kMetricsParallelismFactor != 0) ++whole;
    return whole;
}

void SandboxMetricsCache::Prune(int64_t now_monotonic_ns, int64_t retention_ns) {
    std::map<core::SandboxId, std::deque<Sample> >::iterator it = samples_.begin();
    while (it != samples_.end()) {
        std::deque<Sample>& history = it->second;
        // `>=`, as upstream: a sample exactly at the retention edge is already
        // expired, so the window a client sees is a half-open one and does not
        // flicker by a sample depending on the sweep's timing.
        while (!history.empty() &&
               now_monotonic_ns - history.front().collected_at_monotonic_ns >= retention_ns) {
            history.pop_front();
        }
        if (history.empty()) {
            // Erase through the returned iterator: dropping the entry is what
            // keeps the map sized by live sandboxes rather than by every
            // sandbox the process has ever seen.
            std::map<core::SandboxId, std::deque<Sample> >::iterator dead = it;
            ++it;
            samples_.erase(dead);
        } else {
            ++it;
        }
    }
}

void SandboxMetricsCache::Insert(const core::SandboxId& id, uint64_t generation,
                                 int64_t collected_at_monotonic_ns,
                                 const sandbox::SandboxMetric& metric) {
    Sample sample;
    sample.generation = generation;
    sample.collected_at_monotonic_ns = collected_at_monotonic_ns;
    sample.metric = metric;
    samples_[id].push_back(sample);
}

bool SandboxMetricsCache::HasGeneration(const core::SandboxId& id, uint64_t generation) const {
    const std::map<core::SandboxId, std::deque<Sample> >::const_iterator at = samples_.find(id);
    if (at == samples_.end() || at->second.empty()) return false;
    return at->second.back().generation == generation;
}

std::vector<sandbox::SandboxMetric> SandboxMetricsCache::History(
    const core::SandboxId& id, const core::Optional<int64_t>& start_unix_secs,
    const core::Optional<int64_t>& end_unix_secs) const {
    std::vector<sandbox::SandboxMetric> result;
    const std::map<core::SandboxId, std::deque<Sample> >::const_iterator at = samples_.find(id);
    // An unknown sandbox is an empty history, not an error: the caller has
    // already established the sandbox exists, and "no samples yet" is a normal
    // state for one that has just started.
    if (at == samples_.end()) return result;

    const int64_t kNanosPerSecond = 1000000000LL;
    for (std::deque<Sample>::const_iterator it = at->second.begin(); it != at->second.end();
         ++it) {
        // Truncating division to whole seconds, matching
        // `sample.metric.timestamp.timestamp()`. The bounds arrive from the
        // client as seconds, so comparing at finer resolution would exclude a
        // sample whose second is exactly the requested bound.
        const int64_t seconds = it->metric.timestamp.unix_nanos / kNanosPerSecond;
        if (start_unix_secs.has_value() && seconds < *start_unix_secs) continue;
        if (end_unix_secs.has_value() && seconds > *end_unix_secs) continue;
        result.push_back(it->metric);
    }

    // Sorted by the sample's own timestamp rather than returned in insertion
    // order: the samples come from the guest, and a guest clock that stepped
    // backwards would otherwise hand a client a non-monotonic series.
    // `stable_sort` so samples sharing a second keep collection order.
    std::stable_sort(result.begin(), result.end(),
                     [](const sandbox::SandboxMetric& left, const sandbox::SandboxMetric& right) {
                         return left.timestamp < right.timestamp;
                     });
    return result;
}

core::Optional<sandbox::SandboxMetric> SandboxMetricsCache::Latest(const core::SandboxId& id,
                                                                   uint64_t generation) const {
    const std::map<core::SandboxId, std::deque<Sample> >::const_iterator at = samples_.find(id);
    if (at == samples_.end() || at->second.empty()) {
        return core::Optional<sandbox::SandboxMetric>();
    }
    const Sample& newest = at->second.back();
    // Newest *by insertion*, not by timestamp -- and only if it is this
    // incarnation's. A fork or resume bumps the generation, and the previous
    // incarnation's final sample reported as current would be stale numbers
    // with nothing marking them stale.
    if (newest.generation != generation) return core::Optional<sandbox::SandboxMetric>();
    return core::Optional<sandbox::SandboxMetric>(newest.metric);
}

}  // namespace orchestrator
}  // namespace agentenv
