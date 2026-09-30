// SPDX-License-Identifier: MIT
// Rust: src/api/impls/auth.rs + the routing half of src/api/proxy.rs.
#include "agentenv/api/auth.h"

#include <string>
#include <vector>

#include "microtest.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT

namespace {

const char* const kValidKey = "secret-key";
const char* const kSandbox  = "11111111-2222-3333-4444-555555555555";

core::SandboxId Id(const std::string& text) {
    auto parsed = core::SandboxId::Parse(text);
    MT_EXPECT_TRUE(parsed.ok());
    return parsed.value();
}

/// Scriptable credential checks, so each test states only what it cares about.
class Checks : public AuthChecks {
 public:
    bool             valid_api_key      = false;
    bool             valid_traffic      = false;
    std::string      valid_envd_token;
    AuthSandboxFacts facts;

    Checks() {
        facts.exists    = true;
        facts.envd_port = 49983;
    }

    bool HasValidApiKey(const HeaderMap&) const override { return valid_api_key; }
    bool HasValidTrafficAccessToken(const HeaderMap&,
                                    const core::SandboxId&) const override {
        return valid_traffic;
    }
    bool ValidateEnvdAccessToken(const core::SandboxId&,
                                 const std::string& candidate) const override {
        return !valid_envd_token.empty() && candidate == valid_envd_token;
    }
    AuthSandboxFacts LookupSandbox(const core::SandboxId&) const override { return facts; }
};

AuthRequest Request(const std::string& path) {
    AuthRequest request;
    request.path = path;
    return request;
}

void Add(AuthRequest* request, const std::string& name, const std::string& value) {
    request->headers.insert(std::make_pair(name, value));
}

}  // namespace

// ---- header helpers ---------------------------------------------------------

MT_TEST(auth_single_header_rejects_duplicates) {
    HeaderMap headers;
    headers.insert(std::make_pair(kApiKeyHeader, "a"));
    MT_EXPECT_TRUE(SingleHeader(headers, kApiKeyHeader).has_value());

    // A repeated credential is ambiguous, so it counts as absent rather than
    // letting one of the two values decide.
    headers.insert(std::make_pair(kApiKeyHeader, "b"));
    MT_EXPECT_TRUE(!SingleHeader(headers, kApiKeyHeader).has_value());
}

MT_TEST(auth_first_header_value_prefers_canonical_name) {
    HeaderMap headers;
    headers.insert(std::make_pair(kE2bSandboxIdHeader, "from-alias"));
    std::vector<std::string> names;
    names.push_back(kSandboxIdHeader);
    names.push_back(kE2bSandboxIdHeader);

    auto alias_only = FirstHeaderValue(headers, names);
    MT_EXPECT_TRUE(alias_only.has_value());
    MT_EXPECT_EQ(*alias_only, std::string("from-alias"));

    headers.insert(std::make_pair(kSandboxIdHeader, "canonical"));
    auto both = FirstHeaderValue(headers, names);
    MT_EXPECT_TRUE(both.has_value());
    MT_EXPECT_EQ(*both, std::string("canonical"));
}

MT_TEST(auth_strip_host_port_leaves_ipv6_intact) {
    MT_EXPECT_EQ(StripHostPort("example.com:8080"), std::string("example.com"));
    MT_EXPECT_EQ(StripHostPort("example.com"), std::string("example.com"));
    // A bracketless IPv6 literal's trailing group is not a port.
    MT_EXPECT_EQ(StripHostPort("::1"), std::string("::1"));
    MT_EXPECT_EQ(StripHostPort("fe80::1"), std::string("fe80::1"));
    // A non-numeric suffix is not a port either.
    MT_EXPECT_EQ(StripHostPort("example.com:http"), std::string("example.com:http"));
}

MT_TEST(auth_has_proxy_prefix) {
    MT_EXPECT_TRUE(HasProxyPrefix("/proxy"));
    MT_EXPECT_TRUE(HasProxyPrefix("/proxy/anything"));
    MT_EXPECT_TRUE(!HasProxyPrefix("/proxyish"));
    MT_EXPECT_TRUE(!HasProxyPrefix("/sandboxes"));
}

