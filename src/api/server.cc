// SPDX-License-Identifier: MIT
// Rust: src/api/mod.rs — the router. `MakeServer` picks the transport;
// `RegisterOrchestratorRoutes` is the route table itself.
#include "agentenv/api/server.h"

#include <cstdlib>
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
    RegisterOrchestratorRoutes(server, svc, std::shared_ptr<volume::VolumeManager>(),
                               std::shared_ptr<image::ImageResolver>());
}

void RegisterOrchestratorRoutes(
    Server* server, orchestrator::Orchestrator* svc,
    std::shared_ptr<volume::VolumeManager> volumes,
    std::shared_ptr<image::ImageResolver> image_resolver,
    std::shared_ptr<snapshot::repository::SnapshotRepository> repository,
    std::shared_ptr<snapshot::SnapshotRuntimeResolver> runtime_resolver) {
    if (server == NULL || svc == NULL) return;

    // The handlers share one ApiImpl. It holds a shared_ptr, so the lambdas
    // below keep it alive for as long as the routes are registered.
    //
    // `svc` is a raw pointer owned by the caller (main holds the shared_ptr),
    // so the aliasing constructor is used: the ApiImpl must not be the thing
    // that decides when the orchestrator dies.
    std::shared_ptr<orchestrator::Orchestrator> alias(
        std::shared_ptr<orchestrator::Orchestrator>(), svc);
    std::shared_ptr<ApiImpl> impl(
        new ApiImpl(alias, volumes, image_resolver, repository, runtime_resolver));

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

    server->Route("GET", "/sandboxes/{sandbox_id}",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<GetSandboxResp, ApiError> r =
            impl->GetSandbox(Param(req, "sandbox_id"));
        if (!r.ok()) return WriteError(r.error(), resp);
        WriteJson(r.value().ToJson(), resp);
    });

    server->Route("DELETE", "/sandboxes/{sandbox_id}",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<core::Unit, ApiError> r =
            impl->DeleteSandbox(Param(req, "sandbox_id"));
        if (!r.ok()) return WriteError(r.error(), resp);
        // 204: the delete succeeded and there is nothing to return. An empty
        // 200 body would be indistinguishable from a handler that forgot to
        // write one.
        resp->status = 204;
    });

    server->Route("POST", "/sandboxes/{sandbox_id}/pause",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<core::Unit, ApiError> r =
            impl->PauseSandbox(Param(req, "sandbox_id"));
        if (!r.ok()) return WriteError(r.error(), resp);
        resp->status = 204;
    });

    server->Route("POST", "/sandboxes/{sandbox_id}/resume",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<GetSandboxResp, ApiError> r =
            impl->ResumeSandbox(Param(req, "sandbox_id"));
        if (!r.ok()) return WriteError(r.error(), resp);
        WriteJson(r.value().ToJson(), resp);
    });

    server->Route("POST", "/sandboxes/{sandbox_id}/exec",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        core::Json body;
        if (!ParseBody(req, &body, resp)) return;
        const core::Expected<ExecReq, core::AnyError> parsed = ExecReq::FromJson(body);
        if (!parsed.ok()) {
            return WriteError(ApiError::Make(400, parsed.error().chain()), resp);
        }
        const core::Expected<ExecResp, ApiError> r =
            impl->Exec(Param(req, "sandbox_id"), parsed.value());
        if (!r.ok()) return WriteError(r.error(), resp);
        WriteJson(r.value().ToJson(), resp);
    });

    server->Route("POST", "/sandboxes/{sandbox_id}/fork",
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
            impl->ForkSandbox(Param(req, "sandbox_id"), parsed);
        if (!r.ok()) return WriteError(r.error(), resp);
        // 201, and the per-child results are in the body. A child that failed
        // is reported there rather than by the status, which describes the
        // fork as a whole.
        WriteJson(r.value().ToJson(), resp);
        resp->status = 201;
    });

    // `snapshots`, plural, and 201 with the published snapshot in the body.
    //
    // This used to be `POST /sandboxes/{id}/snapshot` answering 204, which
    // matched neither upstream's `/sandboxes/{sandboxID}/snapshots` nor this
    // port's own metrics route label (`observability/prometheus.cc`, which has
    // always folded the plural path). While the handler was a 501 the
    // difference did not show; now that it publishes, the old spelling would
    // have left clients unable to find the route and the per-route metrics
    // labelled for a path nothing served.
    server->Route("POST", "/sandboxes/{sandbox_id}/snapshots",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        // Upstream's `requestBody` is `required: true`, so an absent body is
        // a 400. Checked here rather than through `ParseBody`, which treats an
        // empty body as `{}`: that leniency is harmless on routes whose DTO
        // has required fields (`POST /sandboxes` fails on the missing
        // `templateID` either way), but `CreateSnapshotReq`'s only field is
        // optional, so an omitted body would quietly publish unaliased the
        // snapshot the caller meant to name.
        if (req.body.empty()) {
            return WriteError(ApiError::Make(400, "a request body is required"), resp);
        }
        core::Json body;
        if (!ParseBody(req, &body, resp)) return;
        const core::Expected<CreateSnapshotReq, core::AnyError> parsed =
            CreateSnapshotReq::FromJson(body);
        if (!parsed.ok()) {
            return WriteError(ApiError::Make(400, parsed.error().chain()), resp);
        }
        const core::Expected<SnapshotInfoResp, ApiError> r =
            impl->CreateSnapshot(Param(req, "sandbox_id"), parsed.value());
        if (!r.ok()) return WriteError(r.error(), resp);
        // 201: the snapshot is a new resource, and the body names it. A 204
        // would leave the caller with no id to launch from.
        WriteJson(r.value().ToJson(), resp);
        resp->status = 201;
    });

    // ---- volumes ----------------------------------------------------------

    server->Route("GET", "/volumes", [impl](const HttpRequest& req, HttpResponse* resp) {
        // Rust's query params are `nextToken` and `limit`. An absent parameter
        // and an explicitly empty one are treated alike here; upstream's
        // generated layer would pass `Some("")` through to be rejected as an
        // invalid cursor, and refusing a paging request because the client sent
        // `?nextToken=` is not worth preserving.
        core::Optional<std::string> next_token;
        const std::string raw_token = Param(req, "nextToken");
        if (!raw_token.empty()) next_token = core::Optional<std::string>(raw_token);

        core::Optional<std::size_t> limit;
        const std::string raw_limit = Param(req, "limit");
        if (!raw_limit.empty()) {
            // Upstream's `limit` is a `u32` validated to 1..=100 by the
            // generated layer, so a non-numeric value never reaches the
            // handler. Rejecting it here keeps that: parsing it loosely would
            // turn `?limit=abc` into the default page size, silently serving a
            // different request than the one asked for. The range itself stays
            // with the manager, which answers `InvalidPageLimit`.
            char* end = NULL;
            const unsigned long parsed = std::strtoul(raw_limit.c_str(), &end, 10);
            if (end == NULL || *end != '\0') {
                return WriteError(ApiError::Make(400, "invalid `limit`: expected an integer"),
                                  resp);
            }
            limit = core::Optional<std::size_t>(static_cast<std::size_t>(parsed));
        }

        const core::Expected<VolumePageResp, ApiError> r = impl->ListVolumes(next_token, limit);
        if (!r.ok()) return WriteError(r.error(), resp);
        resp->status = 200;
        resp->body = r.value().records.ToString();
        // Rust returns the cursor as a header, not in the body, and omits it
        // entirely on the last page.
        if (r.value().next_token.has_value()) {
            resp->headers["x-next-token"] = *r.value().next_token;
        }
    });

    server->Route("POST", "/volumes", [impl](const HttpRequest& req, HttpResponse* resp) {
        core::Json body;
        if (!ParseBody(req, &body, resp)) return;
        const core::Expected<NewVolumeReq, core::AnyError> parsed = NewVolumeReq::FromJson(body);
        if (!parsed.ok()) {
            return WriteError(ApiError::Make(400, parsed.error().chain()), resp);
        }
        const core::Expected<core::Json, ApiError> r = impl->CreateVolume(parsed.value());
        if (!r.ok()) return WriteError(r.error(), resp);
        resp->status = 201;
        resp->body = r.value().ToString();
    });

    server->Route("GET", "/volumes/{volume_id}",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<core::Json, ApiError> r = impl->GetVolume(Param(req, "volume_id"));
        if (!r.ok()) return WriteError(r.error(), resp);
        WriteJson(r.value(), resp);
    });

    server->Route("DELETE", "/volumes/{volume_id}",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<core::Unit, ApiError> r = impl->DeleteVolume(Param(req, "volume_id"));
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
