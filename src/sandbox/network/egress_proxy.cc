// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/egress_proxy.rs
#include "agentenv/sandbox/network/egress_proxy.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <sstream>
#include <system_error>
#include <thread>

#include "agentenv/core/logging.h"
#include "agentenv/sandbox/network.h"

namespace agentenv {
namespace sandbox {
namespace network {

namespace {

bool IsAsciiWhitespace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
bool IsAsciiControl(unsigned char c) { return c < 0x20 || c == 0x7F; }

char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
std::string ToAsciiLowercase(const std::string& s) {
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i) out[i] = LowerAscii(out[i]);
    return out;
}

bool EqIgnoreAsciiCase(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    }
    return i == a.size() && b[i] == '\0';
}

/// Rust `port.parse::<u16>().is_ok()` — all digits, non-empty, in range.
bool ParsesAsU16(const std::string& s) {
    if (s.empty() || s.size() > 5) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        value = value * 10 + static_cast<uint32_t>(s[i] - '0');
    }
    return value <= 0xFFFF;
}

// ---- a minimal HTTP request-head parser (Rust: the `httparse` crate) -------
//
// Only what `parse_http_host` needs: is the head complete, and what are the
// header name/value pairs. Rust caps at 64 headers (`[EMPTY_HEADER; 64]`) and
// reports `TooManyHeaders` as a parse error; that cap is reproduced here.

const size_t kMaxHeaders = 64;

struct HttpHeader {
    std::string name;
    std::string value;
};

enum class HeadParse {
    Complete,
    Partial,
    Malformed,
};

HeadParse ParseRequestHead(const uint8_t* buf, size_t len,
                           std::vector<HttpHeader>* headers) {
    const std::string data(reinterpret_cast<const char*>(buf), len);

    // The request line must be terminated before any header can be read.
    const size_t line_end = data.find("\r\n");
    if (line_end == std::string::npos) return HeadParse::Partial;
    // "METHOD SP TARGET SP VERSION" — three tokens minimum.
    {
        const std::string line = data.substr(0, line_end);
        const size_t first_sp = line.find(' ');
        if (first_sp == std::string::npos) return HeadParse::Malformed;
        const size_t second_sp = line.find(' ', first_sp + 1);
        if (second_sp == std::string::npos) return HeadParse::Malformed;
        if (first_sp == 0 || second_sp == first_sp + 1) return HeadParse::Malformed;
    }

    size_t pos = line_end + 2;
    while (true) {
        const size_t next = data.find("\r\n", pos);
        if (next == std::string::npos) return HeadParse::Partial;
        if (next == pos) return HeadParse::Complete;  // the blank line

        const std::string field = data.substr(pos, next - pos);
        const size_t colon = field.find(':');
        // A header line without a colon is not a header.
        if (colon == std::string::npos || colon == 0) return HeadParse::Malformed;
        // Whitespace between the name and the colon is forbidden by RFC 7230
        // and is a classic smuggling vector.
        if (IsAsciiWhitespace(static_cast<unsigned char>(field[colon - 1]))) {
            return HeadParse::Malformed;
        }

        HttpHeader header;
        header.name = field.substr(0, colon);
        size_t value_start = colon + 1;
        while (value_start < field.size() &&
               (field[value_start] == ' ' || field[value_start] == '\t')) {
            ++value_start;
        }
        header.value = field.substr(value_start);
        if (headers->size() >= kMaxHeaders) return HeadParse::Malformed;
        headers->push_back(header);

        pos = next + 2;
    }
}

}  // namespace

std::string NormalizeHost(const uint8_t* value, size_t len) {
    return NormalizeHost(std::string(reinterpret_cast<const char*>(value), len));
}

std::string NormalizeHost(const std::string& raw) {
    // Rust `value.trim()`.
    size_t begin = 0, end = raw.size();
    while (begin < end && IsAsciiWhitespace(static_cast<unsigned char>(raw[begin]))) ++begin;
    while (end > begin && IsAsciiWhitespace(static_cast<unsigned char>(raw[end - 1]))) --end;
    std::string host = raw.substr(begin, end - begin);

    // Rust `trim_end_matches('.')` — strips *every* trailing dot, so
    // "example.com..." and "example.com" normalize alike.
    while (!host.empty() && host[host.size() - 1] == '.') {
        host.erase(host.size() - 1);
    }

    // IPv4-only dataplane: a bracketed IPv6 literal fails closed.
    if (!host.empty() && host[0] == '[') return std::string();

    // Rust `rsplit_once(':')`, kept only when the tail is a valid port and the
    // head holds no ']' (which would mean an unbracketed IPv6 form).
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
        const std::string head = host.substr(0, colon);
        const std::string tail = host.substr(colon + 1);
        if (head.find(']') == std::string::npos && ParsesAsU16(tail)) {
            return ToAsciiLowercase(head);
        }
    }
    return ToAsciiLowercase(host);
}

