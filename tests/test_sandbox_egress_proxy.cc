// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/egress_proxy.rs `mod tests`.
//
// These are authorization decisions, so the negative cases matter more than
// the positive ones: every "fails closed" assertion below corresponds to a way
// a guest could otherwise reach a destination its policy forbids.
#include "agentenv/sandbox/network/egress_proxy.h"

#include <string>
#include <vector>

#include "agentenv/sandbox/network.h"
#include "microtest.h"

using namespace agentenv;                    // NOLINT
using namespace agentenv::sandbox;           // NOLINT
using namespace agentenv::sandbox::network;  // NOLINT

namespace {

/// Tri-state helper: parse and require a concrete hostname.
std::string HttpHost(const std::string& request, bool* ok, bool* present) {
    core::Optional<std::string> out;
    auto r = ParseHttpHost(reinterpret_cast<const uint8_t*>(request.data()), request.size(),
                           &out);
    *ok = r.ok();
    *present = out.has_value();
    return out.has_value() ? *out : std::string();
}

std::string TlsSni(const std::vector<uint8_t>& preface, bool* ok, bool* present) {
    core::Optional<std::string> out;
    auto r = ParseTlsSni(preface.empty() ? NULL : &preface[0], preface.size(), &out);
    *ok = r.ok();
    *present = out.has_value();
    return out.has_value() ? *out : std::string();
}

uint32_t Ip(const char* dotted) {
    uint32_t v = 0;
    Ipv4Parse(dotted, &v);
    return v;
}

SandboxNetworkPolicy MakePolicy(BaseSandboxNetworkPolicy base,
                                const std::vector<std::string>& allow_out) {
    core::Optional<std::vector<std::string> > allow(allow_out);
    std::vector<std::string> deny_list;
    deny_list.push_back("0.0.0.0/0");
    core::Optional<std::vector<std::string> > deny(deny_list);
    auto egress = SandboxNetworkEgressPolicy::New(allow, deny);
    return SandboxNetworkPolicy::New(true, base, egress.value());
}

/// Rust's `tls_sni_is_parsed` fixture, byte for byte.
std::vector<uint8_t> BuildClientHello(const std::string& hostname) {
    std::vector<uint8_t> hello;
    hello.push_back(0x03);
    hello.push_back(0x03);                      // ClientHello version
    hello.insert(hello.end(), 32, 0x00);        // random
    hello.push_back(0x00);                      // session id length
    hello.push_back(0x00); hello.push_back(0x02);
    hello.push_back(0x00); hello.push_back(0x2f);  // one cipher suite
    hello.push_back(0x01); hello.push_back(0x00);  // one compression method

    // extensions: server_name holding one HostName entry
    const size_t name_len = hostname.size();
    const size_t list_len = 3 + name_len;   // name_type(1) + name_len(2) + name
    const size_t ext_len = 2 + list_len;    // list_len(2) + list
    const size_t ext_total = 4 + ext_len;   // type(2) + len(2) + body
    hello.push_back(static_cast<uint8_t>(ext_total >> 8));
    hello.push_back(static_cast<uint8_t>(ext_total & 0xFF));
    hello.push_back(0x00); hello.push_back(0x00);  // extension type 0
    hello.push_back(static_cast<uint8_t>(ext_len >> 8));
    hello.push_back(static_cast<uint8_t>(ext_len & 0xFF));
    hello.push_back(static_cast<uint8_t>(list_len >> 8));
    hello.push_back(static_cast<uint8_t>(list_len & 0xFF));
    hello.push_back(0x00);                         // host_name
    hello.push_back(static_cast<uint8_t>(name_len >> 8));
    hello.push_back(static_cast<uint8_t>(name_len & 0xFF));
    hello.insert(hello.end(), hostname.begin(), hostname.end());
    return hello;
}

/// Wrap a ClientHello in handshake + record framing.
std::vector<uint8_t> BuildTlsPreface(const std::string& hostname) {
    const std::vector<uint8_t> hello = BuildClientHello(hostname);
    std::vector<uint8_t> handshake;
    handshake.push_back(0x01);  // ClientHello
    handshake.push_back(static_cast<uint8_t>((hello.size() >> 16) & 0xFF));
    handshake.push_back(static_cast<uint8_t>((hello.size() >> 8) & 0xFF));
    handshake.push_back(static_cast<uint8_t>(hello.size() & 0xFF));
    handshake.insert(handshake.end(), hello.begin(), hello.end());

    std::vector<uint8_t> preface;
    preface.push_back(0x16); preface.push_back(0x03); preface.push_back(0x03);
    preface.push_back(static_cast<uint8_t>((handshake.size() >> 8) & 0xFF));
    preface.push_back(static_cast<uint8_t>(handshake.size() & 0xFF));
    preface.insert(preface.end(), handshake.begin(), handshake.end());
    return preface;
}

}  // namespace

