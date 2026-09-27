// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/proxy.rs
#include "agentenv/orchestrator/proxy.h"

#include <chrono>

namespace agentenv {
namespace orchestrator {

static int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

ProxyRoute ProxyRouteTable::Upsert(const core::SandboxId& id,
                                   ProxyTarget target,
                                   uint64_t version) {
    std::lock_guard<std::mutex> g(mu_);
    ProxyRoute r;
    r.target = std::move(target);
    r.version = version;
    r.updated_at_ms = now_ms();
    routes_[id.ToString()] = r;
    return r;
}

core::Optional<ProxyRoute> ProxyRouteTable::Remove(const core::SandboxId& id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = routes_.find(id.ToString());
    if (it == routes_.end()) return core::Optional<ProxyRoute>();
    ProxyRoute r = it->second;
    routes_.erase(it);
    return core::Optional<ProxyRoute>(r);
}

core::Optional<ProxyRoute> ProxyRouteTable::Route(const core::SandboxId& id) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = routes_.find(id.ToString());
    if (it == routes_.end()) return core::Optional<ProxyRoute>();
    return core::Optional<ProxyRoute>(it->second);
}

}  // namespace orchestrator
}  // namespace agentenv
