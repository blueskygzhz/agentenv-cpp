// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/{socket,connector}.rs — the API unix socket.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_SOCKET_H_
#define AGENTENV_SANDBOX_FIRECRACKER_SOCKET_H_

#include <memory>
#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust: firecracker/socket.rs — a minimal HTTP-over-unix-socket client to the
/// Firecracker API socket.
class Socket {
 public:
    virtual ~Socket() {}
    virtual core::Expected<std::string, std::string>
        Request(const std::string& method,
                const std::string& path,
                const std::string& body) = 0;
};

/// Rust: firecracker/connector.rs — opens a `Socket` to an api socket path.
class Connector {
 public:
    virtual ~Connector() {}
    virtual core::Expected<std::shared_ptr<Socket>, std::string>
        Connect(const std::string& api_socket_path) = 0;
};

/// A real HTTP/1.1-over-AF_UNIX connector. It connects to the given socket path
/// and its `Socket` sends `<METHOD> <path> HTTP/1.1` requests (with a JSON body)
/// and returns the response body. This is exactly the Firecracker API dialect
/// and needs no external HTTP library.
std::shared_ptr<Connector> MakeUnixConnector();

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_SOCKET_H_
