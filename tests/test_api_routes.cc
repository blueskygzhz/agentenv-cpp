// SPDX-License-Identifier: MIT
// Rust: src/api/mod.rs / src/api/server.rs — the route table itself
//       (`generated/src/server/mod.rs:46-158` plus the hand-merged routes).
//
// There was no test for the route *table* until this one, and that is exactly
// how `GET /nodes` and `GET /nodes/{nodeID}` stayed unreachable: both handlers
// were ported in full, both were covered by `test_api_admin.cc`, and neither
// was ever registered. Every existing API test calls `ApiImpl` directly, so a
// handler that no URL reaches looks identical to one that is wired.
//
// So these assert the registration, not the handlers: which (method, path)
// pairs exist, and -- for the routes whose whole content is their status --
// what they answer. A handler reached through no route is the one defect this
// file is here to make visible.
#include "agentenv/api/server.h"

#include <algorithm>
#include <string>
#include <vector>

#include "agentenv/api/admin.h"
#include "agentenv/api/auth.h"
#include "microtest.h"
#include "orchestrator_fakes.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT
using agentenv::testing::Fixture;

namespace {

/// A `Server` that records the table instead of binding a socket.
///
/// `Server` has no way to enumerate what was registered -- nothing in
/// production needs that -- so observing the table at all requires standing in
/// for the server. The handlers are kept, not just the keys, so a route's
/// answer can be read back without a transport.
class RecordingServer : public Server {
 public:
    struct Entry {
        std::string method;
        std::string path;
        Handler     handler;
    };
    std::vector<Entry>      routes;
    std::vector<Middleware> layers;

    void Route(const std::string& method, const std::string& path_pattern,
               Handler h) override {
        Entry entry;
        entry.method  = method;
        entry.path    = path_pattern;
        entry.handler = h;
        routes.push_back(entry);
    }
    void Use(Middleware layer) override { layers.push_back(layer); }
    core::Expected<core::Unit, core::AnyError> ListenAndServe(
        const std::string&) override {
        return core::Unit();
    }
    void Stop() override {}

    bool Has(const std::string& method, const std::string& path) const {
        for (std::size_t i = 0; i < routes.size(); ++i) {
            if (routes[i].method == method && routes[i].path == path) return true;
        }
        return false;
    }

    /// Runs a registered route and returns its response. Fails the test rather
    /// than returning a default when the route is absent: a 0-status response
    /// would otherwise read as "the handler answered nothing".
    HttpResponse Call(const std::string& method, const std::string& path,
                      const HttpRequest& request) const {
        for (std::size_t i = 0; i < routes.size(); ++i) {
            if (routes[i].method == method && routes[i].path == path) {
                HttpResponse resp;
                routes[i].handler(request, &resp);
                return resp;
            }
        }
        MT_EXPECT_TRUE(false);  // route not registered
        return HttpResponse();
    }
};

}  // namespace

MT_TEST(the_node_admin_routes_are_registered) {
    // The regression this file exists for. Both handlers were complete and
    // neither was reachable; a test that only called `NodesGet` could not tell.
    Fixture f;
    RecordingServer server;
    RegisterOrchestratorRoutes(&server, f.orch.get());

    MT_EXPECT_TRUE(server.Has("GET", "/nodes"));
    // `{node_id}`, matching the generated router's `/nodes/{node_id}`. The
    // spec's `nodeID` names the *parameter*, not the path template, and a
    // mismatch here would make `Param(req, "node_id")` read empty -- a 404 for
    // every node, on a route that looks registered.
    MT_EXPECT_TRUE(server.Has("GET", "/nodes/{node_id}"));
}