core::Expected<core::Unit, std::string>
ParseHttpHost(const uint8_t* preface, size_t len, core::Optional<std::string>* out) {
    out->reset();

    std::vector<HttpHeader> headers;
    const HeadParse status = ParseRequestHead(preface, len, &headers);
    if (status == HeadParse::Malformed) {
        return core::make_unexpected(std::string("parse HTTP request"));
    }
    if (status == HeadParse::Partial) {
        return core::Unit();  // Rust `Status::Partial` -> Ok(None)
    }

    // Collect every Host header; the count itself is the security signal.
    std::vector<const HttpHeader*> host_headers;
    for (size_t i = 0; i < headers.size(); ++i) {
        if (EqIgnoreAsciiCase(headers[i].name, "host")) host_headers.push_back(&headers[i]);
    }

    if (host_headers.empty()) {
        return core::Unit();  // Rust `[] => Ok(None)`
    }
    if (host_headers.size() == 1) {
        const std::string& value = host_headers[0]->value;
        bool clean = true;
        for (size_t i = 0; i < value.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            if (IsAsciiControl(c) || IsAsciiWhitespace(c)) {
                clean = false;
                break;
            }
        }
        if (clean) {
            *out = NormalizeHost(value);
            return core::Unit();
        }
    }
    // Duplicate or malformed: authorising on one of several conflicting values
    // is the smuggling case. Empty string == fail closed in policy.
    *out = std::string();
    return core::Unit();
}

core::Expected<core::Unit, std::string>
ParseTlsSni(const uint8_t* preface, size_t len, core::Optional<std::string>* out) {
    out->reset();

    // Rust: not a handshake record -> Ok(None).
    if (len == 0 || preface[0] != 22) return core::Unit();

    // Accumulate handshake payloads across record boundaries so that SNI
    // extraction does not depend on how the peer framed its records.
    std::vector<uint8_t> handshake;
    size_t handshake_offset = 0;
    size_t pos = 0;

    while (true) {
        // TLSPlaintext: type(1) version(2) length(2) then payload.
        if (len - pos < 5) return core::Unit();  // Err::Incomplete -> Ok(None)
        const uint8_t record_type = preface[pos];
        const size_t record_len =
            (static_cast<size_t>(preface[pos + 3]) << 8) | preface[pos + 4];
        if (record_len == 0 || record_len > 0x4000 + 2048) {
            return core::make_unexpected(std::string("parse TLS record"));
        }
        if (len - pos < 5 + record_len) return core::Unit();  // incomplete record

        const uint8_t* data = preface + pos + 5;
        pos += 5 + record_len;

        if (record_type == 22) {
            handshake.insert(handshake.end(), data, data + record_len);

            while (true) {
                if (handshake.size() - handshake_offset < 4) break;
                // Handshake: msg_type(1) length(3).
                const size_t message_len =
                    (static_cast<size_t>(handshake[handshake_offset + 1]) << 16) |
                    (static_cast<size_t>(handshake[handshake_offset + 2]) << 8) |
                    static_cast<size_t>(handshake[handshake_offset + 3]);
                const size_t message_size = 4 + message_len;
                if (handshake.size() - handshake_offset < message_size) break;

                const uint8_t* message = &handshake[handshake_offset];
                handshake_offset += message_size;
                // 1 == ClientHello; skip anything else.
                if (message[0] != 1) continue;

                // ---- ClientHello body ----
                const uint8_t* body = message + 4;
                const size_t body_len = message_len;
                size_t p = 0;
                // client_version(2) + random(32)
                if (body_len < 34) return core::make_unexpected(std::string("parse TLS ClientHello"));
                p = 34;
                // legacy_session_id
                if (body_len < p + 1) return core::make_unexpected(std::string("parse TLS ClientHello"));
                const size_t session_id_len = body[p];
                p += 1 + session_id_len;
                // cipher_suites
                if (body_len < p + 2) return core::make_unexpected(std::string("parse TLS ClientHello"));
                const size_t cipher_len =
                    (static_cast<size_t>(body[p]) << 8) | body[p + 1];
                p += 2 + cipher_len;
                // legacy_compression_methods
                if (body_len < p + 1) return core::make_unexpected(std::string("parse TLS ClientHello"));
                const size_t comp_len = body[p];
                p += 1 + comp_len;

                // Rust: `hello.ext` absent -> Ok(Some("")). A ClientHello with
                // no extension block carries no SNI, which must fail closed
                // rather than look like a parse error.
                if (body_len < p + 2) {
                    *out = std::string();
                    return core::Unit();
                }
                const size_t ext_total =
                    (static_cast<size_t>(body[p]) << 8) | body[p + 1];
                p += 2;
                if (body_len < p + ext_total) {
                    return core::make_unexpected(std::string("parse TLS ClientHello extensions"));
                }

                const size_t ext_end = p + ext_total;
                while (p + 4 <= ext_end) {
                    const size_t ext_type =
                        (static_cast<size_t>(body[p]) << 8) | body[p + 1];
                    const size_t ext_len =
                        (static_cast<size_t>(body[p + 2]) << 8) | body[p + 3];
                    p += 4;
                    if (p + ext_len > ext_end) {
                        return core::make_unexpected(std::string("parse TLS ClientHello extensions"));
                    }
                    if (ext_type == 0) {  // server_name
                        const uint8_t* sni = body + p;
                        if (ext_len >= 5) {
                            // ServerNameList: list_len(2), then entries of
                            // name_type(1) + name_len(2) + name.
                            const size_t name_type = sni[2];
                            const size_t name_len =
                                (static_cast<size_t>(sni[3]) << 8) | sni[4];
                            // Rust takes only the first entry, and only when it
                            // is a HostName.
                            if (name_type == 0 && 5 + name_len <= ext_len) {
                                *out = ToAsciiLowercase(std::string(
                                    reinterpret_cast<const char*>(sni + 5), name_len));
                                return core::Unit();
                            }
                        }
                        *out = std::string();
                        return core::Unit();
                    }
                    p += ext_len;
                }
                // Extensions present but no SNI among them.
                *out = std::string();
                return core::Unit();
            }
        }

        if (pos >= len) return core::Unit();
    }
}

