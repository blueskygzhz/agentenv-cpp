// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/p2p/facade.rs
//
// An HTTP shim that lets the overlaybd runtime read layers over P2P without
// knowing P2P exists: overlaybd is pointed at this facade's URL prefix and
// keeps issuing ordinary ranged GETs. Each request is answered from the P2P
// catalog when the layer is available there, and transparently proxied to the
// origin registry when it is not.
//
// Porting note. Rust builds this on axum (server) and reqwest (client); this
// tree has neither. The port therefore splits the module in two:
//
//   * everything protocol-shaped — range parsing, origin URL reconstruction,
//     header policy, response framing, publish validation — is ported here as
//     plain functions and a `FacadeCore` that owns the transport and cache;
//   * the socket listener and the origin HTTP client are left behind an
//     injectable `OriginRangeFetcher` interface.
//
// That keeps every decision this module makes testable, which matters because
// most of them are security decisions (digest boundaries, publish path
// allowlisting, conditional-request rejection).
#ifndef AGENTENV_OVERLAYBD_P2P_FACADE_H_
#define AGENTENV_OVERLAYBD_P2P_FACADE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/overlaybd/p2p/artifact.h"
#include "agentenv/overlaybd/p2p/cache.h"
#include "agentenv/p2p/transport.h"

namespace agentenv {
namespace overlaybd {
namespace p2p {

/// Rust `DEFAULT_*` constants.
extern const uint64_t kDefaultLookupTimeoutMs;
extern const uint64_t kDefaultFetchRangeTimeoutMs;
extern const uint64_t kDefaultDescriptorCacheTtlMs;
extern const uint64_t kDefaultDescriptorMissCacheTtlMs;
extern const std::size_t kDefaultDescriptorCacheMaxEntries;
extern const uint64_t kDefaultShutdownTimeoutMs;

/// Rust `ADDRESS_PREFIX` / `UUID_ADDRESS_PREFIX` / `PUBLISH_LAYER_PATH`.
extern const char* const kAddressPrefix;
extern const char* const kUuidAddressPrefix;
extern const char* const kPublishLayerPath;

/// HTTP status codes this module returns. Named so the intent is legible at
/// each call site.
enum HttpStatus {
    kStatusOk = 200,
    kStatusPartialContent = 206,
    kStatusBadRequest = 400,
    kStatusForbidden = 403,
    kStatusNotFound = 404,
    kStatusPreconditionFailed = 412,
    kStatusRangeNotSatisfiable = 416,
    kStatusInternalServerError = 500,
    kStatusBadGateway = 502,
};

/// Case-insensitive header map, standing in for `axum::http::HeaderMap`.
class HeaderMap {
 public:
    /// Names are compared case-insensitively, as HTTP requires.
    void Set(const std::string& name, const std::string& value);
    core::Optional<std::string> Get(const std::string& name) const;
    bool Has(const std::string& name) const { return Get(name).has_value(); }
    /// Lowercased names, in sorted order.
    std::vector<std::string> Names() const;
    std::size_t size() const { return entries_.size(); }

 private:
    std::map<std::string, std::string> entries_;  // lowercased name -> value
};

/// Rust struct `HttpFacadeError`.
struct HttpFacadeError {
    int status = kStatusInternalServerError;
    std::string message;

    HttpFacadeError() {}
    HttpFacadeError(int status_code, const std::string& text)
        : status(status_code), message(text) {}
};

/// Rust struct `RequestRange` — an HTTP byte range with an **inclusive** end.
struct RequestRange {
    uint64_t start = 0;
    uint64_t end = 0;

    /// Rust `len` — inclusive, so `0-0` is one byte. Overflow is an error
    /// rather than a wrap, which would produce a tiny read for a huge range.
    core::Expected<std::size_t, HttpFacadeError> Len() const;

    bool operator==(const RequestRange& o) const { return start == o.start && end == o.end; }
    bool operator!=(const RequestRange& o) const { return !(*this == o); }
};

/// Rust struct `ContentRange`.
struct ContentRange {
    uint64_t start = 0;
    uint64_t end = 0;
    /// Absent when the origin sent `*`.
    core::Optional<uint64_t> total;