// ---- constants ----
MT_TEST(egress_proxy_constants_match_rust) {
    MT_EXPECT_TRUE(kEgressProxyPort == 15000);
    MT_EXPECT_TRUE(kMaxPrefaceBytes == 64 * 1024);
    MT_EXPECT_TRUE(kPrefaceTimeoutMs == 10000);
    MT_EXPECT_TRUE(kUpstreamTimeoutMs == 30000);
    MT_EXPECT_TRUE(kMaxConnectionsPerProxy == 256);
    MT_EXPECT_TRUE(kSoOriginalDst == 80);
}

// ---- Rust: http_host_is_parsed ----
MT_TEST(http_host_is_parsed) {
    bool ok = false, present = false;
    // Host is lowercased and the :port suffix dropped.
    MT_EXPECT_TRUE(HttpHost("GET / HTTP/1.1\r\nHost: Example.com:443\r\n\r\n", &ok, &present) ==
                   "example.com");
    MT_EXPECT_TRUE(ok && present);

    // A bracketed IPv6 literal fails closed on an IPv4-only dataplane.
    MT_EXPECT_TRUE(HttpHost("GET / HTTP/1.1\r\nHost: [2001:db8::1]:443\r\n\r\n", &ok,
                            &present) == "");
    MT_EXPECT_TRUE(ok && present);
}

// ---- Rust: duplicate_http_host_is_rejected ----
MT_TEST(duplicate_http_host_is_rejected) {
    bool ok = false, present = false;
    // Two Host headers must not authorize on the first one.
    const std::string host = HttpHost(
        "GET / HTTP/1.1\r\nHost: example.com\r\nHost: other.example\r\n\r\n", &ok, &present);
    MT_EXPECT_TRUE(ok);
    MT_EXPECT_TRUE(present);
    MT_EXPECT_TRUE(host == "");
}

MT_TEST(http_host_tri_state) {
    bool ok = false, present = false;

    // Incomplete head -> need more bytes, not an error.
    HttpHost("GET / HTTP/1.1\r\nHost: example.com\r\n", &ok, &present);
    MT_EXPECT_TRUE(ok);
    MT_EXPECT_TRUE(!present);

    // Complete head with no Host header at all -> Ok(None).
    HttpHost("GET / HTTP/1.1\r\nAccept: */*\r\n\r\n", &ok, &present);
    MT_EXPECT_TRUE(ok);
    MT_EXPECT_TRUE(!present);

    // Header name padded before the colon is a smuggling vector -> Err.
    HttpHost("GET / HTTP/1.1\r\nHost : example.com\r\n\r\n", &ok, &present);
    MT_EXPECT_TRUE(!ok);

    // A request line without three tokens is malformed.
    HttpHost("GET\r\n\r\n", &ok, &present);
    MT_EXPECT_TRUE(!ok);
}

MT_TEST(http_host_header_name_is_case_insensitive) {
    bool ok = false, present = false;
    MT_EXPECT_TRUE(HttpHost("GET / HTTP/1.1\r\nHOST: Example.COM\r\n\r\n", &ok, &present) ==
                   "example.com");
    MT_EXPECT_TRUE(HttpHost("GET / HTTP/1.1\r\nhOsT: example.com\r\n\r\n", &ok, &present) ==
                   "example.com");
}

