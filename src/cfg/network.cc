// SPDX-License-Identifier: MIT
// Rust: src/cfg/network.rs
#include "agentenv/cfg/network.h"

#include <sstream>

#include "agentenv/core/dns.h"

namespace agentenv {
namespace cfg {

const char* const kFixedNetworkVmLinkCidr = "169.254.0.20/30";

NetworkEgressConfig::NetworkEgressConfig() {
    // Rust `#[config(default = [...])]`, in upstream order.
    always_denied_cidrs.push_back("10.0.0.0/8");
    always_denied_cidrs.push_back("100.64.0.0/10");
    always_denied_cidrs.push_back("127.0.0.0/8");
    always_denied_cidrs.push_back("169.254.0.0/16");
    always_denied_cidrs.push_back("172.16.0.0/12");
    always_denied_cidrs.push_back("192.168.0.0/16");
}

bool Ipv4CidrOverlaps(const sandbox::network::Ipv4Cidr& left, const sandbox::network::Ipv4Cidr& right) {
    // Two CIDRs overlap iff one contains the other's network address, which for
    // prefix-aligned ranges reduces to comparing under the shorter prefix.
    const uint8_t prefix = left.prefix < right.prefix ? left.prefix : right.prefix;
    if (prefix == 0) return true;
    const uint32_t mask = prefix == 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1u);
    return (left.network & mask) == (right.network & mask);
}

core::Expected<sandbox::network::NetworkAddressPlan, std::string>
NetworkAddressPlanFromConfig(const NetworkConfig& config) {
    // Rust: `let internal = NetworkConfig::resolved_internal(config)?;`
    core::Expected<ResolvedNetworkInternalConfig, std::string> internal =
        NetworkConfig::ResolvedInternal(config);
    if (!internal.ok()) return core::make_unexpected(internal.error());
    return sandbox::network::NetworkAddressPlan(internal.value().host_interaction_cidr,
                                                internal.value().veth_cidr,
                                                internal.value().vm_link_cidr);
}

bool IsValidDnsName(const std::string& domain) {
    return core::IsValidDnsName(domain);
}

core::Optional<std::string> NormalizeDnsName(const std::string& domain) {
    // Implementation moved to `core::dns` so `sandbox::network::policy` can
    // share it; see the note in that header.
    return core::NormalizeDnsName(domain);
}

core::Expected<ResolvedNetworkInternalConfig, std::string> NetworkConfig::ResolvedInternal(
    const NetworkConfig& config) {
    core::Expected<sandbox::network::Ipv4Cidr, std::string> host_interaction =
        sandbox::network::Ipv4Cidr::Parse(config.internal.host_interaction_cidr);
    if (!host_interaction.has_value()) {
        return core::make_unexpected(std::string("invalid network.internal.host_interaction_cidr"));
    }
    core::Expected<sandbox::network::Ipv4Cidr, std::string> veth =
        sandbox::network::Ipv4Cidr::Parse(config.internal.veth_cidr);
    if (!veth.has_value()) {
        return core::make_unexpected(std::string("invalid network.internal.veth_cidr"));
    }
    core::Expected<sandbox::network::Ipv4Cidr, std::string> vm_link =
        sandbox::network::Ipv4Cidr::Parse(kFixedNetworkVmLinkCidr);
    if (!vm_link.has_value()) {
        return core::make_unexpected(std::string("invalid fixed VM link CIDR"));
    }
    if (vm_link.value().prefix != 30) {
        return core::make_unexpected(std::string("fixed VM link CIDR must be a /30 network"));
    }

    const uint64_t max_slots = static_cast<uint64_t>(kNetworkMaxSlots);
    const uint64_t max_slot_index = max_slots - 1;

    if (host_interaction.value().Size() < max_slots) {
        std::ostringstream oss;
        oss << "network.internal.host_interaction_cidr (" << host_interaction.value().ToString()
            << ") must contain at least " << max_slots << " addresses to cover slot indexes 1..="
            << max_slot_index << "; slot 0 is reserved";
        return core::make_unexpected(oss.str());
    }
    if (veth.value().Size() < max_slots * 2) {
        std::ostringstream oss;
        oss << "network.internal.veth_cidr (" << veth.value().ToString()
            << ") must contain at least " << (max_slots * 2)
            << " addresses to cover two veth addresses per slot through slot " << max_slot_index;
        return core::make_unexpected(oss.str());
    }

    struct Pair {
        const char* left_name;
        const sandbox::network::Ipv4Cidr* left;
        const char* right_name;
        const sandbox::network::Ipv4Cidr* right;
    };
    const Pair pairs[3] = {
        {"network.internal.host_interaction_cidr", &host_interaction.value(),
         "network.internal.veth_cidr", &veth.value()},
        {"network.internal.host_interaction_cidr", &host_interaction.value(),
         "fixed VM link CIDR", &vm_link.value()},
        {"network.internal.veth_cidr", &veth.value(), "fixed VM link CIDR", &vm_link.value()},
    };
    for (int i = 0; i < 3; ++i) {
        if (Ipv4CidrOverlaps(*pairs[i].left, *pairs[i].right)) {
            std::ostringstream oss;
            oss << pairs[i].left_name << " (" << pairs[i].left->ToString() << ") must not overlap "
                << pairs[i].right_name << " (" << pairs[i].right->ToString() << ")";
            return core::make_unexpected(oss.str());
        }
    }

    ResolvedNetworkInternalConfig resolved;
    resolved.host_interaction_cidr = host_interaction.value();
    resolved.veth_cidr = veth.value();
    resolved.vm_link_cidr = vm_link.value();
    return resolved;
}

core::Expected<core::Unit, std::string> NetworkConfig::Validate(const NetworkConfig& config) {
    for (std::size_t i = 0; i < config.egress.always_denied_cidrs.size(); ++i) {
        const std::string& cidr = config.egress.always_denied_cidrs[i];
        core::Expected<sandbox::network::Ipv4Cidr, std::string> parsed = sandbox::network::Ipv4Cidr::Parse(cidr);
        if (!parsed.has_value()) {
            std::ostringstream oss;
            oss << "invalid network.egress.always_denied_cidrs entry \"" << cidr << "\"";
            return core::make_unexpected(oss.str());
        }
    }

    core::Expected<ResolvedNetworkInternalConfig, std::string> resolved = ResolvedInternal(config);
    if (!resolved.has_value()) return core::make_unexpected(resolved.error());
    return core::Unit();
}

core::Expected<core::Unit, std::string> NetworkConfig::LoadFrom(const core::TomlTable& table) {
    {
        const core::TomlValue* value = table.Find("network.egress.always_denied_cidrs");
        if (value != nullptr) {
            core::Expected<std::vector<std::string>, std::string> parsed = value->AsStringArray();
            if (!parsed.has_value()) {
                return core::make_unexpected(
                    std::string("network.egress.always_denied_cidrs: ") + parsed.error());
            }
            egress.always_denied_cidrs = parsed.value();
        }
    }
    {
        const core::TomlValue* value = table.Find("network.internal.host_interaction_cidr");
        if (value != nullptr) {
            core::Expected<std::string, std::string> parsed = value->AsString();
            if (!parsed.has_value()) {
                return core::make_unexpected(
                    std::string("network.internal.host_interaction_cidr: ") + parsed.error());
            }
            internal.host_interaction_cidr = parsed.value();
        }
    }
    {
        const core::TomlValue* value = table.Find("network.internal.veth_cidr");
        if (value != nullptr) {
            core::Expected<std::string, std::string> parsed = value->AsString();
            if (!parsed.has_value()) {
                return core::make_unexpected(std::string("network.internal.veth_cidr: ") +
                                             parsed.error());
            }
            internal.veth_cidr = parsed.value();
        }
    }
    return core::Unit();
}

}  // namespace cfg
}  // namespace agentenv