    bool operator==(const ContentRange& o) const;
    bool operator!=(const ContentRange& o) const { return !(*this == o); }
};

/// Rust `parse_single_range` — only a single closed `bytes=a-b` range is
/// accepted. Multi-range and open-ended forms are rejected because the facade
/// cannot frame them against a P2P byte stream.
core::Expected<RequestRange, HttpFacadeError> ParseSingleRange(const HeaderMap& headers);

/// Rust `parse_content_range`.
core::Expected<ContentRange, std::string> ParseContentRange(const std::string& raw);

/// Rust `reject_conditional_headers` — any `If-*` header is refused. The
/// facade has no validators to compare against, and answering a conditional
/// request unconditionally would be wrong rather than merely unsupported.
core::Expected<core::Unit, HttpFacadeError> RejectConditionalHeaders(const HeaderMap& headers);

/// Rust `forwarded_headers` — only `Authorization` and `User-Agent` are passed
/// through to the origin. An allowlist, so a future client header cannot leak
/// by default.
std::vector<std::pair<std::string, std::string> > ForwardedHeaders(const HeaderMap& headers);

/// Rust `reconstruct_origin_url` — recovers the origin URL that was appended
/// to the facade prefix, re-attaching the query string.
core::Expected<std::string, std::string> ReconstructOriginUrl(const std::string& uri);

/// Rust `validate_origin_url` — http/https only.
core::Expected<core::Unit, HttpFacadeError> ValidateOriginUrl(const std::string& origin_url);

/// Rust `validate_digest` — exactly `sha256:` plus 64 hex characters.
core::Expected<core::Unit, HttpFacadeError> ValidateDigest(const std::string& digest);

/// Rust `validate_publish_path`.
///
/// An empty allowlist denies everything: a facade with no configured roots
/// must not be able to publish arbitrary files.
core::Expected<core::Unit, HttpFacadeError> ValidatePublishPath(
    const std::string& canonical_path, const std::vector<std::string>& allowed_roots);

/// Rust `p2p_fetch_len` — clamps the requested length to what the layer
/// actually holds, and rejects a start past the end.
core::Expected<std::size_t, std::string> P2pFetchLen(const RequestRange& request_range,
                                                     std::size_t requested_len,
                                                     const core::Optional<uint64_t>& total_size);

/// The headers of a `206 Partial Content` response, as
/// `build_range_body_response` computes them.
struct RangeResponseHeaders {
    int status = kStatusPartialContent;
    uint64_t content_length = 0;
    /// `bytes <start>-<end>/<total|*>`.
    std::string content_range;
    std::string accept_ranges;  // always "bytes"
};

/// Rust `build_range_body_response`, minus the body.
core::Expected<RangeResponseHeaders, HttpFacadeError> BuildRangeResponseHeaders(
    const RequestRange& request_range, uint64_t response_len,
    const core::Optional<uint64_t>& total_size);

/// Rust struct `PublishLayerRequest`.
struct PublishLayerRequest {
    std::string path;
    std::string digest;
    uint64_t size = 0;
    /// May be a presigned registry URL. Used only to derive
    /// `source_url_hash`; never stored or logged raw.
    core::Optional<std::string> source_url;

    static core::Expected<PublishLayerRequest, std::string> ParseJson(const std::string& text);
    std::string ToJson() const;
};

/// Rust `canonicalize_publish_roots` — creates each root, then canonicalizes
/// it. Canonical form is required for the prefix check to be sound: a
/// symlinked or `..`-containing root would otherwise let a path escape.
core::Expected<std::vector<std::string>, std::string> CanonicalizePublishRoots(
    const std::vector<std::string>& roots);

/// Rust struct `P2pHttpFacadeConfig`.
struct FacadeConfig {
    uint64_t lookup_timeout_ms = kDefaultLookupTimeoutMs;
    uint64_t fetch_range_timeout_ms = kDefaultFetchRangeTimeoutMs;
    uint64_t descriptor_cache_ttl_ms = kDefaultDescriptorCacheTtlMs;
    uint64_t descriptor_miss_cache_ttl_ms = kDefaultDescriptorMissCacheTtlMs;
    std::size_t descriptor_cache_max_entries = kDefaultDescriptorCacheMaxEntries;
    std::vector<std::string> allowed_publish_roots;
};

/// What the origin returned for a ranged GET. Replaces Rust's
/// `OriginRangeStream`; the body is a stream so the facade never buffers a
/// whole range.
struct OriginRangeResult {
    std::shared_ptr<agentenv::p2p::P2pByteStream> body;
    uint64_t response_len = 0;
    core::Optional<uint64_t> total_size;
};

/// The origin HTTP client, injected. Rust uses `reqwest::Client` directly.
class OriginRangeFetcher {
 public:
    virtual ~OriginRangeFetcher() {}

