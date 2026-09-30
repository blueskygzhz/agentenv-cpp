// SPDX-License-Identifier: MIT
// Rust: src/api/proxy.rs — request routing.
#include "agentenv/api/proxy_route.h"

#include <cctype>
#include <cstdlib>

#include "agentenv/sandbox/backend.h"

namespace agentenv {
namespace api {

const char* const kProxyRoute              = "/proxy";
const char* const kSandboxIdHeader         = "x-agentenv-sandbox-id";
const char* const kE2bSandboxIdHeader      = "e2b-sandbox-id";
const char* const kTargetPortHeader        = "x-agentenv-target-port";
const char* const kE2bTargetPortHeader     = "e2b-sandbox-port";
const char* const kApiKeyHeader            = "x-api-key";
const char* const kTrafficAccessTokenHeader = "e2b-traffic-access-token";
const char* const kEnvdAccessTokenHeader   = "x-access-token";

// ---- errors -----------------------------------------------------------------

ProxyRequestError ProxyRequestError::Make(Kind kind) {
    ProxyRequestError error;
    error.kind = kind;
    return error;
}

ProxyRequestError ProxyRequestError::InvalidHostRoute(const std::string& detail) {
    ProxyRequestError error;
    error.kind   = Kind::InvalidHostRoute;
    error.detail = detail;
    return error;
}

int ProxyRequestError::Status() const {
    switch (kind) {
        case Kind::MissingSandboxId:
        case Kind::InvalidSandboxId:
        case Kind::MissingTargetPort:
        case Kind::InvalidTargetPort:
        case Kind::InvalidHostRoute:
        case Kind::InvalidUpstreamUri:
            return 400;
        case Kind::SandboxNotFound:
            return 404;
        // Gone, not NotFound: the sandbox exists but cannot be proxied in its
        // current state, which is a different thing for a client to retry.
        case Kind::SandboxUnavailable:
            return 410;
        case Kind::AutoResumeFailed:
        case Kind::MissingRuntimeRoute:
            return 502;
        case Kind::AutoResumeTimedOut:
            return 504;
        case Kind::InternalServerError:
            return 500;
    }
    return 500;
}

std::string ProxyRequestError::Message() const {
    switch (kind) {
        case Kind::MissingSandboxId:   return "missing sandbox routing header";
        case Kind::InvalidSandboxId:   return "invalid sandbox routing header";
        case Kind::MissingTargetPort:  return "missing target port routing header";
        case Kind::InvalidTargetPort:  return "invalid target port routing header";
        case Kind::InvalidHostRoute:   return detail;
        case Kind::SandboxNotFound:    return "sandbox not found";
        case Kind::SandboxUnavailable: return "sandbox is not proxyable in its current state";
        case Kind::AutoResumeFailed:   return "sandbox auto-resume failed";
        case Kind::AutoResumeTimedOut: return "sandbox auto-resume timed out";
        case Kind::MissingRuntimeRoute: return "sandbox route is temporarily unavailable";
        case Kind::InvalidUpstreamUri: return "failed to construct upstream URI";
        // Rust returns a bare status with no body for this one.
        case Kind::InternalServerError: return std::string();
    }
    return std::string();
}

// ---- header helpers ---------------------------------------------------------

namespace {

std::string ToLower(const std::string& value) {
    std::string lowered = value;
    for (std::size_t i = 0; i < lowered.size(); ++i) {
        lowered[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(lowered[i])));
    }
    return lowered;
}

std::string Trim(const std::string& value) {
    const std::size_t begin = value.find_first_not_of(" \t\n\r");
    if (begin == std::string::npos) return std::string();
    const std::size_t end = value.find_last_not_of(" \t\n\r");
    return value.substr(begin, end - begin + 1);
}

bool AllAsciiDigits(const std::string& value) {
    if (value.empty()) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

/// Parses a port, rejecting 0 and anything out of range. Rust's
/// `parse::<u16>().filter(|p| *p > 0)`.
core::Optional<uint16_t> ParsePort(const std::string& raw) {
    if (!AllAsciiDigits(raw)) return core::Optional<uint16_t>();
    const unsigned long value = std::strtoul(raw.c_str(), NULL, 10);
    if (value == 0 || value > 65535) return core::Optional<uint16_t>();
    return core::Optional<uint16_t>(static_cast<uint16_t>(value));
}

}  // namespace

core::Optional<std::string> SingleHeader(const HeaderMap& headers,
                                         const std::string& name) {
    const std::string key = ToLower(name);
    const std::pair<HeaderMap::const_iterator, HeaderMap::const_iterator> range =
        headers.equal_range(key);
    if (range.first == range.second) return core::Optional<std::string>();

    HeaderMap::const_iterator first = range.first;
    HeaderMap::const_iterator second = first;
    ++second;
    // Rust requires exactly one value: a repeated credential header is
    // ambiguous, so it is treated as absent rather than resolved arbitrarily.
    if (second != range.second) return core::Optional<std::string>();
    return core::Optional<std::string>(first->second);
}

core::Optional<std::string> FirstHeaderValue(const HeaderMap& headers,
                                             const std::vector<std::string>& names) {
    for (std::size_t i = 0; i < names.size(); ++i) {
        HeaderMap::const_iterator it = headers.find(ToLower(names[i]));
        if (it != headers.end()) return core::Optional<std::string>(it->second);
    }
    return core::Optional<std::string>();
}

std::string StripHostPort(const std::string& host) {
    const std::size_t colon = host.rfind(':');
    if (colon == std::string::npos) return host;

    const std::string without_port = host.substr(0, colon);
    const std::string port         = host.substr(colon + 1);
    // A second colon means an IPv6 literal, whose trailing group is not a
    // port; leaving it intact is what Rust does.
    if (!without_port.empty() && without_port.find(':') == std::string::npos &&
        AllAsciiDigits(port)) {
        return without_port;
    }
    return host;
}

bool HasProxyPrefix(const std::string& path) {
    return path == kProxyRoute || path.compare(0, 7, "/proxy/") == 0;
}

bool HasRoutingHeader(const HeaderMap& headers) {
    return headers.find(kSandboxIdHeader) != headers.end() ||
           headers.find(kE2bSandboxIdHeader) != headers.end();
}

// ---- host route -------------------------------------------------------------

core::Expected<core::Optional<HostProxyRoute>, ProxyRequestError>
ParseHostProxyRoute(const core::Optional<std::string>& raw_host,
                    const std::vector<std::string>& domains) {
    if (domains.empty()) return core::Optional<HostProxyRoute>();

    std::string host;
    if (raw_host.has_value()) {
        host = ToLower(StripHostPort(Trim(*raw_host)));
        // A fully-qualified host may carry a trailing root label.
        while (!host.empty() && host[host.size() - 1] == '.') {
            host.erase(host.size() - 1);
        }
    }
    if (host.empty()) return core::Optional<HostProxyRoute>();

    for (std::size_t i = 0; i < domains.size(); ++i) {
        const std::string& domain = domains[i];
        if (domain.empty()) continue;

        // The bare proxy domain addresses the API itself, not a sandbox.
        if (host == domain) return core::Optional<HostProxyRoute>();

        if (host.size() <= domain.size()) continue;
        if (host.compare(host.size() - domain.size(), domain.size(), domain) != 0) {
            continue;
        }
        std::string prefix = host.substr(0, host.size() - domain.size());
        if (prefix.empty() || prefix[prefix.size() - 1] != '.') continue;
        prefix.erase(prefix.size() - 1);

        // Exactly one label: `a.b.<domain>` is not a sandbox host.
        if (prefix.empty() || prefix.find('.') != std::string::npos) continue;

        const std::size_t dash = prefix.find('-');
        if (dash == std::string::npos) continue;
        const std::string port_text  = prefix.substr(0, dash);
        const std::string sandbox_id = prefix.substr(dash + 1);

        // Past this point the host clearly meant to address a sandbox, so a
        // malformed one is an error rather than a fallthrough to the API.
        if (sandbox_id.empty()) {
            return core::make_unexpected(ProxyRequestError::InvalidHostRoute(
                "invalid sandbox data-plane host: sandbox id is empty"));
        }
        core::Optional<uint16_t> port = ParsePort(port_text);
        if (!port.has_value()) {
            return core::make_unexpected(ProxyRequestError::InvalidHostRoute(
                "invalid sandbox data-plane host: port is invalid"));
        }
        core::Expected<core::SandboxId, std::string> parsed_id =
            core::SandboxId::Parse(sandbox_id);
        if (!parsed_id.ok()) {
            return core::make_unexpected(ProxyRequestError::InvalidHostRoute(
                "invalid sandbox data-plane host: sandbox id is invalid"));
        }

        HostProxyRoute route;
        route.sandbox_id  = parsed_id.value();
        route.target_port = *port;
        return core::Optional<HostProxyRoute>(route);
    }

    return core::Optional<HostProxyRoute>();
}

// ---- header route -----------------------------------------------------------

core::Expected<core::SandboxId, ProxyRequestError>
ParseSandboxIdHeader(const HeaderMap& headers) {
    std::vector<std::string> names;
    names.push_back(kSandboxIdHeader);
    names.push_back(kE2bSandboxIdHeader);

    core::Optional<std::string> raw = FirstHeaderValue(headers, names);
    if (!raw.has_value()) {
        return core::make_unexpected(
            ProxyRequestError::Make(ProxyRequestError::Kind::MissingSandboxId));
    }
    core::Expected<core::SandboxId, std::string> parsed = core::SandboxId::Parse(*raw);
    if (!parsed.ok()) {
        return core::make_unexpected(
            ProxyRequestError::Make(ProxyRequestError::Kind::InvalidSandboxId));
    }
    return parsed.value();
}

core::Expected<uint16_t, ProxyRequestError>
ParseTargetPortHeader(const HeaderMap& headers) {
    std::vector<std::string> names;
    names.push_back(kTargetPortHeader);
    names.push_back(kE2bTargetPortHeader);

    core::Optional<std::string> raw = FirstHeaderValue(headers, names);
    if (!raw.has_value()) {
        return core::make_unexpected(
            ProxyRequestError::Make(ProxyRequestError::Kind::MissingTargetPort));
    }
    core::Optional<uint16_t> port = ParsePort(*raw);
    if (!port.has_value()) {
        return core::make_unexpected(
            ProxyRequestError::Make(ProxyRequestError::Kind::InvalidTargetPort));
    }
    return *port;
}

// ---- combined routing -------------------------------------------------------

namespace {

core::Optional<std::string> RequestHost(const HeaderMap& headers) {
    HeaderMap::const_iterator it = headers.find("host");
    if (it != headers.end()) return core::Optional<std::string>(it->second);
    return core::Optional<std::string>();
}

}  // namespace

core::Optional<HostProxyRoute>
RouteForAuth(const std::string& path, const HeaderMap& headers,
             const std::vector<std::string>& domains) {
    if (!HasProxyPrefix(path)) {
        core::Expected<core::Optional<HostProxyRoute>, ProxyRequestError> host_route =
            ParseHostProxyRoute(RequestHost(headers), domains);
        if (!host_route.ok()) {
            // A malformed sandbox host cannot be attributed to a sandbox, so
            // authorization has nothing to check against.
            return core::Optional<HostProxyRoute>();
        }
        if (host_route.value().has_value()) return host_route.value();
    }

    core::Expected<core::SandboxId, ProxyRequestError> sandbox_id =
        ParseSandboxIdHeader(headers);
    if (!sandbox_id.ok()) return core::Optional<HostProxyRoute>();
    core::Expected<uint16_t, ProxyRequestError> port = ParseTargetPortHeader(headers);
    if (!port.ok()) return core::Optional<HostProxyRoute>();

    HostProxyRoute route;
    route.sandbox_id  = sandbox_id.value();
    route.target_port = port.value();
    return core::Optional<HostProxyRoute>(route);
}

bool IsSandboxProxyRequest(const std::string& path, const HeaderMap& headers,
                           const std::vector<std::string>& domains,
                           bool matched_api_route) {
    if (HasProxyPrefix(path)) return true;

    core::Expected<core::Optional<HostProxyRoute>, ProxyRequestError> host_route =
        ParseHostProxyRoute(RequestHost(headers), domains);
    // A malformed sandbox host still *is* a proxy request — it just fails
    // later with a routing error instead of being handled as an API call.
    if (!host_route.ok()) return true;
    if (host_route.value().has_value()) return true;

    // Bare routing headers only make it a proxy request when the router did
    // not already match an API route.
    return !matched_api_route && HasRoutingHeader(headers);
}

uint16_t EffectiveEnvdPort(const orchestrator::SandboxMetadata& metadata,
                           uint16_t configured_control_plane_port) {
    if (metadata.paused_state) {
        core::Optional<uint16_t> captured = metadata.paused_state->ControlPlanePort();
        // A paused sandbox keeps the port it was captured with: the guest is
        // listening there, and the configured default may since have changed.
        if (captured.has_value()) return *captured;
    }
    return configured_control_plane_port;
}

}  // namespace api
}  // namespace agentenv
