// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/policy.rs
#include "agentenv/sandbox/network/policy.h"

#include <arpa/inet.h>

#include <cstdio>
#include <cstring>
#include <set>

#include "agentenv/core/dns.h"
#include "agentenv/sandbox/network.h"

namespace agentenv {
namespace sandbox {
namespace network {

const char* const kAllInternetTrafficCidr = "0.0.0.0/0";
const char* const kEgressChain = "AGENTENV-EGRESS";
const char* const kUserEgressChain = "AGENTENV-USER-EGRESS";
const char* const kEgressProxyChain = "AGENTENV-EGRESS-PROXY";

namespace {

std::vector<IpNetwork> g_platform_denied_cidrs;

/// Rust `append_egress_command`.
IptablesRestoreCommand AppendEgressCommand(const std::string& rule) {
    return IptablesRestoreCommand::Append("filter", kEgressChain, rule);
}

/// Rust `append_user_egress_command`.
IptablesRestoreCommand AppendUserEgressCommand(const std::string& rule) {
    return IptablesRestoreCommand::Append("filter", kUserEgressChain, rule);
}

std::string TrimTrailingDots(const std::string& value) {
    std::size_t end = value.size();
    while (end > 0 && value[end - 1] == '.') --end;
    return value.substr(0, end);
}

bool EqualsIgnoreAsciiCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

const std::vector<uint16_t>& DomainInspectionTcpPorts() {
    // Rust `DOMAIN_INSPECTION_TCP_PORTS`: only the two ports the proxy can
    // actually inspect a hostname on.
    static std::vector<uint16_t> ports;
    if (ports.empty()) {
        ports.push_back(80);
        ports.push_back(443);
    }
    return ports;
}

// ---- IpNetwork ------------------------------------------------------------

IpNetwork::IpNetwork() { std::memset(v6_address, 0, sizeof(v6_address)); }

core::Expected<IpNetwork, std::string> IpNetwork::Parse(const std::string& text) {
    std::string address = text;
    core::Optional<uint8_t> explicit_prefix;

    const std::size_t slash = text.rfind('/');
    if (slash != std::string::npos) {
        address = text.substr(0, slash);
        const std::string prefix_text = text.substr(slash + 1);
        if (prefix_text.empty()) {
            return core::make_unexpected(std::string("missing prefix length"));
        }
        for (std::size_t i = 0; i < prefix_text.size(); ++i) {
            if (prefix_text[i] < '0' || prefix_text[i] > '9') {
                return core::make_unexpected(std::string("invalid prefix length"));
            }
        }
        const long value = std::strtol(prefix_text.c_str(), NULL, 10);
        if (value < 0 || value > 128) {
            return core::make_unexpected(std::string("prefix length out of range"));
        }
        explicit_prefix = core::Optional<uint8_t>(static_cast<uint8_t>(value));
    }

    IpNetwork network;
    in_addr v4;
    if (::inet_pton(AF_INET, address.c_str(), &v4) == 1) {
        network.is_v4 = true;
        network.v4_address = ntohl(v4.s_addr);
        network.prefix = explicit_prefix.has_value() ? *explicit_prefix : 32;
        if (network.prefix > 32) {
            return core::make_unexpected(std::string("IPv4 prefix length out of range"));
        }
        return network;
    }

    in6_addr v6;
    if (::inet_pton(AF_INET6, address.c_str(), &v6) == 1) {
        network.is_v4 = false;
        std::memcpy(network.v6_address, v6.s6_addr, sizeof(network.v6_address));
        network.prefix = explicit_prefix.has_value() ? *explicit_prefix : 128;
        return network;
    }

    return core::make_unexpected(std::string("invalid IP or CIDR entry \"") + text + "\"");
}

std::string IpNetwork::ToString() const {
    char buffer[INET6_ADDRSTRLEN + 8];
    if (is_v4) {
        in_addr v4;
        v4.s_addr = htonl(v4_address);
        char address[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &v4, address, sizeof(address));
        std::snprintf(buffer, sizeof(buffer), "%s/%u", address,
                      static_cast<unsigned>(prefix));
        return buffer;
    }
    in6_addr v6;
    std::memcpy(v6.s6_addr, v6_address, sizeof(v6_address));
    char address[INET6_ADDRSTRLEN];
    ::inet_ntop(AF_INET6, &v6, address, sizeof(address));
    std::snprintf(buffer, sizeof(buffer), "%s/%u", address, static_cast<unsigned>(prefix));
    return buffer;
}

bool IpNetwork::ContainsV4(uint32_t ip) const {
    if (!is_v4) return false;
    if (prefix == 0) return true;
    if (prefix > 32) return false;
    const uint32_t mask =
        prefix == 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1u);
    return (ip & mask) == (v4_address & mask);
}

bool IpNetwork::operator==(const IpNetwork& o) const {
    if (is_v4 != o.is_v4 || prefix != o.prefix) return false;
    if (is_v4) return v4_address == o.v4_address;
    return std::memcmp(v6_address, o.v6_address, sizeof(v6_address)) == 0;
}

// ---- platform denies ------------------------------------------------------

void SetPlatformDeniedCidrs(const std::vector<std::string>& cidrs) {
    g_platform_denied_cidrs.clear();
    for (std::size_t i = 0; i < cidrs.size(); ++i) {
        // Rust `filter_map(|cidr| cidr.parse::<Ipv4Network>().ok())`: an
        // unparsable entry is skipped rather than failing startup.
        const core::Expected<IpNetwork, std::string> parsed = IpNetwork::Parse(cidrs[i]);
        if (parsed.ok() && parsed.value().is_v4) {
            g_platform_denied_cidrs.push_back(parsed.value());
        }
    }
}

const std::vector<IpNetwork>& PlatformDeniedCidrs() { return g_platform_denied_cidrs; }

namespace {

/// Rust `is_absolutely_denied` — platform and slot denies that no user rule
/// can override, which is what stops one sandbox reaching another's namespace.
bool IsAbsolutelyDenied(uint32_t ip) {
    const std::vector<IpNetwork>& denied = PlatformDeniedCidrs();
    for (std::size_t i = 0; i < denied.size(); ++i) {
        if (denied[i].ContainsV4(ip)) return true;
    }
    return false;
}

}  // namespace

// ---- domain matching ------------------------------------------------------

bool DomainMatches(const std::string& hostname, const std::string& pattern) {
    const std::string host = TrimTrailingDots(hostname);
    const std::string pat = TrimTrailingDots(pattern);

    if (pat == "*") return true;
    if (EqualsIgnoreAsciiCase(pat, host)) return true;

    if (pat.size() > 2 && pat[0] == '*' && pat[1] == '.') {
        const std::string suffix = pat.substr(2);
        if (host.size() < suffix.size()) return false;
        const std::size_t suffix_start = host.size() - suffix.size();
        const std::string prefix = host.substr(0, suffix_start);
        const std::string host_suffix = host.substr(suffix_start);
        // The prefix must be a whole label boundary, so `*.example.com` does
        // not match `notexample.com`. Byte-indexed on purpose: Rust uses
        // `get(..)` which returns None mid-codepoint, and the extra label-dot
        // check makes a non-ASCII host fall out the same way.
        return !prefix.empty() && prefix[prefix.size() - 1] == '.' &&
               EqualsIgnoreAsciiCase(host_suffix, suffix);
    }
    return false;
}

core::Expected<std::string, std::string> NormalizeDomainPattern(const std::string& pattern) {
    bool wildcard = false;
    std::string domain = pattern;
    if (pattern.size() >= 2 && pattern[0] == '*' && pattern[1] == '.') {
        wildcard = true;
        domain = pattern.substr(2);
    }
    const core::Optional<std::string> normalized = core::NormalizeDnsName(domain);
    if (!normalized.has_value()) {
        return core::make_unexpected(std::string("invalid DNS domain pattern"));
    }
    return wildcard ? ("*." + *normalized) : *normalized;
}

core::Expected<core::Optional<IpNetwork>, std::string> TryNormalizeIpOrCidr(
    const std::string& text) {
    const core::Expected<IpNetwork, std::string> parsed = IpNetwork::Parse(text);
    if (parsed.ok()) return core::Optional<IpNetwork>(parsed.value());

    // Rust distinguishes these two: a value containing '/' was meant to be a
    // CIDR, so a parse failure is an error rather than "this is a domain".
    if (text.find('/') != std::string::npos) {
        return core::make_unexpected(std::string("invalid IP or CIDR entry \"") + text + "\"");
    }
    return core::Optional<IpNetwork>();
}

// ---- SandboxNetworkEgressPolicy ------------------------------------------

core::Expected<SandboxNetworkEgressPolicy, std::string> SandboxNetworkEgressPolicy::New(
    const core::Optional<std::vector<std::string> >& allow_out,
    const core::Optional<std::vector<std::string> >& deny_out) {
    SandboxNetworkEgressPolicy policy;
    // Deduplicate on the *normalised* form so `8.8.8.8` and `8.8.8.8/32`
    // collapse, while preserving first-seen order for rule precedence.
    std::set<std::string> seen_allowed_cidrs;
    std::set<std::string> seen_allowed_domains;
    std::set<std::string> seen_denied_cidrs;

    if (allow_out.has_value()) {
        for (std::size_t i = 0; i < allow_out->size(); ++i) {
            const std::string& entry = (*allow_out)[i];
            const core::Expected<core::Optional<IpNetwork>, std::string> cidr =
                TryNormalizeIpOrCidr(entry);
            if (!cidr.ok()) return core::make_unexpected(cidr.error());

            if (cidr.value().has_value()) {
                const std::string key = cidr.value()->ToString();
                if (seen_allowed_cidrs.insert(key).second) {
                    policy.allowed_cidrs.push_back(*cidr.value());
                }
                continue;
            }

            const core::Expected<std::string, std::string> domain =
                NormalizeDomainPattern(entry);
            if (!domain.ok()) {
                return core::make_unexpected(std::string("invalid allowOut domain entry \"") +
                                             entry + "\": " + domain.error());
            }
            if (seen_allowed_domains.insert(domain.value()).second) {
                policy.allowed_domains.push_back(domain.value());
            }
        }
    }

    if (deny_out.has_value()) {
        for (std::size_t i = 0; i < deny_out->size(); ++i) {
            const std::string& entry = (*deny_out)[i];
            const core::Expected<core::Optional<IpNetwork>, std::string> cidr =
                TryNormalizeIpOrCidr(entry);
            if (!cidr.ok()) return core::make_unexpected(cidr.error());
            if (!cidr.value().has_value()) {
                // A domain deny cannot be enforced without resolving it, and
                // resolution is attacker-influenced, so it is refused.
                return core::make_unexpected(std::string("denyOut entry \"") + entry +
                                             "\" must be an IP address or CIDR block");
            }
            const std::string key = cidr.value()->ToString();
            if (seen_denied_cidrs.insert(key).second) {
                policy.denied_cidrs.push_back(*cidr.value());
            }
        }
    }

    return policy;
}

bool SandboxNetworkEgressPolicy::HasExplicitRules() const {
    return !allowed_cidrs.empty() || !allowed_domains.empty() || !denied_cidrs.empty();
}

bool SandboxNetworkEgressPolicy::HasDomainAllowRules() const {
    return !allowed_domains.empty();
}

core::Json SandboxNetworkEgressPolicy::ToJson() const {
    core::JsonObject object;
    core::JsonArray allowed;
    for (std::size_t i = 0; i < allowed_cidrs.size(); ++i) {
        allowed.push_back(core::Json(allowed_cidrs[i].ToString()));
    }
    core::JsonArray domains;
    for (std::size_t i = 0; i < allowed_domains.size(); ++i) {
        domains.push_back(core::Json(allowed_domains[i]));
    }
    core::JsonArray denied;
    for (std::size_t i = 0; i < denied_cidrs.size(); ++i) {
        denied.push_back(core::Json(denied_cidrs[i].ToString()));
    }
    // Rust serialises the structured CIDRs back as strings, so the wire format
    // is unchanged from the original string-based representation.
    object["allowed_cidrs"] = core::Json(allowed);
    object["allowed_domains"] = core::Json(domains);
    object["denied_cidrs"] = core::Json(denied);
    return core::Json(object);
}

core::Expected<SandboxNetworkEgressPolicy, std::string>
SandboxNetworkEgressPolicy::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("egress policy must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    SandboxNetworkEgressPolicy policy;

    struct Target {
        const char* field;
        std::vector<IpNetwork>* out;
    };
    const Target targets[] = {
        {"allowed_cidrs", &policy.allowed_cidrs},
        {"denied_cidrs", &policy.denied_cidrs},
    };
    for (std::size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
        const core::JsonObject::const_iterator it = fields.find(targets[t].field);
        if (it == fields.end()) continue;
        if (it->second.kind() != core::Json::Kind::Array) {
            return core::make_unexpected(std::string("field `") + targets[t].field +
                                         "` must be an array");
        }
        const core::JsonArray& values = it->second.as_array();
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i].kind() != core::Json::Kind::String) {
                return core::make_unexpected(std::string("field `") + targets[t].field +
                                             "` must contain strings");
            }
            const core::Expected<IpNetwork, std::string> parsed =
                IpNetwork::Parse(values[i].as_string());
            if (!parsed.ok()) return core::make_unexpected(parsed.error());
            targets[t].out->push_back(parsed.value());
        }
    }