    /// Issues `GET origin_url` with `Range: bytes=<start>-<end>` plus
    /// `forwarded`. Implementations must surface the raw status so the caller
    /// can apply the checks in `ValidateOriginRangeResponse`.
    virtual core::Expected<OriginRangeResult, std::string> FetchRange(
        const std::string& origin_url, const RequestRange& range,
        const std::vector<std::pair<std::string, std::string> >& forwarded) = 0;
};

/// Rust's post-response checks inside `fetch_origin_range_stream`, split out so
/// they can be tested without a server.
///
/// These exist because an origin that ignores `Range` (200) or returns a
/// different window than asked would otherwise be framed as if it had complied,
/// silently corrupting the layer the runtime reads.
core::Expected<uint64_t, std::string> ValidateOriginRangeResponse(
    int status, const HeaderMap& response_headers, const RequestRange& request_range,
    ContentRange* parsed_out);

/// The facade's request-handling core: owns the transport and descriptor cache
/// and implements the three endpoints' logic.
class FacadeCore {
 public:
    FacadeCore(std::shared_ptr<agentenv::p2p::P2pTransport> transport, const FacadeConfig& config,
               std::shared_ptr<OriginRangeFetcher> origin_fetcher);

    /// Rust `lookup_descriptor`.
    ///
    /// A lookup *failure* is cached as a miss, but a lookup *timeout* is not:
    /// a slow peer should be retried on the next read rather than written off
    /// for the whole miss TTL.
    core::Optional<agentenv::p2p::P2pArtifactDescriptor> LookupDescriptor(
        const agentenv::p2p::P2pArtifactKey& artifact_key,
        const CanonicalBlobIdentity& canonical);

    /// Rust `fetch_p2p_range` — validates metadata, clamps the length, then
    /// opens the byte stream.
    core::Expected<OriginRangeResult, std::string> FetchP2pRange(
        const agentenv::p2p::P2pArtifactDescriptor& descriptor,
        const CanonicalBlobIdentity& canonical, const RequestRange& request_range,
        std::size_t len);

    /// Rust `handle_http_facade_result` — P2P first, origin as fallback.
    core::Expected<OriginRangeResult, HttpFacadeError> HandleOriginRequest(
        const std::string& uri, const HeaderMap& headers);

    /// Rust `handle_uuid_facade_result` — P2P only. A UUID-addressed layer has
    /// no origin to fall back to, so a miss is a 404.
    core::Expected<OriginRangeResult, HttpFacadeError> HandleUuidRequest(
        const std::string& uuid, const HeaderMap& headers);

    /// Rust `handle_publish_layer_result`.
    ///
    /// `is_loopback` is supplied by the caller because the localhost check
    /// belongs to the connection, not the payload. Note that localhost is a
    /// process boundary, not a trust boundary: the path allowlist and the
    /// re-hash below are the actual gates.
    core::Expected<core::Unit, HttpFacadeError> HandlePublishLayer(
        bool is_loopback, const PublishLayerRequest& request);

    DescriptorCache& descriptor_cache() { return descriptor_cache_; }
    const std::vector<std::string>& allowed_publish_roots() const {
        return allowed_publish_roots_;
    }
    /// Replaces the roots with an already-canonicalized list.
    void SetAllowedPublishRoots(const std::vector<std::string>& roots) {
        allowed_publish_roots_ = roots;
    }

 private:
    std::shared_ptr<agentenv::p2p::P2pTransport> transport_;
    std::shared_ptr<OriginRangeFetcher> origin_fetcher_;
    DescriptorCache descriptor_cache_;
    uint64_t lookup_timeout_ms_;
    uint64_t fetch_range_timeout_ms_;
    std::vector<std::string> allowed_publish_roots_;
};

/// Rust struct `P2pHttpFacadeHandle` — the published addresses of a running
/// facade.
struct FacadeAddresses {
    std::string address;
    std::string uuid_address;
    std::string publish_address;

    /// Builds the three from a bound `host:port`.
    static FacadeAddresses ForAuthority(const std::string& authority);
};

}  // namespace p2p
}  // namespace overlaybd
}  // namespace agentenv
#endif  // AGENTENV_OVERLAYBD_P2P_FACADE_H_
