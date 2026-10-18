// SPDX-License-Identifier: MIT
// HTTP transport for the remote storage backends.
//
// Rust reaches the network through `reqwest` (registryfs) and `opendal`
// (OSS/S3). Neither has a C++ counterpart here, and `core/http.h` cannot be
// used: it speaks plain HTTP over a Unix socket or TCP with no TLS, while
// every registry and object store is HTTPS. So this is a small libcurl-backed
// transport shaped to what those two backends actually need — ranged GET,
// HEAD, PUT — rather than a general HTTP client.
#ifndef AGENTENV_STORAGE_OVERLAYBD_HTTP_TRANSPORT_H_
#define AGENTENV_STORAGE_OVERLAYBD_HTTP_TRANSPORT_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace http {

/// Header name comparison is case-insensitive, per RFC 7230: a server may
/// answer `Content-Length` or `content-length` and both must be found.
struct CaseInsensitiveLess {
    bool operator()(const std::string& a, const std::string& b) const;
};

typedef std::map<std::string, std::string, CaseInsensitiveLess> HeaderMap;

/// One HTTP exchange.
struct Response {
    long status;
    HeaderMap headers;
    std::vector<uint8_t> body;

    Response() : status(0) {}

    bool IsSuccess() const { return status >= 200 && status < 300; }

    /// `Content-Length`, or nothing when the server did not send one.
    ///
    /// Absent is distinct from zero: a chunked response has no length, and
    /// treating that as an empty object would silently truncate it.
    bool ContentLength(uint64_t* out) const;

    /// The total object size parsed out of `Content-Range`.
    ///
    /// A ranged response's `Content-Length` is the length of the *range*, so
    /// the full size can only come from the `/total` suffix here.
    bool ContentRangeTotal(uint64_t* out) const;
};

/// What a request needs beyond its URL.
struct Request {
    /// `"GET"`, `"HEAD"` or `"PUT"`.
    std::string method;
    std::string url;
    HeaderMap headers;
    /// PUT body. Empty for GET and HEAD.
    std::vector<uint8_t> body;

    /// Inclusive byte range; sent as `Range: bytes=begin-end` when
    /// `has_range` is set.
    bool has_range;
    uint64_t range_begin;
    uint64_t range_end;

    /// Seconds for the whole exchange, and for the connect phase alone.
    long timeout_secs;
    long connect_timeout_secs;

    /// Follow 3xx. On for registries, which redirect blob reads to a CDN.
    bool follow_redirects;

    Request()
        : method("GET"),
          has_range(false),
          range_begin(0),
          range_end(0),
          timeout_secs(30),
          connect_timeout_secs(10),
          follow_redirects(true) {}
};

/// Perform one request.
///
/// A non-2xx status is returned as a `Response`, not an error: the callers
/// have to distinguish 404 from 416 from 503 to decide between falling back,
/// retrying and failing, and collapsing them into one error string would
/// destroy that. Only a transport failure — no connection, TLS failure,
/// timeout — is an error here.
core::Expected<Response, std::string> Perform(const Request& request);

/// Whether this build can talk HTTP at all.
///
/// False when libcurl was not available at build time, which makes every
/// `Perform` fail. Callers that can degrade (a local-only image) check this
/// instead of discovering it per request.
bool Available();

/// Percent-encode one path segment, leaving `/` intact.
///
/// Object keys routinely contain characters that are legal in a key but not
/// in a URL path, and `/` must survive because it separates key components.
std::string EncodePath(const std::string& path);

/// Percent-encode a query-string value, encoding `/` as well.
std::string EncodeQueryValue(const std::string& value);

}  // namespace http
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_HTTP_TRANSPORT_H_