    const core::JsonObject::const_iterator domains = fields.find("allowed_domains");
    if (domains != fields.end()) {
        if (domains->second.kind() != core::Json::Kind::Array) {
            return core::make_unexpected(std::string("field `allowed_domains` must be an array"));
        }
        const core::JsonArray& values = domains->second.as_array();
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i].kind() != core::Json::Kind::String) {
                return core::make_unexpected(
                    std::string("field `allowed_domains` must contain strings"));
            }
            policy.allowed_domains.push_back(values[i].as_string());
        }
    }

    return policy;
}

bool SandboxNetworkEgressPolicy::operator==(const SandboxNetworkEgressPolicy& o) const {
    return allowed_cidrs == o.allowed_cidrs && allowed_domains == o.allowed_domains &&
           denied_cidrs == o.denied_cidrs;
}

// ---- BaseSandboxNetworkPolicy --------------------------------------------

core::Optional<BaseSandboxNetworkPolicy> ParseBasePolicy(const std::string& value) {
    if (value == "Default") return core::Optional<BaseSandboxNetworkPolicy>(
        BaseSandboxNetworkPolicy::Default);
    if (value == "Allow") return core::Optional<BaseSandboxNetworkPolicy>(
        BaseSandboxNetworkPolicy::Allow);
    if (value == "Deny") return core::Optional<BaseSandboxNetworkPolicy>(
        BaseSandboxNetworkPolicy::Deny);
    return core::Optional<BaseSandboxNetworkPolicy>();
}