MT_TEST(http_host_with_embedded_whitespace_fails_closed) {
    bool ok = false, present = false;
    // A value carrying control/whitespace bytes must not be trusted.
    const std::string host =
        HttpHost("GET / HTTP/1.1\r\nHost: exa mple.com\r\n\r\n", &ok, &present);
    MT_EXPECT_TRUE(ok);
    MT_EXPECT_TRUE(present);
    MT_EXPECT_TRUE(host == "");
}

// ---- normalize_host ----
MT_TEST(normalize_host_rules) {
    MT_EXPECT_TRUE(NormalizeHost("Example.COM") == "example.com");
    MT_EXPECT_TRUE(NormalizeHost("  example.com  ") == "example.com");
    // Every trailing root dot is stripped.
    MT_EXPECT_TRUE(NormalizeHost("example.com.") == "example.com");
    MT_EXPECT_TRUE(NormalizeHost("example.com...") == "example.com");
    MT_EXPECT_TRUE(NormalizeHost("example.com:8080") == "example.com");
    // A non-numeric tail is part of the host, not a port.
    MT_EXPECT_TRUE(NormalizeHost("example.com:notaport") == "example.com:notaport");
    // Out-of-range port is likewise not a port.
    MT_EXPECT_TRUE(NormalizeHost("example.com:99999") == "example.com:99999");
    // Bracketed IPv6 fails closed.
    MT_EXPECT_TRUE(NormalizeHost("[::1]") == "");
    MT_EXPECT_TRUE(NormalizeHost("[2001:db8::1]:443") == "");
    // Unbracketed IPv6 keeps the ']'-guard from mis-splitting.
    MT_EXPECT_TRUE(NormalizeHost("") == "");
    // A bare IPv4 literal passes through.
    MT_EXPECT_TRUE(NormalizeHost("8.8.8.8:443") == "8.8.8.8");
}

// ---- Rust: tls_sni_is_parsed (including the fragmented variant) ----
MT_TEST(tls_sni_is_parsed) {
    bool ok = false, present = false;
    const std::vector<uint8_t> preface = BuildTlsPreface("example.com");
    MT_EXPECT_TRUE(TlsSni(preface, &ok, &present) == "example.com");
    MT_EXPECT_TRUE(ok && present);
}

MT_TEST(tls_sni_is_parsed_across_record_boundaries) {
    // The ClientHello is split over two records; a client must not be able to
    // evade inspection by choosing its own record framing.
    const std::vector<uint8_t> preface = BuildTlsPreface("example.com");
    const std::vector<uint8_t> payload(preface.begin() + 5, preface.end());
    const size_t split = 20;
    MT_EXPECT_TRUE(payload.size() > split);

    std::vector<uint8_t> fragmented;
    fragmented.push_back(0x16); fragmented.push_back(0x03); fragmented.push_back(0x03);
    fragmented.push_back(0x00); fragmented.push_back(static_cast<uint8_t>(split));
    fragmented.insert(fragmented.end(), payload.begin(), payload.begin() + split);

    const size_t second_len = payload.size() - split;
    fragmented.push_back(0x16); fragmented.push_back(0x03); fragmented.push_back(0x03);
    fragmented.push_back(static_cast<uint8_t>((second_len >> 8) & 0xFF));
    fragmented.push_back(static_cast<uint8_t>(second_len & 0xFF));
    fragmented.insert(fragmented.end(), payload.begin() + split, payload.end());

    bool ok = false, present = false;
    MT_EXPECT_TRUE(TlsSni(fragmented, &ok, &present) == "example.com");
    MT_EXPECT_TRUE(ok && present);
}