// ---- host route -------------------------------------------------------------

MT_TEST(auth_host_route_parses_port_and_sandbox) {
    std::vector<std::string> domains;
    domains.push_back("agentenv.dev");

    auto route = ParseHostProxyRoute(
        core::Optional<std::string>(std::string("8080-") + kSandbox + ".agentenv.dev"),
        domains);
    MT_EXPECT_TRUE(route.ok());
    MT_EXPECT_TRUE(route.value().has_value());
    MT_EXPECT_EQ(route.value()->target_port, static_cast<uint16_t>(8080));
    MT_EXPECT_TRUE(route.value()->sandbox_id == Id(kSandbox));
}

MT_TEST(auth_host_route_normalizes_case_port_and_root_label) {
    std::vector<std::string> domains;
    domains.push_back("agentenv.dev");

    // Uppercase, an explicit port, a trailing root label and surrounding
    // whitespace all normalize away.
    const std::string host =
        std::string("  8080-") + kSandbox + ".AgentENV.dev.:443  ";
    auto route = ParseHostProxyRoute(core::Optional<std::string>(host), domains);
    MT_EXPECT_TRUE(route.ok());
    MT_EXPECT_TRUE(route.value().has_value());
    MT_EXPECT_EQ(route.value()->target_port, static_cast<uint16_t>(8080));
}

MT_TEST(auth_host_route_bare_domain_is_not_a_sandbox) {
    std::vector<std::string> domains;
    domains.push_back("agentenv.dev");

    // The bare proxy domain addresses the API itself.
    auto route =
        ParseHostProxyRoute(core::Optional<std::string>(std::string("agentenv.dev")),
                            domains);
    MT_EXPECT_TRUE(route.ok());
    MT_EXPECT_TRUE(!route.value().has_value());
}

MT_TEST(auth_host_route_multi_label_prefix_is_not_a_sandbox) {
    std::vector<std::string> domains;
    domains.push_back("agentenv.dev");

    // Exactly one label is a sandbox host; `a.b.<domain>` is something else.
    auto route = ParseHostProxyRoute(
        core::Optional<std::string>(std::string("8080-") + kSandbox + ".x.agentenv.dev"),
        domains);
    MT_EXPECT_TRUE(route.ok());
    MT_EXPECT_TRUE(!route.value().has_value());
}

MT_TEST(auth_host_route_malformed_is_an_error_not_a_fallthrough) {
    std::vector<std::string> domains;
    domains.push_back("agentenv.dev");

    // The host clearly meant to address a sandbox, so a bad port or id is
    // rejected rather than silently reinterpreted as an API call.
    auto bad_port = ParseHostProxyRoute(
        core::Optional<std::string>(std::string("0-") + kSandbox + ".agentenv.dev"),
        domains);
    MT_EXPECT_TRUE(!bad_port.ok());

    auto empty_id = ParseHostProxyRoute(
        core::Optional<std::string>(std::string("8080-.agentenv.dev")), domains);
    MT_EXPECT_TRUE(!empty_id.ok());

    auto bad_id = ParseHostProxyRoute(
        core::Optional<std::string>(std::string("8080-not-a-uuid.agentenv.dev")), domains);
    MT_EXPECT_TRUE(!bad_id.ok());
}

MT_TEST(auth_host_route_without_domains_is_never_a_sandbox) {
    auto route = ParseHostProxyRoute(
        core::Optional<std::string>(std::string("8080-") + kSandbox + ".agentenv.dev"),
        std::vector<std::string>());
    MT_EXPECT_TRUE(route.ok());
    MT_EXPECT_TRUE(!route.value().has_value());
}

// ---- header route -----------------------------------------------------------

MT_TEST(auth_header_route_parses_and_validates) {
    HeaderMap headers;
    headers.insert(std::make_pair(kSandboxIdHeader, kSandbox));
    headers.insert(std::make_pair(kTargetPortHeader, "3000"));

    auto id = ParseSandboxIdHeader(headers);
    MT_EXPECT_TRUE(id.ok());
    auto port = ParseTargetPortHeader(headers);
    MT_EXPECT_TRUE(port.ok());
    MT_EXPECT_EQ(port.value(), static_cast<uint16_t>(3000));
}

