// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/proxy.rs
#ifndef AGENTENV_ORCHESTRATOR_PROXY_H_
#define AGENTENV_ORCHESTRATOR_PROXY_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/orchestrator/types.h"

namespace agentenv {
namespace orchestrator {

/// Rust struct `ProxyTarget` (contains an Ipv4Addr).
struct ProxyTarget {
    // Dotted IPv4 string form, e.g. "10.0.0.42". Kept as string for zero-dep.
    std::string ip;
    ProxyTarget() {}
    /// Rust `ProxyTarget::new(host_interaction_ip)`.
    explicit ProxyTarget(std::string ipv4) : ip(std::move(ipv4)) {}

    // Rust `#[derive(PartialEq, Eq)]`.
    bool operator==(const ProxyTarget& o) const { return ip == o.ip; }
    bool operator!=(const ProxyTarget& o) const { return !(*this == o); }
};

/// Rust enum `ProxyLookupResult`.
enum class ProxyLookupKind {
    Ready,
    NotFound,
    Paused,
    Unavailable,
    RouteMissing,
};

struct ProxyLookupResult {
    ProxyLookupKind kind = ProxyLookupKind::NotFound;
    ProxyTarget     target;                   // when kind == Ready
    bool            auto_resume = false;      // when kind == Paused
    /// Rust `Unavailable(SandboxState)`.
    SandboxState    unavailable_state = SandboxState::Creating;

    static ProxyLookupResult Ready(ProxyTarget t);
    static ProxyLookupResult NotFound();
    static ProxyLookupResult Paused(bool auto_resume);
    static ProxyLookupResult Unavailable(SandboxState state);
    static ProxyLookupResult RouteMissing();

    bool operator==(const ProxyLookupResult& o) const;
    bool operator!=(const ProxyLookupResult& o) const { return !(*this == o); }
};

/// Rust struct `ProxyRoute`.
struct ProxyRoute {
    ProxyTarget target;
    uint64_t    version = 0;
    int64_t     updated_at_ms = 0;
};

/// Rust struct `ProxyRouteTable`.
class ProxyRouteTable {
 public:
    /// Rust `upsert`.
    ProxyRoute Upsert(const core::SandboxId& id, ProxyTarget target, uint64_t version);
    /// Rust `remove`.
    core::Optional<ProxyRoute> Remove(const core::SandboxId& id);
    /// Rust `route`.
    core::Optional<ProxyRoute> Route(const core::SandboxId& id) const;
    /// Rust `#[cfg(test)] proxy_target`.
    core::Optional<ProxyTarget> ProxyTargetOf(const core::SandboxId& id) const;

 private:
    mutable std::mutex mu_;
    std::map<std::string, ProxyRoute> routes_;
};

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_PROXY_H_
