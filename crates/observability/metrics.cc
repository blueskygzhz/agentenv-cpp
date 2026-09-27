// SPDX-License-Identifier: MIT
#include "agentenv/observability/metrics.h"
namespace agentenv {
namespace observability {

void CounterIncr(const std::string&, int64_t) {}
void GaugeSet(const std::string&, double) {}
void HistogramObserve(const std::string&, double) {}

}  // namespace observability
}  // namespace agentenv
