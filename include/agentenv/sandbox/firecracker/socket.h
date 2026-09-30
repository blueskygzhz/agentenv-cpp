// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/{socket,connector}.rs — the API unix socket.
//
// `Request` returns the response body only on success. A non-2xx reply becomes
// an error carrying the status *and* Firecracker's fault body, matching Rust's
// `UnixSocketClient::request`: Firecracker explains a rejection (a bad drive
// path, an unsupported field) in the body, and a caller that saw only "no
// transport error" would carry on and fail much later somewhere unrelated.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_SOCKET_H_
#define AGENTENV_SANDBOX_FIRECRACKER_SOCKET_H_

#include <future>
#include <memory>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/http.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust: firecracker/socket.rs — an HTTP-over-unix-socket client to the
/// Firecracker API socket.
class Socket {
 public:
    virtual ~Socket() {}

    /// Sends one JSON request. The body is returned on 2xx; anything else is
    /// an error naming the status and the fault body.
    virtual core::Expected<std::string, std::string>
        Request(const std::string& method, const std::string& path,
                const std::string& body) = 0;

    /// The same call on the shared thread pool.
    ///
    /// Rust reaches this endpoint from async tasks; the blocking call is moved
    /// to a pool thread so a slow or wedged Firecracker cannot hold the
    /// calling thread. The bounded read timeout is what makes losing a worker
    /// temporary rather than permanent.
    virtual std::future<core::Expected<std::string, std::string> >
        RequestAsync(const std::string& method, const std::string& path,
                     const std::string& body) = 0;
};

/// Rust: firecracker/connector.rs — opens a `Socket` to an api socket path.
class Connector {
 public:
    virtual ~Connector() {}
    virtual core::Expected<std::shared_ptr<Socket>, std::string>
        Connect(const std::string& api_socket_path) = 0;
};

/// A real HTTP/1.1-over-AF_UNIX connector, built on `core::http`.
///
/// `timeouts` bounds both the connect and every read. Firecracker is local so
/// the defaults are generous; a wedged VMM must still not pin a thread.
std::shared_ptr<Connector> MakeUnixConnector(
    const core::http::Timeouts& timeouts = core::http::Timeouts::Default());

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_SOCKET_H_
