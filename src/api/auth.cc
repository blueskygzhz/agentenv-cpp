// SPDX-License-Identifier: MIT
// Rust: src/api/impls/auth.rs
#include "agentenv/api/auth.h"

namespace agentenv {
namespace api {

AuthDecision AuthDecision::Allow() {
    AuthDecision decision;
    decision.action = Action::Allow;
    return decision;
}

AuthDecision AuthDecision::Reject(Action action) {
    AuthDecision decision;
    decision.action = action;
    return decision;
}

bool IsUnauthenticatedPath(const std::string& path) {
    return path == "/health" || path == "/metrics";
}

AuthDecision Decide(const AuthRequest& request, const AuthChecks& checks) {
    const bool proxy_request = IsSandboxProxyRequest(
        request.path, request.headers, request.proxy_domains, request.matched_api_route);

    // The health/metrics bypass does not apply to a proxy request: a path
    // inside the sandbox that happens to be `/health` still needs authorizing.
    if (IsUnauthenticatedPath(request.path) && !proxy_request) {
        return AuthDecision::Allow();
    }

    // ---- control plane ------------------------------------------------------

    if (!proxy_request) {
        if (!checks.HasValidApiKey(request.headers)) {
            return AuthDecision::Reject(AuthDecision::Action::Unauthorized);
        }

        // A template-builder sandbox is an internal build artifact, so it is
        // hidden rather than refused: the public API must not reveal that the
        // id exists.
        core::Optional<HostProxyRoute> route =
            RouteForAuth(request.path, request.headers, request.proxy_domains);
        if (route.has_value()) {
            const AuthSandboxFacts facts = checks.LookupSandbox(route->sandbox_id);
            if (facts.lookup_failed) {
                return AuthDecision::Reject(AuthDecision::Action::InternalServerError);
            }
            if (facts.exists && facts.template_builder) {
                return AuthDecision::Reject(AuthDecision::Action::NotFound);
            }
        }
        return AuthDecision::Allow();
    }

    // ---- data plane ---------------------------------------------------------

    const bool has_api_key = checks.HasValidApiKey(request.headers);

    core::Optional<HostProxyRoute> route =
        RouteForAuth(request.path, request.headers, request.proxy_domains);
    if (!route.has_value()) {
        // Unattributable: the envd token cannot have authorized anything, so
        // it must not be forwarded.
        AuthDecision decision = (HasProxyPrefix(request.path) || has_api_key)
            ? AuthDecision::Allow()
            : AuthDecision::Reject(AuthDecision::Action::Unauthorized);
        decision.strip_envd_access_token = true;
        return decision;
    }

    const core::SandboxId sandbox_id = route->sandbox_id;
    const AuthSandboxFacts facts     = checks.LookupSandbox(sandbox_id);

    if (facts.lookup_failed) {
        return AuthDecision::Reject(AuthDecision::Action::InternalServerError);
    }
    if (!facts.exists) {
        AuthDecision decision = AuthDecision::Reject(AuthDecision::Action::NotFound);
        decision.sandbox_id              = core::Optional<core::SandboxId>(sandbox_id);
        decision.strip_envd_access_token = true;
        return decision;
    }
    if (facts.template_builder) {
        AuthDecision decision = AuthDecision::Reject(AuthDecision::Action::NotFound);
        decision.sandbox_id = core::Optional<core::SandboxId>(sandbox_id);
        return decision;
    }

    const bool envd_request = route->target_port == facts.envd_port;

    // The envd token only counts when the sandbox is secure *and* the token
    // validates. A non-secure sandbox has no token to check.
    bool envd_authorized = false;
    if (envd_request && facts.secure) {
        core::Optional<std::string> candidate =
            SingleHeader(request.headers, kEnvdAccessTokenHeader);
        envd_authorized = candidate.has_value() &&
                          checks.ValidateEnvdAccessToken(sandbox_id, *candidate);
    }

    const bool authorized =
        envd_request ? (!facts.secure || envd_authorized)
                     : (facts.allow_public_traffic ||
                        checks.HasValidTrafficAccessToken(request.headers, sandbox_id));

    AuthDecision decision = authorized
        ? AuthDecision::Allow()
        : AuthDecision::Reject(AuthDecision::Action::Unauthorized);
    decision.sandbox_id = core::Optional<core::SandboxId>(sandbox_id);

    // An API key that authenticated a proxy request is a control-plane
    // credential; forwarding it into the sandbox would let a guest replay it.
    decision.strip_api_key = has_api_key;
    // Likewise the envd token, unless it is precisely what authorized this
    // request.
    decision.strip_envd_access_token = !envd_authorized;
    return decision;
}

}  // namespace api
}  // namespace agentenv
