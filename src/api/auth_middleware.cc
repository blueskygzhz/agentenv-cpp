// SPDX-License-Identifier: MIT
// Rust: src/api/impls/auth.rs — production checks + the server layer.
#include "agentenv/api/auth_middleware.h"

#include <map>
#include <sstream>
#include <unordered_map>

#include "agentenv/api/proxy_route.h"
#include "agentenv/orchestrator/service.h"

namespace agentenv {
namespace api {
namespace {

/// Removes every value of a header from the request.
void EraseRequestHeader(HttpRequest* request, const char* name) {
    if (request == NULL) return;
    request->headers.erase(name);
    request->repeated_headers.erase(name);
}

/// Projects the request's headers into the multimap the decision expects.
///
/// A header the client sent more than once is projected twice, so
/// `SingleHeader` sees the duplicate and treats it as absent exactly as Rust's
/// `single_header` does. Only the count matters there; the routing helpers
/// read the first value, which is the one `headers` holds.
HeaderMap ToAuthHeaderMap(const HttpRequest& request) {
    HeaderMap out;
    for (std::unordered_map<std::string, std::string>::const_iterator it =
             request.headers.begin();
         it != request.headers.end(); ++it) {
        out.insert(std::make_pair(it->first, it->second));
        if (request.repeated_headers.count(it->first) != 0) {
            out.insert(std::make_pair(it->first, it->second));
        }
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
        EraseRequestHeader(request, kApiKeyHeader);
    }
    if (decision.strip_envd_access_token) {
        EraseRequestHeader(request, kEnvdAccessTokenHeader);
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
        // Rust inspects `uri().path()`, the path as sent: `/proxy%2F..` is not
        // under the proxy prefix there, and must not be here.
        auth_request.path          = request->raw_path.empty() ? request->path
                                                               : request->raw_path;
        auth_request.headers       = ToAuthHeaderMap(*request);
        auth_request.proxy_domains = proxy_domains;
        // Rust derives these from `MatchedPath` / `RawPathParams`, which the
        // server records before any layer runs.
        auth_request.matched_api_route = request->matched_route;
        std::map<std::string, std::string>::const_iterator param =
            request->path_params.find("sandbox_id");
        if (param != request->path_params.end()) {
            auth_request.path_sandbox_id = core::Optional<std::string>(param->second);
        }

        const AuthDecision decision = Decide(auth_request, *checks);
        ApplyAuthDecision(decision, request, response);
        return decision.action == AuthDecision::Action::Allow;
    };
}

}  // namespace api
}  // namespace agentenv
