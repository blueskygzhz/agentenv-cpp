// SPDX-License-Identifier: MIT
// Rust: src/api/proxy.rs — the dispatch half: which requests are proxied, the
// header hygiene applied on the hop, and the error responses. The routing
// parsers live in `proxy_route.h`.
//
// What this guarantees, whatever the forwarder behind it can do: a request
// that addresses a sandbox — `/proxy/*`, a sandbox data-plane Host, or bare
// routing headers on a path no API route matched — is answered by the proxy
// and never reaches a control-plane handler. Rust gets that from
// `sandbox_proxy_classifier` plus the `/proxy` routes and the router fallback;
// without it a request authorized by a sandbox's *data-plane* policy would be
// served by the control-plane API with no API key.
#ifndef AGENTENV_API_PROXY_H_
#define AGENTENV_API_PROXY_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/api/proxy_route.h"
#include "agentenv/api/server.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace orchestrator { class Orchestrator; }
namespace api {

/// Rust `HttpRouteSource` — how the proxied request named its sandbox.
enum class ProxyRouteSource {
    /// `/proxy` and `/proxy/*`.
    Prefix,
    /// `<port>-<sandbox-id>.<proxy-domain>`.
    Host,
    /// Routing headers on a path no API route matched (the router fallback).
    Header,
};

/// The data plane: forwards one request to the sandbox it addresses.
///
/// `forward_path` is the path the upstream sees (Rust strips `/proxy` for the
/// prefix form and keeps the original path otherwise).
class ProxyForwarder {
 public:
    virtual ~ProxyForwarder() {}
    virtual HttpResponse Forward(const HttpRequest& request, const std::string& forward_path,
                                 ProxyRouteSource source) const = 0;
};

/// Rust `proxy_request` / `resolve_proxy_request` / `proxy_http_request`.
///
/// Resolution is Rust's: routing headers parsed (400 on a bad one), the
/// sandbox looked up through `Orchestrator::ProxyLookupFor`, a paused
/// auto-resume sandbox resumed once with `EnsureMinimum`, and the
/// not-found / unavailable / route-missing outcomes mapped to the same
/// statuses. The upstream hop is a buffered HTTP/1.1 exchange over
/// `core::http`, with the same header sanitizing and `x-forwarded-*`
/// injection. Deviation: WebSocket upgrades are refused with 501 (Rust
/// tunnels them), since `core::http` cannot hold a connection open.
std::shared_ptr<ProxyForwarder> MakeOrchestratorProxyForwarder(
    orchestrator::Orchestrator* orchestrator, int64_t auto_resume_min_timeout_ms);

/// Rust `sandbox_proxy_classifier` + the `/proxy` routes + `proxy_via_fallback`.
///
/// Install it after the auth layer, as Rust does (`require_auth` wraps the
/// classifier). Returns false — the request was answered — for every proxy
/// request, so none of them can fall through to an API handler.
Middleware MakeSandboxProxyClassifier(std::vector<std::string> proxy_domains,
                                      std::shared_ptr<ProxyForwarder> forwarder);

/// Rust `proxy_error_response`: `text/plain` with the message, or a bare
/// status for `InternalServerError`.
HttpResponse ProxyErrorResponse(const ProxyRequestError& error);

/// Rust `sandbox_not_found_response`.
HttpResponse SandboxNotFoundResponse();

/// Rust `remove_hop_by_hop_headers` — the fixed list plus every header the
/// `Connection` header nominates, plus `keep-alive`.
void RemoveHopByHopHeaders(HeaderMap* headers);

/// Rust `sanitize_request_headers` — the routing headers, the traffic token
/// and `Host` belong to the client→AgentENV hop and must not reach the guest.
void SanitizeRequestHeaders(HeaderMap* headers);

/// Rust `sanitize_websocket_request_headers`.
void SanitizeWebsocketRequestHeaders(HeaderMap* headers);

/// Rust `remove_websocket_handshake_headers` — every `sec-websocket*` header.
void RemoveWebsocketHandshakeHeaders(HeaderMap* headers);

/// Rust `header_contains_token` — comma-split, trimmed, ASCII
/// case-insensitive.
bool HeaderContainsToken(const HeaderMap& headers, const std::string& name,
                         const std::string& token);

/// Rust `is_websocket_upgrade_request`.
bool IsWebsocketUpgradeRequest(const HeaderMap& headers);

/// Rust `inject_forwarded_headers`. `x-forwarded-*` are *replaced*, never
/// appended to, so a client cannot pre-seed them.
void InjectForwardedHeaders(HeaderMap* headers, const core::Optional<std::string>& original_host,
                            const std::string& method,
                            const core::Optional<std::string>& upstream_path_and_query,
                            const std::string& forwarded_proto);

/// Rust `map_upstream_response` header filtering: the same hop-by-hop removal
/// on the way back.
void SanitizeResponseHeaders(HeaderMap* headers);

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_PROXY_H_
