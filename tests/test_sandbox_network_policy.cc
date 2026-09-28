// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/policy.rs and src/sandbox/network/iptables_util.rs
// test modules.
#include <string>
#include <vector>

#include "agentenv/sandbox/network.h"
#include "agentenv/sandbox/network/iptables_util.h"
#include "agentenv/sandbox/network/policy.h"
#include "microtest.h"

namespace {

using agentenv::core::Json;
using agentenv::core::Optional;
using agentenv::core::Unit;
namespace net = agentenv::sandbox::network;

std::vector<std::string> List(const char* a = NULL, const char* b = NULL,
                              const char* c = NULL, const char* d = NULL,
                              const char* e = NULL) {
    std::vector<std::string> out;
    const char* const values[] = {a, b, c, d, e};
    for (std::size_t i = 0; i < 5; ++i) {
        if (values[i] != NULL) out.push_back(values[i]);
    }
    return out;
}

Optional<std::vector<std::string> > Some(const std::vector<std::string>& values) {
    return Optional<std::vector<std::string> >(values);
}

uint32_t Ip(const std::string& text) {
    uint32_t out = 0;
    MT_EXPECT_TRUE(net::Ipv4Parse(text, &out));
    return out;
}

/// Rust test helper `append_rule`.
Optional<std::string> AppendRule(const net::IptablesRestoreCommand& command) {
    if (command.kind != net::IptablesRestoreCommand::Kind::Append) {
        return Optional<std::string>();
    }
    return Optional<std::string>(command.rule);
}

std::vector<std::string> AppendRules(
    const std::vector<net::IptablesRestoreCommand>& commands) {
    std::vector<std::string> rules;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const Optional<std::string> rule = AppendRule(commands[i]);
        if (rule.has_value()) rules.push_back(*rule);
    }
    return rules;
}

/// Index of the first Append command whose rule equals `rule`, or -1.
int PositionOf(const std::vector<net::IptablesRestoreCommand>& commands,
               const std::string& rule) {
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const Optional<std::string> found = AppendRule(commands[i]);
        if (found.has_value() && *found == rule) return static_cast<int>(i);
    }
    return -1;
}

net::SandboxNetworkEgressPolicy EgressWith(const std::vector<std::string>& allowed,
                                           const std::vector<std::string>& denied) {
    net::SandboxNetworkEgressPolicy policy;
    for (std::size_t i = 0; i < allowed.size(); ++i) {
        policy.allowed_cidrs.push_back(net::IpNetwork::Parse(allowed[i]).value());
    }
    for (std::size_t i = 0; i < denied.size(); ++i) {
        policy.denied_cidrs.push_back(net::IpNetwork::Parse(denied[i]).value());
    }
    return policy;
}

/// Rust `NetworkConfig::default().egress.always_denied_cidrs`.
std::vector<std::string> DefaultAlwaysDeniedCidrs() {
    std::vector<std::string> cidrs;
    cidrs.push_back("10.0.0.0/8");
    cidrs.push_back("100.64.0.0/10");
    cidrs.push_back("127.0.0.0/8");
    cidrs.push_back("169.254.0.0/16");
    cidrs.push_back("172.16.0.0/12");
    cidrs.push_back("192.168.0.0/16");
    return cidrs;
}

/// Captures the script the last apply produced, standing in for
/// `iptables-restore`.
std::string g_last_script;
int g_apply_count = 0;
bool g_fail_apply = false;
std::string g_failure_message;

agentenv::core::Expected<Unit, std::string> FakeRunner(const std::string& script) {
    g_last_script = script;
    ++g_apply_count;
    if (g_fail_apply) return agentenv::core::make_unexpected(g_failure_message);
    return Unit();
}

struct RunnerGuard {
    RunnerGuard() {
        g_last_script.clear();
        g_apply_count = 0;
        g_fail_apply = false;
        g_failure_message.clear();
        net::SetRestoreScriptRunnerForTesting(&FakeRunner);
    }
    ~RunnerGuard() { net::SetRestoreScriptRunnerForTesting(NULL); }
};

}  // namespace

// ---------------------------------------------------------------------------
// IpNetwork
// ---------------------------------------------------------------------------

MT_TEST(ip_network_parses_bare_addresses_as_full_prefixes) {
    // A bare address must normalise to /32 so it deduplicates against the
    // explicit form.
    MT_EXPECT_EQ(net::IpNetwork::Parse("8.8.8.8").value().ToString(),
                 std::string("8.8.8.8/32"));
    MT_EXPECT_EQ(net::IpNetwork::Parse("8.8.8.8/32").value().ToString(),
                 std::string("8.8.8.8/32"));
    MT_EXPECT_EQ(net::IpNetwork::Parse("1.1.1.0/24").value().ToString(),
                 std::string("1.1.1.0/24"));
    MT_EXPECT_EQ(net::IpNetwork::Parse("0.0.0.0/0").value().ToString(),
                 std::string("0.0.0.0/0"));
}

MT_TEST(ip_network_parses_ipv6) {
    const net::IpNetwork v6 = net::IpNetwork::Parse("2001:db8::1").value();
    MT_EXPECT_TRUE(!v6.is_v4);
    MT_EXPECT_EQ(v6.ToString(), std::string("2001:db8::1/128"));
    MT_EXPECT_EQ(net::IpNetwork::Parse("2001:db8::/32").value().ToString(),
                 std::string("2001:db8::/32"));
}

MT_TEST(ip_network_rejects_malformed_values) {
    const char* const rejected[] = {"", "example.com", "8.8.8", "8.8.8.8/33",
                                    "8.8.8.8/", "8.8.8.8/abc", "999.1.1.1"};
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        MT_EXPECT_TRUE(!net::IpNetwork::Parse(rejected[i]).ok());
    }
}

