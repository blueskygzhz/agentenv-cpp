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

// ---- ProxyLookupResult constructors (Rust enum variants) ------------------

ProxyLookupResult ProxyLookupResult::Ready(ProxyTarget t) {
    ProxyLookupResult r;
    r.kind   = ProxyLookupKind::Ready;
    r.target = std::move(t);
    return r;
}
ProxyLookupResult ProxyLookupResult::NotFound() {
    ProxyLookupResult r;
    r.kind = ProxyLookupKind::NotFound;
    return r;
}
ProxyLookupResult ProxyLookupResult::Paused(bool auto_resume) {
    ProxyLookupResult r;
    r.kind        = ProxyLookupKind::Paused;
    r.auto_resume = auto_resume;
    return r;
}
ProxyLookupResult ProxyLookupResult::Unavailable(SandboxState state) {
    ProxyLookupResult r;
    r.kind              = ProxyLookupKind::Unavailable;
    r.unavailable_state = state;
    return r;
}
ProxyLookupResult ProxyLookupResult::RouteMissing() {
    ProxyLookupResult r;
    r.kind = ProxyLookupKind::RouteMissing;
    return r;
}

// Rust `#[derive(PartialEq, Eq)]` — only the active variant's payload counts.
bool ProxyLookupResult::operator==(const ProxyLookupResult& o) const {
    if (kind != o.kind) return false;
    switch (kind) {
        case ProxyLookupKind::Ready:       return target == o.target;
        case ProxyLookupKind::Paused:      return auto_resume == o.auto_resume;
        case ProxyLookupKind::Unavailable: return unavailable_state == o.unavailable_state;
        case ProxyLookupKind::NotFound:
        case ProxyLookupKind::RouteMissing: return true;
    }
    return false;
}

// ---- ProxyRouteTable -------------------------------------------------------

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

// Rust `#[cfg(test)] fn proxy_target`.
core::Optional<ProxyTarget> ProxyRouteTable::ProxyTargetOf(
    const core::SandboxId& id) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = routes_.find(id.ToString());
    if (it == routes_.end()) return core::Optional<ProxyTarget>();
    return core::Optional<ProxyTarget>(it->second.target);
}

}  // namespace orchestrator
}  // namespace agentenv
