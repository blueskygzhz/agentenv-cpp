// SPDX-License-Identifier: MIT
// Rust: src/api/mod.rs — the axum server's framing and routing, minus axum.
//
// A hand-written HTTP/1.1 server, for the same reason `core/http.h` is a
// hand-written client: the tree carries no mandatory third-party dependency.
// The dialect is small (JSON in, JSON out, no upgrades, no trailers), but the
// parts a real server cannot skip are here:
//
//   * Request framing. A body shorter than its `Content-Length` must be waited
//     for, not handed to a handler half-read; one longer must not bleed into
//     the next request on the connection.
//
//   * A bound on the request size. Without one, a client that announces a
//     10 GiB body makes the server allocate it.
//
//   * Path parameters. `/sandboxes/{id}` has to match one segment and capture
//     it, and must not match `/sandboxes/abc/exec` — which a prefix test does.
//
// The parsing and routing are free functions over strings so they can be
// tested without a socket. That is where the bugs are: a wedged connection is
// obvious, a path pattern that matches one segment too many is not.
#ifndef AGENTENV_API_HTTP_SERVER_H_
#define AGENTENV_API_HTTP_SERVER_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "agentenv/api/server.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace api {

/// Largest request this server will buffer, headers and body together.
///
/// A create request is a few hundred bytes; the ceiling exists so a client
/// cannot name a body the server then tries to allocate.
static const std::size_t kMaxRequestBytes = 1024 * 1024;

/// How much of a request has been read so far.
enum class RequestFraming {
    /// Headers are incomplete, or the body is shorter than `Content-Length`.
    Incomplete,
    /// A whole request is present; `consumed` says how many bytes it spans.
    Complete,
    /// Unparseable, or larger than `kMaxRequestBytes`. The connection is not
    /// recoverable: without valid framing there is no way to find where the
    /// next request starts.
    Malformed,
};

struct FramedRequest {
    RequestFraming framing = RequestFraming::Incomplete;
    HttpRequest    request;
    /// Bytes this request occupies in the buffer, once `Complete`.
    std::size_t    consumed = 0;
    /// Why it was rejected, once `Malformed`.
    std::string    error;
};

/// Parses one request out of `buffer`, which may hold a partial or several.
///
/// Returning `Incomplete` rather than an error for a short read is the whole
/// point: TCP delivers a request in however many pieces it likes, and a parser
/// that treats the first piece as the request drops the rest of the body.
FramedRequest ParseRequest(const std::string& buffer);

/// Splits `a=1&b=2` into pairs, percent-decoding both halves.
std::map<std::string, std::string> ParseQuery(const std::string& query);

/// Percent-decoding. `+` is a space, as in a query string.
std::string PercentDecode(const std::string& text);

/// Matches a path against a pattern with `{name}` placeholders.
///
/// A placeholder matches exactly one non-empty segment, so `/sandboxes/{id}`
/// matches `/sandboxes/abc` but not `/sandboxes/abc/exec` and not
/// `/sandboxes/`. Captured values land in `params`.
bool MatchPath(const std::string& pattern, const std::string& path,
               std::map<std::string, std::string>* params);

/// Renders a response onto the wire, including the framing headers.
///
/// `Content-Length` is always written, even for an empty body: a response
/// without it is read as "until close", which turns every reply into a
/// connection teardown.
std::string SerializeResponse(const HttpResponse& response);

/// A blocking HTTP/1.1 server over TCP.
///
/// One thread per connection. The endpoints here are administrative and
/// called rarely; an event loop would buy nothing and cost the ability to
/// call blocking orchestrator methods straight from a handler.
class HttpServer : public Server {
 public:
    HttpServer();
    ~HttpServer() override;

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void Route(const std::string& method, const std::string& path_pattern,
               Handler h) override;

    /// Binds, listens and serves until `Stop`. `bind_addr` is `host:port`.
    core::Expected<core::Unit, core::AnyError>
        ListenAndServe(const std::string& bind_addr) override;

    void Stop() override;

    /// The port actually bound. Meaningful after `ListenAndServe` has bound,
    /// which is what lets a test ask for port 0 and still connect.
    uint16_t BoundPort() const;

    /// Routes one already-parsed request. Exposed so the routing table can be
    /// exercised without a socket.
    HttpResponse Dispatch(const HttpRequest& request) const;

 private:
    class Impl;
    Impl* impl_;
};

/// Splits `host:port`. An address with no host binds all interfaces.
core::Expected<core::Unit, std::string> ParseBindAddr(const std::string& bind_addr,
                                                      std::string* host,
                                                      uint16_t* port);

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_HTTP_SERVER_H_
