// SPDX-License-Identifier: MIT
// Rust: src/api/mod.rs — the serving half.
#include "agentenv/api/http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>

#include "agentenv/core/logging.h"

namespace agentenv {
namespace api {

namespace {

std::string ToLower(const std::string& s) {
    std::string out(s);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

std::string Trim(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

/// Splits a path into non-empty segments. Both `/a/b` and `/a/b/` yield
/// `{a, b}`, so a trailing slash does not change which route matches.
std::vector<std::string> Segments(const std::string& path) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        const std::size_t start = i;
        while (i < path.size() && path[i] != '/') ++i;
        if (i > start) out.push_back(path.substr(start, i - start));
    }
    return out;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string PercentDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out.push_back(' ');
        } else if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = HexValue(text[i + 1]);
            const int lo = HexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
            } else {
                // A malformed escape is kept verbatim rather than dropped:
                // silently deleting it would change the value the handler
                // sees into one the client never sent.
                out.push_back(text[i]);
            }
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::map<std::string, std::string> ParseQuery(const std::string& query) {
    std::map<std::string, std::string> out;
    std::size_t cursor = 0;
    while (cursor <= query.size()) {
        std::size_t amp = query.find('&', cursor);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(cursor, amp - cursor);
        if (!pair.empty()) {
            const std::size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                // A bare flag, as in `?verbose`. Present with an empty value
                // is the distinction a handler needs; dropping it would make
                // the flag unreadable.
                out[PercentDecode(pair)] = std::string();
            } else {
                out[PercentDecode(pair.substr(0, eq))] = PercentDecode(pair.substr(eq + 1));
            }
        }
        if (amp == query.size()) break;
        cursor = amp + 1;
    }
    return out;
}

bool MatchPath(const std::string& pattern, const std::string& path,
               std::map<std::string, std::string>* params) {
    const std::vector<std::string> want = Segments(pattern);
    const std::vector<std::string> got = Segments(path);
    // Segment count must agree. This is what keeps `/sandboxes/{id}` from
    // swallowing `/sandboxes/abc/exec`, which a prefix comparison would match
    // and then route to the wrong handler with a truncated id.
    if (want.size() != got.size()) return false;

    std::map<std::string, std::string> captured;
    for (std::size_t i = 0; i < want.size(); ++i) {
        const std::string& w = want[i];
        if (w.size() >= 2 && w[0] == '{' && w[w.size() - 1] == '}') {
            captured[w.substr(1, w.size() - 2)] = PercentDecode(got[i]);
            continue;
        }
        if (w != got[i]) return false;
    }
    if (params) *params = captured;
    return true;
}

FramedRequest ParseRequest(const std::string& buffer) {
    FramedRequest out;

    const std::size_t head_end = buffer.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        // Only bound the headers here. A body is checked against the ceiling
        // once its declared length is known, so a large-but-legal body is not
        // rejected for the bytes that happen to have arrived.
        if (buffer.size() > kMaxRequestBytes) {
            out.framing = RequestFraming::Malformed;
            out.error = "request headers exceed the size limit";
        }
        return out;
    }

    const std::string head = buffer.substr(0, head_end);
    const std::size_t body_begin = head_end + 4;

    std::istringstream lines(head);
    std::string request_line;
    if (!std::getline(lines, request_line)) {
        out.framing = RequestFraming::Malformed;
        out.error = "empty request";
        return out;
    }
    if (!request_line.empty() && request_line[request_line.size() - 1] == '\r') {
        request_line.erase(request_line.size() - 1);
    }

    std::istringstream parts(request_line);
    std::string method;
    std::string target;
    std::string version;
    if (!(parts >> method >> target >> version)) {
        out.framing = RequestFraming::Malformed;
        out.error = "malformed request line: " + request_line;
        return out;
    }

    out.request.method = method;
    const std::size_t question = target.find('?');
    if (question == std::string::npos) {
        out.request.path = PercentDecode(target);
    } else {
        out.request.path = PercentDecode(target.substr(0, question));
        const std::map<std::string, std::string> query =
            ParseQuery(target.substr(question + 1));
        for (std::map<std::string, std::string>::const_iterator it = query.begin();
             it != query.end(); ++it) {
            out.request.query[it->first] = it->second;
        }
    }

    std::size_t content_length = 0;
    bool has_length = false;
    std::string header_line;
    while (std::getline(lines, header_line)) {
        if (!header_line.empty() && header_line[header_line.size() - 1] == '\r') {
            header_line.erase(header_line.size() - 1);
        }
        if (header_line.empty()) continue;
        const std::size_t colon = header_line.find(':');
        if (colon == std::string::npos) {
            out.framing = RequestFraming::Malformed;
            out.error = "malformed header: " + header_line;
            return out;
        }
        const std::string name = Trim(header_line.substr(0, colon));
        const std::string value = Trim(header_line.substr(colon + 1));
        out.request.headers[ToLower(name)] = value;
        if (ToLower(name) == "content-length") {
            char* parse_end = NULL;
            const unsigned long declared = std::strtoul(value.c_str(), &parse_end, 10);
            if (parse_end == NULL || *parse_end != '\0') {
                out.framing = RequestFraming::Malformed;
                out.error = "malformed Content-Length: " + value;
                return out;
            }
            content_length = static_cast<std::size_t>(declared);
            has_length = true;
        }
    }