MT_TEST(auth_header_route_rejects_missing_and_invalid) {
    HeaderMap empty;
    auto missing_id = ParseSandboxIdHeader(empty);
    MT_EXPECT_TRUE(!missing_id.ok());
    MT_EXPECT_TRUE(missing_id.error().kind == ProxyRequestError::Kind::MissingSandboxId);
    MT_EXPECT_EQ(missing_id.error().Status(), 400);

    auto missing_port = ParseTargetPortHeader(empty);
    MT_EXPECT_TRUE(!missing_port.ok());
    MT_EXPECT_TRUE(missing_port.error().kind ==
                   ProxyRequestError::Kind::MissingTargetPort);

    HeaderMap bad;
    bad.insert(std::make_pair(kSandboxIdHeader, "not-a-uuid"));
    // Port 0 cannot be connected to, so it is invalid rather than a default.
    bad.insert(std::make_pair(kTargetPortHeader, "0"));
    MT_EXPECT_TRUE(!ParseSandboxIdHeader(bad).ok());
    MT_EXPECT_TRUE(!ParseTargetPortHeader(bad).ok());

    HeaderMap overflow;
    overflow.insert(std::make_pair(kTargetPortHeader, "70000"));
    MT_EXPECT_TRUE(!ParseTargetPortHeader(overflow).ok());
}

MT_TEST(auth_proxy_error_status_mapping) {
    MT_EXPECT_EQ(ProxyRequestError::Make(ProxyRequestError::Kind::SandboxNotFound).Status(),
                 404);
    // Gone, not NotFound: the sandbox exists but is not proxyable now.
    MT_EXPECT_EQ(
        ProxyRequestError::Make(ProxyRequestError::Kind::SandboxUnavailable).Status(), 410);
    MT_EXPECT_EQ(ProxyRequestError::Make(ProxyRequestError::Kind::AutoResumeFailed).Status(),
                 502);
    MT_EXPECT_EQ(
        ProxyRequestError::Make(ProxyRequestError::Kind::AutoResumeTimedOut).Status(), 504);
    MT_EXPECT_EQ(
        ProxyRequestError::Make(ProxyRequestError::Kind::MissingRuntimeRoute).Status(), 502);
    // Rust returns a bare status with no body for the internal case.
    MT_EXPECT_TRUE(
        ProxyRequestError::Make(ProxyRequestError::Kind::InternalServerError)
            .Message().empty());
    MT_EXPECT_EQ(ProxyRequestError::InvalidHostRoute("boom").Message(), std::string("boom"));
}

// ---- control plane ----------------------------------------------------------

MT_TEST(auth_health_and_metrics_bypass_when_not_proxying) {
    Checks checks;
    MT_EXPECT_TRUE(Decide(Request("/health"), checks).action ==
                   AuthDecision::Action::Allow);
    MT_EXPECT_TRUE(Decide(Request("/metrics"), checks).action ==
                   AuthDecision::Action::Allow);
    // Any other path still needs a key.
    MT_EXPECT_TRUE(Decide(Request("/sandboxes"), checks).action ==
                   AuthDecision::Action::Unauthorized);
}

MT_TEST(auth_health_bypass_does_not_apply_to_proxy_request) {
    Checks checks;
    AuthRequest request = Request("/health");
    // Routing headers make this a data-plane request for a sandbox path that
    // merely happens to be called /health.
    Add(&request, kSandboxIdHeader, kSandbox);
    Add(&request, kTargetPortHeader, "8080");

    // Not authorized: no public traffic, no traffic token.
    MT_EXPECT_TRUE(Decide(request, checks).action == AuthDecision::Action::Unauthorized);
}

MT_TEST(auth_control_plane_requires_api_key) {
    Checks checks;
    MT_EXPECT_TRUE(Decide(Request("/sandboxes"), checks).action ==
                   AuthDecision::Action::Unauthorized);

    checks.valid_api_key = true;
    MT_EXPECT_TRUE(Decide(Request("/sandboxes"), checks).action ==
                   AuthDecision::Action::Allow);
}