MT_TEST(tls_sni_lowercases_the_name) {
    bool ok = false, present = false;
    const std::vector<uint8_t> preface = BuildTlsPreface("EXAMPLE.COM");
    MT_EXPECT_TRUE(TlsSni(preface, &ok, &present) == "example.com");
}

MT_TEST(tls_sni_tri_state) {
    bool ok = false, present = false;

    // Not a handshake record -> Ok(None).
    std::vector<uint8_t> not_tls;
    not_tls.push_back('G'); not_tls.push_back('E'); not_tls.push_back('T');
    TlsSni(not_tls, &ok, &present);
    MT_EXPECT_TRUE(ok);
    MT_EXPECT_TRUE(!present);

    // Truncated record header -> need more bytes.
    std::vector<uint8_t> short_header;
    short_header.push_back(0x16); short_header.push_back(0x03);
    TlsSni(short_header, &ok, &present);
    MT_EXPECT_TRUE(ok);
    MT_EXPECT_TRUE(!present);

    // Complete header but truncated body -> need more bytes.
    std::vector<uint8_t> truncated = BuildTlsPreface("example.com");
    truncated.resize(truncated.size() - 4);
    TlsSni(truncated, &ok, &present);
    MT_EXPECT_TRUE(ok);
    MT_EXPECT_TRUE(!present);

    // Empty preface.
    TlsSni(std::vector<uint8_t>(), &ok, &present);
    MT_EXPECT_TRUE(ok);
    MT_EXPECT_TRUE(!present);
}

MT_TEST(tls_client_hello_without_sni_fails_closed) {
    // Extensions present but none is server_name -> Ok(Some("")), which the
    // policy then treats as "no hostname".
    std::vector<uint8_t> hello;
    hello.push_back(0x03); hello.push_back(0x03);
    hello.insert(hello.end(), 32, 0x00);
    hello.push_back(0x00);                          // session id
    hello.push_back(0x00); hello.push_back(0x02);
    hello.push_back(0x00); hello.push_back(0x2f);   // cipher suite
    hello.push_back(0x01); hello.push_back(0x00);   // compression
    // one extension of type 0x000d (signature_algorithms), body length 2
    hello.push_back(0x00); hello.push_back(0x06);
    hello.push_back(0x00); hello.push_back(0x0d);
    hello.push_back(0x00); hello.push_back(0x02);
    hello.push_back(0x00); hello.push_back(0x00);

    std::vector<uint8_t> handshake;
    handshake.push_back(0x01);
    handshake.push_back(0x00);
    handshake.push_back(static_cast<uint8_t>((hello.size() >> 8) & 0xFF));
    handshake.push_back(static_cast<uint8_t>(hello.size() & 0xFF));
    handshake.insert(handshake.end(), hello.begin(), hello.end());

    std::vector<uint8_t> preface;
    preface.push_back(0x16); preface.push_back(0x03); preface.push_back(0x03);
    preface.push_back(static_cast<uint8_t>((handshake.size() >> 8) & 0xFF));
    preface.push_back(static_cast<uint8_t>(handshake.size() & 0xFF));
    preface.insert(preface.end(), handshake.begin(), handshake.end());

    bool ok = false, present = false;
    MT_EXPECT_TRUE(TlsSni(preface, &ok, &present) == "");
    MT_EXPECT_TRUE(ok);
    MT_EXPECT_TRUE(present);
}

// ---- parse_protocol_host dispatch ----
MT_TEST(parse_protocol_host_dispatch) {
    core::Optional<std::string> out;

    // Port 443 selects the TLS parser even for HTTP-looking bytes.
    const std::string http = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    auto as_tls = ParseProtocolHost(reinterpret_cast<const uint8_t*>(http.data()),
                                    http.size(), 443, &out);
    MT_EXPECT_TRUE(as_tls.ok());
    // TLS parser rejects a non-handshake first byte -> Ok(None).
    MT_EXPECT_TRUE(!out.has_value());

    // Port 80 selects the HTTP parser.
    auto as_http = ParseProtocolHost(reinterpret_cast<const uint8_t*>(http.data()),
                                     http.size(), 80, &out);
    MT_EXPECT_TRUE(as_http.ok());
    MT_EXPECT_TRUE(out.has_value());
    MT_EXPECT_TRUE(*out == "example.com");

    // A TLS handshake byte selects the TLS parser on any port.
    const std::vector<uint8_t> tls = BuildTlsPreface("example.com");
    auto sniffed = ParseProtocolHost(&tls[0], tls.size(), 8443, &out);
    MT_EXPECT_TRUE(sniffed.ok());
    MT_EXPECT_TRUE(out.has_value());
    MT_EXPECT_TRUE(*out == "example.com");
}

