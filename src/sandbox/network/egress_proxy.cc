// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/egress_proxy.rs
#include "agentenv/sandbox/network/egress_proxy.h"

#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

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

// ---- EgressProxy policy staging ------------------------------------------

void EgressProxy::Prepare(uint32_t host_interaction_ip, const SandboxNetworkPolicy& policy) {
    pending_[host_interaction_ip] = policy;
}

void EgressProxy::Activate(uint32_t host_interaction_ip) {
    std::map<uint32_t, SandboxNetworkPolicy>::iterator it = pending_.find(host_interaction_ip);
    // Rust `if let Some(policy) = pending.remove(&ip)`: activating without a
    // staged policy is a no-op, not a clear of the active one.
    if (it == pending_.end()) return;
    active_[host_interaction_ip] = it->second;
    pending_.erase(it);
}

void EgressProxy::DiscardPending(uint32_t host_interaction_ip) {
    pending_.erase(host_interaction_ip);
}

void EgressProxy::Deactivate(uint32_t host_interaction_ip) {
    // Connections stay registered: teardown still has to stop them, and a
    // deactivated policy must not leave live sockets unaccounted for.
    active_.erase(host_interaction_ip);
    pending_.erase(host_interaction_ip);
}

void EgressProxy::Teardown(uint32_t host_interaction_ip) {
    active_.erase(host_interaction_ip);
    pending_.erase(host_interaction_ip);
    connections_.erase(host_interaction_ip);
}

bool EgressProxy::HasActive(uint32_t host_interaction_ip) const {
    return active_.find(host_interaction_ip) != active_.end();
}

bool EgressProxy::HasPending(uint32_t host_interaction_ip) const {
    return pending_.find(host_interaction_ip) != pending_.end();
}

bool EgressProxy::ActivePolicy(uint32_t host_interaction_ip,
                               SandboxNetworkPolicy* out) const {
    std::map<uint32_t, SandboxNetworkPolicy>::const_iterator it =
        active_.find(host_interaction_ip);
    if (it == active_.end()) return false;
    if (out) *out = it->second;
    return true;
}

bool EgressProxy::RegisterConnection(uint32_t host_interaction_ip, size_t* id_out) {
    std::vector<size_t>& live = connections_[host_interaction_ip];
    // The cap is per host-interaction IP, so one busy sandbox cannot exhaust
    // another's budget.
    if (live.size() >= kMaxConnectionsPerProxy) return false;
    const size_t id = next_connection_id_++;
    live.push_back(id);
    if (id_out) *id_out = id;
    return true;
}

void EgressProxy::UnregisterConnection(uint32_t host_interaction_ip, size_t id) {
    std::map<uint32_t, std::vector<size_t> >::iterator it =
        connections_.find(host_interaction_ip);
    if (it == connections_.end()) return;
    for (size_t i = 0; i < it->second.size(); ++i) {
        if (it->second[i] == id) {
            it->second.erase(it->second.begin() + static_cast<long>(i));
            break;
        }
    }
}

size_t EgressProxy::ConnectionCount(uint32_t host_interaction_ip) const {
    std::map<uint32_t, std::vector<size_t> >::const_iterator it =
        connections_.find(host_interaction_ip);
    return it == connections_.end() ? 0 : it->second.size();
}

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
