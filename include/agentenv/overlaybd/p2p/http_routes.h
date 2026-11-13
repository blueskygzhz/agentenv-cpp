// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/p2p/facade.rs — the axum router half of the facade.
//
// `facade.h` ports everything protocol-shaped as plain functions plus a
// `FacadeCore`; what was missing was the half that puts those on a socket, so
// `FacadeCore`'s three handlers had no caller and the whole block-level P2P
// path was dead on every node.
//
// The server is named here as an interface and injected, for the reason
// `p2p/http_transport.h` documents for `P2pBlobServer`: `agentenv-api` reaches
// this library transitively, so a dependency back on it would be a CMake
// cycle. The shapes are deliberately not shared with `P2pServerRequest` —
// this one needs two things that one does not (the raw target including its
// query, and whether the peer is loopback), and widening the blob server's
// request to carry facade-only fields would put a security-relevant field in
// front of handlers that have no business reading it.
#ifndef AGENTENV_OVERLAYBD_P2P_HTTP_ROUTES_H_
#define AGENTENV_OVERLAYBD_P2P_HTTP_ROUTES_H_

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/overlaybd/p2p/facade.h"

namespace agentenv {
namespace overlaybd {
namespace p2p {

struct FacadeServerRequest {
    std::string method;
    /// The request target **exactly as sent**, path and query together.
    ///
    /// `ReconstructOriginUrl` parses this, and it needs both halves: a
    /// presigned registry URL carries its signature in the query, so a handler
    /// given only the path would reconstruct an origin URL that the registry
    /// then rejects as unauthenticated. It must also be undecoded, since the
    /// origin URL's own escapes are part of it.
    std::string raw_target;
    /// Case-insensitive, as HTTP requires.
    HeaderMap headers;
    std::string body;
    /// Whether the connection came from a loopback address.
    ///
    /// Supplied by the adapter from the socket, never from a header: the
    /// publish endpoint gates on it, and a client-supplied `X-Forwarded-For`
    /// would let a remote caller claim to be local. False when unknown, so the
    /// gate fails closed.
    bool peer_is_loopback = false;
};

struct FacadeServerResponse {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;
};

/// The subset of an HTTP server the facade needs.
class FacadeServer {
 public:
    typedef std::function<void(const FacadeServerRequest&, FacadeServerResponse*)> Handler;

    virtual ~FacadeServer() {}

    /// `path_pattern` uses the tree's `{name}` placeholders, plus a trailing
    /// `{*name}` catch-all that matches the remainder of the path with its
    /// separators intact. The catch-all is required, not a convenience: the
    /// origin route's path *is* a URL, so a segment-wise pattern cannot
    /// express it.
    virtual void Route(const std::string& method, const std::string& path_pattern,
                       Handler handler) = 0;
    /// Blocking; returns once `Stop()` has been called.
    virtual core::Expected<core::Unit, std::string>
        ListenAndServe(const std::string& bind_addr) = 0;
    virtual void Stop() = 0;
};

/// Largest single range the facade will answer, in bytes.
///
/// **Deviation from upstream, and the reason this constant exists.** Rust
/// streams the body straight through (`Body::from_stream`), so a range's size
/// bounds nothing but time. Both server interfaces in this tree carry the body
/// as a `std::string`, so answering a range means holding it. overlaybd asks
/// in bounded blocks, so this never binds in practice -- but without a ceiling
/// a client could ask for a 64 GiB range and make the facade allocate it,
/// which is a denial of service the streaming version does not have. A request
/// above it is refused with 416 rather than truncated: a short answer framed
/// as the full window would corrupt the layer the runtime reads.
extern const std::size_t kMaxFacadeRangeBytes;

/// Registers the facade's three endpoints.
///
///   * `GET  /p2p-http/{*origin}`            — P2P first, origin fallback.
///   * `GET  /p2p-uuid/{uuid}`               — P2P only; a miss is a 404.
///   * `POST /p2p-control/publish-layer`     — loopback only.
///
/// `core` must be non-null; a null one registers nothing, so a disabled
/// runtime cannot accidentally serve.
void RegisterOverlaybdP2pFacadeRoutes(FacadeServer* server, std::shared_ptr<FacadeCore> core);

/// Drains a byte stream into `out`, refusing anything past `limit`.
///
/// Exposed for tests: the refusal is the interesting half. A stream that keeps
/// yielding past the length the headers promised must fail the response rather
/// than extend it, because the `Content-Range` has already been computed from
/// that promise.
core::Expected<core::Unit, std::string> DrainByteStream(
    const std::shared_ptr<agentenv::p2p::P2pByteStream>& stream, std::size_t limit,
    std::string* out);

}  // namespace p2p
}  // namespace overlaybd
}  // namespace agentenv
#endif  // AGENTENV_OVERLAYBD_P2P_HTTP_ROUTES_H_
