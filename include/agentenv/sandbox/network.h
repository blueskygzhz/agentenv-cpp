// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/{mod,manager,policy,slot,address_plan,iptables_util}.rs
#ifndef AGENTENV_SANDBOX_NETWORK_H_
#define AGENTENV_SANDBOX_NETWORK_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace sandbox {
namespace network {

/// Rust struct `SandboxNetworkEgressPolicy`.
struct SandboxNetworkEgressPolicy {
    // Simplified: allow/deny CIDR lists.
    std::vector<std::string> allow_cidrs;
    std::vector<std::string> deny_cidrs;
};

/// Rust struct `BaseSandboxNetworkPolicy`.
struct BaseSandboxNetworkPolicy {
    bool                      internet_egress = true;
    SandboxNetworkEgressPolicy egress;
};

/// Rust struct `SandboxNetworkPolicy`.
struct SandboxNetworkPolicy {
    BaseSandboxNetworkPolicy base;
};

/// Rust struct `AddressPlan` — deterministic per-slot address allocation.
struct AddressPlan {
    uint32_t slot = 0;
    std::string host_ip;
    std::string guest_ip;
    std::string tap_ifname;
};

/// Rust struct `NetworkSlot`.
struct NetworkSlot {
    uint32_t index = 0;
    bool     in_use = false;
    AddressPlan plan;
};

/// Rust trait `NetworkManager`.
class NetworkManager {
 public:
    virtual ~NetworkManager() {}
    virtual core::Expected<NetworkSlot, std::string> Allocate() = 0;
    virtual core::Expected<core::Unit, std::string>  Release(uint32_t slot) = 0;
    virtual core::Expected<core::Unit, std::string>  Apply(const NetworkSlot& slot,
                                                           const SandboxNetworkPolicy& policy) = 0;
};

/// Rust `prepare_runtime`.
core::Expected<core::Unit, std::string> PrepareRuntime();

// ==== Rust: src/sandbox/network/address_plan.rs ====

/// A parsed IPv4 CIDR "a.b.c.d/prefix" (zero-dependency; string<->u32 helpers).
struct Ipv4Cidr {
    uint32_t network = 0;   // network address as host-order u32
    uint8_t  prefix = 32;

    static core::Expected<Ipv4Cidr, std::string> Parse(const std::string& cidr);
    uint64_t Size() const { return 1ULL << (32 - prefix); }
    std::string ToString() const;
};

/// Format a host-order u32 as dotted IPv4.
std::string Ipv4ToString(uint32_t addr);
/// Parse dotted IPv4 into a host-order u32. Returns false on error.
bool Ipv4Parse(const std::string& s, uint32_t* out);

/// Rust struct `NetworkAddressPlan` — deterministic per-slot address allocation.
class NetworkAddressPlan {
 public:
    NetworkAddressPlan(Ipv4Cidr host_interaction, Ipv4Cidr veth, Ipv4Cidr vm_link)
        : host_interaction_(host_interaction), veth_(veth), vm_link_(vm_link) {}

    /// Rust `default()` — the built-in internal pools.
    static NetworkAddressPlan Default();

    /// Rust `slot_ips(idx)` — (host_interaction_ip, veth_host_ip, veth_vm_ip).
    core::Expected<core::Unit, std::string>
        SlotIps(uint32_t idx, uint32_t* host_interaction_ip,
                uint32_t* veth_host_ip, uint32_t* veth_vm_ip) const;

    /// Rust `vm_ip()` / `tap_ip()` — fixed addresses in the VM-link CIDR.
    uint32_t VmIp() const;
    uint32_t TapIp() const;

    uint8_t  VmLinkPrefix() const { return vm_link_.prefix; }
    const Ipv4Cidr& HostInteractionCidr() const { return host_interaction_; }

    /// Rust `internal_egress_denied_cidrs()`.
    std::vector<std::string> InternalEgressDeniedCidrs() const;

 private:
    Ipv4Cidr host_interaction_;
    Ipv4Cidr veth_;
    Ipv4Cidr vm_link_;
};

/// iptables helpers — Rust src/sandbox/network/iptables_util.rs.
namespace iptables {
core::Expected<core::Unit, std::string>
    ApplyRule(const std::string& chain, const std::string& rule);
core::Expected<core::Unit, std::string>
    ClearChain(const std::string& chain);
}

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_NETWORK_H_
