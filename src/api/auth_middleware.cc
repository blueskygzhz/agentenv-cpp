// SPDX-License-Identifier: MIT
// Rust: src/api/impls/auth.rs — production checks + the server layer.
#include "agentenv/api/auth_middleware.h"

#include <sstream>
#include <unordered_map>

#include "agentenv/api/proxy_route.h"
#include "agentenv/orchestrator/service.h"

namespace agentenv {
namespace api {
namespace {

/// Removes every value of a header from the request's header map.
///
/// `HttpRequest::headers` is an `unordered_map` (one value per name) while
/// `auth.h` works on a `multimap`. That difference matters: Rust's
/// `single_header` treats a header sent twice as absent, and an
/// `unordered_map` cannot represent "sent twice" at all. See
/// `ToAuthHeaderMap` for how the request is presented to the decision.
void EraseRequestHeader(std::unordered_map<std::string, std::string>* headers,
                        const char* name) {
    if (headers == NULL) return;
    headers->erase(name);
}

/// Projects the request's headers into the multimap the decision expects.
///
/// This is lossy in the safe direction only: a duplicate header cannot survive
/// the parser's `unordered_map`, so `SingleHeader` here can never see the
/// two-values case that Rust rejects. If the framing layer ever preserves
/// duplicates, this is the single place that has to carry them through.
HeaderMap ToAuthHeaderMap(const std::unordered_map<std::string, std::string>& headers) {
    HeaderMap out;
    for (std::unordered_map<std::string, std::string>::const_iterator it = headers.begin();
         it != headers.end(); ++it) {
        out.insert(std::make_pair(it->first, it->second));
    }
    return out;
}

void WriteStatusOnly(int status, const char* message, HttpResponse* response) {
    response->status = status;
    std::ostringstream body;
    body << "{\"code\":" << status << ",\"message\":\"" << message << "\"}";
    response->body = body.str();
    response->headers["Content-Type"] = "application/json";
}

}  // namespace

OrchestratorAuthChecks::OrchestratorAuthChecks(
    ApiKey api_key, const orchestrator::Orchestrator* orchestrator,
    uint16_t configured_control_plane_port)
    : api_key_(std::move(api_key)),
      orchestrator_(orchestrator),
      control_plane_port_(configured_control_plane_port) {}

bool OrchestratorAuthChecks::HasValidApiKey(const HeaderMap& headers) const {
    // Rust `single_header`: a header sent more than once is treated as absent
    // rather than as its first value, so a duplicate cannot smuggle a second
    // credential past the check.
    const core::Optional<std::string> value = SingleHeader(headers, kApiKeyHeader);
    if (!value.has_value()) return false;
    return api_key_.Matches(*value);
}

bool OrchestratorAuthChecks::HasValidTrafficAccessToken(const HeaderMap& headers,
                                                        const core::SandboxId& id) const {
    const core::Optional<std::string> value =
        SingleHeader(headers, kTrafficAccessTokenHeader);
    if (!value.has_value() || orchestrator_ == NULL) return false;
    return orchestrator_->ValidateTrafficAccessToken(id, *value);
}

bool OrchestratorAuthChecks::ValidateEnvdAccessToken(const core::SandboxId& id,
                                                     const std::string& candidate) const {
    if (orchestrator_ == NULL) return false;
    return orchestrator_->ValidateEnvdAccessToken(id, candidate);
}

AuthSandboxFacts OrchestratorAuthChecks::LookupSandbox(const core::SandboxId& id) const {
    AuthSandboxFacts facts;
    if (orchestrator_ == NULL) {
        // Rust always has an orchestrator. Reporting "does not exist" would
        // turn a wiring fault into a 404 and hide it; a failed lookup is a 500.
        facts.lookup_failed = true;
        return facts;
    }

    const orchestrator::OrchestratorResult<core::Optional<orchestrator::SandboxMetadata> >
        found = orchestrator_->GetSandbox(id);
    if (!found.ok()) {
        facts.lookup_failed = true;
        return facts;
    }
    if (!found.value().has_value()) {
        return facts;  // exists == false
    }

    const orchestrator::SandboxMetadata& metadata = *found.value();
    facts.exists               = true;
    facts.template_builder     = metadata.template_builder;
    facts.secure               = metadata.secure;
    facts.allow_public_traffic = metadata.network_policy.allow_public_traffic;
    facts.envd_port            = EffectiveEnvdPort(metadata, control_plane_port_);
    return facts;
}

void ApplyAuthDecision(const AuthDecision& decision, HttpRequest* request,
                       HttpResponse* response) {
    // Strip first, unconditionally. Rust removes the headers on the rejection
    // paths too, so a short-circuit response is built from a request that no
    // longer carries a credential it did not use.
    if (decision.strip_api_key) {
        EraseRequestHeader(&request->headers, kApiKeyHeader);
    }
    if (decision.strip_envd_access_token) {
        EraseRequestHeader(&request->headers, kEnvdAccessTokenHeader);
    }

    switch (decision.action) {
        case AuthDecision::Action::Allow:
            return;
        case AuthDecision::Action::Unauthorized:
            WriteStatusOnly(401, "unauthorized", response);
            return;
        case AuthDecision::Action::NotFound:
            // Rust `proxy::sandbox_not_found_response` / `StatusCode::NOT_FOUND`
            // — a template-builder sandbox is hidden, not refused, so the
            // response must not differ from a genuinely absent one.
            WriteStatusOnly(404, "sandbox not found", response);
            return;
        case AuthDecision::Action::InternalServerError:
            WriteStatusOnly(500, "internal server error", response);
            return;
    }
    WriteStatusOnly(500, "internal server error", response);
}

Middleware MakeAuthMiddleware(std::shared_ptr<AuthChecks> checks,
                              std::vector<std::string> proxy_domains) {
    // A null `checks` must not degrade to "allow everything". Refusing every
    // request is the safe direction for a misconfigured server.
    return [checks, proxy_domains](HttpRequest* request, HttpResponse* response) -> bool {
        if (!checks) {
            WriteStatusOnly(500, "authorization unavailable", response);
            return false;
        }

        AuthRequest auth_request;
        auth_request.path          = request->path;
        auth_request.headers       = ToAuthHeaderMap(request->headers);
        auth_request.proxy_domains = proxy_domains;
        // This port has no sandbox proxy data plane, so a request can never
        // have been matched as one. Rust derives this from `MatchedPath`.
        auth_request.matched_api_route = true;

        const AuthDecision decision = Decide(auth_request, *checks);
        ApplyAuthDecision(decision, request, response);
        return decision.action == AuthDecision::Action::Allow;
    };
}

}  // namespace api
}  // namespace agentenv