core::Expected<core::Unit, std::string>
ParseProtocolHost(const uint8_t* preface, size_t len, uint16_t original_port,
                  core::Optional<std::string>* out) {
    // Port 443 implies TLS even before any byte arrives; byte 22 catches TLS
    // on a non-standard port.
    if (original_port == 443 || (len > 0 && preface[0] == 22)) {
        return ParseTlsSni(preface, len, out);
    }
    return ParseHttpHost(preface, len, out);
}

bool ResolveTrustedUpstream(const SandboxNetworkPolicy& policy, HostNetResolver* resolver,
                            const std::string& hostname, uint16_t port,
                            const std::atomic<bool>* cancel,
                            std::vector<ResolvedAddr>* out) {
    out->clear();
    if (resolver == NULL) return false;

    std::vector<ResolvedAddr> resolved;
    // The resolver worker stays in the host namespace, so guest-controlled
    // /etc/hosts, DNS config, and routes cannot redirect this lookup.
    if (!resolver->Resolve(hostname, port, cancel, &resolved)) return false;

    for (size_t i = 0; i < resolved.size(); ++i) {
        // Re-check each resolved address against the policy: a hostname grant
        // does not authorise whatever address DNS happens to return.
        if (policy.IsDomainAllowed(hostname, core::Optional<uint32_t>(resolved[i].ipv4))) {
            out->push_back(resolved[i]);
        }
    }
    return !out->empty();
}

UpstreamDecision SelectUpstream(const SandboxNetworkPolicy& policy,
                                HostNetResolver* resolver,
                                const std::string& hostname,
                                uint32_t original_dst_ip, uint16_t original_dst_port,
                                const std::atomic<bool>* cancel) {
    // E2B treats allowOut entries additively: an explicitly allowed CIDR stays
    // valid while domain interception is on. Checking it before requiring a
    // Host/SNI is also what makes direct-IP HTTPS work.
    if (policy.IsIpAllowed(original_dst_ip)) {
        ResolvedAddr addr;
        addr.ipv4 = original_dst_ip;
        addr.port = original_dst_port;
        return UpstreamDecision::Forward(std::vector<ResolvedAddr>(1, addr));
    }

    if (policy.HasDomainAllowRules()) {
        // No hostname means nothing to authorise -> fail closed.
        if (hostname.empty()) return UpstreamDecision::Deny();
        if (!policy.IsDomainAllowed(hostname, core::Optional<uint32_t>())) {
            return UpstreamDecision::Deny();
        }
        std::vector<ResolvedAddr> addresses;
        if (!ResolveTrustedUpstream(policy, resolver, hostname, original_dst_port, cancel,
                                    &addresses)) {
            // Allowed by policy but unresolvable: a transient failure, not a
            // policy violation.
            return UpstreamDecision::Unavailable();
        }
        return UpstreamDecision::Forward(addresses);
    }

    return UpstreamDecision::Deny();
}

core::Expected<core::Unit, std::string>
OriginalDestination(int fd, uint32_t* ip_out, uint16_t* port_out) {
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    socklen_t length = sizeof(address);

    if (::getsockopt(fd, SOL_IP, kSoOriginalDst, &address, &length) != 0) {
        return core::make_unexpected(std::string(std::strerror(errno)));
    }
    if (static_cast<size_t>(length) < sizeof(struct sockaddr_in)) {
        std::ostringstream os;
        os << "SO_ORIGINAL_DST returned a short sockaddr_in (" << length << " bytes)";
        return core::make_unexpected(os.str());
    }
    if (address.sin_family != AF_INET) {
        std::ostringstream os;
        os << "SO_ORIGINAL_DST returned unexpected address family " << address.sin_family;
        return core::make_unexpected(os.str());
    }
    if (ip_out) *ip_out = ntohl(address.sin_addr.s_addr);
    if (port_out) *port_out = ntohs(address.sin_port);
    return core::Unit();
}

// ---- sockets ---------------------------------------------------------------

