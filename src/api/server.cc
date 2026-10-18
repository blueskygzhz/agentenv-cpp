// SPDX-License-Identifier: MIT
// Rust: src/api/mod.rs — the router. `MakeServer` picks the transport;
// `RegisterOrchestratorRoutes` is the route table itself.
#include "agentenv/api/server.h"

#include <vector>

#include "agentenv/api/http_server.h"
#include "agentenv/api/impls.h"

namespace agentenv {
namespace api {

namespace {

/// Writes an `ApiError` out as the `{code, message}` body with a matching
/// HTTP status. The two must agree: a 200 carrying an error body is read as
/// success by every client that checks the status first.
void WriteError(const ApiError& error, HttpResponse* out) {
    out->status = error.code;
    out->body = error.ToJson().ToString();
}

void WriteJson(const core::Json& value, HttpResponse* out) {
    out->status = 200;
    out->body = value.ToString();
}

/// Looks a path parameter up. Captured segments are merged into `query` by
/// the router before the handler runs.
std::string Param(const HttpRequest& request, const char* name) {
    const std::unordered_map<std::string, std::string>::const_iterator it =
        request.query.find(name);
    return it == request.query.end() ? std::string() : it->second;
}

/// Parses a JSON request body, reporting a 400 rather than letting a
/// malformed body reach a handler as a default-constructed request — which
/// would create a sandbox from a request the client never sent.
bool ParseBody(const HttpRequest& request, core::Json* out, HttpResponse* resp) {
    const core::Expected<core::Json, core::AnyError> parsed =
        core::Json::Parse(request.body.empty() ? std::string("{}") : request.body);
    if (!parsed.ok()) {
        WriteError(ApiError::Make(400, "malformed JSON body: " + parsed.error().chain()),
                   resp);
        return false;
    }
    *out = parsed.value();
    return true;
}

}  // namespace

std::unique_ptr<Server> MakeServer() {
    // One real transport. The in-memory dispatcher this used to return made
    // `ListenAndServe` fail at runtime while every build and test passed,
    // so the binary looked complete and served nothing.
    return std::unique_ptr<Server>(new HttpServer());
}

void RegisterOrchestratorRoutes(Server* server, orchestrator::Orchestrator* svc) {
    if (server == NULL || svc == NULL) return;

    // The handlers share one ApiImpl. It holds a shared_ptr, so the lambdas
    // below keep it alive for as long as the routes are registered.
    //
    // `svc` is a raw pointer owned by the caller (main holds the shared_ptr),
    // so the aliasing constructor is used: the ApiImpl must not be the thing
    // that decides when the orchestrator dies.
    std::shared_ptr<orchestrator::Orchestrator> alias(
        std::shared_ptr<orchestrator::Orchestrator>(), svc);
    std::shared_ptr<ApiImpl> impl(new ApiImpl(alias));

    // ---- sandboxes --------------------------------------------------------

    server->Route("GET", "/sandboxes", [impl](const HttpRequest&, HttpResponse* resp) {
        const core::Expected<ListSandboxesResp, ApiError> r = impl->ListSandboxes();
        if (!r.ok()) return WriteError(r.error(), resp);
        WriteJson(r.value().ToJson(), resp);
    });

    server->Route("POST", "/sandboxes", [impl](const HttpRequest& req, HttpResponse* resp) {
        core::Json body;
        if (!ParseBody(req, &body, resp)) return;
        const core::Expected<CreateSandboxReq, core::AnyError> parsed =
            CreateSandboxReq::FromJson(body);
        if (!parsed.ok()) {
            return WriteError(ApiError::Make(400, parsed.error().chain()), resp);
        }
        const core::Expected<CreateSandboxResp, ApiError> r =
            impl->CreateSandbox(parsed.value());
        if (!r.ok()) return WriteError(r.error(), resp);
        resp->status = 201;
        resp->body = r.value().ToJson().ToString();
    });

    server->Route("GET", "/sandboxes/{id}",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<GetSandboxResp, ApiError> r =
            impl->GetSandbox(Param(req, "id"));
        if (!r.ok()) return WriteError(r.error(), resp);
        WriteJson(r.value().ToJson(), resp);
    });