MT_TEST(a_node_list_without_observability_is_an_empty_list_not_an_error) {
    // Upstream's `let Some(observability) = .. else { return Ok(vec![]) }`.
    // This build passes no observability, so this is the arm it lands in, and
    // it is a 200: a scheduler reading the list must see "no nodes here",
    // which is true, rather than a 500 it would retry.
    Fixture f;
    RecordingServer server;
    RegisterOrchestratorRoutes(&server, f.orch.get());

    HttpRequest req;
    req.method = "GET";
    req.path   = "/nodes";
    const HttpResponse resp = server.Call("GET", "/nodes", req);
    MT_EXPECT_EQ(resp.status, 200);
    // A bare array, as the schema has it. Wrapped in a `nodes` key it would
    // not deserialize for a client generated from the spec.
    MT_EXPECT_EQ(resp.body, std::string("[]"));
}

MT_TEST(a_node_detail_without_observability_is_a_404_in_upstreams_words) {
    Fixture f;
    RecordingServer server;
    RegisterOrchestratorRoutes(&server, f.orch.get());

    HttpRequest req;
    req.method = "GET";
    req.path   = "/nodes/node-1";
    req.query["node_id"] = "node-1";
    const HttpResponse resp = server.Call("GET", "/nodes/{node_id}", req);
    MT_EXPECT_EQ(resp.status, 404);
    // Upstream's message verbatim: a 404 whose body said "node not found"
    // would send an operator looking for a node id that was never the problem.
    MT_EXPECT_TRUE(resp.body.find("observability is disabled on this node") !=
                   std::string::npos);
}

MT_TEST(the_metrics_route_is_registered_and_reports_an_uninitialized_recorder) {
    // Rust `.route("/metrics", get(metrics_handler))`, whose no-recorder arm
    // is a 503. This build has no recorder -- `observability/metrics.h` is a
    // facade that writes to stderr, with no registry and no exposition
    // encoder -- so the 503 is the whole route.
    //
    // Registered rather than omitted because the auth layer already declared
    // it public, so leaving it out made the two layers contradict each other:
    // exempt from authentication, and 404.
    Fixture f;
    RecordingServer server;
    RegisterOrchestratorRoutes(&server, f.orch.get());

    MT_EXPECT_TRUE(server.Has("GET", "/metrics"));

    HttpRequest req;
    req.method = "GET";
    req.path   = "/metrics";
    const HttpResponse resp = server.Call("GET", "/metrics", req);
    // 503, not 404: to a scraper the first means "endpoint is there, backend
    // not ready" and the second means "wrong path". Only one is true.
    MT_EXPECT_EQ(resp.status, 503);
    MT_EXPECT_EQ(resp.body, std::string("prometheus recorder is not initialized"));
    // Plain text, as Prometheus expects on this route -- the control plane's
    // `{code, message}` JSON envelope is not what a scraper parses.
    const std::unordered_map<std::string, std::string>::const_iterator type =
        resp.headers.find("content-type");
    MT_EXPECT_TRUE(type != resp.headers.end());
    if (type != resp.headers.end()) {
        MT_EXPECT_TRUE(type->second.find("text/plain") != std::string::npos);
    }
    MT_EXPECT_TRUE(resp.body.find('{') == std::string::npos);
}

MT_TEST(the_metrics_path_the_auth_layer_exempts_is_the_path_that_is_routed) {
    // The two were out of step, which is what made the missing route a real
    // fault rather than a cosmetic one: `IsUnauthenticatedPath` has exempted
    // `/metrics` all along. Pinned together so neither can move alone -- an
    // exemption for a path that is not served is a dead rule, and a served
    // metrics path that is not exempt is a scraper that gets 401.
    Fixture f;
    RecordingServer server;
    RegisterOrchestratorRoutes(&server, f.orch.get());

    MT_EXPECT_TRUE(IsUnauthenticatedPath("/metrics"));
    MT_EXPECT_TRUE(server.Has("GET", "/metrics"));
    MT_EXPECT_TRUE(IsUnauthenticatedPath("/health"));
    MT_EXPECT_TRUE(server.Has("GET", "/health"));
}

