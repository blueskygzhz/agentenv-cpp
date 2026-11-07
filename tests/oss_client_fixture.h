// SPDX-License-Identifier: MIT
// Shared harness for the OSS client and the repository assembled on it.
//
// Both test binaries drive the *real* client through an injected transport,
// which is what makes the requests it builds and the answers it reads
// observable without a server. Keeping the two stubs here rather than copying
// them is deliberate: the one thing that would quietly invalidate both suites
// is a stub that stops behaving like the transport it stands in for.
#ifndef AGENTENV_TESTS_OSS_CLIENT_FIXTURE_H_
#define AGENTENV_TESTS_OSS_CLIENT_FIXTURE_H_

#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <cstdint>
#include <cstdlib>

#include "agentenv/snapshot/repository/oss/client.h"
#include "agentenv/snapshot/repository/oss/documents.h"
#include "microtest.h"

namespace agentenv {
namespace testing {
namespace oss_fixture {

namespace shttp = agentenv::storage::overlaybd::http;

using agentenv::snapshot::repository::RepositoryResult;
using agentenv::snapshot::repository::oss::DocumentUpdatePolicy;
using agentenv::snapshot::repository::oss::OssClient;
using agentenv::snapshot::repository::oss::OssClientConfig;

/// Hands a successful response's body to the request's sink, as the real
/// transport does — and only for a 2xx, which is what lets a test assert that
/// a failed download leaves no file behind.
inline shttp::Response DeliverBody(const shttp::Request& request, shttp::Response response) {
    if (request.body_sink == 0 || !response.IsSuccess()) return response;
    if (!response.body.empty()) request.body_sink->Write(&response.body[0], response.body.size());
    // Emptied: the bytes went to the sink instead of being accumulated here,
    // so a caller that reads `body` anyway must see nothing.
    response.body.clear();
    return response;
}

/// Records every request and replays scripted responses.
struct RecordingTransport {
    std::vector<shttp::Request> requests;
    std::vector<shttp::Response> responses;
    /// Returned once `responses` runs out.
    shttp::Response fallback;
    int transport_failures = 0;

    RecordingTransport() { fallback.status = 200; }

    void Push(long status, const std::string& body = std::string(),
              const std::string& etag = std::string()) {
        shttp::Response r;
        r.status = status;
        r.body.assign(body.begin(), body.end());
        if (!etag.empty()) r.headers["ETag"] = etag;
        responses.push_back(r);
    }

    /// Adds a `Content-Length` to the last pushed response, so a truncated
    /// transfer can be distinguished from a complete short one.
    void SetContentLength(uint64_t length) {
        std::ostringstream o;
        o << length;
        responses.back().headers["Content-Length"] = o.str();
    }

    OssClient::Transport Fn() {
        return [this](const shttp::Request& request)
                   -> core::Expected<shttp::Response, std::string> {
            requests.push_back(request);
            if (transport_failures > 0) {
                --transport_failures;
                return core::make_unexpected(std::string("injected transport failure"));
            }
            if (requests.size() <= responses.size()) {
                return DeliverBody(request, responses[requests.size() - 1]);
            }
            return DeliverBody(request, fallback);
        };
    }
};

inline cfg::OssBackendConfig GoodConfig() {
    cfg::OssBackendConfig c;
    c.endpoint = "https://minio.internal:9000";
    c.bucket = "agentenv";
    c.region = core::Optional<std::string>("us-east-1");
    c.access_key_id = core::Optional<std::string>("AKID");
    c.access_key_secret = core::Optional<std::string>("SECRET");
    return c;
}

inline OssClientConfig Normalized(const cfg::OssBackendConfig& c) {
    const RepositoryResult<OssClientConfig> normalized =
        OssClientConfig::FromBackendConfig(c);
    MT_EXPECT_TRUE(normalized.ok());
    return normalized.ok() ? normalized.value() : OssClientConfig();
}

/// A client over a scripted transport.
inline OssClient* Client(RecordingTransport* transport, const cfg::OssBackendConfig& c) {
    const RepositoryResult<OssClient*> client =
        OssClient::NewWithTransport(Normalized(c), transport->Fn());
    MT_EXPECT_TRUE(client.ok());
    return client.ok() ? client.value() : 0;
}

inline std::string HeaderOf(const shttp::Request& request, const std::string& name) {
    const shttp::HeaderMap::const_iterator it = request.headers.find(name);
    return it == request.headers.end() ? std::string() : it->second;
}

/// Decodes a percent-encoded query value, which is how a prefix with `/` in
/// it arrives.
inline std::string PercentDecode(const std::string& value) {
    std::string out;
    for (std::size_t i = 0; i < value.size();) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const std::string hex = value.substr(i + 1, 2);
            out.push_back(static_cast<char>(std::strtol(hex.c_str(), 0, 16)));
            i += 3;
            continue;
        }
        out.push_back(value[i]);
        ++i;
    }
    return out;
}

/// One query parameter of a URL, decoded; empty when absent.
inline std::string QueryParam(const std::string& url, const std::string& name) {
    const std::size_t q = url.find('?');
    if (q == std::string::npos) return std::string();
    std::string rest = url.substr(q + 1);
    while (!rest.empty()) {
        const std::size_t amp = rest.find('&');
        const std::string pair = amp == std::string::npos ? rest : rest.substr(0, amp);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == name) {
            return PercentDecode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        rest = rest.substr(amp + 1);
    }
    return std::string();
}

/// An HTTP-level stub with ETag semantics: the closest thing to a MinIO
/// available here, and what lets the compare-and-swap be exercised end to end
/// over the real client rather than over a fake store.
struct StubObjectServer {
    std::map<std::string, std::string> bodies;
    std::map<std::string, std::string> etags;
    uint64_t next_version = 1;
    /// Runs before a PUT is evaluated — the seam a competing writer uses.
    std::function<void()> before_put;
    /// When set, every DELETE answers 500.
    bool fail_delete = false;
    std::vector<shttp::Request> requests;