std::string BasePolicyToString(BaseSandboxNetworkPolicy policy) {
    switch (policy) {
        case BaseSandboxNetworkPolicy::Allow:
            return "Allow";
        case BaseSandboxNetworkPolicy::Deny:
            return "Deny";
        case BaseSandboxNetworkPolicy::Default:
        default:
            return "Default";
    }
}

// ---- SandboxNetworkPolicy ------------------------------------------------

SandboxNetworkPolicy SandboxNetworkPolicy::New(bool allow_public_traffic,
                                               BaseSandboxNetworkPolicy base_policy,
                                               const SandboxNetworkEgressPolicy& egress) {
    SandboxNetworkPolicy policy;
    policy.allow_public_traffic = allow_public_traffic;
    policy.base_policy = base_policy;
    policy.egress = egress;
    return policy;
}

core::Optional<SandboxNetworkPolicy> SandboxNetworkPolicy::RuntimePolicy() const {
    if (!HasRuntimeEgressRules()) return core::Optional<SandboxNetworkPolicy>();
    return core::Optional<SandboxNetworkPolicy>(*this);
}

bool SandboxNetworkPolicy::HasExplicitEgressRules() const {
    return egress.HasExplicitRules();
}

bool SandboxNetworkPolicy::HasRuntimeEgressRules() const {
    return base_policy == BaseSandboxNetworkPolicy::Deny || HasExplicitEgressRules();
}