    if (has_length && content_length > kMaxRequestBytes) {
        // Rejected on the declared length, before a single body byte is
        // buffered. Waiting to find out would mean allocating what the client
        // claimed it would send.
        out.framing = RequestFraming::Malformed;
        out.error = "request body exceeds the size limit";
        return out;
    }

    if (buffer.size() < body_begin + content_length) return out;  // Incomplete

    out.request.body = buffer.substr(body_begin, content_length);
    out.framing = RequestFraming::Complete;
    out.consumed = body_begin + content_length;
    return out;
}

std::string SerializeResponse(const HttpResponse& response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << " "
        << (response.status == 200 ? "OK" : "") << "\r\n";
    bool has_type = false;
    for (std::unordered_map<std::string, std::string>::const_iterator it =
             response.headers.begin();
         it != response.headers.end(); ++it) {
        const std::string lower = ToLower(it->first);
        // Length and Connection are ours; a handler override would contradict
        // what is actually written below.
        if (lower == "content-length" || lower == "connection") continue;
        if (lower == "content-type") has_type = true;
        out << it->first << ": " << it->second << "\r\n";
    }
    if (!has_type) out << "Content-Type: application/json\r\n";
    // Always present, including for an empty body: without it the client
    // reads until close, so every response would end the connection.
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Connection: close\r\n";
    out << "\r\n";
    out << response.body;
    return out.str();
}

core::Expected<core::Unit, std::string> ParseBindAddr(const std::string& bind_addr,
                                                      std::string* host,
                                                      uint16_t* port) {
    const std::size_t colon = bind_addr.rfind(':');
    if (colon == std::string::npos) {
        return core::make_unexpected(std::string("bind address has no port: ") + bind_addr);
    }
    const std::string host_part = bind_addr.substr(0, colon);
    const std::string port_part = bind_addr.substr(colon + 1);

    char* parse_end = NULL;
    const unsigned long value = std::strtoul(port_part.c_str(), &parse_end, 10);
    if (parse_end == NULL || *parse_end != '\0' || value > 65535) {
        return core::make_unexpected(std::string("bind address has a bad port: ") + port_part);
    }
    // An empty host is "all interfaces", which is what `:8080` means.
    *host = host_part.empty() ? std::string("0.0.0.0") : host_part;
    *port = static_cast<uint16_t>(value);
    return core::Unit{};
}

// ---------------------------------------------------------------------------

class HttpServer::Impl {
 public:
    Impl() : listen_fd(-1), bound_port(0), running(false) {}

    struct RouteEntry {
        std::string method;
        std::string pattern;
        Handler     handler;
    };

    std::vector<RouteEntry> routes;
    /// Pre-routing layers, in registration order. Rust: the tower layers
    /// wrapped around the router in `api::server::new`.
    std::vector<Middleware> layers;
    int      listen_fd;
    uint16_t bound_port;
    /// Read by the accept loop and the (other-thread) `Stop`. The loop also
    /// wakes on the listening socket closing, so this is not a spin flag.
    volatile bool running;

    /// Reads one request off `fd`, answers it, closes.
    ///
    /// One request per connection, matching the `Connection: close` the
    /// response advertises. Keep-alive would need idle-timeout handling to
    /// avoid a client holding threads open, and these endpoints are called
    /// rarely enough that it would buy nothing.
    void ServeConnection(int fd) const {
        std::string buffer;
        for (;;) {
            const FramedRequest framed = ParseRequest(buffer);
            if (framed.framing == RequestFraming::Malformed) {
                HttpResponse bad;
                bad.status = 400;
                bad.body = "{\"code\":400,\"message\":\"" + framed.error + "\"}";
                WriteAll(fd, SerializeResponse(bad));
                ::close(fd);
                return;
            }
            if (framed.framing == RequestFraming::Complete) {
                HttpResponse resp = Dispatch(framed.request);
                WriteAll(fd, SerializeResponse(resp));
                ::close(fd);
                return;
            }

            // Incomplete: wait for more. The read timeout bounds a client
            // that opens a connection and never finishes its request, which
            // would otherwise hold this thread forever.
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            const int ready = ::poll(&pfd, 1, 30 * 1000);
            if (ready <= 0) {
                ::close(fd);
                return;
            }
            char chunk[4096];
            const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
            if (got <= 0) {
                ::close(fd);
                return;
            }
            buffer.append(chunk, static_cast<std::size_t>(got));
        }
    }