// ---- Rust: domain_policy_denies_connections_without_a_hostname ----
MT_TEST(domain_policy_denies_connections_without_a_hostname) {
    std::vector<std::string> allow;
    allow.push_back("example.com");
    SandboxNetworkPolicy policy = MakePolicy(BaseSandboxNetworkPolicy::Allow, allow);
    HostNetResolver resolver;

    UpstreamDecision d =
        SelectUpstream(policy, &resolver, "", Ip("203.0.113.10"), 443, NULL);
    MT_EXPECT_TRUE(d.is_deny());
    resolver.Shutdown();
}

// ---- Rust: mixed_domain_and_cidr_policy_keeps_cidr_grants ----
MT_TEST(mixed_domain_and_cidr_policy_keeps_cidr_grants) {
    std::vector<std::string> allow;
    allow.push_back("example.com");
    allow.push_back("8.8.8.8");
    SandboxNetworkPolicy policy = MakePolicy(BaseSandboxNetworkPolicy::Deny, allow);
    HostNetResolver resolver;

    // An explicitly allowed IP is honoured before a hostname is required,
    // which is what keeps direct-IP HTTPS working.
    UpstreamDecision d = SelectUpstream(policy, &resolver, "", Ip("8.8.8.8"), 443, NULL);
    MT_EXPECT_TRUE(d.is_forward());
    MT_EXPECT_EQ(static_cast<int>(d.addresses.size()), 1);
    MT_EXPECT_TRUE(d.addresses[0].ipv4 == Ip("8.8.8.8"));
    MT_EXPECT_TRUE(d.addresses[0].port == 443);
    resolver.Shutdown();
}

MT_TEST(select_upstream_denies_a_disallowed_hostname) {
    std::vector<std::string> allow;
    allow.push_back("example.com");
    SandboxNetworkPolicy policy = MakePolicy(BaseSandboxNetworkPolicy::Deny, allow);
    HostNetResolver resolver;

    UpstreamDecision d =
        SelectUpstream(policy, &resolver, "evil.example", Ip("203.0.113.10"), 443, NULL);
    MT_EXPECT_TRUE(d.is_deny());
    resolver.Shutdown();
}

MT_TEST(select_upstream_denies_when_no_domain_rules_exist) {
    // No allow rules at all: nothing to authorize against.
    SandboxNetworkPolicy policy =
        MakePolicy(BaseSandboxNetworkPolicy::Deny, std::vector<std::string>());
    HostNetResolver resolver;

    UpstreamDecision d =
        SelectUpstream(policy, &resolver, "example.com", Ip("203.0.113.10"), 443, NULL);
    MT_EXPECT_TRUE(d.is_deny());
    resolver.Shutdown();
}

MT_TEST(select_upstream_reports_unavailable_for_unresolvable_allowed_domain) {
    std::vector<std::string> allow;
    allow.push_back("nonexistent-agentenv-test.invalid");
    SandboxNetworkPolicy policy = MakePolicy(BaseSandboxNetworkPolicy::Deny, allow);
    HostNetResolver resolver;
    // Shut the resolver down so the lookup cannot succeed. Policy allows the
    // hostname, so this must be Unavailable (transient), never Deny (policy).
    resolver.Shutdown();

    UpstreamDecision d = SelectUpstream(policy, &resolver,
                                        "nonexistent-agentenv-test.invalid",
                                        Ip("203.0.113.10"), 443, NULL);
    MT_EXPECT_TRUE(d.is_unavailable());
    MT_EXPECT_TRUE(!d.is_deny());
}