bool SandboxNetworkPolicy::HasDomainAllowRules() const {
    return egress.HasDomainAllowRules();
}

bool SandboxNetworkPolicy::RequiresEgressProxy() const { return HasDomainAllowRules(); }

std::vector<uint16_t> SandboxNetworkPolicy::EgressProxyTcpPorts() const {
    if (HasDomainAllowRules()) return DomainInspectionTcpPorts();
    return std::vector<uint16_t>();
}

bool SandboxNetworkPolicy::IsIpAllowed(uint32_t ip) const {
    // Platform denies come first and cannot be overridden by a user allow.
    if (IsAbsolutelyDenied(ip)) return false;

    for (std::size_t i = 0; i < egress.allowed_cidrs.size(); ++i) {
        if (egress.allowed_cidrs[i].ContainsV4(ip)) return true;
    }
    for (std::size_t i = 0; i < egress.denied_cidrs.size(); ++i) {
        if (egress.denied_cidrs[i].ContainsV4(ip)) return false;
    }

    // A domain allowlist is opt-in: once set, an address that matched no
    // allowed domain must not fall through to the base policy, or the
    // allowlist would be advisory.
    if (HasDomainAllowRules()) return false;

    return base_policy != BaseSandboxNetworkPolicy::Deny;
}

bool SandboxNetworkPolicy::IsDomainAllowed(const std::string& hostname,
                                           const core::Optional<uint32_t>& resolved_ip) const {
    if (hostname.empty()) return false;

    bool matched = false;
    for (std::size_t i = 0; i < egress.allowed_domains.size(); ++i) {
        if (DomainMatches(hostname, egress.allowed_domains[i])) {
            matched = true;
            break;
        }
    }
    if (!matched) return false;

    // A user CIDR deny does not override an explicit domain allow (matching
    // E2B's precedence), but the absolute platform denies still do.
    if (!resolved_ip.has_value()) return true;
    return !IsAbsolutelyDenied(*resolved_ip);
}

