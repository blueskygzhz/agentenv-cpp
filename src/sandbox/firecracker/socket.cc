// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/{socket,connector}.rs
//
// A real HTTP/1.1-over-AF_UNIX client — exactly the Firecracker API transport.
// No external HTTP library is needed: the request line + headers + JSON body are
// written by hand and the response body is read back after the CRLFCRLF split.
#include "agentenv/sandbox/firecracker/socket.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace agentenv {
namespace sandbox {
namespace firecracker {

namespace {

bool write_all(int fd, const char* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(w);
    }
    return true;
}

class UnixSocket : public Socket {
 public:
    explicit UnixSocket(int fd) : fd_(fd) {}
    ~UnixSocket() { if (fd_ >= 0) ::close(fd_); }

    core::Expected<std::string, std::string>
    Request(const std::string& method, const std::string& path,
            const std::string& body) override {
        if (fd_ < 0) return core::make_unexpected(std::string("socket closed"));

        // Build a minimal HTTP/1.1 request. Firecracker expects Host + a JSON
        // Content-Type; Connection: close lets us read until EOF.
        std::string req;
        req.reserve(body.size() + 160);
        req += method; req += " "; req += path; req += " HTTP/1.1\r\n";
        req += "Host: localhost\r\n";
        req += "Accept: application/json\r\n";
        if (!body.empty()) {
            req += "Content-Type: application/json\r\n";
            char len[32];
            std::snprintf(len, sizeof(len), "%zu", body.size());
            req += "Content-Length: "; req += len; req += "\r\n";
        }
        req += "Connection: close\r\n\r\n";
        req += body;

        if (!write_all(fd_, req.data(), req.size())) {
            return core::make_unexpected(std::string("write request failed:") + std::strerror(errno));
        }

        // Read the whole response.
        std::string resp;
        char buf[4096];
        for (;;) {
            ssize_t r = ::read(fd_, buf, sizeof(buf));
            if (r < 0) {
                if (errno == EINTR) continue;
                return core::make_unexpected(std::string("read response failed: ") + std::strerror(errno));
            }
            if (r == 0) break;  // peer closed
            resp.append(buf, static_cast<size_t>(r));
        }

        // Split headers/body at CRLFCRLF; return the body (Firecracker replies
        // with a JSON document or an empty 204).
        std::string::size_type sep = resp.find("\r\n\r\n");
        if (sep == std::string::npos) {
            // No header terminator — surface the raw response for diagnostics.
            return resp;
        }
        // Validate the status line minimally (must start with "HTTP/1.").
        if (resp.compare(0, 7, "HTTP/1.") != 0) {
            return core::make_unexpected(std::string("malformed HTTP response"));
        }
        return resp.substr(sep + 4);
    }

 private:
    int fd_;
};

class UnixConnector : public Connector {
 public:
    core::Expected<std::shared_ptr<Socket>, std::string>
    Connect(const std::string& api_socket_path) override {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return core::make_unexpected(std::string("socket() failed"));

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (api_socket_path.size() >= sizeof(addr.sun_path)) {
            ::close(fd);
            return core::make_unexpected(std::string("api socket path too long"));
        }
        std::strncpy(addr.sun_path, api_socket_path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::string e = std::strerror(errno);
            ::close(fd);
            return core::make_unexpected(std::string("connect() to api socket failed: ") + e);
        }
        return std::shared_ptr<Socket>(new UnixSocket(fd));
    }
};

}  // namespace

std::shared_ptr<Connector> MakeUnixConnector() {
    return std::shared_ptr<Connector>(new UnixConnector());
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
