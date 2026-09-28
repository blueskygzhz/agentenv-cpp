// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/policy.rs
//
// The sandbox egress policy and the iptables chains that enforce it. The rule
// ordering here is the security property: platform denies are installed ahead
// of user rules so a sandbox cannot reach another sandbox's namespace, and
// user allows precede user denies so an explicit allow wins.
#ifndef AGENTENV_SANDBOX_NETWORK_POLICY_H_
#define AGENTENV_SANDBOX_NETWORK_POLICY_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/network/iptables_util.h"

namespace agentenv {
namespace sandbox {
namespace network {

/// Rust `ALL_INTERNET_TRAFFIC_CIDR`.
extern const char* const kAllInternetTrafficCidr;

/// Rust `EGRESS_CHAIN` / `USER_EGRESS_CHAIN` / `EGRESS_PROXY_CHAIN`.
extern const char* const kEgressChain;
extern const char* const kUserEgressChain;
extern const char* const kEgressProxyChain;

/// Rust `DOMAIN_INSPECTION_TCP_PORTS`.
const std::vector<uint16_t>& DomainInspectionTcpPorts();

/// Rust `enum BaseSandboxNetworkPolicy`.
enum class BaseSandboxNetworkPolicy {
    /// Rust `#[default] Default` — outbound is allowed except for the static
    /// namespace rejects.
    Default,
    Allow,
    Deny,
};

core::Optional<BaseSandboxNetworkPolicy> ParseBasePolicy(const std::string& value);
std::string BasePolicyToString(BaseSandboxNetworkPolicy policy);

/// An IP network, either v4 or v6.
///
/// Rust uses `ipnetwork::IpNetwork`. Only v4 is enforced by the iptables rules
/// below; v6 entries are kept so a policy round-trips on the wire without
/// pretending v6 is filtered.
struct IpNetwork {
    bool is_v4 = true;
    /// Host-order address for v4; the 16 raw bytes for v6.
    uint32_t v4_address = 0;
    uint8_t v6_address[16];
    uint8_t prefix = 32;

    IpNetwork();

    /// Parses `a.b.c.d`, `a.b.c.d/len`, an IPv6 literal, or `v6/len`. A bare
    /// address takes the full-length prefix, as Rust does.
    static core::Expected<IpNetwork, std::string> Parse(const std::string& text);

    /// Canonical `address/prefix` form. This is the wire format, so it must be
    /// stable: `8.8.8.8` and `8.8.8.8/32` both render as `8.8.8.8/32` and
    /// therefore deduplicate against each other.
    std::string ToString() const;

    /// v4 containment. Always false for a v6 network, matching the Rust
    /// `matches!(cidr, IpNetwork::V4(n) if n.contains(ip))` guards.
    bool ContainsV4(uint32_t ip) const;

    bool operator==(const IpNetwork& o) const;
    bool operator!=(const IpNetwork& o) const { return !(*this == o); }
};

/// Rust `struct SandboxNetworkEgressPolicy`.
struct SandboxNetworkEgressPolicy {
    std::vector<IpNetwork> allowed_cidrs;
    std::vector<std::string> allowed_domains;
    std::vector<IpNetwork> denied_cidrs;

    /// Rust `SandboxNetworkEgressPolicy::new`.
    ///
    /// `allow_out` entries may be either an IP/CIDR or a domain pattern;
    /// `deny_out` entries must be IP/CIDR, because a domain deny cannot be
    /// enforced without resolving it. Entries are deduplicated *after*
    /// normalisation and keep their first-seen order.
    static core::Expected<SandboxNetworkEgressPolicy, std::string> New(
        const core::Optional<std::vector<std::string> >& allow_out,
        const core::Optional<std::vector<std::string> >& deny_out);

    /// Rust `has_explicit_rules`.
    bool HasExplicitRules() const;
    /// Rust `has_domain_allow_rules`.
    bool HasDomainAllowRules() const;

    core::Json ToJson() const;
    static core::Expected<SandboxNetworkEgressPolicy, std::string> FromJson(
        const core::Json& json);

