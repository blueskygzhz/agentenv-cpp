// SPDX-License-Identifier: MIT
// Rust: src/api/proxy.rs — dispatch, header hygiene, error responses and the
// buffered HTTP forwarder.
#include "agentenv/api/proxy.h"

#include <cctype>
#include <sstream>

#include "agentenv/core/http.h"
#include "agentenv/core/logging.h"
#include "agentenv/orchestrator/service.h"

namespace agentenv {
namespace api {
namespace {

std::string AsciiLower(const std::string& value) {
    std::string out = value;
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

std::string TrimSpaces(const std::string& value) {
    const std::size_t begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos) return std::string();
    const std::size_t end = value.find_last_not_of(" \t");
    return value.substr(begin, end - begin + 1);
}

/// RFC 7230 `token` characters — what `HeaderName::from_bytes` accepts.
bool IsHeaderNameToken(const std::string& name) {
    if (name.empty()) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (std::isalnum(c)) continue;
        switch (c) {
            case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
            case '+': case '-': case '.': case '^': case '_': case '`': case '|':
            case '~':
                continue;
            default:
                return false;
        }
    }
    return true;
}

void EraseAll(HeaderMap* headers, const std::string& name) {
    headers->erase(AsciiLower(name));
}

void InsertReplacing(HeaderMap* headers, const std::string& name, const std::string& value) {
    const std::string key = AsciiLower(name);
    headers->erase(key);
    headers->insert(std::make_pair(key, value));
}

/// Rust's `HeaderValue::from_str` accepts visible ASCII, space and tab.
bool IsValidHeaderValue(const std::string& value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == '\t') continue;
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

HeaderMap ToHeaderMap(const HttpRequest& request) {
    HeaderMap out;
    for (std::unordered_map<std::string, std::string>::const_iterator it =
             request.headers.begin();
         it != request.headers.end(); ++it) {
        out.insert(std::make_pair(AsciiLower(it->first), it->second));
    }
    return out;
}

std::string RequestPath(const HttpRequest& request) {
    return request.raw_path.empty() ? request.path : request.raw_path;
}

core::Optional<std::string> HostOf(const HttpRequest& request) {
    std::unordered_map<std::string, std::string>::const_iterator it =
        request.headers.find("host");
    if (it == request.headers.end()) return core::Optional<std::string>();
    return core::Optional<std::string>(it->second);
}

HttpResponse PlainStatus(int status) {
    HttpResponse response;
    response.status = status;
    // Rust `status.into_response()` has an empty body; the server would
    // otherwise label it JSON.
    response.headers["Content-Type"] = "text/plain; charset=utf-8";
    return response;
}

/// Rust `build_upstream_uri_with_scheme`, returning the origin-form
/// path-and-query (scheme and authority are the `core::http::Target`).
core::Optional<std::string> UpstreamPathAndQuery(const std::string& proxy_path,
                                                 const HttpRequest& request) {
    std::string normalized;
    if (proxy_path.empty()) {
        normalized = "/";
    } else if (proxy_path[0] == '/') {
        normalized = proxy_path;
    } else {
        normalized = "/" + proxy_path;
    }
    if (request.has_query) normalized += "?" + request.raw_query;
    // `Uri::builder().path_and_query(..)` rejects what cannot appear in a
    // request target; refusing it here keeps the request line well-formed.
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(normalized[i]);
        if (c <= 0x20 || c == 0x7f || c == '#') return core::Optional<std::string>();
    }
    return core::Optional<std::string>(normalized);
}

class OrchestratorProxyForwarder : public ProxyForwarder {
 public:
    OrchestratorProxyForwarder(orchestrator::Orchestrator* orchestrator,
                               int64_t auto_resume_min_timeout_ms)
        : orchestrator_(orchestrator),
          auto_resume_min_timeout_ms_(auto_resume_min_timeout_ms) {}

