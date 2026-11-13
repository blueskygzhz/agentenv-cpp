// SPDX-License-Identifier: MIT
// Rust: src/api/mod.rs — the router. `MakeServer` picks the transport;
// `RegisterOrchestratorRoutes` is the route table itself.
#include "agentenv/api/server.h"

#include <cstdlib>
#include <vector>

#include "agentenv/api/admin.h"
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

/// A query parameter that was actually sent, distinguished from one that was
/// not.
///
/// `Param` cannot express the difference — it returns "" for both — and here
/// the difference decides the answer: upstream's `cluster_id` is an
/// `Option<Uuid>`, and an *absent* filter matches this node while a present
/// one that disagrees with the node's cluster filters it out. Collapsing the
/// two would make `?clusterID=` (empty) hide a node that upstream reports.
core::Optional<std::string> OptionalParam(const HttpRequest& request, const char* name) {
    const std::unordered_map<std::string, std::string>::const_iterator it =
        request.query.find(name);
    if (it == request.query.end()) return core::Optional<std::string>();
    return core::Optional<std::string>(it->second);
}

/// Writes an `AdminError` out the same way `WriteError` writes an `ApiError`:
/// the admin endpoints are declared with the same `{code, message}` error
/// responses as the rest of the API.
void WriteAdminError(const AdminError& error, HttpResponse* out) {
    WriteError(ApiError::Make(error.status, error.message), out);
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
    std::shared_ptr<snapshot::SnapshotRuntimeResolver> runtime_resolver,
    const AdminObservability* observability,
    std::shared_ptr<p2p::P2pTransport> p2p_transport) {
    if (server == NULL || svc == NULL) return;

    // The handlers share one ApiImpl. It holds a shared_ptr, so the lambdas
    // below keep it alive for as long as the routes are registered.
    //
    // `svc` is a raw pointer owned by the caller (main holds the shared_ptr),
    // so the aliasing constructor is used: the ApiImpl must not be the thing
    // that decides when the orchestrator dies.
    std::shared_ptr<orchestrator::Orchestrator> alias(
        std::shared_ptr<orchestrator::Orchestrator>(), svc);
    std::shared_ptr<ApiImpl> impl(new ApiImpl(alias, volumes, image_resolver, repository,
                                              runtime_resolver, p2p_transport));

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

    // Rust `POST /sandboxes-cold`. Registered as its own route because that is
    // what upstream's schema declares: `NewSandbox` carries only `templateID`
    // and its description points callers here to "create a sandbox directly
    // from an external OCI image". This port had the whole cold-start
    // apparatus -- `ColdStartResources`, the metrics stage label, the
    // gateway's `new_cold_sandbox` scheduling hint -- and no route calling any
    // of it, so there was no way to go from an OCI reference to a running
    // Firecracker VM over HTTP at all.
    server->Route("POST", "/sandboxes-cold",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        core::Json body;
        if (!ParseBody(req, &body, resp)) return;
        const core::Expected<CreateColdSandboxReq, core::AnyError> parsed =
            CreateColdSandboxReq::FromJson(body);
        if (!parsed.ok()) {
            return WriteError(ApiError::Make(400, parsed.error().chain()), resp);
        }
        const core::Expected<CreateSandboxResp, ApiError> r =
            impl->CreateColdSandbox(parsed.value());
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

    // No `POST /sandboxes/{id}/exec` here, and that is the alignment rather
    // than a gap: upstream has no such route. Its `openapi.yml` never mentions
    // `exec` and the generated router does not list it -- running a command in
    // a sandbox is a *data plane* operation that reaches envd through the
    // proxy, not a control plane endpoint.
    //
    // This port invented one, answering an unconditional 501. It was removed
    // with its handler and DTOs: a route upstream does not serve should 404,
    // and a 501 says something different and false -- "this endpoint exists,
    // your build lacks the feature" -- to a client calling a URL that is not
    // part of the API. Keeping it also made `ApiImpl::Exec` and
    // `ExecReq`/`ExecResp` look like a ported surface with a missing backend,
    // when the whole shape was ours.

    // Rust `GET` / `PATCH /sandboxes/{sandboxID}/custom-extension-params`
    // (`generated/src/server/mod.rs:69-70`).
    //
    // The orchestrator half (`PatchSandboxCustomExtensionParams`, the
    // `hook_patch_params` call, the state guard and the metadata write) was
    // ported and reachable only from its own test. These two routes are what
    // a client uses to reconfigure a running sandbox's extension, and without
    // them the whole hook path was unreachable in production.
    server->Route("GET", "/sandboxes/{sandbox_id}/custom-extension-params",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<core::Json, ApiError> r =
            impl->GetSandboxCustomExtensionParams(Param(req, "sandbox_id"));
        if (!r.ok()) return WriteError(r.error(), resp);
        WriteJson(r.value(), resp);
    });

    server->Route("PATCH", "/sandboxes/{sandbox_id}/custom-extension-params",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        // The body *is* the patch map, so an absent one is an empty patch
        // rather than an error -- `ParseBody` already turns "" into `{}`.
        core::Json body;
        if (!ParseBody(req, &body, resp)) return;
        const core::Expected<core::Json, ApiError> r =
            impl->PatchSandboxCustomExtensionParams(Param(req, "sandbox_id"), body);
        if (!r.ok()) return WriteError(r.error(), resp);
        // The full params after the patch, which is not what was sent: the
        // extension decides the merge.
        WriteJson(r.value(), resp);
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
        const core::Expected<std::vector<TemplateModel>, ApiError> r = impl->ListTemplates();
        if (!r.ok()) return WriteError(r.error(), resp);
        // A bare JSON array, which is what `TemplatesGetResponse`'s 200
        // variant carries (`Vec<models::Template>`). The hard-coded
        // `{"templates":[]}` this used to write was a second defect hidden
        // behind the first: while the handler was a 501 the body never
        // reached a client, so nothing showed that the shape was wrong as
        // well as the content.
        core::JsonArray items;
        for (std::size_t i = 0; i < r.value().size(); ++i) {
            items.emplace_back(r.value()[i].ToJson());
        }
        WriteJson(core::Json(items), resp);
    });

    // Rust `POST /v3/templates`, 202. The first half of upstream's two-step
    // build: this reserves the build by committing a `TemplateWaiting` record
    // and claiming the name, and `POST /v2/templates/{id}/builds/{id}` runs
    // it.
    //
    // Versioned path, as upstream declares it. The V3 request body and the V2
    // start body are different schemas on purpose -- V3 names the template
    // and sizes the machine, V2 carries the build steps -- so folding them
    // onto one unversioned `/templates` route would have made the second
    // step unreachable by any client written against the real API.
    server->Route("POST", "/v3/templates", [impl](const HttpRequest& req, HttpResponse* resp) {
        // Upstream's `requestBody` is `required: true`. Not delegated to
        // `ParseBody`'s empty-body-as-`{}` leniency: every field of
        // `TemplateBuildRequestV3` is optional in the schema, so an omitted
        // body would reach the handler as a request with no name and be
        // reported as a missing field rather than a missing body.
        if (req.body.empty()) {
            return WriteError(ApiError::Make(400, "a request body is required"), resp);
        }
        core::Json body;
        if (!ParseBody(req, &body, resp)) return;
        const core::Expected<TemplateBuildRequestV3, core::AnyError> parsed =
            TemplateBuildRequestV3::FromJson(body);
        if (!parsed.ok()) {
            return WriteError(ApiError::Make(400, parsed.error().chain()), resp);
        }
        const core::Expected<TemplateRequestResponseV3, ApiError> r =
            impl->CreateTemplateBuild(parsed.value());
        if (!r.ok()) return WriteError(r.error(), resp);
        // 202, not 201: nothing has been built yet. The record is `waiting`
        // and the caller must still start the build with the ids in this body.
        resp->status = 202;
        resp->body = r.value().ToJson().ToString();
    });

    // Rust `POST /v2/templates/{templateID}/builds/{buildID}` — the second
    // step, which claims the reservation above and runs the build.
    //
    // Registered even though the handler cannot finish: the route is where
    // every synchronous rule lives (the two ids must agree *and* must name
    // the same record, the base source is exclusive, the spec must
    // translate), and leaving it unrouted meant none of that could be
    // reached by a client. It answers 400/404 for a bad request and 501 only
    // when the request was entirely in order.
    server->Route("POST", "/v2/templates/{template_id}/builds/{build_id}",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        // The body is required by the schema, but every field in it is
        // optional: a build with no steps and no base is a valid request for
        // the configured default image. So an empty body is parsed as `{}`
        // rather than refused -- unlike `POST /v3/templates`, where an
        // omitted body would read as a missing name.
        core::Json body;
        if (!ParseBody(req, &body, resp)) return;
        const core::Expected<TemplateBuildStartRequest, core::AnyError> parsed =
            TemplateBuildStartRequest::FromJson(body);
        if (!parsed.ok()) {
            return WriteError(ApiError::Make(400, parsed.error().chain()), resp);
        }
        const core::Expected<core::Unit, ApiError> r = impl->StartTemplateBuild(
            Param(req, "template_id"), Param(req, "build_id"), parsed.value());
        if (!r.ok()) return WriteError(r.error(), resp);
        // Rust's `Status202_TheBuildHasStarted` carries no body: the build is
        // asynchronous, and its outcome is read back from the status route
        // below.
        resp->status = 202;
    });

    // Rust `GET /templates/{templateID}/builds/{buildID}/status` — what a
    // client polls after starting a build, and the only route that reports
    // *why* one failed.
    //
    // `GET /templates` was the only way to observe a reserved template before
    // this: it lists every template on the node and carries neither the
    // failure reason nor any log line, so a client could see that a build had
    // failed and had nowhere to learn what failed.
    server->Route("GET", "/templates/{template_id}/builds/{build_id}/status",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<BuildLogQuery, core::AnyError> query = BuildLogQuery::ForStatus(
            Param(req, "logsOffset"), Param(req, "limit"), Param(req, "level"));
        if (!query.ok()) {
            return WriteError(ApiError::Make(400, query.error().chain()), resp);
        }
        const core::Expected<TemplateBuildInfoModel, ApiError> r = impl->GetTemplateBuildStatus(
            Param(req, "template_id"), Param(req, "build_id"), query.value());
        if (!r.ok()) return WriteError(r.error(), resp);
        WriteJson(r.value().ToJson(), resp);
    });

    // Rust `GET /templates/{templateID}/builds/{buildID}/logs`.
    server->Route("GET", "/templates/{template_id}/builds/{build_id}/logs",
                  [impl](const HttpRequest& req, HttpResponse* resp) {
        const core::Expected<BuildLogQuery, core::AnyError> query = BuildLogQuery::ForLogs(
            Param(req, "cursor"), Param(req, "limit"), Param(req, "level"),
            Param(req, "direction"), Param(req, "source"));
        if (!query.ok()) {
            return WriteError(ApiError::Make(400, query.error().chain()), resp);
        }
        const core::Expected<std::vector<tpl::BuildLogEntry>, ApiError> r =
            impl->GetTemplateBuildLogs(Param(req, "template_id"), Param(req, "build_id"),
                                       query.value());
        if (!r.ok()) return WriteError(r.error(), resp);
        // `TemplateBuildLogsResponse` wraps the array in a `logs` key -- which
        // is *not* the shape `GET /templates` uses, where the 200 variant is a
        // bare array. The two differ upstream and a client generated from the
        // schema reads them differently, so the wrapper is not optional.
        core::JsonObject body;
        core::JsonArray  entries;
        for (std::size_t i = 0; i < r.value().size(); ++i) {
            entries.emplace_back(r.value()[i].ToJson());
        }
        body["logs"] = core::Json(entries);
        WriteJson(core::Json(body), resp);
    });

    // ---- admin ------------------------------------------------------------
    //
    // Rust `GET /nodes` / `GET /nodes/{node_id}`
    // (`generated/src/server/mod.rs:47-48`, handlers in `impls/admin.rs`).
    //
    // `NodesGet` and `NodesNodeIdGet` were ported in full and then never
    // reached from anywhere but their own test -- the same "complete,
    // configured, zero production callers" shape the README names as a defect
    // class. The projection was right; only these two lines were missing.
    //
    // `observability` is null in this build, and that is a faithful answer
    // rather than a stub: upstream's handlers begin with
    // `let Some(observability) = self.observability() else { .. }` and report
    // an empty collection / a 404 for exactly this case. A node that cannot
    // describe itself says so in upstream's own words.
    //
    // The path placeholder is `{node_id}`, matching the generated router's
    // `/nodes/{node_id}`; `nodeID` is the spec's *parameter* name, not the
    // template's.
    server->Route("GET", "/nodes", [observability](const HttpRequest& req,
                                                   HttpResponse* resp) {
        const core::Expected<std::vector<core::Json>, AdminError> r =
            NodesGet(observability, OptionalParam(req, "clusterID"));
        if (!r.ok()) return WriteAdminError(r.error(), resp);
        // A bare array, as the schema has it -- not wrapped in a `nodes` key.
        core::JsonArray nodes;
        for (std::size_t i = 0; i < r.value().size(); ++i) {
            nodes.emplace_back(r.value()[i]);
        }
        WriteJson(core::Json(nodes), resp);
    });

    server->Route("GET", "/nodes/{node_id}", [observability](const HttpRequest& req,
                                                             HttpResponse* resp) {
        const core::Expected<core::Json, AdminError> r = NodesNodeIdGet(
            observability, Param(req, "node_id"), OptionalParam(req, "clusterID"));
        if (!r.ok()) return WriteAdminError(r.error(), resp);
        WriteJson(r.value(), resp);
    });

    // ---- health -----------------------------------------------------------

    // Deliberately not routed through ApiImpl: this must answer while the
    // orchestrator is busy or degraded, which is exactly when a load balancer
    // asks. It reports that the process is serving, nothing more.
    server->Route("GET", "/health", [](const HttpRequest&, HttpResponse* resp) {
        resp->status = 200;
        resp->body = "{\"status\":\"ok\"}";
    });

    // ---- metrics ----------------------------------------------------------
    //
    // Rust `.route("/metrics", get(metrics_handler))` (`api/server.rs:41`),
    // whose handler renders the global prometheus recorder and, when none was
    // installed, answers `503 "prometheus recorder is not initialized"`
    // (`crates/observability/src/lib.rs:70-79`).
    //
    // That second arm is the one this build is in, and it is why the route is
    // registered rather than left out. `observability/metrics.h` is a facade
    // that writes samples to stderr: there is no registry to render and no
    // text-exposition encoder, so a 200 here is not reachable honestly.
    //
    // Registered anyway, because the alternative was worse than a 503. The
    // auth layer already exempts `/metrics` from authentication
    // (`auth.cc:21`), so the two layers disagreed: one declared the endpoint
    // public, the other answered 404. And to a scraper the two statuses mean
    // different things -- a 404 reads as "wrong path, fix your config", a 503
    // as "endpoint is there, backend not ready", which is the true statement.
    //
    // Plain text, not JSON, and upstream's wording verbatim: this response is
    // consumed by Prometheus, which never sees the control plane's `{code,
    // message}` envelope on this route.
    server->Route("GET", "/metrics", [](const HttpRequest&, HttpResponse* resp) {
        resp->status = 503;
        resp->headers["content-type"] = "text/plain; charset=utf-8";
        resp->body = "prometheus recorder is not initialized";
    });
}

}  // namespace api
}  // namespace agentenv