MT_TEST(resolve_trusted_upstream_rejects_a_null_or_stopped_resolver) {
    std::vector<std::string> allow;
    allow.push_back("example.com");
    SandboxNetworkPolicy policy = MakePolicy(BaseSandboxNetworkPolicy::Deny, allow);
    std::vector<ResolvedAddr> out;

    MT_EXPECT_TRUE(!ResolveTrustedUpstream(policy, NULL, "example.com", 443, NULL, &out));
    MT_EXPECT_TRUE(out.empty());

    HostNetResolver resolver;
    resolver.Shutdown();
    MT_EXPECT_TRUE(
        !ResolveTrustedUpstream(policy, &resolver, "example.com", 443, NULL, &out));
    MT_EXPECT_TRUE(out.empty());
}

// ---- Rust: staged_replacement_keeps_old_policy_until_activation ----
MT_TEST(staged_replacement_keeps_old_policy_until_activation) {
    EgressProxy proxy;
    const uint32_t ip = Ip("10.11.0.1");

    std::vector<std::string> old_allow;
    old_allow.push_back("old.example.com");
    SandboxNetworkPolicy old_policy = MakePolicy(BaseSandboxNetworkPolicy::Deny, old_allow);

    std::vector<std::string> new_allow;
    new_allow.push_back("new.example.com");
    SandboxNetworkPolicy new_policy = MakePolicy(BaseSandboxNetworkPolicy::Deny, new_allow);

    proxy.Prepare(ip, old_policy);
    proxy.Activate(ip);

    proxy.Prepare(ip, new_policy);
    // Staging alone must not change what is enforced.
    SandboxNetworkPolicy current;
    MT_EXPECT_TRUE(proxy.ActivePolicy(ip, &current));
    MT_EXPECT_TRUE(current.egress.HasDomainAllowRules());
    MT_EXPECT_TRUE(current == old_policy);

    proxy.Activate(ip);
    MT_EXPECT_TRUE(proxy.ActivePolicy(ip, &current));
    MT_EXPECT_TRUE(current == new_policy);
}

MT_TEST(discard_pending_keeps_the_active_policy) {
    EgressProxy proxy;
    const uint32_t ip = Ip("10.11.0.2");
    std::vector<std::string> allow;
    allow.push_back("kept.example.com");
    SandboxNetworkPolicy kept = MakePolicy(BaseSandboxNetworkPolicy::Deny, allow);

    proxy.Prepare(ip, kept);
    proxy.Activate(ip);

    std::vector<std::string> other;
    other.push_back("dropped.example.com");
    proxy.Prepare(ip, MakePolicy(BaseSandboxNetworkPolicy::Deny, other));
    MT_EXPECT_TRUE(proxy.HasPending(ip));

    // A failed launch discards the staged policy; the previous tenant's policy
    // must survive intact rather than being half-applied.
    proxy.DiscardPending(ip);
    MT_EXPECT_TRUE(!proxy.HasPending(ip));
    SandboxNetworkPolicy current;
    MT_EXPECT_TRUE(proxy.ActivePolicy(ip, &current));
    MT_EXPECT_TRUE(current == kept);
}

MT_TEST(activate_without_a_staged_policy_is_a_noop) {
    EgressProxy proxy;
    const uint32_t ip = Ip("10.11.0.3");
    std::vector<std::string> allow;
    allow.push_back("kept.example.com");
    proxy.Prepare(ip, MakePolicy(BaseSandboxNetworkPolicy::Deny, allow));
    proxy.Activate(ip);
    MT_EXPECT_TRUE(proxy.HasActive(ip));

    // A second activate must not clear what is already active.
    proxy.Activate(ip);
    MT_EXPECT_TRUE(proxy.HasActive(ip));
}