    HttpResponse Forward(const HttpRequest& request, const std::string& forward_path,
                         ProxyRouteSource /*source*/) const override {
        const HeaderMap inbound = ToHeaderMap(request);

        // Rust `resolve_proxy_request`.
        const core::Expected<core::SandboxId, ProxyRequestError> sandbox_id =
            ParseSandboxIdHeader(inbound);
        if (!sandbox_id.ok()) return ProxyErrorResponse(sandbox_id.error());
        const core::Expected<uint16_t, ProxyRequestError> target_port =
            ParseTargetPortHeader(inbound);
        if (!target_port.ok()) return ProxyErrorResponse(target_port.error());

        if (orchestrator_ == NULL) {
            return ProxyErrorResponse(
                ProxyRequestError::Make(ProxyRequestError::Kind::InternalServerError));
        }

        bool auto_resume_attempted = false;
        orchestrator::ProxyTarget target;
        for (;;) {
            const orchestrator::OrchestratorResult<orchestrator::ProxyLookupResult> lookup =
                orchestrator_->ProxyLookupFor(sandbox_id.value());
            if (!lookup.ok()) {
                if (lookup.error().kind == orchestrator::OrchestratorErrorKind::SandboxNotFound) {
                    return SandboxNotFoundResponse();
                }
                AGENTENV_WARN("failed to resolve proxy target sandbox_id="
                              << sandbox_id.value().ToString()
                              << " error=" << lookup.error().Message());
                return ProxyErrorResponse(ProxyRequestError::Make(
                    auto_resume_attempted ? ProxyRequestError::Kind::AutoResumeFailed
                                          : ProxyRequestError::Kind::InternalServerError));
            }
            const orchestrator::ProxyLookupResult& found = lookup.value();
            if (found.kind == orchestrator::ProxyLookupKind::Ready) {
                target = found.target;
                break;
            }
            if (found.kind == orchestrator::ProxyLookupKind::NotFound) {
                return SandboxNotFoundResponse();
            }
            if (found.kind == orchestrator::ProxyLookupKind::Paused) {
                if (!found.auto_resume) {
                    return ProxyErrorResponse(
                        ProxyRequestError::Make(ProxyRequestError::Kind::SandboxUnavailable));
                }
                if (auto_resume_attempted) {
                    return ProxyErrorResponse(
                        ProxyRequestError::Make(ProxyRequestError::Kind::AutoResumeFailed));
                }
                // Rust `try_auto_resume`. The resume call is synchronous here,
                // so the 60s `PROXY_AUTO_RESUME_TIMEOUT` is the orchestrator's
                // own bound rather than a separate timer.
                const orchestrator::OrchestratorResult<orchestrator::SandboxMetadata> resumed =
                    orchestrator_->ResumeSandbox(
                        sandbox_id.value(),
                        orchestrator::NewTimeout::EnsureMinimum(auto_resume_min_timeout_ms_));
                if (!resumed.ok()) {
                    AGENTENV_WARN("sandbox auto-resume failed sandbox_id="
                                  << sandbox_id.value().ToString()
                                  << " error=" << resumed.error().Message());
                    return ProxyErrorResponse(
                        ProxyRequestError::Make(ProxyRequestError::Kind::AutoResumeFailed));
                }
                auto_resume_attempted = true;
                continue;
            }
            if (auto_resume_attempted) {
                return ProxyErrorResponse(
                    ProxyRequestError::Make(ProxyRequestError::Kind::AutoResumeFailed));
            }
            if (found.kind == orchestrator::ProxyLookupKind::Unavailable) {
                return ProxyErrorResponse(
                    ProxyRequestError::Make(ProxyRequestError::Kind::SandboxUnavailable));
            }
            return ProxyErrorResponse(
                ProxyRequestError::Make(ProxyRequestError::Kind::MissingRuntimeRoute));
        }

        const core::Optional<std::string> upstream_path =
            UpstreamPathAndQuery(forward_path, request);
        if (!upstream_path.has_value()) {
            return ProxyErrorResponse(
                ProxyRequestError::Make(ProxyRequestError::Kind::InvalidUpstreamUri));
        }

        if (IsWebsocketUpgradeRequest(inbound)) {
            HttpResponse refused = PlainStatus(501);
            refused.body = "websocket proxying is not available in this build";
            return refused;
        }

        // Rust `proxy_http_request`.
        HeaderMap outbound = inbound;
        SanitizeRequestHeaders(&outbound);
        InjectForwardedHeaders(&outbound, HostOf(request), request.method, upstream_path,
                               "http");

        core::http::Request upstream;
        upstream.method = request.method;
        upstream.path = *upstream_path;
        upstream.body = request.body;
        for (HeaderMap::const_iterator it = outbound.begin(); it != outbound.end(); ++it) {
            // The body is re-framed by the client from what was buffered.
            if (it->first == "content-length") continue;
            upstream.headers[it->first] = it->second;
        }

        core::http::Timeouts timeouts;
        timeouts.connect_ms = kProxyConnectTimeoutMs;
        timeouts.read_ms = kProxyResponseHeaderTimeoutMs;
        const core::http::HttpClient client(
            core::http::Target::Tcp(target.ip, target_port.value()), timeouts);
        const core::Expected<core::http::Response, std::string> answered =
            client.Request(upstream);
        if (!answered.ok()) {
            AGENTENV_WARN("upstream proxy request failed sandbox_id="
                          << sandbox_id.value().ToString() << " error=" << answered.error());
            return PlainStatus(502);
        }

        // Rust `map_upstream_response`.
        HeaderMap returned;
        for (core::http::Headers::const_iterator it = answered.value().headers.begin();
             it != answered.value().headers.end(); ++it) {
            returned.insert(std::make_pair(AsciiLower(it->first), it->second));
        }
        SanitizeResponseHeaders(&returned);

        HttpResponse response;
        response.status = answered.value().status;
        response.body = answered.value().body;
        for (HeaderMap::const_iterator it = returned.begin(); it != returned.end(); ++it) {
            // Framing is the server's: the body was decoded and is re-sent
            // with its own length.
            if (it->first == "content-length") continue;
            response.headers[it->first] = it->second;
        }
        if (response.headers.find("content-type") == response.headers.end()) {
            // Keep the server from labelling an untyped upstream body JSON.
            response.headers["content-type"] = "application/octet-stream";
        }
        return response;
    }

