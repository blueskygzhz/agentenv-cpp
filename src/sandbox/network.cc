// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/*.rs
//   - address_plan.rs is a pure IPv4 计算 module and is ported faithfully here.
//   - the rtnetlink/iptables side-effects remain TODOs (need root + netlink).
#include "agentenv/sandbox/network.h"

#include <cstdio>
#include <cstdlib>

namespace agentenv {
namespace sandbox {
namespace network {

// ---------------- IPv4 helpers ----------------
std::string Ipv4ToString(uint32_t a) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  (a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
    return buf;
}

bool Ipv4Parse(const std::string& s, uint32_t* out) {
    unsigned o0, o1, o2, o3;
    char extra;
    int n = std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &o0, &o1, &o2, &o3, &extra);
    if (n != 4) return false;
    if (o0 > 255 || o1 > 255 || o2 > 255 || o3 > 255) return false;
    *out = (o0 << 24) | (o1 << 16) | (o2 << 8) | o3;
    return true;
}

core::Expected<Ipv4Cidr, std::string> Ipv4Cidr::Parse(const std::string& cidr) {
    std::string::size_type slash = cidr.find('/');
    if (slash == std::string::npos) {
        return core::make_unexpected(std::string("cidr missing '/'"));
    }
    std::string ip = cidr.substr(0, slash);
    std::string pfx = cidr.substr(slash + 1);
    uint32_t addr = 0;
    if (!Ipv4Parse(ip, &addr)) {
        return core::make_unexpected(std::string("bad ipv4 in cidr"));
    }
    long p = std::strtol(pfx.c_str(), nullptr, 10);
    if (p < 0 || p > 32) {
        return core::make_unexpected(std::string("bad prefix in cidr"));
    }
    Ipv4Cidr c;
    c.prefix = static_cast<uint8_t>(p);
    // Normalize to the network address (mask off host bits).
    uint32_t mask = (p == 0) ? 0u : (0xFFFFFFFFu << (32 - p));
    c.network = addr & mask;
    return c;
}

std::string Ipv4Cidr::ToString() const {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%s/%u", Ipv4ToString(network).c_str(), prefix);
    return buf;
}

// ---------------- NetworkAddressPlan ----------------
namespace {
// Rust `network_ip_at`: base + offset, bounds-checked against network size.
core::Expected<uint32_t, std::string> NetworkIpAt(const Ipv4Cidr& net, uint32_t offset) {
    if (static_cast<uint64_t>(offset) >= net.Size()) {
        return core::make_unexpected(std::string("offset outside network"));
    }
    return net.network + offset;
}
}  // namespace

NetworkAddressPlan NetworkAddressPlan::Default() {
    // Mirrors Rust NetworkConfig::default internal pools.
    Ipv4Cidr host_interaction = Ipv4Cidr::Parse("10.0.0.0/16").value();
    Ipv4Cidr veth= Ipv4Cidr::Parse("10.1.0.0/16").value();
    Ipv4Cidr vm_link          = Ipv4Cidr::Parse("169.254.0.0/30").value();
    return NetworkAddressPlan(host_interaction, veth, vm_link);
}

core::Expected<core::Unit, std::string>
NetworkAddressPlan::SlotIps(uint32_t idx, uint32_t* host_interaction_ip,
                            uint32_t* veth_host_ip, uint32_t* veth_vm_ip) const {
    auto hi = NetworkIpAt(host_interaction_, idx);
    if (!hi.ok()) return core::make_unexpected(hi.error());

    // veth_offset = idx * 2, checked for overflow.
    if (idx > (0xFFFFFFFFu / 2)) {
        return core::make_unexpected(std::string("veth offset overflow"));
    }
    uint32_t veth_offset = idx * 2;
    auto vh = NetworkIpAt(veth_, veth_offset);
    if (!vh.ok()) return core::make_unexpected(vh.error());
    auto vv = NetworkIpAt(veth_, veth_offset + 1);
    if (!vv.ok()) return core::make_unexpected(vv.error());

    if (host_interaction_ip) *host_interaction_ip = hi.value();
    if (veth_host_ip) *veth_host_ip = vh.value();
    if (veth_vm_ip) *veth_vm_ip = vv.value();
    return core::Unit{};
}

uint32_t NetworkAddressPlan::VmIp() const {
    return NetworkIpAt(vm_link_, 1).value();
}
uint32_t NetworkAddressPlan::TapIp() const {
    return NetworkIpAt(vm_link_, 2).value();
}

// Rust `vm_link_mask` — `vm_link_cidr.mask()`.
uint32_t NetworkAddressPlan::VmLinkMask() const {
    // A /0 would shift by 32, which is undefined for uint32_t.
    if (vm_link_.prefix == 0) return 0;
    return 0xFFFFFFFFu << (32 - vm_link_.prefix);
}

namespace {

// Rust `network_conflict_pattern` — keep only as many leading octets as the
// prefix length makes meaningful.
std::string NetworkConflictPattern(const Ipv4Cidr& cidr) {
    const uint32_t a = (cidr.network >> 24) & 0xFFu;
    const uint32_t b = (cidr.network >> 16) & 0xFFu;
    const uint32_t c = (cidr.network >> 8) & 0xFFu;

    std::ostringstream os;
    if (cidr.prefix <= 8) {
        os << a << ".";
    } else if (cidr.prefix <= 16) {
        os << a << "." << b << ".";
    } else {
        os << a << "." << b << "." << c << ".";
    }
    return os.str();
}

}  // namespace

// Rust `conflict_patterns`.
std::vector<std::string> NetworkAddressPlan::ConflictPatterns() const {
    std::vector<std::string> out;
    out.push_back(NetworkConflictPattern(host_interaction_));
    out.push_back(NetworkConflictPattern(veth_));
    out.push_back(NetworkConflictPattern(vm_link_));
    return out;
}

std::vector<std::string> NetworkAddressPlan::InternalEgressDeniedCidrs() const {
    std::vector<std::string> out;
    out.push_back(host_interaction_.ToString());
    out.push_back(veth_.ToString());
    out.push_back(vm_link_.ToString());
    return out;
}

// ---------------- side-effecting runtime (TODO: root + netlink) ----------------
core::Expected<core::Unit, std::string> PrepareRuntime() {
    // TODO: create bridge, tap pool, ip_forward, iptables NAT.
    return core::Unit{};
}

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
