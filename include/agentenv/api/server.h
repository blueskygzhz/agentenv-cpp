// SPDX-License-Identifier: MIT
// Rust: src/api/mod.rs — the HTTP server. Uses axum + tower.
// C++11: define an abstract server contract; concrete impls (cpp-httplib, beast,
// mock) live in separate .cc files.
#ifndef AGENTENV_API_SERVER_H_
#define AGENTENV_API_SERVER_H_

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"

namespace agentenv {
namespace orchestrator { class Orchestrator; }

namespace api {

struct HttpRequest {
    std::string method;               // GET / POST / DELETE / PATCH
    std::string path;                 // "/sandboxes/abc"
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> query;
    std::string body;
};

struct HttpResponse {
    int         status = 200;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

using Handler = std::function<void(const HttpRequest&, HttpResponse*)>;

/// Rust: an axum `middleware::from_fn_with_state` layer.
///
/// Returns true to continue to the route handler, false to short-circuit with
/// whatever it wrote into `*resp`. The request is mutable because Rust's
/// `require_auth` strips credential headers before calling `next.run`, and a
/// layer that could not modify the request would be unable to express that.
using Middleware = std::function<bool(HttpRequest*, HttpResponse*)>;

/// Abstract server interface. Concrete impl selected at compile time via CMake.
class Server {
 public:
    virtual ~Server() = default;
    virtual void Route(const std::string& method,
                       const std::string& path_pattern,
                       Handler h) = 0;

    /// Installs a layer that runs before routing, for every request.
    ///
    /// Running before routing is deliberate: Rust's `require_auth` wraps the
    /// whole router, so an unauthenticated request to an unknown path is
    /// rejected rather than answered with a 404 that confirms what does not
    /// exist. Layers run in registration order.
    virtual void Use(Middleware layer) = 0;

    /// Blocking. Returns when Stop() is called from another thread.
    virtual core::Expected<core::Unit, core::AnyError>
        ListenAndServe(const std::string& bind_addr) = 0;

    virtual void Stop() = 0;
};

/// Factory: build a Server backed by the best backend that was compiled in.
std::unique_ptr<Server> MakeServer();

/// Convenience: register all `/sandboxes*` routes onto the given server,
/// dispatching to the given orchestrator::Orchestrator.
void RegisterOrchestratorRoutes(Server* server, orchestrator::Orchestrator* svc);

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_SERVER_H_