 private:
    orchestrator::Orchestrator* orchestrator_;
    int64_t auto_resume_min_timeout_ms_;
};

}  // namespace

// ---- responses --------------------------------------------------------------

HttpResponse ProxyErrorResponse(const ProxyRequestError& error) {
    HttpResponse response = PlainStatus(error.Status());
    if (error.kind == ProxyRequestError::Kind::InternalServerError) {
        return response;
    }
    response.body = error.Message();
    return response;
}

HttpResponse SandboxNotFoundResponse() {
    return ProxyErrorResponse(ProxyRequestError::Make(ProxyRequestError::Kind::SandboxNotFound));
}

// ---- header hygiene ---------------------------------------------------------

void RemoveHopByHopHeaders(HeaderMap* headers) {
    if (headers == NULL) return;

    std::vector<std::string> nominated;
    const std::pair<HeaderMap::const_iterator, HeaderMap::const_iterator> connection =
        headers->equal_range("connection");
    for (HeaderMap::const_iterator it = connection.first; it != connection.second; ++it) {
        std::stringstream parts(it->second);
        std::string item;
        while (std::getline(parts, item, ',')) {
            const std::string name = TrimSpaces(item);
            if (name.empty() || !IsHeaderNameToken(name)) continue;
            nominated.push_back(AsciiLower(name));
        }
    }

    static const char* const kHopByHop[] = {
        "connection",         "upgrade",             "te", "trailer", "transfer-encoding",
        "proxy-authenticate", "proxy-authorization",
    };
    for (std::size_t i = 0; i < sizeof(kHopByHop) / sizeof(kHopByHop[0]); ++i) {
        EraseAll(headers, kHopByHop[i]);
    }
    for (std::size_t i = 0; i < nominated.size(); ++i) {
        EraseAll(headers, nominated[i]);
    }
    EraseAll(headers, "keep-alive");
}

void SanitizeRequestHeaders(HeaderMap* headers) {
    if (headers == NULL) return;
    EraseAll(headers, kSandboxIdHeader);
    EraseAll(headers, kE2bSandboxIdHeader);
    EraseAll(headers, kTargetPortHeader);
    EraseAll(headers, kE2bTargetPortHeader);
    EraseAll(headers, kTrafficAccessTokenHeader);
    EraseAll(headers, "host");
    RemoveHopByHopHeaders(headers);
}

void SanitizeWebsocketRequestHeaders(HeaderMap* headers) {
    if (headers == NULL) return;
    SanitizeRequestHeaders(headers);
    EraseAll(headers, "sec-websocket-accept");
    EraseAll(headers, "sec-websocket-extensions");
    EraseAll(headers, "sec-websocket-key");
    EraseAll(headers, "sec-websocket-version");
}

void RemoveWebsocketHandshakeHeaders(HeaderMap* headers) {
    if (headers == NULL) return;
    for (HeaderMap::iterator it = headers->begin(); it != headers->end();) {
        if (it->first.compare(0, 13, "sec-websocket") == 0) {
            headers->erase(it++);
        } else {
            ++it;
        }
    }
}