core::Json SandboxNetworkPolicy::ToJson() const {
    core::JsonObject object;
    object["allow_public_traffic"] = core::Json(allow_public_traffic);
    object["base_policy"] = core::Json(BasePolicyToString(base_policy));
    object["egress"] = egress.ToJson();
    return core::Json(object);
}

core::Expected<SandboxNetworkPolicy, std::string> SandboxNetworkPolicy::FromJson(
    const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("network policy must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    SandboxNetworkPolicy policy;

    // `#[serde(default = "default_allow_public_traffic")]`: an older record
    // without the field describes a public sandbox.
    const core::JsonObject::const_iterator public_traffic =
        fields.find("allow_public_traffic");
    if (public_traffic != fields.end()) {
        if (public_traffic->second.kind() != core::Json::Kind::Bool) {
            return core::make_unexpected(
                std::string("field `allow_public_traffic` must be a boolean"));
        }
        policy.allow_public_traffic = public_traffic->second.as_bool();
    }

    const core::JsonObject::const_iterator base = fields.find("base_policy");
    if (base == fields.end()) {
        return core::make_unexpected(std::string("missing field `base_policy`"));
    }
    if (base->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("field `base_policy` must be a string"));
    }
    const core::Optional<BaseSandboxNetworkPolicy> parsed_base =
        ParseBasePolicy(base->second.as_string());
    if (!parsed_base.has_value()) {
        return core::make_unexpected(std::string("unknown base policy `") +
                                     base->second.as_string() + "`");
    }
    policy.base_policy = *parsed_base;

    const core::JsonObject::const_iterator egress = fields.find("egress");
    if (egress == fields.end()) {
        return core::make_unexpected(std::string("missing field `egress`"));
    }
    const core::Expected<SandboxNetworkEgressPolicy, std::string> parsed_egress =
        SandboxNetworkEgressPolicy::FromJson(egress->second);
    if (!parsed_egress.ok()) return core::make_unexpected(parsed_egress.error());
    policy.egress = parsed_egress.value();

    return policy;
}