    bool operator==(const SandboxNetworkEgressPolicy& o) const;
    bool operator!=(const SandboxNetworkEgressPolicy& o) const { return !(*this == o); }
};

/// Rust `struct SandboxNetworkPolicy`.
struct SandboxNetworkPolicy {
    /// Rust `#[serde(default = "default_allow_public_traffic")]` — an older
    /// record without the field describes a public sandbox.
    bool allow_public_traffic = true;
    BaseSandboxNetworkPolicy base_policy = BaseSandboxNetworkPolicy::Default;
    SandboxNetworkEgressPolicy egress;

    /// Rust `SandboxNetworkPolicy::new`.
    static SandboxNetworkPolicy New(bool allow_public_traffic,
                                    BaseSandboxNetworkPolicy base_policy,
                                    const SandboxNetworkEgressPolicy& egress);

    /// Rust `runtime_policy` — empty when nothing has to be installed.
    core::Optional<SandboxNetworkPolicy> RuntimePolicy() const;

    /// Rust `has_explicit_egress_rules`.
    bool HasExplicitEgressRules() const;
    /// Rust `has_runtime_egress_rules`.
    bool HasRuntimeEgressRules() const;
    /// Rust `has_domain_allow_rules`.
    bool HasDomainAllowRules() const;

    /// Rust `requires_egress_proxy` — domain allowlists are the first
    /// proxy-backed capability; future ones extend this decision here.
    bool RequiresEgressProxy() const;

    /// Rust `egress_proxy_tcp_ports`.
    std::vector<uint16_t> EgressProxyTcpPorts() const;

    /// Rust `is_ip_allowed`.
    bool IsIpAllowed(uint32_t ip) const;

    /// Rust `is_domain_allowed`. Passing an absent `resolved_ip` checks only
    /// the hostname branch, which is what the proxy does before resolving.
    bool IsDomainAllowed(const std::string& hostname,
                         const core::Optional<uint32_t>& resolved_ip) const;

    core::Json ToJson() const;
    static core::Expected<SandboxNetworkPolicy, std::string> FromJson(const core::Json& json);

    bool operator==(const SandboxNetworkPolicy& o) const;
    bool operator!=(const SandboxNetworkPolicy& o) const { return !(*this == o); }
};

/// Rust `PLATFORM_DENIED_CIDRS`, as an explicit parameter instead of a
/// process-wide `OnceLock` over the global config. The Rust version reads the
/// global config lazily; making it an argument keeps `is_ip_allowed` testable
/// and free of global state.
void SetPlatformDeniedCidrs(const std::vector<std::string>& cidrs);
const std::vector<IpNetwork>& PlatformDeniedCidrs();

/// Rust `domain_matches` — case-insensitive, with `*` and `*.suffix` support.
bool DomainMatches(const std::string& hostname, const std::string& pattern);

/// Rust `normalize_domain_pattern`.
core::Expected<std::string, std::string> NormalizeDomainPattern(const std::string& pattern);

/// Rust `try_normalize_ip_or_cidr` — an empty optional means "not an IP or
/// CIDR", which the caller treats as a domain; an error means it looked like a
/// CIDR but did not parse.
core::Expected<core::Optional<IpNetwork>, std::string> TryNormalizeIpOrCidr(
    const std::string& text);

/// Rust `build_static_egress_commands`.
std::vector<IptablesRestoreCommand> BuildStaticEgressCommands(
    uint32_t guest_dns_ip, const std::vector<std::string>& internal_egress_denied_cidrs,
    const std::vector<std::string>& node_always_denied_cidrs);

/// Rust `build_user_egress_commands`.
std::vector<IptablesRestoreCommand> BuildUserEgressCommands(
    const SandboxNetworkPolicy& policy, bool replace);

/// Rust `build_egress_proxy_commands`.
std::vector<IptablesRestoreCommand> BuildEgressProxyCommands(
    const SandboxNetworkPolicy& policy, uint16_t egress_proxy_port);

/// Rust `initialize_namespace_egress_chain`.
core::Expected<core::Unit, std::string> InitializeNamespaceEgressChain(
    uint32_t guest_dns_ip, const std::vector<std::string>& internal_egress_denied_cidrs,
    const std::vector<std::string>& node_always_denied_cidrs);

/// Rust `set_namespace_egress_policy`.
core::Expected<core::Unit, std::string> SetNamespaceEgressPolicy(
    const core::Optional<SandboxNetworkPolicy>& policy, uint16_t egress_proxy_port);

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_NETWORK_POLICY_H_
