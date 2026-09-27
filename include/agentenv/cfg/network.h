// SPDX-License-Identifier: MIT
// Rust: src/cfg/network.rs
#ifndef AGENTENV_CFG_NETWORK_H_
#define AGENTENV_CFG_NETWORK_H_

#include <cstddef>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/toml.h"
#include "agentenv/sandbox/network.h"

namespace agentenv {
namespace cfg {

/// Rust `NETWORK_MAX_SLOTS`.
const std::size_t kNetworkMaxSlots = 32768;

/// Rust `FIXED_NETWORK_VM_LINK_CIDR`.
///
/// Part of the snapshot ABI: fresh boots pass this VM/tap link through the
/// kernel `ip=` argument and snapshot resume does not re-run boot args. Do not
/// make this configurable unless vmLinkCidr is persisted in snapshot manifests.
extern const char* const kFixedNetworkVmLinkCidr;

/// Rust struct `NetworkEgressConfig`.
struct NetworkEgressConfig {
    /// Node-level destinations that sandbox egress policy cannot override.
    std::vector<std::string> always_denied_cidrs;

    NetworkEgressConfig();
};

/// Rust struct `NetworkInternalConfig`.
struct NetworkInternalConfig {
    std::string host_interaction_cidr = "10.11.0.0/16";
    std::string veth_cidr = "10.12.0.0/16";
};

/// Rust struct `ResolvedNetworkInternalConfig`.
struct ResolvedNetworkInternalConfig {
    sandbox::network::Ipv4Cidr host_interaction_cidr;
    sandbox::network::Ipv4Cidr veth_cidr;
    sandbox::network::Ipv4Cidr vm_link_cidr;
};

/// Rust struct `NetworkConfig`.
struct NetworkConfig {
    NetworkEgressConfig egress;
    NetworkInternalConfig internal;

    /// Rust `NetworkConfig::validate`.
    static core::Expected<core::Unit, std::string> Validate(const NetworkConfig& config);
    /// Rust `NetworkConfig::resolved_internal`.
    static core::Expected<ResolvedNetworkInternalConfig, std::string> ResolvedInternal(
        const NetworkConfig& config);

    /// Overlay TOML keys under the `network.` prefix onto `*this`.
    core::Expected<core::Unit, std::string> LoadFrom(const core::TomlTable& table);
};

/// Rust `normalize_dns_name` — lowercases and returns nothing when invalid.
core::Optional<std::string> NormalizeDnsName(const std::string& domain);

/// Rust `is_valid_dns_name`.
bool IsValidDnsName(const std::string& domain);

/// Two CIDRs share at least one address. `sandbox::network::Ipv4Cidr` has no `overlaps`
/// (Rust gets it from the `ipnetwork` crate), so it lives here to avoid
/// touching the already-tested sandbox module.
bool Ipv4CidrOverlaps(const sandbox::network::Ipv4Cidr& left, const sandbox::network::Ipv4Cidr& right);

}  // namespace cfg
}  // namespace agentenv
#endif  // AGENTENV_CFG_NETWORK_H_