MT_TEST(auth_control_plane_hides_template_builder_sandbox) {
    Checks checks;
    checks.valid_api_key          = true;
    checks.facts.template_builder = true;

    AuthRequest request = Request("/sandboxes/x");
    Add(&request, kSandboxIdHeader, kSandbox);
    Add(&request, kTargetPortHeader, "8080");
    request.matched_api_route = true;  // the router matched an API route

    // 404 rather than 403: the public API must not reveal that the id exists.
    MT_EXPECT_TRUE(Decide(request, checks).action == AuthDecision::Action::NotFound);
}

MT_TEST(auth_control_plane_reports_lookup_failure) {
    Checks checks;
    checks.valid_api_key       = true;
    checks.facts.lookup_failed = true;

    AuthRequest request = Request("/sandboxes/x");
    Add(&request, kSandboxIdHeader, kSandbox);
    Add(&request, kTargetPortHeader, "8080");
    request.matched_api_route = true;

    MT_EXPECT_TRUE(Decide(request, checks).action ==
                   AuthDecision::Action::InternalServerError);
}

// ---- data plane -------------------------------------------------------------

MT_TEST(auth_data_plane_allows_public_traffic) {
    Checks checks;
    checks.facts.allow_public_traffic = true;

    AuthRequest request = Request("/");
    Add(&request, kSandboxIdHeader, kSandbox);
    Add(&request, kTargetPortHeader, "8080");

    MT_EXPECT_TRUE(Decide(request, checks).action == AuthDecision::Action::Allow);
}

MT_TEST(auth_data_plane_accepts_traffic_token) {
    Checks checks;
    checks.valid_traffic = true;

    AuthRequest request = Request("/");
    Add(&request, kSandboxIdHeader, kSandbox);
    Add(&request, kTargetPortHeader, "8080");
    Add(&request, kTrafficAccessTokenHeader, "token");

    MT_EXPECT_TRUE(Decide(request, checks).action == AuthDecision::Action::Allow);
}

MT_TEST(auth_data_plane_envd_port_needs_token_when_secure) {
    Checks checks;
    checks.facts.secure    = true;
    checks.valid_envd_token = "envd-token";

    AuthRequest denied = Request("/");
    Add(&denied, kSandboxIdHeader, kSandbox);
    Add(&denied, kTargetPortHeader, "49983");  // the envd control port
    MT_EXPECT_TRUE(Decide(denied, checks).action == AuthDecision::Action::Unauthorized);

    AuthRequest allowed = Request("/");
    Add(&allowed, kSandboxIdHeader, kSandbox);
    Add(&allowed, kTargetPortHeader, "49983");
    Add(&allowed, kEnvdAccessTokenHeader, "envd-token");
    AuthDecision decision = Decide(allowed, checks);
    MT_EXPECT_TRUE(decision.action == AuthDecision::Action::Allow);
    // The token authorized this request, so it is the one credential that may
    // be forwarded.
    MT_EXPECT_TRUE(!decision.strip_envd_access_token);
}

MT_TEST(auth_data_plane_envd_port_open_when_not_secure) {
    Checks checks;
    checks.facts.secure = false;

    AuthRequest request = Request("/");
    Add(&request, kSandboxIdHeader, kSandbox);
    Add(&request, kTargetPortHeader, "49983");

    // A non-secure sandbox has no envd token to check.
    MT_EXPECT_TRUE(Decide(request, checks).action == AuthDecision::Action::Allow);
}

MT_TEST(auth_data_plane_envd_token_does_not_authorize_other_ports) {
    Checks checks;
    checks.facts.secure     = true;
    checks.valid_envd_token = "envd-token";

    AuthRequest request = Request("/");
    Add(&request, kSandboxIdHeader, kSandbox);
    Add(&request, kTargetPortHeader, "8080");  // not the envd port
    Add(&request, kEnvdAccessTokenHeader, "envd-token");

    // A non-envd port is governed by the traffic policy, which denies here.
    AuthDecision decision = Decide(request, checks);
    MT_EXPECT_TRUE(decision.action == AuthDecision::Action::Unauthorized);
    MT_EXPECT_TRUE(decision.strip_envd_access_token);
}