MT_TEST(the_routes_that_were_already_served_stay_served) {
    // A guard on the edit that added the admin routes, not a restatement of
    // the table: these are the twenty routes that were registered before, and
    // the risk of touching the registration function is dropping one.
    Fixture f;
    RecordingServer server;
    RegisterOrchestratorRoutes(&server, f.orch.get());

    static const char* const kGet[] = {
        "/sandboxes", "/sandboxes/{sandbox_id}", "/volumes", "/volumes/{volume_id}",
        "/templates", "/templates/{template_id}/builds/{build_id}/status",
        "/templates/{template_id}/builds/{build_id}/logs", "/health",
    };
    for (std::size_t i = 0; i < sizeof(kGet) / sizeof(kGet[0]); ++i) {
        MT_EXPECT_TRUE(server.Has("GET", kGet[i]));
    }

    static const char* const kPost[] = {
        "/sandboxes", "/sandboxes-cold", "/sandboxes/{sandbox_id}/pause",
        "/sandboxes/{sandbox_id}/resume", "/sandboxes/{sandbox_id}/fork",
        "/sandboxes/{sandbox_id}/snapshots", "/volumes", "/v3/templates",
        "/v2/templates/{template_id}/builds/{build_id}",
    };
    for (std::size_t i = 0; i < sizeof(kPost) / sizeof(kPost[0]); ++i) {
        MT_EXPECT_TRUE(server.Has("POST", kPost[i]));
    }

    MT_EXPECT_TRUE(server.Has("DELETE", "/sandboxes/{sandbox_id}"));
    MT_EXPECT_TRUE(server.Has("DELETE", "/volumes/{volume_id}"));
}

MT_TEST(the_custom_extension_params_routes_are_registered) {
    // The same gap as the node routes: `PatchSandboxCustomExtensionParams`
    // was complete in the orchestrator, including the hook call and the state
    // guard, and no URL reached it.
    Fixture f;
    RecordingServer server;
    RegisterOrchestratorRoutes(&server, f.orch.get());

    MT_EXPECT_TRUE(server.Has("GET", "/sandboxes/{sandbox_id}/custom-extension-params"));
    // PATCH, not PUT: the body is a partial map the extension merges, and a
    // PUT would promise replacement semantics the hook does not implement.
    MT_EXPECT_TRUE(server.Has("PATCH", "/sandboxes/{sandbox_id}/custom-extension-params"));
    MT_EXPECT_TRUE(!server.Has("PUT", "/sandboxes/{sandbox_id}/custom-extension-params"));
}

MT_TEST(no_exec_route_is_served_because_upstream_has_none) {
    // The divergence that ran the other way. This port registered
    // `POST /sandboxes/{id}/exec` answering an unconditional 501, and upstream
    // has no such endpoint at all -- `openapi.yml` never mentions `exec`, and
    // running a command reaches envd through the proxy as a data plane call.
    //
    // A 501 and a 404 say different things, and only one of them is true here:
    // 501 claims the endpoint is part of the API and unimplemented locally,
    // which would have a client wait for a build that supports it.
    Fixture f;
    RecordingServer server;
    RegisterOrchestratorRoutes(&server, f.orch.get());

    MT_EXPECT_TRUE(!server.Has("POST", "/sandboxes/{sandbox_id}/exec"));
    for (std::size_t i = 0; i < server.routes.size(); ++i) {
        MT_EXPECT_TRUE(server.routes[i].path.find("exec") == std::string::npos);
    }
}

MT_TEST(no_route_is_registered_twice) {
    // Two handlers on one (method, path) is a silent shadowing: whichever the
    // router prefers answers, and the other is dead code that still compiles,
    // still has tests, and never runs. That is the same failure mode as an
    // unregistered handler, from the other direction.
    Fixture f;
    RecordingServer server;
    RegisterOrchestratorRoutes(&server, f.orch.get());

    for (std::size_t i = 0; i < server.routes.size(); ++i) {
        int matches = 0;
        for (std::size_t j = 0; j < server.routes.size(); ++j) {
            if (server.routes[i].method == server.routes[j].method &&
                server.routes[i].path == server.routes[j].path) {
                ++matches;
            }
        }
        MT_EXPECT_EQ(matches, 1);
    }
}

MT_MAIN
