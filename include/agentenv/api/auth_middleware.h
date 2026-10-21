// SPDX-License-Identifier: MIT
// Rust: src/api/impls/auth.rs — the production `AuthChecks` + the server layer.
//
// `auth.h` holds the decision, which is pure and therefore testable without a
// key store or an orchestrator. This header is the other half: the concrete
// checks backed by `ApiKey` + `Orchestrator`, and the adapter that turns a
// decision into a `Server` middleware.
//
// Until this was wired, `Decide` had no production caller at all: every route
// served unauthenticated, while the logic that was supposed to protect them
// sat in the binary being exercised only by tests.
#ifndef AGENTENV_API_AUTH_MIDDLEWARE_H_
#define AGENTENV_API_AUTH_MIDDLEWARE_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/api/auth.h"
#include "agentenv/api/server.h"
#include "agentenv/api_key.h"

namespace agentenv {
namespace orchestrator { class Orchestrator; }

namespace api {

/// Rust: the `ApiImpl` methods `require_auth` calls into.
///
/// Holds the orchestrator by raw pointer because `main` owns it and outlives
/// the server; the checks must not be what decides when it dies.
class OrchestratorAuthChecks : public AuthChecks {
 public:
    OrchestratorAuthChecks(ApiKey api_key, const orchestrator::Orchestrator* orchestrator,
                           uint16_t configured_control_plane_port);

    bool HasValidApiKey(const HeaderMap& headers) const override;
    bool HasValidTrafficAccessToken(const HeaderMap& headers,
                                    const core::SandboxId& id) const override;
    bool ValidateEnvdAccessToken(const core::SandboxId& id,
                                 const std::string& candidate) const override;
    AuthSandboxFacts LookupSandbox(const core::SandboxId& id) const override;

 private:
    ApiKey                          api_key_;
    const orchestrator::Orchestrator* orchestrator_;
    uint16_t                        control_plane_port_;
};

/// Builds the `require_auth` layer.
///
/// `checks` and `proxy_domains` are captured by the returned closure, so the
/// `shared_ptr` keeps them alive for the life of the server.
Middleware MakeAuthMiddleware(std::shared_ptr<AuthChecks> checks,
                              std::vector<std::string> proxy_domains);

/// Rust: `API_KEY_HEADER` / `ENVD_ACCESS_TOKEN_HEADER` stripping plus the
/// status mapping. Exposed for tests; `MakeAuthMiddleware` applies it.
void ApplyAuthDecision(const AuthDecision& decision, HttpRequest* request,
                       HttpResponse* response);

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_AUTH_MIDDLEWARE_H_