MT_TEST(auth_data_plane_strips_api_key_it_consumed) {
    Checks checks;
    checks.valid_api_key              = true;
    checks.facts.allow_public_traffic = true;

    AuthRequest request = Request("/");
    Add(&request, kSandboxIdHeader, kSandbox);
    Add(&request, kTargetPortHeader, "8080");
    Add(&request, kApiKeyHeader, kValidKey);

    // Forwarding a control-plane credential into the sandbox would let a guest
    // replay it.
    AuthDecision decision = Decide(request, checks);
    MT_EXPECT_TRUE(decision.action == AuthDecision::Action::Allow);
    MT_EXPECT_TRUE(decision.strip_api_key);
}

MT_TEST(auth_data_plane_unattributable_request_strips_envd_token) {
    Checks checks;

    // `/proxy`-prefixed but with no routing headers: it cannot be attributed
    // to a sandbox, so the envd token cannot have authorized anything.
    AuthRequest request = Request("/proxy/");
    Add(&request, kEnvdAccessTokenHeader, "envd-token");

    AuthDecision decision = Decide(request, checks);
    MT_EXPECT_TRUE(decision.action == AuthDecision::Action::Allow);
    MT_EXPECT_TRUE(decision.strip_envd_access_token);
}

MT_TEST(auth_data_plane_unattributable_without_prefix_or_key_is_denied) {
    Checks checks;
    AuthRequest request = Request("/");
    // A routing header makes it a proxy request, but an unparseable id means
    // it cannot be attributed.
    Add(&request, kSandboxIdHeader, "not-a-uuid");

    MT_EXPECT_TRUE(Decide(request, checks).action == AuthDecision::Action::Unauthorized);
}

MT_TEST(auth_data_plane_missing_sandbox_is_not_found) {
    Checks checks;
    checks.facts.exists = false;

    AuthRequest request = Request("/");
    Add(&request, kSandboxIdHeader, kSandbox);
    Add(&request, kTargetPortHeader, "8080");

    AuthDecision decision = Decide(request, checks);
    MT_EXPECT_TRUE(decision.action == AuthDecision::Action::NotFound);
    MT_EXPECT_TRUE(decision.strip_envd_access_token);
    MT_EXPECT_TRUE(decision.sandbox_id.has_value());
}

MT_TEST(auth_data_plane_template_builder_is_not_found) {
    Checks checks;
    checks.facts.template_builder     = true;
    checks.facts.allow_public_traffic = true;

    AuthRequest request = Request("/");
    Add(&request, kSandboxIdHeader, kSandbox);
    Add(&request, kTargetPortHeader, "8080");

    MT_EXPECT_TRUE(Decide(request, checks).action == AuthDecision::Action::NotFound);
}

MT_TEST(auth_is_sandbox_proxy_request_classification) {
    HeaderMap none;
    std::vector<std::string> domains;
    domains.push_back("agentenv.dev");

    MT_EXPECT_TRUE(IsSandboxProxyRequest("/proxy/x", none, domains, false));

    HeaderMap host;
    host.insert(std::make_pair("host",
                               std::string("8080-") + kSandbox + ".agentenv.dev"));
    MT_EXPECT_TRUE(IsSandboxProxyRequest("/", host, domains, false));

    HeaderMap bad_host;
    bad_host.insert(std::make_pair("host", std::string("0-") + kSandbox + ".agentenv.dev"));
    // A malformed sandbox host is still a proxy request; it just fails later
    // with a routing error rather than being served as an API call.
    MT_EXPECT_TRUE(IsSandboxProxyRequest("/", bad_host, domains, false));

    HeaderMap routing;
    routing.insert(std::make_pair(kSandboxIdHeader, kSandbox));
    MT_EXPECT_TRUE(IsSandboxProxyRequest("/", routing, domains, false));
    // Once the router matched an API route, bare routing headers do not make
    // it a proxy request.
    MT_EXPECT_TRUE(!IsSandboxProxyRequest("/", routing, domains, true));

    MT_EXPECT_TRUE(!IsSandboxProxyRequest("/sandboxes", none, domains, false));
}

