// SPDX-License-Identifier: MIT
// Rust: src/api/impls/auth.rs — the `require_auth` middleware.
//
// Scope. Rust's `require_auth` is an axum middleware: it inspects the request,
// decides, and either calls `next` or short-circuits. The decision itself is
// pure, so it is separated here from the framework plumbing. A server
// integration calls `Decide`, then applies the returned action.
//
// Two authorization worlds:
//   * control plane (not a proxy request) — a valid API key, full stop. A
//     template-builder sandbox is additionally hidden behind 404, so an
//     internal build sandbox is not addressable through the public API.
//   * data plane (proxy request) — the sandbox's own policy decides. Requests
//     to the envd control port need the envd access token when the sandbox is
//     secure; everything else needs either public traffic to be allowed or a
//     valid traffic access token.
//
// Header stripping is part of the contract, not hygiene: a credential that was
// not *used* for this decision must not reach the sandbox, or a guest could
// replay it. The API key is removed whenever it authenticated a proxy request,
// and the envd token is removed unless it was the thing that authorized it.
#ifndef AGENTENV_API_AUTH_H_
#define AGENTENV_API_AUTH_H_

#include <string>
#include <vector>

#include "agentenv/api/proxy_route.h"
#include "agentenv/core/optional.h"
#include "agentenv/orchestrator/store.h"

namespace agentenv {
namespace api {

/// What the caller should do with the request.
struct AuthDecision {
    enum class Action {
        /// Rust `next.run(request)`.
        Allow,
        /// Rust `StatusCode::UNAUTHORIZED`.
        Unauthorized,
        /// Rust `proxy::sandbox_not_found_response` / `StatusCode::NOT_FOUND`.
        NotFound,
        /// Rust `StatusCode::INTERNAL_SERVER_ERROR`.
        InternalServerError,
    };

    Action action = Action::Unauthorized;

    /// Headers the caller must strip before forwarding. Only meaningful for
    /// `Allow`, but populated on rejection too so a short-circuit response is
    /// built from a request that no longer carries the credential.
    bool strip_api_key           = false;
    bool strip_envd_access_token = false;

    /// Set when the request was attributed to a sandbox.
    core::Optional<core::SandboxId> sandbox_id;

    static AuthDecision Allow();
    static AuthDecision Reject(Action action);
};

/// The facts `Decide` needs about the addressed sandbox. Absent when the
/// sandbox does not exist; the lookup failing is reported separately.
struct AuthSandboxFacts {
    bool     exists = false;
    bool     lookup_failed = false;
    bool     template_builder = false;
    bool     secure = false;
    bool     allow_public_traffic = false;
    uint16_t envd_port = 0;
};

/// Everything about the request that the decision depends on.
struct AuthRequest {
    std::string              path;
    HeaderMap                headers;
    std::vector<std::string> proxy_domains;
    /// Rust's `MatchedPath` extension: the router already matched an API route.
    bool                     matched_api_route = false;
};

/// The credential checks, supplied by the caller so this layer stays free of
/// the key store and the orchestrator.
class AuthChecks {
 public:
    virtual ~AuthChecks() {}

    /// Rust `ApiImpl::has_valid_api_key`.
    virtual bool HasValidApiKey(const HeaderMap& headers) const = 0;

    /// Rust `ApiImpl::has_valid_traffic_access_token`.
    virtual bool HasValidTrafficAccessToken(const HeaderMap& headers,
                                            const core::SandboxId& id) const = 0;

    /// Rust `Orchestrator::validate_envd_access_token`.
    virtual bool ValidateEnvdAccessToken(const core::SandboxId& id,
                                         const std::string& candidate) const = 0;

    /// Looks the sandbox up. Rust does this through
    /// `Orchestrator::get_sandbox`.
    virtual AuthSandboxFacts LookupSandbox(const core::SandboxId& id) const = 0;
};

/// Rust `require_auth`.
AuthDecision Decide(const AuthRequest& request, const AuthChecks& checks);

/// Rust's `/health` | `/metrics` bypass — exempt unless the request is a
/// proxy request, so a sandbox path that happens to be named `/health` is
/// still authorized.
bool IsUnauthenticatedPath(const std::string& path);

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_AUTH_H_