namespace {

/// Rust `write_all` on a socket; MSG_NOSIGNAL so a reset peer is an error,
/// not a process-wide SIGPIPE.
bool SendAll(int fd, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

/// Rust `std::io::copy` between sockets (8 KiB buffer, EINTR retried).
void CopyStream(int from, int to) {
    uint8_t buffer[8192];
    for (;;) {
        const ssize_t n = ::recv(from, buffer, sizeof(buffer), 0);
        if (n == 0) return;
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (!SendAll(to, buffer, static_cast<size_t>(n))) return;
    }
}

/// Rust `set_read_timeout(Some(d))` / `set_read_timeout(None)` (ms == 0).
bool SetReadTimeoutMs(int fd, int64_t ms) {
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

std::string Endpoint(uint32_t ip, uint16_t port) {
    std::ostringstream os;
    os << Ipv4ToString(ip) << ":" << port;
    return os.str();
}

}  // namespace

int ConnectTimeout(uint32_t ipv4, uint16_t port, int timeout_ms, std::string* error) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error) *error = std::strerror(errno);
        return -1;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        if (error) *error = std::strerror(errno);
        ::close(fd);
        return -1;
    }

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(ipv4);

    if (::connect(fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS) {
            if (error) *error = std::strerror(errno);
            ::close(fd);
            return -1;
        }
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const int64_t remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          deadline - std::chrono::steady_clock::now())
                                          .count();
            if (remaining <= 0) {
                if (error) *error = "connection timed out";
                ::close(fd);
                return -1;
            }
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            const int ready = ::poll(&pfd, 1, static_cast<int>(remaining));
            if (ready < 0) {
                if (errno == EINTR) continue;
                if (error) *error = std::strerror(errno);
                ::close(fd);
                return -1;
            }
            if (ready == 0) continue;
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) {
                so_error = errno;
            }
            if (so_error != 0) {
                if (error) *error = std::strerror(so_error);
                ::close(fd);
                return -1;
            }
            break;
        }
    }

    if (::fcntl(fd, F_SETFL, flags) != 0) {
        if (error) *error = std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

void RelayStreams(int client_fd, int upstream_fd) {
    std::thread forward;
    try {
        forward = std::thread([client_fd, upstream_fd]() {
            CopyStream(client_fd, upstream_fd);
            ::shutdown(upstream_fd, SHUT_WR);
        });
    } catch (const std::system_error&) {
        return;
    }
    CopyStream(upstream_fd, client_fd);
    // Close only the completed direction so the peer can still drain data in
    // the opposite one before the relay exits.
    ::shutdown(client_fd, SHUT_WR);
    ::shutdown(upstream_fd, SHUT_RD);
    forward.join();
}

// ---- EgressProxy -----------------------------------------------------------

/// Rust `struct ConnectionSockets` — duplicates of the handler's sockets, so
/// teardown can shut them down underneath a blocked handler.
struct EgressProxy::ConnectionSockets {
    std::mutex mu;
    int client = -1;
    int upstream = -1;
    std::shared_ptr<std::atomic<bool> > cancel;
    bool closed = false;

    ~ConnectionSockets() {
        if (client >= 0) ::close(client);
        if (upstream >= 0) ::close(upstream);
    }
};

/// Rust `JoinHandle<()>` plus `is_finished`. The flag is shared with the
/// thread separately, so the thread never holds the object that joins it.
struct EgressProxy::HandlerJoin {
    std::thread thread;
    std::shared_ptr<std::atomic<bool> > finished;

    HandlerJoin() : finished(std::make_shared<std::atomic<bool> >(false)) {}
    void Join() {
        if (thread.joinable()) thread.join();
    }
};

/// Rust `struct ConnectionState`.
struct EgressProxy::ConnectionState {
    std::map<size_t, std::shared_ptr<ConnectionSockets> > sockets;
    std::map<size_t, std::shared_ptr<HandlerJoin> > joins;
};

/// Rust `struct ListenerHandle` — the stop channel becomes a flag the accept
/// loop polls on its 5 ms cadence.
struct EgressProxy::ListenerHandle {
    std::shared_ptr<std::atomic<bool> > stop;
    std::thread join;
};

EgressProxy::EgressProxy() : next_connection_id_(0) {}

EgressProxy::~EgressProxy() {
    StopAllListeners();
    std::vector<uint32_t> hosts;
    {
        std::lock_guard<std::mutex> lg(connections_mu_);
        for (std::map<uint32_t, std::shared_ptr<ConnectionState> >::const_iterator it =
                 connections_.begin();
             it != connections_.end(); ++it) {
            hosts.push_back(it->first);
        }
    }
    for (size_t i = 0; i < hosts.size(); ++i) StopConnections(hosts[i]);
}

core::Expected<core::Unit, std::string> EgressProxy::EnsureListener(
    uint32_t host_interaction_ip, const std::string& netns_path) {
    std::lock_guard<std::mutex> startup(listener_start_mu_);
    if (HasListener(host_interaction_ip)) return core::Unit();

    const int netns = ::open(netns_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (netns < 0) {
        return core::make_unexpected("open egress proxy namespace " + netns_path + ": " +
                                     std::strerror(errno));
    }

    std::shared_ptr<std::atomic<bool> > stop = std::make_shared<std::atomic<bool> >(false);
    std::shared_ptr<std::promise<std::string> > ready =
        std::make_shared<std::promise<std::string> >();
    std::future<std::string> ready_rx = ready->get_future();

    std::shared_ptr<ListenerHandle> handle(new ListenerHandle);
    handle->stop = stop;
    try {
        handle->join = std::thread([this, netns, stop, ready, host_interaction_ip]() {
            // Per-thread: everything this thread spawns inherits the namespace.
            if (::setns(netns, CLONE_NEWNET) != 0) {
                const int err = errno;
                ::close(netns);
                ready->set_value(std::string("enter egress proxy namespace: ") +
                                 std::strerror(err));
                return;
            }
            ::close(netns);

            const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (listener < 0) {
                ready->set_value(std::string("bind egress proxy port: ") + std::strerror(errno));
                return;
            }
            // Rust `TcpListener::bind` sets SO_REUSEADDR on Unix.
            const int one = 1;
            (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            struct sockaddr_in address;
            std::memset(&address, 0, sizeof(address));
            address.sin_family = AF_INET;
            address.sin_port = htons(kEgressProxyPort);
            address.sin_addr.s_addr = htonl(INADDR_ANY);
            if (::bind(listener, reinterpret_cast<const struct sockaddr*>(&address),
                       sizeof(address)) != 0 ||
                ::listen(listener, 128) != 0) {
                const int err = errno;
                ::close(listener);
                ready->set_value(std::string("bind egress proxy port: ") + std::strerror(err));
                return;
            }
            const int flags = ::fcntl(listener, F_GETFL, 0);
            if (flags < 0 || ::fcntl(listener, F_SETFL, flags | O_NONBLOCK) != 0) {
                const int err = errno;
                ::close(listener);
                ready->set_value(std::string("set egress proxy nonblocking: ") +
                                 std::strerror(err));
                return;
            }
            ready->set_value(std::string());
            RunNamespaceListener(listener, stop, host_interaction_ip);
        });
    } catch (const std::system_error& error) {
        ::close(netns);
        return core::make_unexpected(std::string("spawn egress proxy namespace listener: ") +
                                     error.what());
    }

    const std::string started = ready_rx.get();
    if (!started.empty()) {
        handle->join.join();
        return core::make_unexpected(started);
    }
    std::lock_guard<std::mutex> lg(listeners_mu_);
    listeners_[host_interaction_ip] = handle;
    return core::Unit();
}

void EgressProxy::Prepare(uint32_t host_interaction_ip, const SandboxNetworkPolicy& policy) {
    std::lock_guard<std::mutex> lg(policy_mu_);
    pending_[host_interaction_ip] = policy;
}

void EgressProxy::Activate(uint32_t host_interaction_ip) {
    std::lock_guard<std::mutex> lg(policy_mu_);
    std::map<uint32_t, SandboxNetworkPolicy>::iterator it = pending_.find(host_interaction_ip);
    // Rust `if let Some(policy) = pending.remove(&ip)`: activating without a
    // staged policy is a no-op, not a clear of the active one.
    if (it == pending_.end()) return;
    active_[host_interaction_ip] = it->second;
    pending_.erase(it);
}

void EgressProxy::DiscardPending(uint32_t host_interaction_ip) {
    std::lock_guard<std::mutex> lg(policy_mu_);
    pending_.erase(host_interaction_ip);
}

void EgressProxy::Deactivate(uint32_t host_interaction_ip) {
    // Connections stay registered: relays already admitted keep the policy
    // snapshot they captured, and teardown still has to stop them.
    {
        std::lock_guard<std::mutex> lg(policy_mu_);
        pending_.erase(host_interaction_ip);
        active_.erase(host_interaction_ip);
    }
    StopListener(host_interaction_ip);
}

void EgressProxy::Teardown(uint32_t host_interaction_ip) {
    Deactivate(host_interaction_ip);
    StopConnections(host_interaction_ip);
}

bool EgressProxy::HasActive(uint32_t host_interaction_ip) const {
    std::lock_guard<std::mutex> lg(policy_mu_);
    return active_.find(host_interaction_ip) != active_.end();
}

bool EgressProxy::HasPending(uint32_t host_interaction_ip) const {
    std::lock_guard<std::mutex> lg(policy_mu_);
    return pending_.find(host_interaction_ip) != pending_.end();
}

bool EgressProxy::HasListener(uint32_t host_interaction_ip) const {
    std::lock_guard<std::mutex> lg(listeners_mu_);
    return listeners_.find(host_interaction_ip) != listeners_.end();
}

bool EgressProxy::ActivePolicy(uint32_t host_interaction_ip,
                               SandboxNetworkPolicy* out) const {
    std::lock_guard<std::mutex> lg(policy_mu_);
    std::map<uint32_t, SandboxNetworkPolicy>::const_iterator it =
        active_.find(host_interaction_ip);
    if (it == active_.end()) return false;
    if (out) *out = it->second;
    return true;
}

void EgressProxy::Shutdown() {
    StopAllListeners();
    std::vector<uint32_t> hosts;
    {
        std::lock_guard<std::mutex> lg(connections_mu_);
        for (std::map<uint32_t, std::shared_ptr<ConnectionState> >::const_iterator it =
                 connections_.begin();
             it != connections_.end(); ++it) {
            hosts.push_back(it->first);
        }
    }
    for (size_t i = 0; i < hosts.size(); ++i) Teardown(hosts[i]);
    resolver_.Shutdown();
}

void EgressProxy::StopAllListeners() {
    std::map<uint32_t, std::shared_ptr<ListenerHandle> > listeners;
    {
        std::lock_guard<std::mutex> lg(listeners_mu_);
        listeners.swap(listeners_);
    }
    for (std::map<uint32_t, std::shared_ptr<ListenerHandle> >::iterator it = listeners.begin();
         it != listeners.end(); ++it) {
        it->second->stop->store(true);
        if (it->second->join.joinable()) it->second->join.join();
    }
}

void EgressProxy::StopListener(uint32_t host_interaction_ip) {
    std::shared_ptr<ListenerHandle> listener;
    {
        std::lock_guard<std::mutex> lg(listeners_mu_);
        std::map<uint32_t, std::shared_ptr<ListenerHandle> >::iterator it =
            listeners_.find(host_interaction_ip);
        if (it == listeners_.end()) return;
        listener = it->second;
        listeners_.erase(it);
    }
    listener->stop->store(true);
    if (listener->join.joinable()) listener->join.join();
}

core::Expected<bool, std::string> EgressProxy::RegisterConnectionFd(
    uint32_t host_interaction_ip, int client_fd, size_t* id_out,
    std::shared_ptr<std::atomic<bool> >* cancel_out) {
    std::lock_guard<std::mutex> lg(connections_mu_);
    std::shared_ptr<ConnectionState>& state = connections_[host_interaction_ip];
    if (!state) state = std::make_shared<ConnectionState>();
    // The cap is per host-interaction IP, so one busy sandbox cannot exhaust
    // another's budget.
    if (state->sockets.size() >= kMaxConnectionsPerProxy) return false;

    int clone = -1;
    if (client_fd >= 0) {
        clone = ::fcntl(client_fd, F_DUPFD_CLOEXEC, 0);
        if (clone < 0) return core::make_unexpected(std::string(std::strerror(errno)));
    }
    const size_t id = next_connection_id_.fetch_add(1);
    std::shared_ptr<ConnectionSockets> sockets = std::make_shared<ConnectionSockets>();
    sockets->client = clone;
    sockets->cancel = std::make_shared<std::atomic<bool> >(false);
    state->sockets[id] = sockets;
    if (id_out) *id_out = id;
    if (cancel_out) *cancel_out = sockets->cancel;
    return true;
}

bool EgressProxy::RegisterConnection(uint32_t host_interaction_ip, size_t* id_out) {
    core::Expected<bool, std::string> registered =
        RegisterConnectionFd(host_interaction_ip, -1, id_out, NULL);
    return registered.ok() && registered.value();
}

std::shared_ptr<EgressProxy::HandlerJoin> EgressProxy::RegisterJoin(
    uint32_t host_interaction_ip, size_t id, const std::shared_ptr<HandlerJoin>& join) {
    std::lock_guard<std::mutex> lg(connections_mu_);
    std::map<uint32_t, std::shared_ptr<ConnectionState> >::iterator it =
        connections_.find(host_interaction_ip);
    if (it == connections_.end()) return join;
    it->second->joins[id] = join;
    return std::shared_ptr<HandlerJoin>();
}

void EgressProxy::SetUpstreamConnection(uint32_t host_interaction_ip, size_t id,
                                        int upstream_fd) {
    std::shared_ptr<ConnectionSockets> entry;
    {
        std::lock_guard<std::mutex> lg(connections_mu_);
        std::map<uint32_t, std::shared_ptr<ConnectionState> >::iterator state =
            connections_.find(host_interaction_ip);
        if (state == connections_.end()) return;
        std::map<size_t, std::shared_ptr<ConnectionSockets> >::iterator it =
            state->second->sockets.find(id);
        if (it == state->second->sockets.end()) return;
        entry = it->second;
    }
    const int clone = ::fcntl(upstream_fd, F_DUPFD_CLOEXEC, 0);
    if (clone < 0) return;
    std::lock_guard<std::mutex> lg(entry->mu);
    if (entry->closed) {
        // Teardown already ran: the relay must not start on a live upstream.
        ::shutdown(clone, SHUT_RDWR);
        ::close(clone);
    } else {
        if (entry->upstream >= 0) ::close(entry->upstream);
        entry->upstream = clone;
    }
}

void EgressProxy::UnregisterConnection(uint32_t host_interaction_ip, size_t id) {
    // Moved out so the duplicated sockets close after the lock is released.
    std::shared_ptr<ConnectionSockets> released;
    std::lock_guard<std::mutex> lg(connections_mu_);
    std::map<uint32_t, std::shared_ptr<ConnectionState> >::iterator it =
        connections_.find(host_interaction_ip);
    if (it == connections_.end()) return;
    std::map<size_t, std::shared_ptr<ConnectionSockets> >::iterator socket =
        it->second->sockets.find(id);
    if (socket == it->second->sockets.end()) return;
    released = socket->second;
    it->second->sockets.erase(socket);
}

size_t EgressProxy::ConnectionCount(uint32_t host_interaction_ip) const {
    std::lock_guard<std::mutex> lg(connections_mu_);
    std::map<uint32_t, std::shared_ptr<ConnectionState> >::const_iterator it =
        connections_.find(host_interaction_ip);
    return it == connections_.end() ? 0 : it->second->sockets.size();
}

void EgressProxy::ReapFinished(uint32_t host_interaction_ip) {
    std::vector<std::shared_ptr<HandlerJoin> > finished;
    {
        std::lock_guard<std::mutex> lg(connections_mu_);
        std::map<uint32_t, std::shared_ptr<ConnectionState> >::iterator it =
            connections_.find(host_interaction_ip);
        if (it == connections_.end()) return;
        std::map<size_t, std::shared_ptr<HandlerJoin> >& joins = it->second->joins;
        for (std::map<size_t, std::shared_ptr<HandlerJoin> >::iterator join = joins.begin();
             join != joins.end();) {
            if (join->second->finished->load()) {
                finished.push_back(join->second);
                joins.erase(join++);
            } else {
                ++join;
            }
        }
    }
    for (size_t i = 0; i < finished.size(); ++i) finished[i]->Join();
}

void EgressProxy::StopConnections(uint32_t host_interaction_ip) {
    std::shared_ptr<ConnectionState> state;
    {
        std::lock_guard<std::mutex> lg(connections_mu_);
        std::map<uint32_t, std::shared_ptr<ConnectionState> >::iterator it =
            connections_.find(host_interaction_ip);
        if (it == connections_.end()) return;
        state = it->second;
        connections_.erase(it);
    }
    for (std::map<size_t, std::shared_ptr<ConnectionSockets> >::iterator it =
             state->sockets.begin();
         it != state->sockets.end(); ++it) {
        std::lock_guard<std::mutex> lg(it->second->mu);
        it->second->closed = true;
        it->second->cancel->store(true);
        if (it->second->client >= 0) ::shutdown(it->second->client, SHUT_RDWR);
        if (it->second->upstream >= 0) ::shutdown(it->second->upstream, SHUT_RDWR);
    }
    for (std::map<size_t, std::shared_ptr<HandlerJoin> >::iterator it = state->joins.begin();
         it != state->joins.end(); ++it) {
        it->second->Join();
    }
}

void EgressProxy::RunNamespaceListener(int listener_fd,
                                       std::shared_ptr<std::atomic<bool> > stop,
                                       uint32_t host_interaction_ip) {
    for (;;) {
        ReapFinished(host_interaction_ip);
        if (stop->load()) break;

        const int stream = ::accept4(listener_fd, NULL, NULL, SOCK_CLOEXEC);
        if (stream < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } else {
                AGENTENV_WARN("egress proxy accept failed: " << std::strerror(errno));
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }

        size_t connection_id = 0;
        std::shared_ptr<std::atomic<bool> > cancel;
        core::Expected<bool, std::string> registered =
            RegisterConnectionFd(host_interaction_ip, stream, &connection_id, &cancel);
        if (!registered.ok()) {
            AGENTENV_WARN("clone egress proxy connection failed host_interaction_ip="
                          << Ipv4ToString(host_interaction_ip) << ": " << registered.error());
            ::close(stream);
            continue;
        }
        if (!registered.value()) {
            AGENTENV_WARN("egress proxy connection limit reached host_interaction_ip="
                          << Ipv4ToString(host_interaction_ip));
            ::close(stream);
            continue;
        }

        std::shared_ptr<HandlerJoin> join(new HandlerJoin);
        std::shared_ptr<std::atomic<bool> > finished = join->finished;
        try {
            join->thread = std::thread(
                [this, stream, host_interaction_ip, connection_id, cancel, finished]() {
                    HandleConnection(stream, host_interaction_ip, connection_id, cancel);
                    finished->store(true);
                });
        } catch (const std::system_error& error) {
            // Dropping the stream rejects this connection and unregistering it
            // makes the bounded handler count recover.
            UnregisterConnection(host_interaction_ip, connection_id);
            AGENTENV_WARN("spawn egress proxy connection handler failed host_interaction_ip="
                          << Ipv4ToString(host_interaction_ip) << ": " << error.what());
            ::close(stream);
            continue;
        }
        std::shared_ptr<HandlerJoin> orphan =
            RegisterJoin(host_interaction_ip, connection_id, join);
        if (orphan) orphan->Join();
    }
    ::close(listener_fd);
}

namespace {

/// Rust `struct ConnectionRegistration` — unregisters on every exit path and
/// closes the handler's own client socket.
class ConnectionRegistration {
 public:
    ConnectionRegistration(EgressProxy* proxy, uint32_t ip, size_t id, int client_fd)
        : proxy_(proxy), ip_(ip), id_(id), client_fd_(client_fd) {}
    ~ConnectionRegistration() {
        proxy_->UnregisterConnection(ip_, id_);
        ::close(client_fd_);
    }

 private:
    EgressProxy* proxy_;
    uint32_t ip_;
    size_t id_;
    int client_fd_;
};

}  // namespace

void EgressProxy::HandleConnection(int client_fd, uint32_t host_interaction_ip,
                                   size_t connection_id,
                                   std::shared_ptr<std::atomic<bool> > cancel) {
    ConnectionRegistration registration(this, host_interaction_ip, connection_id, client_fd);
    const std::string host_ip = Ipv4ToString(host_interaction_ip);

    SandboxNetworkPolicy policy;
    if (!ActivePolicy(host_interaction_ip, &policy)) return;
    if (cancel->load()) return;

    const std::chrono::steady_clock::time_point preface_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kPrefaceTimeoutMs);
    std::vector<uint8_t> preface;
    preface.reserve(4096);
    uint8_t scratch[4096];

    uint32_t original_ip = 0;
    uint16_t original_port = 0;
    core::Expected<core::Unit, std::string> destination =
        OriginalDestination(client_fd, &original_ip, &original_port);
    if (!destination.ok()) {
        AGENTENV_DEBUG("egress proxy could not read original destination host_interaction_ip="
                       << host_ip << ": " << destination.error());
        return;
    }

    // E2B keeps an explicit CIDR grant usable even when the same policy also
    // has domain rules: connect straight to the original destination. Only
    // the domain branch needs protocol inspection and trusted resolution.
    std::string hostname;
    std::vector<ResolvedAddr> upstreams;
    if (policy.IsIpAllowed(original_ip)) {
        ResolvedAddr original;
        original.ipv4 = original_ip;
        original.port = original_port;
        upstreams.push_back(original);
    } else {
        for (;;) {
            core::Optional<std::string> host;
            core::Expected<core::Unit, std::string> parsed = ParseProtocolHost(
                preface.empty() ? NULL : &preface[0], preface.size(), original_port, &host);
            if (!parsed.ok()) {
                AGENTENV_DEBUG("egress proxy could not inspect connection host_interaction_ip="
                               << host_ip << ": " << parsed.error());
                return;
            }
            if (host.has_value()) {
                hostname = *host;
                break;
            }
            if (preface.size() >= kMaxPrefaceBytes) return;

            const int64_t remaining_us =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    preface_deadline - std::chrono::steady_clock::now())
                    .count();
            if (remaining_us <= 0) return;
            // Rust rounds a sub-millisecond remainder up rather than to zero,
            // which would mean "no timeout".
            const int64_t remaining_ms = (remaining_us + 999) / 1000;
            if (!SetReadTimeoutMs(client_fd, remaining_ms)) return;
            const ssize_t read = ::recv(client_fd, scratch, sizeof(scratch), 0);
            if (read <= 0) return;
            preface.insert(preface.end(), scratch, scratch + read);
        }

        const UpstreamDecision decision = SelectUpstream(
            policy, &resolver_, hostname, original_ip, original_port, cancel.get());
        if (decision.is_deny()) {
            AGENTENV_DEBUG("egress proxy denied connection host_interaction_ip="
                           << host_ip << " hostname=" << hostname << " original="
                           << Endpoint(original_ip, original_port));
            return;
        }
        if (decision.is_unavailable()) {
            AGENTENV_DEBUG("egress proxy could not select an eligible upstream "
                           "host_interaction_ip="
                           << host_ip << " hostname=" << hostname << " original="
                           << Endpoint(original_ip, original_port));
            return;
        }
        upstreams = decision.addresses;
    }
    if (cancel->load()) return;

    int upstream = -1;
    for (size_t i = 0; i < upstreams.size(); ++i) {
        if (cancel->load()) return;
        std::string error;
        upstream = ConnectTimeout(upstreams[i].ipv4, upstreams[i].port, kUpstreamTimeoutMs,
                                  &error);
        if (upstream >= 0) break;
        AGENTENV_DEBUG("egress proxy upstream connect failed; trying next address "
                       "host_interaction_ip="
                       << host_ip << " hostname=" << hostname << " upstream="
                       << Endpoint(upstreams[i].ipv4, upstreams[i].port) << ": " << error);
    }
    if (upstream < 0) return;
    if (cancel->load()) {
        ::shutdown(upstream, SHUT_RDWR);
        ::close(upstream);
        return;
    }

    SetUpstreamConnection(host_interaction_ip, connection_id, upstream);
    (void)SetReadTimeoutMs(upstream, 0);
    (void)SetReadTimeoutMs(client_fd, 0);
    if (!preface.empty() && !SendAll(upstream, &preface[0], preface.size())) {
        AGENTENV_DEBUG("egress proxy upstream preface write failed host_interaction_ip="
                       << host_ip << " hostname=" << hostname << ": " << std::strerror(errno));
        ::close(upstream);
        return;
    }
    // Domain inspection is per TCP connection, matching E2B's initial
    // Host/SNI inspection: keep-alive requests on an authorised relay are not
    // reparsed.
    RelayStreams(client_fd, upstream);
    ::close(upstream);
}

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
