// SPDX-License-Identifier: MIT
// Rust: src/api/mod.rs — the HTTP server. Uses axum + tower.
// C++11: define an abstract server contract; concrete impls (cpp-httplib, beast,
// mock) live in separate .cc files.
#ifndef AGENTENV_API_SERVER_H_
#define AGENTENV_API_SERVER_H_

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"

namespace agentenv {
// Forward-declared rather than included: this header is pulled in by every
// transport and test, and the volume/image headers drag in the repository and
// overlaybd surfaces. The routes only need to pass these through.
namespace orchestrator { class Orchestrator; }
namespace volume { class VolumeManager; }
namespace image { class ImageResolver; }
namespace p2p { class P2pTransport; }
namespace snapshot {
class SnapshotRuntimeResolver;
namespace repository { class SnapshotRepository; }
}  // namespace snapshot

namespace api {

/// Forward-declared for the same reason as the collaborators above: the routes
/// only pass a pointer through, and `admin.h` drags in the observability model.
class AdminObservability;

struct HttpRequest {
    std::string method;               // GET / POST / DELETE / PATCH
    std::string path;                 // "/sandboxes/abc"
    /// The request target's path and query exactly as sent (not
    /// percent-decoded). Rust's proxy forwards `uri.path()` / `uri.query()`
    /// verbatim; forwarding the decoded `path` would change what the guest
    /// sees (an encoded `/` would become a segment separator).
    std::string raw_path;
    std::string raw_query;
    bool        has_query = false;
    /// Lowercased name -> the *first* value, as `HeaderMap::get` returns it.
    std::unordered_map<std::string, std::string> headers;
    /// Lowercased names that were sent more than once. Rust's `HeaderMap`
    /// keeps every value, and `single_header` treats a repeated credential as
    /// absent; this set is what lets the auth layer see "sent twice" through
    /// the one-value `headers` map.
    std::set<std::string> repeated_headers;
    std::unordered_map<std::string, std::string> query;
    std::string body;
    /// Rust `MatchedPath`: set by the server, before any middleware runs, when
    /// the path matched a registered route (whatever the method).
    bool matched_route = false;
    /// Rust `RawPathParams` of the matched route.
    std::map<std::string, std::string> path_params;
    /// The connection's peer address, or empty when it could not be read.
    ///
    /// Rust gets this from axum's `ConnectInfo<SocketAddr>`. It is a property
    /// of the connection rather than the payload, so a handler must not infer
    /// it from a header: `X-Forwarded-For` is client-supplied and would let a
    /// remote caller claim to be local. The overlaybd P2P facade's publish
    /// endpoint is the consumer — it accepts loopback clients only.
    std::string peer_addr;
    /// Whether `peer_addr` is a loopback address.
    ///
    /// Computed by the server from the parsed address rather than by comparing
    /// strings at the call site, because the textual forms are several
    /// (`127.0.0.1`, any `127.x.y.z`, `::1`, `::ffff:127.0.0.1`) and a handler
    /// that checked only the first would reject legitimate local clients.
    /// False when the peer address is unknown, which is the safe default for
    /// an endpoint that gates on it.
    bool peer_is_loopback = false;
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
///
/// This overload registers `/volumes*` too, but every one of those routes then
/// answers 500: the handlers need a volume manager. Prefer the four-argument
/// form for anything that serves volumes.
void RegisterOrchestratorRoutes(Server* server, orchestrator::Orchestrator* svc);

/// As above, plus the collaborators the `/volumes*` handlers need.
///
/// `volumes` may be null, which is what the two-argument overload passes.
/// `image_resolver` may be null independently: it is read only by
/// `POST /volumes` when the request names an `image`, so a deployment that
/// never creates volumes from images does not need one.
///
/// `repository` may be null on the same terms: only
/// `POST /sandboxes/{id}/snapshots` reads it, and that route answers 500
/// rather than reporting a snapshot it never published.
///
/// `runtime_resolver` is what `POST /sandboxes` needs to turn a `templateID`
/// into launchable paths. Null is legal and means that route answers 501:
/// only the posix-fs repository backend can supply a resolver (see
/// `snapshot::repository::SnapshotBackend`). Passing it is what connects
/// `MakeSnapshotBackendFromConfig`'s second return value to the one handler
/// that consumes it.
///
/// `observability` backs `GET /nodes` and `GET /nodes/{nodeID}`. Null is legal
/// and is *not* a refusal: it is upstream's "observability is disabled on this
/// node", which answers the collection with an empty list and the detail with
/// a 404. Borrowed, not owned — the caller outlives the routes.
///
/// `p2p_transport` is what `POST /sandboxes/{id}/snapshots` announces a newly
/// committed snapshot over. Null is legal and announces nothing; it is
/// `SnapshotBackend::p2p_transport`, which already applies the
/// `snapshot.p2p_enabled` gate.
void RegisterOrchestratorRoutes(
    Server* server, orchestrator::Orchestrator* svc,
    std::shared_ptr<volume::VolumeManager> volumes,
    std::shared_ptr<image::ImageResolver> image_resolver,
    std::shared_ptr<snapshot::repository::SnapshotRepository> repository =
        std::shared_ptr<snapshot::repository::SnapshotRepository>(),
    std::shared_ptr<snapshot::SnapshotRuntimeResolver> runtime_resolver =
        std::shared_ptr<snapshot::SnapshotRuntimeResolver>(),
    const AdminObservability* observability = NULL,
    std::shared_ptr<p2p::P2pTransport> p2p_transport =
        std::shared_ptr<p2p::P2pTransport>());

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_SERVER_H_
