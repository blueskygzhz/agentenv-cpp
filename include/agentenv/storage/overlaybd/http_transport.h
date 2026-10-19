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
    /// Empty when the request supplied a `body_sink`: the bytes went there
    /// instead of being accumulated here.
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

/// Where a response body goes as it arrives, instead of into `Response.body`.
///
/// Rust's readers stream: `read_range_into` consumes `resp.chunk()` in a loop
/// and writes straight into the caller's buffer, so a 4 MiB range costs one
/// chunk of memory rather than 4 MiB. Accumulating the whole body first and
/// copying afterwards would double the peak and differ from Rust on exactly
/// the reads that matter most.
class BodySink {
 public:
    virtual ~BodySink() {}

    /// One chunk, in order. Return false to abort the transfer — used to
    /// refuse a response longer than was asked for, rather than discovering
    /// it after the whole thing arrived.
    virtual bool Write(const uint8_t* data, size_t len) = 0;
};

/// A `BodySink` filling a fixed caller-owned buffer.
///
/// Refuses a surplus rather than truncating it: more bytes than requested
/// means the server ignored the range, so the extra is a different region of
/// the object, not the tail of the window.
class BufferSink : public BodySink {
 public:
    BufferSink(uint8_t* dst, size_t cap) : dst_(dst), cap_(cap), filled_(0), overflow_(false) {}

    bool Write(const uint8_t* data, size_t len);

    size_t filled() const { return filled_; }
    /// True when the server sent more than `cap`.
    bool overflow() const { return overflow_; }

 private:
    uint8_t* dst_;
    size_t cap_;
    size_t filled_;
    bool overflow_;
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

    /// Rust `Client::builder().identity(...)` — a client certificate for
    /// mutual TLS. Both empty disables it.
    ///
    /// Carried per request rather than per transport because this layer has no
    /// persistent client object; the pair is applied together, since a cert
    /// without its key cannot complete a handshake.
    std::string client_cert_file;
    std::string client_key_file;

    /// Where the body goes. Null accumulates it into `Response.body`.
    ///
    /// Not owned; it has to outlive the call. Only consulted for a 2xx: an
    /// error body is small and belongs in `Response.body` where the caller
    /// can read the message, not in a sink sized for payload.
    BodySink* body_sink;

    Request()
        : method("GET"),
          has_range(false),
          range_begin(0),
          range_end(0),
          timeout_secs(30),
          connect_timeout_secs(10),
          follow_redirects(true),
          body_sink(0) {}
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
