// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/{socket,connector}.rs
//
// Built on `core::http`, which supplies the HTTP/1.1 framing, the status code
// and the timeouts. What this file adds is the Firecracker dialect: JSON in,
// JSON or empty out, and a non-2xx turned into an error that carries the fault
// body.
#include "agentenv/sandbox/firecracker/socket.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "agentenv/core/executor.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

namespace {

class UnixSocket : public Socket {
 public:
    UnixSocket(const std::string& path, const core::http::Timeouts& timeouts)
        : client_(core::http::Target::UnixSocket(path), timeouts) {}

    core::Expected<std::string, std::string> Request(const std::string& method,
                                                     const std::string& path,
                                                     const std::string& body) override {
        // `RequestJson` is what enforces the status check, so a Firecracker
        // rejection cannot be mistaken for an empty success.
        return client_.RequestJson(method, path, body);
    }

    std::future<core::Expected<std::string, std::string> > RequestAsync(
        const std::string& method, const std::string& path,
        const std::string& body) override {
        return client_.RequestJsonAsync(method, path, body);
    }

 private:
    core::http::HttpClient client_;
};

class UnixConnector : public Connector {
 public:
    explicit UnixConnector(const core::http::Timeouts& timeouts) : timeouts_(timeouts) {}

    core::Expected<std::shared_ptr<Socket>, std::string> Connect(
        const std::string& api_socket_path) override {
        // Lazy, matching Rust: `UnixSocketClient::new` returns `Self`, not a
        // `Result` — it builds a hyper client and connects on first use.
        //
        // An eager probe here would cost an extra connection on every API
        // call (a VM boot makes several) and would still be racy: the socket
        // can go away between the probe and the request. The first request
        // reports the same failure one step later.
        return std::shared_ptr<Socket>(new UnixSocket(api_socket_path, timeouts_));
    }

 private:
    core::http::Timeouts timeouts_;
};

}  // namespace

std::shared_ptr<Connector> MakeUnixConnector(const core::http::Timeouts& timeouts) {
    return std::shared_ptr<Connector>(new UnixConnector(timeouts));
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
