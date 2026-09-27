// SPDX-License-Identifier: MIT
// Go: services/scheduler/internal/strategy.go
#include "services/scheduler/internal.h"

#include <cstdlib>

namespace agentenv {
namespace services {
namespace scheduler {

core::Expected<RichNode, std::string>
RoundRobinStrategy::Select(const std::vector<RichNode>& nodes,
                           const ScheduleRequestHint* /*hint*/) {
    if (nodes.empty()) {
        return core::make_unexpected(std::string("no nodes available"));
    }
    std::lock_guard<std::mutex> g(mu_);
    uint64_t idx = ++next_;
    return nodes[(idx - 1) % nodes.size()];
}

core::Expected<RichNode, std::string>
RandomStrategy::Select(const std::vector<RichNode>& nodes,
                       const ScheduleRequestHint* /*hint*/) {
    if (nodes.empty()) {
        return core::make_unexpected(std::string("no nodes available"));
    }
    size_t i = static_cast<size_t>(std::rand()) % nodes.size();
    return nodes[i];
}

std::unique_ptr<Strategy> NewStrategy(const std::string& name) {
    if (name == "random") {
        return std::unique_ptr<Strategy>(new RandomStrategy());
    }
    // "round_robin" and default.
    return std::unique_ptr<Strategy>(new RoundRobinStrategy());
}

}  // namespace scheduler
}  // namespace services
}  // namespace agentenv