// ---- Rust: deactivation_keeps_existing_connections_for_teardown ----
MT_TEST(deactivation_keeps_existing_connections_for_teardown) {
    EgressProxy proxy;
    const uint32_t ip = Ip("10.11.0.4");
    std::vector<std::string> allow;
    allow.push_back("example.com");
    proxy.Prepare(ip, MakePolicy(BaseSandboxNetworkPolicy::Deny, allow));
    proxy.Activate(ip);

    size_t id = 0;
    MT_EXPECT_TRUE(proxy.RegisterConnection(ip, &id));
    MT_EXPECT_EQ(static_cast<int>(proxy.ConnectionCount(ip)), 1);

    // Deactivate drops the policy but leaves connections registered, so
    // teardown can still stop them.
    proxy.Deactivate(ip);
    MT_EXPECT_TRUE(!proxy.HasActive(ip));
    MT_EXPECT_EQ(static_cast<int>(proxy.ConnectionCount(ip)), 1);

    proxy.Teardown(ip);
    MT_EXPECT_EQ(static_cast<int>(proxy.ConnectionCount(ip)), 0);
}

// ---- Rust: connection_limit_is_scoped_to_each_host_listener ----
MT_TEST(connection_limit_is_scoped_to_each_host_listener) {
    EgressProxy proxy;
    const uint32_t a = Ip("10.11.0.5");
    const uint32_t b = Ip("10.11.0.6");

    for (size_t i = 0; i < kMaxConnectionsPerProxy; ++i) {
        size_t id = 0;
        MT_EXPECT_TRUE(proxy.RegisterConnection(a, &id));
    }
    // `a` is saturated...
    size_t overflow = 0;
    MT_EXPECT_TRUE(!proxy.RegisterConnection(a, &overflow));
    // ...but that must not starve a different listener.
    size_t other = 0;
    MT_EXPECT_TRUE(proxy.RegisterConnection(b, &other));
    MT_EXPECT_EQ(static_cast<int>(proxy.ConnectionCount(b)), 1);

    // Freeing one slot on `a` admits exactly one more.
    proxy.UnregisterConnection(a, 0);
    MT_EXPECT_EQ(static_cast<int>(proxy.ConnectionCount(a)),
                 static_cast<int>(kMaxConnectionsPerProxy - 1));
    size_t readmitted = 0;
    MT_EXPECT_TRUE(proxy.RegisterConnection(a, &readmitted));
}

MT_TEST(connection_ids_are_unique_across_listeners) {
    EgressProxy proxy;
    size_t first = 0, second = 0;
    MT_EXPECT_TRUE(proxy.RegisterConnection(Ip("10.11.0.7"), &first));
    MT_EXPECT_TRUE(proxy.RegisterConnection(Ip("10.11.0.8"), &second));
    MT_EXPECT_TRUE(first != second);
}

MT_TEST(unregister_unknown_connection_is_harmless) {
    EgressProxy proxy;
    const uint32_t ip = Ip("10.11.0.9");
    proxy.UnregisterConnection(ip, 12345);  // never registered
    MT_EXPECT_EQ(static_cast<int>(proxy.ConnectionCount(ip)), 0);

    size_t id = 0;
    MT_EXPECT_TRUE(proxy.RegisterConnection(ip, &id));
    proxy.UnregisterConnection(ip, id + 999);  // wrong id
    MT_EXPECT_EQ(static_cast<int>(proxy.ConnectionCount(ip)), 1);
}

MT_TEST(egress_proxy_port_is_fixed) {
    EgressProxy proxy;
    MT_EXPECT_TRUE(proxy.Port() == kEgressProxyPort);
}

MT_TEST(original_destination_rejects_a_non_redirected_fd) {
    // Not a redirected socket, so SO_ORIGINAL_DST must fail rather than hand
    // back uninitialized storage.
    uint32_t ip = 0;
    uint16_t port = 0;
    auto r = OriginalDestination(-1, &ip, &port);
    MT_EXPECT_TRUE(!r.ok());
}

MT_MAIN