    HttpResponse Dispatch(const HttpRequest& request) const {
        // Layers run before routing, matching Rust's `require_auth` wrapping
        // the whole router: an unauthenticated request to an unknown path is
        // rejected, not answered with a 404 that confirms what does not exist.
        HttpRequest current = request;
        for (std::size_t i = 0; i < layers.size(); ++i) {
            HttpResponse short_circuit;
            if (!layers[i](&current, &short_circuit)) {
                return short_circuit;
            }
        }
        return DispatchRouted(current);
    }

    HttpResponse DispatchRouted(const HttpRequest& request) const {
        bool path_matched = false;
        for (std::size_t i = 0; i < routes.size(); ++i) {
            const RouteEntry& route = routes[i];
            std::map<std::string, std::string> params;
            if (!MatchPath(route.pattern, request.path, &params)) continue;
            path_matched = true;
            if (route.method != request.method) continue;

            // Path parameters reach the handler through `query`, which is the
            // only string map on the request. Captured first so a real query
            // string of the same name cannot shadow the path segment.
            HttpRequest enriched = request;
            for (std::map<std::string, std::string>::const_iterator it = params.begin();
                 it != params.end(); ++it) {
                enriched.query[it->first] = it->second;
            }

            HttpResponse resp;
            route.handler(enriched, &resp);
            return resp;
        }

        HttpResponse resp;
        if (path_matched) {
            // 405, not 404: the resource exists and the method is what is
            // wrong. A client told 404 would conclude the sandbox is gone.
            resp.status = 405;
            resp.body = "{\"code\":405,\"message\":\"method not allowed\"}";
        } else {
            resp.status = 404;
            resp.body = "{\"code\":404,\"message\":\"no route\"}";
        }
        return resp;
    }

    static void WriteAll(int fd, const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            return;  // peer gone; nothing useful left to do
        }
    }
};

HttpServer::HttpServer() : impl_(new Impl()) {}

HttpServer::~HttpServer() {
    Stop();
    delete impl_;
}

void HttpServer::Route(const std::string& method, const std::string& path_pattern,
                       Handler h) {
    Impl::RouteEntry entry;
    entry.method = method;
    entry.pattern = path_pattern;
    entry.handler = std::move(h);
    impl_->routes.push_back(std::move(entry));
}

void HttpServer::Use(Middleware layer) {
    if (!layer) return;
    impl_->layers.push_back(std::move(layer));
}

HttpResponse HttpServer::Dispatch(const HttpRequest& request) const {
    return impl_->Dispatch(request);
}

uint16_t HttpServer::BoundPort() const { return impl_->bound_port; }

core::Expected<core::Unit, core::AnyError>
HttpServer::ListenAndServe(const std::string& bind_addr) {
    std::string host;
    uint16_t port = 0;
    const core::Expected<core::Unit, std::string> parsed =
        ParseBindAddr(bind_addr, &host, &port);
    if (!parsed.ok()) return core::make_unexpected(core::err(parsed.error()));

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return core::make_unexpected(
            core::err(std::string("socket: ") + std::strerror(errno)));
    }

    // Without SO_REUSEADDR a restart fails for the duration of TIME_WAIT on
    // the previous run's sockets, which is exactly when a restart happens.
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return core::make_unexpected(
            core::err(std::string("bind address is not an IPv4 address: ") + host));
    }

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        const std::string why = std::strerror(errno);
        ::close(fd);
        return core::make_unexpected(core::err("bind " + bind_addr + ": " + why));
    }
    if (::listen(fd, 128) != 0) {
        const std::string why = std::strerror(errno);
        ::close(fd);
        return core::make_unexpected(core::err("listen " + bind_addr + ": " + why));
    }

    // Read back the actual port, so a caller that asked for 0 can find out
    // which one it got.
    struct sockaddr_in actual;
    socklen_t actual_len = sizeof(actual);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&actual), &actual_len) == 0) {
        impl_->bound_port = ntohs(actual.sin_port);
    }

    impl_->listen_fd = fd;
    impl_->running = true;
    AGENTENV_INFO("api server listening on " << host << ":" << impl_->bound_port);

    while (impl_->running) {
        // Polled rather than blocking in accept(): `Stop` closes the listening
        // socket, and a thread parked in accept() on some platforms stays
        // parked. The timeout also bounds how long shutdown waits.
        struct pollfd pfd;
        pfd.fd = impl_->listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int ready = ::poll(&pfd, 1, 200);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        const int conn = ::accept(impl_->listen_fd, NULL, NULL);
        if (conn < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;  // listening socket closed by Stop
        }
        // Detached: the connection outlives this loop iteration and nothing
        // joins it. Each one closes its own fd and returns.
        Impl* impl = impl_;
        std::thread([impl, conn]() { impl->ServeConnection(conn); }).detach();
    }

    if (impl_->listen_fd >= 0) {
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    return core::Unit{};
}

void HttpServer::Stop() {
    impl_->running = false;
    if (impl_->listen_fd >= 0) {
        // Shut the socket down as well as closing it: a close alone does not
        // wake a poll already waiting on the fd.
        ::shutdown(impl_->listen_fd, SHUT_RDWR);
    }
}

}  // namespace api
}  // namespace agentenv
