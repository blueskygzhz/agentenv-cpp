// SPDX-License-Identifier: MIT
// A minimal Server implementation that does NO listening — it is a "dispatcher"
// intended to be driven by tests. When AGENTENV_WITH_HTTPLIB is enabled, replace
// this with a cpp-httplib-backed server.
#include "agentenv/api/server.h"

#include <stdexcept>
#include <vector>

namespace agentenv {
namespace api {

class InMemoryServer final : public Server {
 public:
    void Route(const std::string& method, const std::string& path_pattern, Handler h) override {
        RouteEntry e;
        e.method = method;
        e.path_pattern = path_pattern;
        e.handler = std::move(h);
        routes_.push_back(std::move(e));
    }

    core::Expected<core::Unit, core::AnyError>
    ListenAndServe(const std::string& /*bind_addr*/) override {
        // No listener in the skeleton; return "not implemented".
        return core::make_unexpected(core::err(
            "api::Server: no HTTP backend compiled in "
            "(build with AGENTENV_WITH_HTTPLIB to enable)"));
    }

    void Stop() override {}

    /// Test hook: dispatch synchronously.
    HttpResponse Dispatch(const HttpRequest& req) {
        for (std::size_t i = 0; i < routes_.size(); ++i) {
            const RouteEntry& r = routes_[i];
            if (r.method == req.method && r.path_pattern == req.path) {
                HttpResponse resp;
                r.handler(req, &resp);
                return resp;
            }
        }
        HttpResponse resp;
        resp.status = 404;
        resp.body = R"({"code":"not_found","message":"no route"})";
        return resp;
    }

 private:
    struct RouteEntry {
        std::string method;
        std::string path_pattern;
        Handler handler;
    };
    std::vector<RouteEntry> routes_;
};

std::unique_ptr<Server> MakeServer() {
#ifdef AGENTENV_WITH_HTTPLIB
    // TODO: return an httplib-backed server.
    return std::unique_ptr<Server>(new InMemoryServer());
#else
    return std::unique_ptr<Server>(new InMemoryServer());
#endif
}

void RegisterOrchestratorRoutes(Server* /*server*/,
                                orchestrator::Orchestrator* /*svc*/) {
    // Skeleton: real wiring goes here once the Rust `src/api/impls/*` handlers
    // are ported onto the Orchestrator surface.
}

}  // namespace api
}  // namespace agentenv