    server->Route("DELETE", "/sandboxes/{id}",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<core::Unit, ApiError> r =
            impl->DeleteSandbox(Param(req, "id"));
        if (!r.ok()) return WriteError(r.error(), resp);
        // 204: the delete succeeded and there is nothing to return. An empty
        // 200 body would be indistinguishable from a handler that forgot to
        // write one.
        resp->status = 204;
    });

    server->Route("POST", "/sandboxes/{id}/pause",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<core::Unit, ApiError> r =
            impl->PauseSandbox(Param(req, "id"));
        if (!r.ok()) return WriteError(r.error(), resp);
        resp->status = 204;
    });

    server->Route("POST", "/sandboxes/{id}/resume",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<GetSandboxResp, ApiError> r =
            impl->ResumeSandbox(Param(req, "id"));
        if (!r.ok()) return WriteError(r.error(), resp);
        WriteJson(r.value().ToJson(), resp);
    });

    server->Route("POST", "/sandboxes/{id}/exec",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        core::Json body;
        if (!ParseBody(req, &body, resp)) return;
        const core::Expected<ExecReq, core::AnyError> parsed = ExecReq::FromJson(body);
        if (!parsed.ok()) {
            return WriteError(ApiError::Make(400, parsed.error().chain()), resp);
        }
        const core::Expected<ExecResp, ApiError> r =
            impl->Exec(Param(req, "id"), parsed.value());
        if (!r.ok()) return WriteError(r.error(), resp);
        WriteJson(r.value().ToJson(), resp);
    });

    server->Route("POST", "/sandboxes/{id}/fork",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        // The body is optional: Rust's parameter is
        // `Option<SandboxForkRequest>`, and an absent one forks once with the
        // source's timeout. An empty body is therefore not an error.
        ForkSandboxReq parsed;
        if (!req.body.empty()) {
            core::Json body;
            if (!ParseBody(req, &body, resp)) return;
            const core::Expected<ForkSandboxReq, core::AnyError> decoded =
                ForkSandboxReq::FromJson(body);
            if (!decoded.ok()) {
                return WriteError(ApiError::Make(400, decoded.error().chain()), resp);
            }
            parsed = decoded.value();
        }
        const core::Expected<ForkSandboxResp, ApiError> r =
            impl->ForkSandbox(Param(req, "id"), parsed);
        if (!r.ok()) return WriteError(r.error(), resp);
        // 201, and the per-child results are in the body. A child that failed
        // is reported there rather than by the status, which describes the
        // fork as a whole.
        WriteJson(r.value().ToJson(), resp);
        resp->status = 201;
    });

    server->Route("POST", "/sandboxes/{id}/snapshot",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<core::Unit, ApiError> r =
            impl->CreateSnapshot(Param(req, "id"));
        if (!r.ok()) return WriteError(r.error(), resp);
        resp->status = 204;
    });

    // ---- templates --------------------------------------------------------

    server->Route("GET", "/templates", [impl](const HttpRequest&, HttpResponse* resp) {
        const core::Expected<core::Unit, ApiError> r = impl->ListTemplates();
        if (!r.ok()) return WriteError(r.error(), resp);
        resp->status = 200;
        resp->body = "{\"templates\":[]}";
    });

    // ---- health -----------------------------------------------------------

    // Deliberately not routed through ApiImpl: this must answer while the
    // orchestrator is busy or degraded, which is exactly when a load balancer
    // asks. It reports that the process is serving, nothing more.
    server->Route("GET", "/health", [](const HttpRequest&, HttpResponse* resp) {
        resp->status = 200;
        resp->body = "{\"status\":\"ok\"}";
    });
}

}  // namespace api
}  // namespace agentenv
