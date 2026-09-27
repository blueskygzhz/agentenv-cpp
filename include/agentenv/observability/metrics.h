// SPDX-License-Identifier: MIT
// Rust: crates/observability/  — a metrics facade (`metrics` crate).
#ifndef AGENTENV_OBSERVABILITY_METRICS_H_
#define AGENTENV_OBSERVABILITY_METRICS_H_

#include <cstdint>
#include <string>

namespace agentenv {
namespace observability {

// Simple counters/gauges facade. The default impl writes to stderr; a real
// impl plugs in Prometheus / statsd.
void CounterIncr(const std::string& name, int64_t delta = 1);
void GaugeSet(const std::string& name, double value);
void HistogramObserve(const std::string& name, double value);

}  // namespace observability
}  // namespace agentenv
#endif  // AGENTENV_OBSERVABILITY_METRICS_H_