bool SandboxNetworkPolicy::operator==(const SandboxNetworkPolicy& o) const {
    return allow_public_traffic == o.allow_public_traffic && base_policy == o.base_policy &&
           egress == o.egress;
}

// ---- rule construction ----------------------------------------------------

std::vector<IptablesRestoreCommand> BuildStaticEgressCommands(
    uint32_t guest_dns_ip, const std::vector<std::string>& internal_egress_denied_cidrs,
    const std::vector<std::string>& node_always_denied_cidrs) {
    std::vector<IptablesRestoreCommand> commands;

    // Host-initiated proxy/envd connections come back through this chain after
    // the namespace SNATs the response, so established flows are accepted
    // before any deny can reject them.
    commands.push_back(AppendEgressCommand(
        "-i tap0 -o vpeer -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT"));

    const char* const protocols[] = {"udp", "tcp"};
    for (std::size_t i = 0; i < 2; ++i) {
        commands.push_back(AppendEgressCommand(
            std::string("-i tap0 -o vpeer -d ") + Ipv4ToString(guest_dns_ip) + "/32 -p " +
            protocols[i] + " --dport 53 -j ACCEPT"));
    }

    // Internal networks are denied *before* the user chain so a sandbox cannot
    // allowlist its way to another sandbox's namespace or VM link addresses.
    for (std::size_t i = 0; i < internal_egress_denied_cidrs.size(); ++i) {
        commands.push_back(AppendEgressCommand(std::string("-i tap0 -o vpeer -d ") +
                                               internal_egress_denied_cidrs[i] + " -j REJECT"));
    }
    for (std::size_t i = 0; i < node_always_denied_cidrs.size(); ++i) {
        commands.push_back(AppendEgressCommand(std::string("-i tap0 -o vpeer -d ") +
                                               node_always_denied_cidrs[i] + " -j REJECT"));
    }

    commands.push_back(
        AppendEgressCommand(std::string("-i tap0 -o vpeer -j ") + kUserEgressChain));
    return commands;
}

std::vector<IptablesRestoreCommand> BuildUserEgressCommands(
    const SandboxNetworkPolicy& policy, bool replace) {
    std::vector<IptablesRestoreCommand> commands;
    if (replace) {
        // Flush first so a policy update replaces rather than accumulates.
        commands.push_back(IptablesRestoreCommand::FlushChain("filter", kUserEgressChain));
    }

    // Allows before denies: an explicit allow must win over a broader deny,
    // including the catch-all installed for a Deny base policy.
    for (std::size_t i = 0; i < policy.egress.allowed_cidrs.size(); ++i) {
        const IpNetwork& cidr = policy.egress.allowed_cidrs[i];
        // v6 entries are retained on the wire but not enforced here; these are
        // IPv4 iptables rules.
        if (!cidr.is_v4) continue;
        commands.push_back(AppendUserEgressCommand(std::string("-i tap0 -o vpeer -d ") +
                                                   cidr.ToString() + " -j ACCEPT"));
    }

    for (std::size_t i = 0; i < policy.egress.denied_cidrs.size(); ++i) {
        const IpNetwork& cidr = policy.egress.denied_cidrs[i];
        if (!cidr.is_v4) continue;
        commands.push_back(AppendUserEgressCommand(std::string("-i tap0 -o vpeer -d ") +
                                                   cidr.ToString() + " -j REJECT"));
    }

    if (policy.base_policy == BaseSandboxNetworkPolicy::Deny) {
        commands.push_back(AppendUserEgressCommand(std::string("-i tap0 -o vpeer -d ") +
                                                   kAllInternetTrafficCidr + " -j REJECT"));
    }

    return commands;
}