    std::string Etag() {
        std::ostringstream o;
        o << "\"v" << next_version++ << "\"";
        return o.str();
    }

    void Seed(const std::string& key, const std::string& body) {
        bodies[key] = body;
        etags[key] = Etag();
    }

    OssClient::Transport Fn() {
        return [this](const shttp::Request& request)
                   -> core::Expected<shttp::Response, std::string> {
            requests.push_back(request);
            // The key is whatever follows the bucket in a path-style URL.
            const std::string marker = "/agentenv/";
            const std::size_t at = request.url.find(marker);
            std::string key = at == std::string::npos
                                  ? std::string()
                                  : request.url.substr(at + marker.size());
            const std::size_t q = key.find('?');
            if (q != std::string::npos) key = key.substr(0, q);

            shttp::Response r;
            if (request.method == "GET" && request.url.find("list-type=2") != std::string::npos) {
                // ListObjectsV2 over the keys this stub holds. `bodies` is a
                // `std::map`, so iteration is already in the key order the
                // protocol promises.
                const std::string prefix = QueryParam(request.url, "prefix");
                const std::string start_after = QueryParam(request.url, "start-after");
                const std::string max_keys = QueryParam(request.url, "max-keys");
                const std::size_t limit =
                    max_keys.empty() ? 1000 : static_cast<std::size_t>(std::atol(max_keys.c_str()));

                std::ostringstream xml;
                xml << "<ListBucketResult>";
                std::size_t emitted = 0;
                for (std::map<std::string, std::string>::const_iterator it = bodies.begin();
                     it != bodies.end() && emitted < limit; ++it) {
                    if (it->first.compare(0, prefix.size(), prefix) != 0) continue;
                    // Strictly after, as `start-after` means.
                    if (!start_after.empty() && it->first <= start_after) continue;
                    xml << "<Contents><Key>" << it->first << "</Key></Contents>";
                    ++emitted;
                }
                xml << "</ListBucketResult>";
                r.status = 200;
                const std::string body = xml.str();
                r.body.assign(body.begin(), body.end());
                return r;
            }
            if (request.method == "GET" || request.method == "HEAD") {
                const std::map<std::string, std::string>::const_iterator it = bodies.find(key);
                if (it == bodies.end()) {
                    r.status = 404;
                    return r;
                }
                r.status = 200;
                r.headers["ETag"] = etags[key];
                std::ostringstream length;
                length << it->second.size();
                r.headers["Content-Length"] = length.str();
                // A HEAD carries the headers and no body, which is the whole
                // reason an existence probe is cheap.
                if (request.method == "GET") r.body.assign(it->second.begin(), it->second.end());
                return DeliverBody(request, r);
            }
            if (request.method == "PUT") {
                if (before_put) {
                    const std::function<void()> hook = before_put;
                    before_put = std::function<void()>();
                    hook();
                }
                const shttp::HeaderMap::const_iterator if_match =
                    request.headers.find("If-Match");
                const shttp::HeaderMap::const_iterator if_none =
                    request.headers.find("If-None-Match");
                const bool exists = bodies.count(key) != 0;
                if (if_none != request.headers.end() && if_none->second == "*" && exists) {
                    r.status = 412;
                    return r;
                }
                if (if_match != request.headers.end() &&
                    (!exists || etags[key] != if_match->second)) {
                    r.status = 412;
                    return r;
                }
                bodies[key] = std::string(request.body.begin(), request.body.end());
                etags[key] = Etag();
                r.status = 200;
                return r;
            }
            if (request.method == "DELETE") {
                // The seam a test uses to make a *cleanup* fail, which is the
                // only way to reach the paths that must log such a failure
                // rather than return it.
                if (fail_delete) {
                    r.status = 500;
                    return r;
                }
                bodies.erase(key);
                etags.erase(key);
                r.status = 204;
                return r;
            }
            r.status = 405;
            return r;
        };
    }
};

inline DocumentUpdatePolicy InstantPolicy() {
    DocumentUpdatePolicy policy;
    policy.jitter = [](int64_t lo, int64_t) { return lo; };
    policy.sleeper = [](int64_t) {};
    return policy;
}

}  // namespace oss_fixture
}  // namespace testing
}  // namespace agentenv
#endif  // AGENTENV_TESTS_OSS_CLIENT_FIXTURE_H_
