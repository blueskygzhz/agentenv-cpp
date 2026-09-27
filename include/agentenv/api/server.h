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
namespace orchestrator { class Service; }

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

/// Abstract server interface. Concrete impl selected at compile time via CMake.
class Server {
 public:
    virtual ~Server() = default;
    virtual void Route(const std::string& method,
                       const std::string& path_pattern,
                       Handler h) = 0;

    /// Blocking. Returns when Stop() is called from another thread.
    virtual core::Expected<core::Unit, core::AnyError>
        ListenAndServe(const std::string& bind_addr) = 0;

    virtual void Stop() = 0;
};

/// Factory: build a Server backed by the best backend that was compiled in.
std::unique_ptr<Server> MakeServer();

/// Convenience: register all `/sandboxes*` routes onto the given server,
/// dispatching to the given orchestrator::Service.
void RegisterOrchestratorRoutes(Server* server, orchestrator::Service* svc);

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_SERVER_H_