bool HeaderContainsToken(const HeaderMap& headers, const std::string& name,
                         const std::string& token) {
    const std::string wanted = AsciiLower(token);
    const std::pair<HeaderMap::const_iterator, HeaderMap::const_iterator> range =
        headers.equal_range(AsciiLower(name));
    for (HeaderMap::const_iterator it = range.first; it != range.second; ++it) {
        std::stringstream parts(it->second);
        std::string item;
        while (std::getline(parts, item, ',')) {
            if (AsciiLower(TrimSpaces(item)) == wanted) return true;
        }
    }
    return false;
}

bool IsWebsocketUpgradeRequest(const HeaderMap& headers) {
    return HeaderContainsToken(headers, "connection", "upgrade") &&
           HeaderContainsToken(headers, "upgrade", "websocket");
}

void InjectForwardedHeaders(HeaderMap* headers, const core::Optional<std::string>& original_host,
                            const std::string& method,
                            const core::Optional<std::string>& upstream_path_and_query,
                            const std::string& forwarded_proto) {
    if (headers == NULL) return;
    if (original_host.has_value()) {
        InsertReplacing(headers, "x-forwarded-host", *original_host);
    }
    InsertReplacing(headers, "x-forwarded-proto",
                    IsValidHeaderValue(forwarded_proto) ? forwarded_proto : "http");
    InsertReplacing(headers, "x-forwarded-method",
                    IsValidHeaderValue(method) && !method.empty() ? method : "GET");
    if (upstream_path_and_query.has_value() && IsValidHeaderValue(*upstream_path_and_query)) {
        InsertReplacing(headers, "x-forwarded-uri", *upstream_path_and_query);
    }
}

void SanitizeResponseHeaders(HeaderMap* headers) {
    RemoveHopByHopHeaders(headers);
}

// ---- dispatch ---------------------------------------------------------------

std::shared_ptr<ProxyForwarder> MakeOrchestratorProxyForwarder(
    orchestrator::Orchestrator* orchestrator, int64_t auto_resume_min_timeout_ms) {
    return std::shared_ptr<ProxyForwarder>(
        new OrchestratorProxyForwarder(orchestrator, auto_resume_min_timeout_ms));
}

Middleware MakeSandboxProxyClassifier(std::vector<std::string> proxy_domains,
                                      std::shared_ptr<ProxyForwarder> forwarder) {
    return [proxy_domains, forwarder](HttpRequest* request, HttpResponse* response) -> bool {
        const std::string path = RequestPath(*request);

        // A missing forwarder must not let a proxy request through to the API.
        HttpResponse unavailable = PlainStatus(500);

        // Rust: the `/proxy` and `/proxy/{*proxy_path}` routes.
        if (HasProxyPrefix(path)) {
            *response = forwarder ? forwarder->Forward(*request, StripProxyPrefix(path),
                                                       ProxyRouteSource::Prefix)
                                  : unavailable;
            return false;
        }

        // Rust `sandbox_proxy_classifier`.
        const core::Expected<core::Optional<HostProxyRoute>, ProxyRequestError> host_route =
            ParseHostProxyRoute(HostOf(*request), proxy_domains);
        if (!host_route.ok()) {
            *response = ProxyErrorResponse(host_route.error());
            return false;
        }
        if (host_route.value().has_value()) {
            const HostProxyRoute& route = *host_route.value();
            // Overwritten, not merged: the host is authoritative, so a client
            // cannot steer the hop to another sandbox with its own headers.
            request->headers[kSandboxIdHeader] = route.sandbox_id.ToString();
            request->repeated_headers.erase(kSandboxIdHeader);
            std::ostringstream port;
            port << route.target_port;
            request->headers[kTargetPortHeader] = port.str();
            request->repeated_headers.erase(kTargetPortHeader);
            *response = forwarder ? forwarder->Forward(*request, path, ProxyRouteSource::Host)
                                  : unavailable;
            return false;
        }

        // Rust `proxy_via_fallback`: no API route matched and the request
        // carries a sandbox routing header.
        if (!request->matched_route) {
            const HeaderMap headers = ToHeaderMap(*request);
            if (HasRoutingHeader(headers)) {
                *response = forwarder
                                ? forwarder->Forward(*request, path, ProxyRouteSource::Header)
                                : unavailable;
                return false;
            }
        }
        return true;
    };
}

}  // namespace api
}  // namespace agentenv