MT_TEST(ip_network_contains_v4_respects_the_prefix) {
    const net::IpNetwork network = net::IpNetwork::Parse("10.1.0.0/16").value();
    MT_EXPECT_TRUE(network.ContainsV4(Ip("10.1.0.1")));
    MT_EXPECT_TRUE(network.ContainsV4(Ip("10.1.255.255")));
    MT_EXPECT_TRUE(!network.ContainsV4(Ip("10.2.0.1")));

    // A /0 contains everything; a v6 network contains no v4 address.
    MT_EXPECT_TRUE(net::IpNetwork::Parse("0.0.0.0/0").value().ContainsV4(Ip("8.8.8.8")));
    MT_EXPECT_TRUE(!net::IpNetwork::Parse("2001:db8::/32").value().ContainsV4(Ip("8.8.8.8")));
}

// ---------------------------------------------------------------------------
// SandboxNetworkEgressPolicy::New
// ---------------------------------------------------------------------------

MT_TEST(new_splits_allow_cidrs_and_domains) {
    // Rust: `new_splits_allow_cidrs_and_domains`.
    const net::SandboxNetworkEgressPolicy policy =
        net::SandboxNetworkEgressPolicy::New(
            Some(List("8.8.8.8", "1.1.1.0/24", "*.example.com")),
            Some(List("203.0.113.0/24")))
            .value();

    MT_EXPECT_EQ(policy.allowed_cidrs.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(policy.allowed_cidrs[0].ToString(), std::string("8.8.8.8/32"));
    MT_EXPECT_EQ(policy.allowed_cidrs[1].ToString(), std::string("1.1.1.0/24"));
    MT_EXPECT_EQ(policy.allowed_domains.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(policy.allowed_domains[0], std::string("*.example.com"));
    MT_EXPECT_EQ(policy.denied_cidrs.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(policy.denied_cidrs[0].ToString(), std::string("203.0.113.0/24"));
}

MT_TEST(new_deduplicates_normalized_entries_without_reordering) {
    // Rust: `new_deduplicates_normalized_entries_without_reordering`.
    // Deduplication happens after normalisation, so `8.8.8.8` and
    // `8.8.8.8/32` collapse; order must still be first-seen because rule
    // precedence depends on it.
    const net::SandboxNetworkEgressPolicy policy =
        net::SandboxNetworkEgressPolicy::New(
            Some(List("8.8.8.8", "1.1.1.1", "8.8.8.8/32", "Example.com", "example.com")),
            Some(List("203.0.113.1", "203.0.113.1/32", "203.0.113.0/24")))
            .value();

    MT_EXPECT_EQ(policy.allowed_cidrs.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(policy.allowed_cidrs[0].ToString(), std::string("8.8.8.8/32"));
    MT_EXPECT_EQ(policy.allowed_cidrs[1].ToString(), std::string("1.1.1.1/32"));
    // Case-folding happens in normalisation, so the two spellings are one.
    MT_EXPECT_EQ(policy.allowed_domains.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(policy.allowed_domains[0], std::string("example.com"));
    MT_EXPECT_EQ(policy.denied_cidrs.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(policy.denied_cidrs[0].ToString(), std::string("203.0.113.1/32"));
    MT_EXPECT_EQ(policy.denied_cidrs[1].ToString(), std::string("203.0.113.0/24"));
}

MT_TEST(new_rejects_a_domain_in_deny_out) {
    // A domain deny cannot be enforced without resolving it, and resolution is
    // attacker-influenced, so it must be refused rather than silently dropped.
    const agentenv::core::Expected<net::SandboxNetworkEgressPolicy, std::string> policy =
        net::SandboxNetworkEgressPolicy::New(Optional<std::vector<std::string> >(),
                                             Some(List("example.com")));
    MT_EXPECT_TRUE(!policy.ok());
    MT_EXPECT_TRUE(policy.error().find("must be an IP address or CIDR block") !=
                   std::string::npos);
}

MT_TEST(new_rejects_a_malformed_cidr_but_accepts_a_bare_domain) {
    // With a slash it was meant to be a CIDR, so a parse failure is an error.
    MT_EXPECT_TRUE(!net::SandboxNetworkEgressPolicy::New(Some(List("10.0.0.0/99")),
                                                         Optional<std::vector<std::string> >())
                        .ok());
    // Without one, it is treated as a domain.
    MT_EXPECT_TRUE(net::SandboxNetworkEgressPolicy::New(Some(List("example.com")),
                                                        Optional<std::vector<std::string> >())
                       .ok());
    // But not every non-CIDR string is a valid domain.
    MT_EXPECT_TRUE(!net::SandboxNetworkEgressPolicy::New(Some(List("not a domain")),
                                                         Optional<std::vector<std::string> >())
                        .ok());
}

MT_TEST(new_accepts_absent_lists) {
    const net::SandboxNetworkEgressPolicy policy =
        net::SandboxNetworkEgressPolicy::New(Optional<std::vector<std::string> >(),
                                             Optional<std::vector<std::string> >())
            .value();
    MT_EXPECT_TRUE(!policy.HasExplicitRules());
    MT_EXPECT_TRUE(!policy.HasDomainAllowRules());
}

MT_TEST(structured_cidrs_keep_string_wire_format) {
    // Rust: `structured_cidrs_keep_string_wire_format`. The structured type
    // must not change what goes on the wire.
    const net::SandboxNetworkEgressPolicy policy =
        net::SandboxNetworkEgressPolicy::New(Some(List("8.8.8.8", "2001:db8::1/128")),
                                             Some(List("0.0.0.0/0")))
            .value();

    const Json json = policy.ToJson();
    const agentenv::core::JsonArray& allowed =
        json.as_object().find("allowed_cidrs")->second.as_array();
    MT_EXPECT_EQ(allowed.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(allowed[0].as_string(), std::string("8.8.8.8/32"));
    MT_EXPECT_EQ(allowed[1].as_string(), std::string("2001:db8::1/128"));
    MT_EXPECT_EQ(json.as_object().find("allowed_domains")->second.as_array().size(),
                 static_cast<std::size_t>(0));
    MT_EXPECT_EQ(json.as_object().find("denied_cidrs")->second.as_array()[0].as_string(),
                 std::string("0.0.0.0/0"));

    MT_EXPECT_TRUE(net::SandboxNetworkEgressPolicy::FromJson(json).value() == policy);
}

// ---------------------------------------------------------------------------
// SandboxNetworkPolicy
// ---------------------------------------------------------------------------

MT_TEST(new_sets_explicit_policy) {
    // Rust: `new_sets_explicit_policy`.
    const net::SandboxNetworkPolicy policy = net::SandboxNetworkPolicy::New(
        false, net::BaseSandboxNetworkPolicy::Deny,
        net::SandboxNetworkEgressPolicy::New(Some(List("8.8.8.8/32")),
                                             Optional<std::vector<std::string> >())
            .value());

    MT_EXPECT_TRUE(!policy.allow_public_traffic);
    MT_EXPECT_TRUE(policy.base_policy == net::BaseSandboxNetworkPolicy::Deny);
    MT_EXPECT_TRUE(policy.egress.denied_cidrs.empty());
    MT_EXPECT_TRUE(policy.HasRuntimeEgressRules());
}

MT_TEST(default_policy_needs_no_runtime_rules) {
    const net::SandboxNetworkPolicy policy;
    MT_EXPECT_TRUE(policy.allow_public_traffic);
    MT_EXPECT_TRUE(policy.base_policy == net::BaseSandboxNetworkPolicy::Default);
    MT_EXPECT_TRUE(!policy.HasRuntimeEgressRules());
    MT_EXPECT_TRUE(!policy.HasExplicitEgressRules());
    // Nothing to install, so there is no runtime policy to hand to the slot.
    MT_EXPECT_TRUE(!policy.RuntimePolicy().has_value());
}

MT_TEST(runtime_policy_is_present_when_rules_exist) {
    // A Deny base needs the catch-all reject installed even with no explicit
    // entries, which is why it counts as a runtime rule on its own.
    net::SandboxNetworkPolicy deny;
    deny.base_policy = net::BaseSandboxNetworkPolicy::Deny;
    MT_EXPECT_TRUE(deny.RuntimePolicy().has_value());
    MT_EXPECT_TRUE(!deny.HasExplicitEgressRules());

    net::SandboxNetworkPolicy explicit_allow;
    explicit_allow.egress = EgressWith(List("8.8.8.8/32"), List());
    MT_EXPECT_TRUE(explicit_allow.RuntimePolicy().has_value());
}

MT_TEST(egress_proxy_is_required_only_for_domain_allowlists) {
    net::SandboxNetworkPolicy policy;
    MT_EXPECT_TRUE(!policy.RequiresEgressProxy());
    MT_EXPECT_EQ(policy.EgressProxyTcpPorts().size(), static_cast<std::size_t>(0));

    // A CIDR allowlist is enforced by iptables alone.
    policy.egress = EgressWith(List("8.8.8.8/32"), List());
    MT_EXPECT_TRUE(!policy.RequiresEgressProxy());

    // A domain allowlist needs the hostname, which only the proxy sees.
    policy.egress.allowed_domains.push_back("example.com");
    MT_EXPECT_TRUE(policy.RequiresEgressProxy());
    MT_EXPECT_EQ(policy.EgressProxyTcpPorts().size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(policy.EgressProxyTcpPorts()[0], static_cast<uint16_t>(80));
    MT_EXPECT_EQ(policy.EgressProxyTcpPorts()[1], static_cast<uint16_t>(443));
}

MT_TEST(missing_ingress_policy_deserializes_as_public) {
    // Rust: `missing_ingress_policy_deserializes_as_public`.
    Json json = net::SandboxNetworkPolicy().ToJson();
    agentenv::core::JsonObject fields = json.as_object();
    fields.erase("allow_public_traffic");

    const net::SandboxNetworkPolicy parsed =
        net::SandboxNetworkPolicy::FromJson(Json(fields)).value();
    MT_EXPECT_TRUE(parsed.allow_public_traffic);
}

MT_TEST(policy_round_trips_through_json) {
    const net::SandboxNetworkPolicy policy = net::SandboxNetworkPolicy::New(
        false, net::BaseSandboxNetworkPolicy::Deny,
        net::SandboxNetworkEgressPolicy::New(Some(List("8.8.8.8", "*.example.com")),
                                             Some(List("203.0.113.0/24")))
            .value());

    const agentenv::core::Expected<Json, agentenv::core::AnyError> reparsed =
        Json::Parse(policy.ToJson().ToString());
    MT_EXPECT_TRUE(reparsed.ok());
    MT_EXPECT_TRUE(net::SandboxNetworkPolicy::FromJson(reparsed.value()).value() == policy);
}

MT_TEST(policy_from_json_rejects_bad_shapes) {
    const char* const rejected[] = {
        "{}",
        "[]",
        "{\"base_policy\":\"Nope\",\"egress\":{}}",
        "{\"base_policy\":\"Deny\"}",
        "{\"base_policy\":\"Deny\",\"egress\":{\"allowed_cidrs\":[\"bogus\"]}}",
        "{\"allow_public_traffic\":\"yes\",\"base_policy\":\"Deny\",\"egress\":{}}",
    };
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        const agentenv::core::Expected<Json, agentenv::core::AnyError> json =
            Json::Parse(rejected[i]);
        MT_EXPECT_TRUE(json.ok());
        MT_EXPECT_TRUE(!net::SandboxNetworkPolicy::FromJson(json.value()).ok());
    }
}

// ---------------------------------------------------------------------------
// Authorization
// ---------------------------------------------------------------------------

MT_TEST(authorization_uses_platform_denied_ranges) {
    // Rust: `authorization_uses_platform_denied_ranges`.
    net::SetPlatformDeniedCidrs(List("10.12.0.0/16"));

    const net::SandboxNetworkPolicy policy = net::SandboxNetworkPolicy::New(
        true, net::BaseSandboxNetworkPolicy::Allow, net::SandboxNetworkEgressPolicy());

    MT_EXPECT_TRUE(policy.IsIpAllowed(Ip("198.51.100.10")));
    // A platform-denied range is refused even under an Allow base policy.
    MT_EXPECT_TRUE(!policy.IsIpAllowed(Ip("10.12.1.1")));
}

MT_TEST(platform_denies_outrank_a_user_allow) {
    // This is the containment boundary: a sandbox must not be able to
    // allowlist its way into another sandbox's namespace.
    net::SetPlatformDeniedCidrs(List("10.12.0.0/16"));

    net::SandboxNetworkPolicy policy;
    policy.base_policy = net::BaseSandboxNetworkPolicy::Allow;
    policy.egress = EgressWith(List("10.12.1.1/32"), List());

    MT_EXPECT_TRUE(!policy.IsIpAllowed(Ip("10.12.1.1")));
}

MT_TEST(user_allow_precedes_user_deny) {
    net::SetPlatformDeniedCidrs(std::vector<std::string>());

    net::SandboxNetworkPolicy policy;
    policy.base_policy = net::BaseSandboxNetworkPolicy::Deny;
    // Allow a single host inside an otherwise denied range.
    policy.egress = EgressWith(List("203.0.113.5/32"), List("203.0.113.0/24"));

    MT_EXPECT_TRUE(policy.IsIpAllowed(Ip("203.0.113.5")));
    MT_EXPECT_TRUE(!policy.IsIpAllowed(Ip("203.0.113.6")));
}

MT_TEST(base_policy_decides_when_no_rule_matches) {
    net::SetPlatformDeniedCidrs(std::vector<std::string>());

    net::SandboxNetworkPolicy allow;
    allow.base_policy = net::BaseSandboxNetworkPolicy::Allow;
    MT_EXPECT_TRUE(allow.IsIpAllowed(Ip("198.51.100.1")));

    net::SandboxNetworkPolicy fallthrough;
    fallthrough.base_policy = net::BaseSandboxNetworkPolicy::Default;
    MT_EXPECT_TRUE(fallthrough.IsIpAllowed(Ip("198.51.100.1")));

    net::SandboxNetworkPolicy deny;
    deny.base_policy = net::BaseSandboxNetworkPolicy::Deny;
    MT_EXPECT_TRUE(!deny.IsIpAllowed(Ip("198.51.100.1")));
}

MT_TEST(a_domain_allowlist_stops_the_base_policy_fallthrough) {
    // Once a domain allowlist exists it is opt-in: an address that matched no
    // allowed domain must be refused, or the allowlist would be advisory.
    net::SetPlatformDeniedCidrs(std::vector<std::string>());

    net::SandboxNetworkPolicy policy;
    policy.base_policy = net::BaseSandboxNetworkPolicy::Allow;
    policy.egress.allowed_domains.push_back("example.com");

    MT_EXPECT_TRUE(!policy.IsIpAllowed(Ip("198.51.100.1")));
    // An explicit CIDR allow still wins, since it is checked first.
    policy.egress.allowed_cidrs.push_back(net::IpNetwork::Parse("198.51.100.1/32").value());
    MT_EXPECT_TRUE(policy.IsIpAllowed(Ip("198.51.100.1")));
}

MT_TEST(domain_authorization_checks_hostname_and_resolved_ip) {
    // Rust: `domain_authorization_checks_hostname_and_resolved_ip`.
    net::SetPlatformDeniedCidrs(List("10.0.0.0/8", "10.12.0.0/16"));

    const net::SandboxNetworkPolicy policy = net::SandboxNetworkPolicy::New(
        true, net::BaseSandboxNetworkPolicy::Default,
        net::SandboxNetworkEgressPolicy::New(Some(List("example.com")),
                                             Some(std::vector<std::string>()))
            .value());

    MT_EXPECT_TRUE(policy.IsDomainAllowed("example.com", Optional<uint32_t>()));
    MT_EXPECT_TRUE(
        policy.IsDomainAllowed("example.com", Optional<uint32_t>(Ip("192.0.2.10"))));
    MT_EXPECT_TRUE(!policy.IsDomainAllowed("other.example", Optional<uint32_t>()));
    // The hostname is allowed but it resolved into a platform-denied range,
    // which is the DNS-rebinding case.
    MT_EXPECT_TRUE(!policy.IsDomainAllowed("example.com", Optional<uint32_t>(Ip("10.0.0.1"))));
    MT_EXPECT_TRUE(!policy.IsDomainAllowed("example.com", Optional<uint32_t>(Ip("10.12.1.1"))));
}

MT_TEST(domain_allow_is_not_overridden_by_a_user_cidr_deny) {
    // Matches E2B's allow precedence: a user deny does not cancel an explicit
    // domain allow, though platform denies still do.
    net::SetPlatformDeniedCidrs(std::vector<std::string>());

    net::SandboxNetworkPolicy policy;
    policy.egress = EgressWith(List(), List("192.0.2.0/24"));
    policy.egress.allowed_domains.push_back("example.com");

    MT_EXPECT_TRUE(policy.IsDomainAllowed("example.com", Optional<uint32_t>(Ip("192.0.2.10"))));
}

MT_TEST(empty_hostname_is_never_allowed) {
    net::SandboxNetworkPolicy policy;
    policy.egress.allowed_domains.push_back("*");
    MT_EXPECT_TRUE(!policy.IsDomainAllowed("", Optional<uint32_t>()));
}

// ---------------------------------------------------------------------------
// domain_matches
// ---------------------------------------------------------------------------

MT_TEST(domain_matching_handles_wildcards_and_case) {
    // Rust: `domain_matching_handles_non_ascii_hostnames_without_panicking`.
    MT_EXPECT_TRUE(net::DomainMatches("API.Example.COM", "*.example.com"));
    MT_EXPECT_TRUE(net::DomainMatches("example.com", "example.com"));
    MT_EXPECT_TRUE(net::DomainMatches("EXAMPLE.COM", "example.com"));
    MT_EXPECT_TRUE(net::DomainMatches("anything.at.all", "*"));

    // A trailing dot is the same name.
    MT_EXPECT_TRUE(net::DomainMatches("example.com.", "example.com"));
    MT_EXPECT_TRUE(net::DomainMatches("example.com", "example.com."));
}

MT_TEST(domain_matching_requires_a_label_boundary) {
    // `*.example.com` must not match `notexample.com`; without the label-dot
    // check a suffix comparison would accept it.
    MT_EXPECT_TRUE(!net::DomainMatches("notexample.com", "*.example.com"));
    // The bare apex does not match its own wildcard, matching Rust.
    MT_EXPECT_TRUE(!net::DomainMatches("example.com", "*.example.com"));
    MT_EXPECT_TRUE(net::DomainMatches("a.b.example.com", "*.example.com"));
    MT_EXPECT_TRUE(!net::DomainMatches("example.com.evil.test", "*.example.com"));
}

MT_TEST(domain_matching_handles_non_ascii_without_crashing) {
    // Rust guards against slicing mid-codepoint; the C++ port is byte-indexed
    // but must reach the same answers rather than reading out of bounds.
    MT_EXPECT_TRUE(!net::DomainMatches("\xc3\xa9.com", "*.com.long.suffix"));
    MT_EXPECT_TRUE(net::DomainMatches("\xc3\xa9.com", "*.com"));
    MT_EXPECT_TRUE(net::DomainMatches("\xc3\xa9.example.com", "*.example.com"));
    MT_EXPECT_TRUE(!net::DomainMatches("", "*.example.com"));
}

MT_TEST(normalize_domain_pattern_preserves_the_wildcard) {
    MT_EXPECT_EQ(net::NormalizeDomainPattern("*.Example.COM").value(),
                 std::string("*.example.com"));
    MT_EXPECT_EQ(net::NormalizeDomainPattern("Example.COM").value(),
                 std::string("example.com"));
    MT_EXPECT_TRUE(!net::NormalizeDomainPattern("*.").ok());
    MT_EXPECT_TRUE(!net::NormalizeDomainPattern("").ok());
    MT_EXPECT_TRUE(!net::NormalizeDomainPattern("-bad.com").ok());
}

// ---------------------------------------------------------------------------
// Rule construction
// ---------------------------------------------------------------------------

MT_TEST(build_rules_keeps_allow_before_deny) {
    // Rust: `build_rules_keeps_allow_before_deny`.
    net::SandboxNetworkPolicy policy;
    policy.base_policy = net::BaseSandboxNetworkPolicy::Deny;
    policy.egress = EgressWith(List("8.8.8.8/32"), List());

    const std::vector<net::IptablesRestoreCommand> commands =
        net::BuildUserEgressCommands(policy, false);

    const int allow_pos = PositionOf(commands, "-i tap0 -o vpeer -d 8.8.8.8/32 -j ACCEPT");
    const int deny_pos = PositionOf(commands, "-i tap0 -o vpeer -d 0.0.0.0/0 -j REJECT");
    MT_EXPECT_TRUE(allow_pos >= 0);
    MT_EXPECT_TRUE(deny_pos >= 0);
    // iptables evaluates in order, so an allow after the catch-all reject
    // would never be reached.
    MT_EXPECT_TRUE(allow_pos < deny_pos);
}

MT_TEST(build_policy_replacement_flushes_before_installing_rules) {
    // Rust: `build_policy_replacement_flushes_before_installing_rules`.
    net::SandboxNetworkPolicy policy;
    policy.base_policy = net::BaseSandboxNetworkPolicy::Deny;
    policy.egress = EgressWith(List("8.8.8.8/32"), List("203.0.113.0/24"));

    const std::vector<net::IptablesRestoreCommand> commands =
        net::BuildUserEgressCommands(policy, true);

    MT_EXPECT_TRUE(commands[0].kind == net::IptablesRestoreCommand::Kind::FlushChain);
    MT_EXPECT_EQ(commands[0].table, std::string("filter"));
    MT_EXPECT_EQ(commands[0].chain, std::string(net::kUserEgressChain));

    const std::vector<std::string> rules = AppendRules(commands);
    MT_EXPECT_EQ(rules.size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(rules[0], std::string("-i tap0 -o vpeer -d 8.8.8.8/32 -j ACCEPT"));
    MT_EXPECT_EQ(rules[1], std::string("-i tap0 -o vpeer -d 203.0.113.0/24 -j REJECT"));
    MT_EXPECT_EQ(rules[2], std::string("-i tap0 -o vpeer -d 0.0.0.0/0 -j REJECT"));
}

MT_TEST(build_user_rules_ignores_ipv6_for_ipv4_iptables) {
    // Rust: `build_user_rules_ignores_ipv6_for_ipv4_iptables`.
    net::SandboxNetworkPolicy policy;
    policy.base_policy = net::BaseSandboxNetworkPolicy::Allow;
    policy.egress = EgressWith(List("2001:db8::/32"), List("2001:db8:1::/48"));

    // v6 entries round-trip on the wire but produce no IPv4 iptables rules.
    MT_EXPECT_TRUE(net::BuildUserEgressCommands(policy, false).empty());
}

MT_TEST(build_default_policy_replacement_only_flushes_user_chain) {
    // Rust: `build_default_policy_replacement_only_flushes_user_chain`.
    const std::vector<net::IptablesRestoreCommand> commands =
        net::BuildUserEgressCommands(net::SandboxNetworkPolicy(), true);

    MT_EXPECT_EQ(commands.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(commands[0].kind == net::IptablesRestoreCommand::Kind::FlushChain);
    MT_EXPECT_EQ(commands[0].chain, std::string(net::kUserEgressChain));
}

MT_TEST(build_static_rules_include_baseline_in_order) {
    // Rust: `build_static_rules_include_baseline_in_order`.
    const std::vector<net::IptablesRestoreCommand> commands = net::BuildStaticEgressCommands(
        Ip("10.1.2.1"), std::vector<std::string>(), DefaultAlwaysDeniedCidrs());

    MT_EXPECT_EQ(*AppendRule(commands[0]),
                 std::string("-i tap0 -o vpeer -m conntrack --ctstate ESTABLISHED,RELATED "
                             "-j ACCEPT"));

    const int established = PositionOf(
        commands, "-i tap0 -o vpeer -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT");
    const int dns_udp =
        PositionOf(commands, "-i tap0 -o vpeer -d 10.1.2.1/32 -p udp --dport 53 -j ACCEPT");
    const int dns_tcp =
        PositionOf(commands, "-i tap0 -o vpeer -d 10.1.2.1/32 -p tcp --dport 53 -j ACCEPT");
    const int hard_deny = PositionOf(commands, "-i tap0 -o vpeer -d 10.0.0.0/8 -j REJECT");
    const int shared_deny =
        PositionOf(commands, "-i tap0 -o vpeer -d 100.64.0.0/10 -j REJECT");
    const int user_chain =
        PositionOf(commands, "-i tap0 -o vpeer -j AGENTENV-USER-EGRESS");

    MT_EXPECT_TRUE(dns_udp >= 0);
    MT_EXPECT_TRUE(dns_tcp >= 0);
    MT_EXPECT_TRUE(hard_deny >= 0);
    MT_EXPECT_TRUE(shared_deny >= 0);
    MT_EXPECT_TRUE(user_chain >= 0);

    // DNS and established flows must be accepted before the platform denies,
    // or a guest could not resolve the names it is allowed to reach.
    MT_EXPECT_TRUE(established < hard_deny);
    MT_EXPECT_TRUE(dns_udp < hard_deny);
    MT_EXPECT_TRUE(dns_tcp < hard_deny);
    // And every platform deny must precede the jump to the user chain.
    MT_EXPECT_TRUE(hard_deny < shared_deny);
    MT_EXPECT_TRUE(hard_deny < user_chain);
    MT_EXPECT_TRUE(shared_deny < user_chain);

    // The guest's own address is not blanket-accepted.
    MT_EXPECT_EQ(PositionOf(commands, "-i tap0 -o vpeer -d 10.12.0.2/32 -j ACCEPT"), -1);
}

MT_TEST(build_static_rules_use_configured_denied_cidrs) {
    // Rust: `build_static_rules_use_configured_denied_cidrs`.
    const std::vector<net::IptablesRestoreCommand> commands = net::BuildStaticEgressCommands(
        Ip("10.1.2.1"), std::vector<std::string>(), List("203.0.113.0/24"));

    MT_EXPECT_TRUE(PositionOf(commands, "-i tap0 -o vpeer -d 203.0.113.0/24 -j REJECT") >= 0);
    // The defaults are not implicitly added on top of the configured list.
    MT_EXPECT_EQ(PositionOf(commands, "-i tap0 -o vpeer -d 10.0.0.0/8 -j REJECT"), -1);
}

MT_TEST(build_static_rules_place_internal_denies_before_node_denies) {
    const std::vector<net::IptablesRestoreCommand> commands = net::BuildStaticEgressCommands(
        Ip("10.1.2.1"), List("10.200.0.0/16"), List("203.0.113.0/24"));

    const int internal = PositionOf(commands, "-i tap0 -o vpeer -d 10.200.0.0/16 -j REJECT");
    const int node = PositionOf(commands, "-i tap0 -o vpeer -d 203.0.113.0/24 -j REJECT");
    const int user_chain = PositionOf(commands, "-i tap0 -o vpeer -j AGENTENV-USER-EGRESS");
    MT_EXPECT_TRUE(internal >= 0 && node >= 0);
    MT_EXPECT_TRUE(internal < node);
    MT_EXPECT_TRUE(node < user_chain);
}

MT_TEST(egress_proxy_redirects_domain_inspection_ports) {
    // Rust: `egress_proxy_redirects_domain_inspection_ports`.
    const net::SandboxNetworkPolicy policy = net::SandboxNetworkPolicy::New(
        true, net::BaseSandboxNetworkPolicy::Deny,
        net::SandboxNetworkEgressPolicy::New(Some(List("example.com")),
                                             Some(List(net::kAllInternetTrafficCidr)))
            .value());

    const std::vector<std::string> rules =
        AppendRules(net::BuildEgressProxyCommands(policy, 43210));
    MT_EXPECT_EQ(rules.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(rules[0].find("--dport 80") != std::string::npos);
    MT_EXPECT_TRUE(rules[1].find("--dport 443") != std::string::npos);
    for (std::size_t i = 0; i < rules.size(); ++i) {
        MT_EXPECT_TRUE(rules[i].find("--to-ports 43210") != std::string::npos);
    }
}

MT_TEST(egress_proxy_chain_is_cleared_when_policy_needs_no_proxy) {
    // Rust: `egress_proxy_chain_is_cleared_when_policy_needs_no_proxy`. The
    // flush must happen even with no ports, or a previous policy's redirects
    // would keep intercepting traffic.
    const std::vector<net::IptablesRestoreCommand> commands =
        net::BuildEgressProxyCommands(net::SandboxNetworkPolicy(), 43210);

    MT_EXPECT_EQ(commands.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(commands[0].kind == net::IptablesRestoreCommand::Kind::FlushChain);
    MT_EXPECT_EQ(commands[0].table, std::string("nat"));
    MT_EXPECT_EQ(commands[0].chain, std::string(net::kEgressProxyChain));
}

// ---------------------------------------------------------------------------
// iptables_util — script rendering
// ---------------------------------------------------------------------------

MT_TEST(build_restore_script_renders_commands_in_order) {
    // Rust: `build_restore_script_renders_commands_in_order`.
    std::vector<net::IptablesRestoreCommand> commands;
    commands.push_back(net::IptablesRestoreCommand::NewChain("filter", "AGENTENV-EGRESS"));
    commands.push_back(
        net::IptablesRestoreCommand::NewChain("filter", "AGENTENV-USER-EGRESS"));
    commands.push_back(net::IptablesRestoreCommand::Insert(
        "filter", "FORWARD", 1, "-i tap0 -o vpeer -j AGENTENV-EGRESS"));
    commands.push_back(net::IptablesRestoreCommand::FlushChain("filter", "AGENTENV-EGRESS"));
    commands.push_back(net::IptablesRestoreCommand::Append(
        "filter", "AGENTENV-EGRESS", "-i tap0 -o vpeer -j AGENTENV-USER-EGRESS"));

    MT_EXPECT_EQ(net::BuildRestoreScript(commands),
                 std::string("*filter\n"
                             "-N AGENTENV-EGRESS\n"
                             "-N AGENTENV-USER-EGRESS\n"
                             "-I FORWARD 1 -i tap0 -o vpeer -j AGENTENV-EGRESS\n"
                             "-F AGENTENV-EGRESS\n"
                             "-A AGENTENV-EGRESS -i tap0 -o vpeer -j AGENTENV-USER-EGRESS\n"
                             "COMMIT\n"));
}

MT_TEST(build_restore_script_groups_commands_by_table) {
    // Rust: `build_restore_script_groups_commands_by_table`.
    std::vector<net::IptablesRestoreCommand> commands;
    commands.push_back(
        net::IptablesRestoreCommand::Append("filter", "FORWARD", "-i tap0 -j ACCEPT"));
    commands.push_back(
        net::IptablesRestoreCommand::Delete("nat", "POSTROUTING", "-o vpeer -j MASQUERADE"));

    MT_EXPECT_EQ(net::BuildRestoreScript(commands),
                 std::string("*filter\n"
                             "-A FORWARD -i tap0 -j ACCEPT\n"
                             "COMMIT\n"
                             "*nat\n"
                             "-D POSTROUTING -o vpeer -j MASQUERADE\n"
                             "COMMIT\n"));
}

MT_TEST(build_restore_script_preserves_quoted_rule_arguments) {
    // Rust: `build_restore_script_preserves_quoted_rule_arguments`. The rule
    // is passed through verbatim; `iptables-restore` does its own parsing.
    std::vector<net::IptablesRestoreCommand> commands;
    commands.push_back(net::IptablesRestoreCommand::Append(
        "filter", "FORWARD", "-m comment --comment \"sandbox traffic\" -j ACCEPT"));

    MT_EXPECT_EQ(net::BuildRestoreScript(commands),
                 std::string("*filter\n-A FORWARD -m comment --comment \"sandbox traffic\" "
                             "-j ACCEPT\nCOMMIT\n"));
}

MT_TEST(build_restore_script_is_empty_for_no_commands) {
    MT_EXPECT_EQ(net::BuildRestoreScript(std::vector<net::IptablesRestoreCommand>()),
                 std::string(""));
}

MT_TEST(is_missing_iptables_rule_error_recognises_the_delete_messages) {
    // These are what `iptables-restore` says when deleting a rule that is
    // already gone, which teardown must treat as success.
    MT_EXPECT_TRUE(net::IsMissingIptablesRuleError(
        "iptables-restore failed: Bad rule (does a matching rule exist in that chain?)"));
    MT_EXPECT_TRUE(
        net::IsMissingIptablesRuleError("No chain/target/match by that name"));
    MT_EXPECT_TRUE(net::IsMissingIptablesRuleError("  rule does not exist  "));
    // Case-insensitive, as Rust's `eq_ignore_ascii_case` window scan is.
    MT_EXPECT_TRUE(net::IsMissingIptablesRuleError("BAD RULE"));

    // A real failure must not be swallowed.
    MT_EXPECT_TRUE(!net::IsMissingIptablesRuleError("Permission denied"));
    MT_EXPECT_TRUE(!net::IsMissingIptablesRuleError(""));
}

// ---------------------------------------------------------------------------
// iptables_util — application
// ---------------------------------------------------------------------------

MT_TEST(apply_iptables_commands_does_nothing_when_empty) {
    RunnerGuard guard;
    MT_EXPECT_TRUE(net::ApplyIptablesCommands(std::vector<net::IptablesRestoreCommand>(),
                                              net::OpenFailurePolicy::ReturnErr())
                       .ok());
    MT_EXPECT_EQ(g_apply_count, 0);
}

MT_TEST(apply_iptables_commands_batches_a_mixed_set_atomically) {
    RunnerGuard guard;
    std::vector<net::IptablesRestoreCommand> commands;
    commands.push_back(net::IptablesRestoreCommand::FlushChain("filter", "C"));
    commands.push_back(net::IptablesRestoreCommand::Append("filter", "C", "-j ACCEPT"));

    MT_EXPECT_TRUE(
        net::ApplyIptablesCommands(commands, net::OpenFailurePolicy::ReturnErr()).ok());
    // One invocation: a half-installed policy would leave allows without
    // their denies.
    MT_EXPECT_EQ(g_apply_count, 1);
    MT_EXPECT_TRUE(g_last_script.find("-F C") != std::string::npos);
    MT_EXPECT_TRUE(g_last_script.find("-A C -j ACCEPT") != std::string::npos);
}

MT_TEST(apply_iptables_commands_applies_deletes_one_at_a_time) {
    RunnerGuard guard;
    std::vector<net::IptablesRestoreCommand> commands;
    commands.push_back(net::IptablesRestoreCommand::Delete("filter", "C", "-j ACCEPT"));
    commands.push_back(net::IptablesRestoreCommand::Delete("filter", "C", "-j REJECT"));

    MT_EXPECT_TRUE(
        net::ApplyIptablesCommands(commands, net::OpenFailurePolicy::ReturnErr()).ok());
    // Per-command, so an already-absent rule can be recognised individually.
    MT_EXPECT_EQ(g_apply_count, 2);
}

MT_TEST(apply_iptables_commands_ignores_an_already_absent_rule) {
    RunnerGuard guard;
    g_fail_apply = true;
    g_failure_message = "iptables-restore failed: Bad rule (does a matching rule exist?)";

    std::vector<net::IptablesRestoreCommand> commands;
    commands.push_back(net::IptablesRestoreCommand::Delete("filter", "C", "-j ACCEPT"));

    // Teardown must be idempotent even under ReturnErr.
    MT_EXPECT_TRUE(
        net::ApplyIptablesCommands(commands, net::OpenFailurePolicy::ReturnErr()).ok());
}

MT_TEST(apply_iptables_commands_reports_a_real_delete_failure) {
    RunnerGuard guard;
    g_fail_apply = true;
    g_failure_message = "iptables-restore failed: Permission denied";

    std::vector<net::IptablesRestoreCommand> commands;
    commands.push_back(net::IptablesRestoreCommand::Delete("filter", "C", "-j ACCEPT"));

    const agentenv::core::Expected<Unit, std::string> strict =
        net::ApplyIptablesCommands(commands, net::OpenFailurePolicy::ReturnErr());
    MT_EXPECT_TRUE(!strict.ok());
    MT_EXPECT_TRUE(strict.error().find("Permission denied") != std::string::npos);

    // Best-effort teardown continues so one bad rule does not leak the rest.
    MT_EXPECT_TRUE(net::ApplyIptablesCommands(
                       commands, net::OpenFailurePolicy::WarnAndIgnore("teardown failed"))
                       .ok());
}

MT_TEST(apply_iptables_commands_honours_the_open_failure_policy_for_batches) {
    RunnerGuard guard;
    g_fail_apply = true;
    g_failure_message = "iptables-restore failed: Permission denied";

    std::vector<net::IptablesRestoreCommand> commands;
    commands.push_back(net::IptablesRestoreCommand::Append("filter", "C", "-j ACCEPT"));

    MT_EXPECT_TRUE(
        !net::ApplyIptablesCommands(commands, net::OpenFailurePolicy::ReturnErr()).ok());
    MT_EXPECT_TRUE(net::ApplyIptablesCommands(
                       commands, net::OpenFailurePolicy::WarnAndIgnore("apply failed"))
                       .ok());
}

MT_TEST(set_namespace_egress_policy_flushes_even_without_a_policy) {
    RunnerGuard guard;
    // An absent policy still has to clear both chains, or the previous
    // sandbox's rules would remain installed on a reused slot.
    MT_EXPECT_TRUE(
        net::SetNamespaceEgressPolicy(Optional<net::SandboxNetworkPolicy>(), 43210).ok());
    MT_EXPECT_EQ(g_apply_count, 1);
    MT_EXPECT_TRUE(g_last_script.find("-F AGENTENV-USER-EGRESS") != std::string::npos);
    MT_EXPECT_TRUE(g_last_script.find("-F AGENTENV-EGRESS-PROXY") != std::string::npos);
}

MT_TEST(initialize_namespace_egress_chain_groups_filter_before_nat) {
    RunnerGuard guard;
    MT_EXPECT_TRUE(net::InitializeNamespaceEgressChain(Ip("10.1.2.1"),
                                                       std::vector<std::string>(),
                                                       DefaultAlwaysDeniedCidrs())
                       .ok());

    // `build_restore_script` opens one block per table run, so interleaving
    // would silently drop the earlier block's rules.
    const std::size_t filter = g_last_script.find("*filter");
    const std::size_t nat = g_last_script.find("*nat");
    MT_EXPECT_TRUE(filter != std::string::npos);
    MT_EXPECT_TRUE(nat != std::string::npos);
    MT_EXPECT_TRUE(filter < nat);
    // Exactly one block each.
    MT_EXPECT_EQ(g_last_script.find("*filter", filter + 1), std::string::npos);
    MT_EXPECT_EQ(g_last_script.find("*nat", nat + 1), std::string::npos);
}

MT_TEST(initialize_namespace_egress_chain_reports_context_on_failure) {
    RunnerGuard guard;
    g_fail_apply = true;
    g_failure_message = "iptables-restore failed: Permission denied";

    const agentenv::core::Expected<Unit, std::string> result =
        net::InitializeNamespaceEgressChain(Ip("10.1.2.1"), std::vector<std::string>(),
                                            DefaultAlwaysDeniedCidrs());
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("initialize AgentENV namespace egress iptables chains") !=
                   std::string::npos);
}

int main() { return microtest::RunAll(); }