// ---- proxy forwarding helpers (Rust: src/api/proxy.rs) ---------------------

MT_TEST(proxy_strip_prefix_yields_the_forwarded_path) {
    MT_EXPECT_EQ(StripProxyPrefix("/proxy/process.Process/StreamInput"),
                 std::string("/process.Process/StreamInput"));
    // The bare prefix forwards to the sandbox root.
    MT_EXPECT_EQ(StripProxyPrefix("/proxy"), std::string(""));
    MT_EXPECT_EQ(StripProxyPrefix("/proxy/"), std::string("/"));
}

MT_TEST(proxy_strip_prefix_discards_a_non_proxy_path) {
    // Not under the prefix: passing an unstripped API path through would
    // address the wrong thing on the sandbox, so it yields nothing.
    MT_EXPECT_EQ(StripProxyPrefix("/sandboxes"), std::string(""));
    MT_EXPECT_EQ(StripProxyPrefix("/"), std::string(""));
    MT_EXPECT_EQ(StripProxyPrefix(""), std::string(""));
}

MT_TEST(proxy_identifies_the_envd_stream_input_request) {
    MT_EXPECT_TRUE(IsEnvdStreamInputRequest("POST", kEnvdStreamInputPath));
    // Method and path must both match: a GET on the same path is not the
    // long-lived attach request.
    MT_EXPECT_TRUE(!IsEnvdStreamInputRequest("GET", kEnvdStreamInputPath));
    MT_EXPECT_TRUE(!IsEnvdStreamInputRequest("POST", "/process.Process/Start"));
    // Case-sensitive, matching Rust's comparison against `Method::POST`.
    MT_EXPECT_TRUE(!IsEnvdStreamInputRequest("post", kEnvdStreamInputPath));
}

MT_TEST(proxy_stream_closed_failure_classification) {
    // What hyper reports when the stream ended.
    MT_EXPECT_TRUE(IsStreamClosedFailure(true, "anything"));
    // hyper-util keeps its ErrorKind private, so this exact text is the only
    // available signal for a torn-down request channel.
    MT_EXPECT_TRUE(IsStreamClosedFailure(false, "client error (SendRequest)"));
    // Anything else is a genuine transport failure.
    MT_EXPECT_TRUE(!IsStreamClosedFailure(false, "client error (Connect)"));
    MT_EXPECT_TRUE(!IsStreamClosedFailure(false, ""));
}

MT_TEST(proxy_benign_disconnect_is_limited_to_stream_input) {
    // A detaching client on the attach endpoint is expected, not a failure.
    MT_EXPECT_TRUE(
        IsBenignStreamInputDisconnect("POST", kEnvdStreamInputPath, true, ""));
    MT_EXPECT_TRUE(IsBenignStreamInputDisconnect("POST", kEnvdStreamInputPath, false,
                                                 "client error (SendRequest)"));

    // The same error on an ordinary request is a real failure; treating it as
    // benign would hide a broken sandbox.
    MT_EXPECT_TRUE(!IsBenignStreamInputDisconnect("POST", "/process.Process/Start", true, ""));
    MT_EXPECT_TRUE(!IsBenignStreamInputDisconnect("GET", kEnvdStreamInputPath, true, ""));
    // And an unrelated error on the attach endpoint still fails.
    MT_EXPECT_TRUE(!IsBenignStreamInputDisconnect("POST", kEnvdStreamInputPath, false,
                                                  "client error (Connect)"));
}

MT_TEST(proxy_timeouts_are_ordered_sensibly) {
    // Connecting must not be allowed to consume the whole response budget,
    // and an auto-resume legitimately outlasts a single response.
    MT_EXPECT_TRUE(kProxyConnectTimeoutMs < kProxyResponseHeaderTimeoutMs);
    MT_EXPECT_TRUE(kProxyResponseHeaderTimeoutMs <= kProxyAutoResumeTimeoutMs);
    MT_EXPECT_TRUE(kProxyRequestBodyIdleTimeoutMs > 0);
}

int main() { return microtest::RunAll(); }
