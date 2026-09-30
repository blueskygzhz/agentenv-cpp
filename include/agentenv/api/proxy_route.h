// SPDX-License-Identifier: MIT
// Rust: src/api/proxy.rs — the request-routing half (host/header parsing and
// the error-to-status mapping). The forwarding half needs an HTTP client and
// stays in `proxy.h`.
//
// Two ways a request names its sandbox:
//   * host route — `<port>-<sandbox-id>.<proxy-domain>`, which is how a
//     browser reaches a port inside the sandbox;
//   * routing headers — `x-agentenv-sandbox-id` / `x-agentenv-target-port`
//     (with `e2b-*` aliases), which is how a client that cannot control the
//     Host header does it.
//
// The host form is tried first, and a *malformed* host is an error rather than
// a fallthrough: a request that clearly meant to address a sandbox must not be
// silently reinterpreted as an API call.
#ifndef AGENTENV_API_PROXY_ROUTE_H_
#define AGENTENV_API_PROXY_ROUTE_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/orchestrator/store.h"

namespace agentenv {
namespace api {

/// Rust `PROXY_ROUTE`.
extern const char* const kProxyRoute;
/// Rust `SANDBOX_ID_HEADER` + its E2B-compatible alias.
extern const char* const kSandboxIdHeader;
extern const char* const kE2bSandboxIdHeader;
/// Rust `TARGET_PORT_HEADER` + its E2B-compatible alias.
extern const char* const kTargetPortHeader;
extern const char* const kE2bTargetPortHeader;
/// Rust `auth::API_KEY_HEADER` / `TRAFFIC_ACCESS_TOKEN_HEADER` /
/// `ENVD_ACCESS_TOKEN_HEADER`.
extern const char* const kApiKeyHeader;
extern const char* const kTrafficAccessTokenHeader;
extern const char* const kEnvdAccessTokenHeader;

/// Rust `enum ProxyRequestError`.
struct ProxyRequestError {
    enum class Kind {
        MissingSandboxId,
        InvalidSandboxId,
        MissingTargetPort,
        InvalidTargetPort,
        InvalidHostRoute,
        SandboxNotFound,
        SandboxUnavailable,
        AutoResumeFailed,
        AutoResumeTimedOut,
        MissingRuntimeRoute,
        InvalidUpstreamUri,
        InternalServerError,
    };

    Kind        kind = Kind::InternalServerError;
    /// Live for `InvalidHostRoute`.
    std::string detail;

    static ProxyRequestError Make(Kind kind);
    static ProxyRequestError InvalidHostRoute(const std::string& detail);

    /// Rust `proxy_error_response` — the status it maps to.
    int Status() const;
    /// The body message. Empty for `InternalServerError`, which Rust returns
    /// as a bare status with no body.
    std::string Message() const;
};

/// Rust `struct HostProxyRoute`.
struct HostProxyRoute {
    core::SandboxId sandbox_id;
    uint16_t        target_port = 0;
};

/// A request's headers, lowercased keys. `std::multimap` because a repeated
/// header is meaningful: the auth path rejects a duplicated credential rather
/// than picking one.
typedef std::multimap<std::string, std::string> HeaderMap;

/// Rust `single_header` — a value only when the header appears exactly once.
/// A repeated credential header is ambiguous, so it is treated as absent.
core::Optional<std::string> SingleHeader(const HeaderMap& headers,
                                         const std::string& name);

/// Rust `first_header_value` — the first present name wins, so the canonical
/// header takes precedence over its alias.
core::Optional<std::string> FirstHeaderValue(const HeaderMap& headers,
                                             const std::vector<std::string>& names);

/// Rust `strip_host_port` — drops a trailing `:port` only when it really is
/// one, leaving a bracketless IPv6 literal untouched.
std::string StripHostPort(const std::string& host);

/// Rust `has_proxy_prefix`.
bool HasProxyPrefix(const std::string& path);

/// Rust `has_routing_header`.
bool HasRoutingHeader(const HeaderMap& headers);

/// Rust `parse_host_proxy_route`.
///
/// An unset result means "not a sandbox host" — including the case where the
/// host *is* a bare proxy domain, which addresses the API itself.
core::Expected<core::Optional<HostProxyRoute>, ProxyRequestError>
    ParseHostProxyRoute(const core::Optional<std::string>& raw_host,
                        const std::vector<std::string>& domains);

/// Rust `parse_sandbox_id_header`.
core::Expected<core::SandboxId, ProxyRequestError>
    ParseSandboxIdHeader(const HeaderMap& headers);

/// Rust `parse_target_port_header` — port 0 is rejected, since it cannot be
/// connected to.
core::Expected<uint16_t, ProxyRequestError>
    ParseTargetPortHeader(const HeaderMap& headers);

/// Rust `route_for_auth` — resolves the sandbox a request is addressed to, for
/// the authorization decision. An unset result means the request could not be
/// attributed to a sandbox.
core::Optional<HostProxyRoute>
    RouteForAuth(const std::string& path, const HeaderMap& headers,
                 const std::vector<std::string>& domains);

/// Rust `is_sandbox_proxy_request`.
///
/// `matched_api_route` is Rust's `MatchedPath` extension: when the router
/// already matched an API route, bare routing headers do not make the request
/// a proxy request.
bool IsSandboxProxyRequest(const std::string& path, const HeaderMap& headers,
                           const std::vector<std::string>& domains,
                           bool matched_api_route);

/// Rust `effective_envd_port` — a paused sandbox keeps the control-plane port
/// it was captured with, so a resume reaches the same port the guest is
/// actually listening on.
uint16_t EffectiveEnvdPort(const orchestrator::SandboxMetadata& metadata,
                           uint16_t configured_control_plane_port);

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_PROXY_ROUTE_H_