std::vector<IptablesRestoreCommand> BuildEgressProxyCommands(
    const SandboxNetworkPolicy& policy, uint16_t egress_proxy_port) {
    std::vector<IptablesRestoreCommand> commands;
    // Always flush, so a policy that no longer needs the proxy has its
    // redirects removed rather than left behind.
    commands.push_back(IptablesRestoreCommand::FlushChain("nat", kEgressProxyChain));

    const std::vector<uint16_t> ports = policy.EgressProxyTcpPorts();
    for (std::size_t i = 0; i < ports.size(); ++i) {
        char rule[128];
        std::snprintf(rule, sizeof(rule),
                      "-i tap0 -p tcp --dport %u -j REDIRECT --to-ports %u",
                      static_cast<unsigned>(ports[i]),
                      static_cast<unsigned>(egress_proxy_port));
        commands.push_back(IptablesRestoreCommand::Append("nat", kEgressProxyChain, rule));
    }
    return commands;
}

core::Expected<core::Unit, std::string> InitializeNamespaceEgressChain(
    uint32_t guest_dns_ip, const std::vector<std::string>& internal_egress_denied_cidrs,
    const std::vector<std::string>& node_always_denied_cidrs) {
    std::vector<IptablesRestoreCommand> commands;
    commands.push_back(IptablesRestoreCommand::NewChain("filter", kEgressChain));
    commands.push_back(IptablesRestoreCommand::NewChain("filter", kUserEgressChain));
    commands.push_back(IptablesRestoreCommand::Insert(
        "filter", "FORWARD", 1, std::string("-i tap0 -o vpeer -j ") + kEgressChain));
    commands.push_back(IptablesRestoreCommand::FlushChain("filter", kEgressChain));

    const std::vector<IptablesRestoreCommand> statics = BuildStaticEgressCommands(
        guest_dns_ip, internal_egress_denied_cidrs, node_always_denied_cidrs);
    commands.insert(commands.end(), statics.begin(), statics.end());

    // The nat commands come last so the script stays grouped by table.
    commands.push_back(IptablesRestoreCommand::NewChain("nat", kEgressProxyChain));
    commands.push_back(IptablesRestoreCommand::Insert(
        "nat", "PREROUTING", 1, std::string("-i tap0 -j ") + kEgressProxyChain));
    commands.push_back(IptablesRestoreCommand::FlushChain("nat", kEgressProxyChain));

    const core::Expected<core::Unit, std::string> applied =
        ApplyIptablesCommands(commands, OpenFailurePolicy::ReturnErr());
    if (!applied.ok()) {
        return core::make_unexpected(
            std::string("initialize AgentENV namespace egress iptables chains: ") +
            applied.error());
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> SetNamespaceEgressPolicy(
    const core::Optional<SandboxNetworkPolicy>& policy, uint16_t egress_proxy_port) {
    // Rust `policy.unwrap_or(&default_policy)`: an absent policy still has to
    // flush the chains, or a previous sandbox's rules would persist.
    const SandboxNetworkPolicy effective =
        policy.has_value() ? *policy : SandboxNetworkPolicy();

    std::vector<IptablesRestoreCommand> commands = BuildUserEgressCommands(effective, true);
    const std::vector<IptablesRestoreCommand> proxy =
        BuildEgressProxyCommands(effective, egress_proxy_port);
    commands.insert(commands.end(), proxy.begin(), proxy.end());

    return ApplyIptablesCommands(commands, OpenFailurePolicy::ReturnErr());
}

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
