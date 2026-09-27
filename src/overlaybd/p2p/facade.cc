// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/p2p/facade.rs
#include "agentenv/overlaybd/p2p/facade.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

#include <limits.h>
#include <stdlib.h>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/url.h"

namespace agentenv {
namespace overlaybd {
namespace p2p {

const uint64_t kDefaultLookupTimeoutMs = 300;
const uint64_t kDefaultFetchRangeTimeoutMs = 2000;
const uint64_t kDefaultDescriptorCacheTtlMs = 5 * 60 * 1000;
const uint64_t kDefaultDescriptorMissCacheTtlMs = 5 * 1000;
const std::size_t kDefaultDescriptorCacheMaxEntries = 16 * 1024;
const uint64_t kDefaultShutdownTimeoutMs = 5 * 1000;

const char* const kAddressPrefix = "/p2p-http";
const char* const kUuidAddressPrefix = "/p2p-uuid";
const char* const kPublishLayerPath = "/p2p-control/publish-layer";

namespace {

using core::Optional;
using core::Unit;
namespace fs = core::fs;

std::string ToLower(const std::string& value) {
    std::string out = value;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
    return out;
}

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// Strict unsigned parse: no sign, no whitespace, no overflow wrap.
bool ParseU64(const std::string& text, uint64_t* out) {
    if (text.empty()) return false;
    uint64_t value = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

/// True when `path` is inside `root`, comparing whole path components.
///
/// A plain string prefix test would accept `/data/storeX` for root
/// `/data/store`, so the character after the root must be a separator.
bool PathStartsWith(const std::string& path, const std::string& root) {
    if (root.empty()) return false;
    if (path.size() < root.size()) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    if (path.size() == root.size()) return true;
    // `root` may already end in '/' (e.g. "/"), in which case the boundary is
    // implicit.
    if (root[root.size() - 1] == '/') return true;
    return path[root.size()] == '/';
}

}  // namespace

// ---------------------------------------------------------------------------
// HeaderMap
// ---------------------------------------------------------------------------

void HeaderMap::Set(const std::string& name, const std::string& value) {
    entries_[ToLower(name)] = value;
}

Optional<std::string> HeaderMap::Get(const std::string& name) const {
    const std::map<std::string, std::string>::const_iterator found = entries_.find(ToLower(name));
    if (found == entries_.end()) return Optional<std::string>();
    return Optional<std::string>(found->second);
}

std::vector<std::string> HeaderMap::Names() const {
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (std::map<std::string, std::string>::const_iterator it = entries_.begin();
         it != entries_.end(); ++it) {
        names.push_back(it->first);
    }
    return names;
}

// ---------------------------------------------------------------------------
// Ranges
// ---------------------------------------------------------------------------

core::Expected<std::size_t, HttpFacadeError> RequestRange::Len() const {
    if (end < start) {
        return core::make_unexpected(
            HttpFacadeError(kStatusRangeNotSatisfiable, "range is too large"));
    }
    const uint64_t span = end - start;
    if (span == UINT64_MAX) {
        return core::make_unexpected(
            HttpFacadeError(kStatusRangeNotSatisfiable, "range is too large"));
    }
    const uint64_t len = span + 1;
    // On a 32-bit target this is a real narrowing; rejecting keeps the
    // subsequent read length honest.
    if (len > static_cast<uint64_t>(static_cast<std::size_t>(-1))) {
        return core::make_unexpected(
            HttpFacadeError(kStatusRangeNotSatisfiable, "range is too large"));
    }
    return static_cast<std::size_t>(len);
}

bool ContentRange::operator==(const ContentRange& o) const {
    if (start != o.start || end != o.end) return false;
    if (total.has_value() != o.total.has_value()) return false;
    return !total.has_value() || *total == *o.total;
}

core::Expected<RequestRange, HttpFacadeError> ParseSingleRange(const HeaderMap& headers) {
    const Optional<std::string> raw_header = headers.Get("range");
    if (!raw_header.has_value()) {
        return core::make_unexpected(HttpFacadeError(kStatusBadRequest, "missing Range header"));
    }
    const std::string raw = Trim(*raw_header);

    // Multi-range would need a multipart body the facade cannot produce from a
    // single P2P byte stream.
    if (raw.find(',') != std::string::npos) {
        return core::make_unexpected(
            HttpFacadeError(kStatusRangeNotSatisfiable, "multiple ranges are not supported"));
    }
    if (!StartsWith(raw, "bytes=")) {
        return core::make_unexpected(
            HttpFacadeError(kStatusRangeNotSatisfiable, "unsupported Range unit"));
    }
    const std::string spec = raw.substr(6);

    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos) {
        return core::make_unexpected(
            HttpFacadeError(kStatusRangeNotSatisfiable, "invalid Range header"));
    }
    const std::string start_text = spec.substr(0, dash);
    const std::string end_text = spec.substr(dash + 1);

    // Open-ended (`a-` / `-n`) forms are refused: the facade often does not
    // know the total size, so it could not compute the closing offset.
    if (start_text.empty() || end_text.empty()) {
        return core::make_unexpected(
            HttpFacadeError(kStatusRangeNotSatisfiable, "open-ended ranges are not supported"));
    }

    RequestRange range;
    if (!ParseU64(start_text, &range.start)) {
        return core::make_unexpected(
            HttpFacadeError(kStatusRangeNotSatisfiable, "invalid range start"));
    }
    if (!ParseU64(end_text, &range.end)) {
        return core::make_unexpected(
            HttpFacadeError(kStatusRangeNotSatisfiable, "invalid range end"));
    }
    if (range.end < range.start) {
        return core::make_unexpected(
            HttpFacadeError(kStatusRangeNotSatisfiable, "range end is before range start"));
    }
    return range;
}

core::Expected<ContentRange, std::string> ParseContentRange(const std::string& raw) {
    const std::string trimmed = Trim(raw);
    if (!StartsWith(trimmed, "bytes ")) {
        return core::make_unexpected(std::string("Content-Range must use bytes unit"));
    }
    const std::string spec = trimmed.substr(6);

    const std::size_t slash = spec.find('/');
    if (slash == std::string::npos) {
        return core::make_unexpected(std::string("Content-Range missing total separator"));
    }
    const std::string range_text = spec.substr(0, slash);
    const std::string total_text = spec.substr(slash + 1);

    const std::size_t dash = range_text.find('-');
    if (dash == std::string::npos) {
        return core::make_unexpected(std::string("Content-Range missing range separator"));
    }

    ContentRange range;
    if (!ParseU64(range_text.substr(0, dash), &range.start)) {
        return core::make_unexpected(std::string("invalid Content-Range start"));
    }
    if (!ParseU64(range_text.substr(dash + 1), &range.end)) {
        return core::make_unexpected(std::string("invalid Content-Range end"));
    }
    if (range.end < range.start) {
        return core::make_unexpected(std::string("Content-Range end before start"));
    }

    if (total_text != "*") {
        uint64_t total = 0;
        if (!ParseU64(total_text, &total)) {
            return core::make_unexpected(std::string("invalid Content-Range total"));
        }
        range.total = total;
    }
    return range;
}

// ---------------------------------------------------------------------------
// Header policy
// ---------------------------------------------------------------------------

core::Expected<Unit, HttpFacadeError> RejectConditionalHeaders(const HeaderMap& headers) {
    const std::vector<std::string> names = headers.Names();
    for (std::size_t i = 0; i < names.size(); ++i) {
        // Names() already lowercases, so a simple prefix test is enough.
        if (StartsWith(names[i], "if-")) {
            return core::make_unexpected(HttpFacadeError(
                kStatusPreconditionFailed, "conditional requests are not supported"));
        }
    }
    return Unit();
}

std::vector<std::pair<std::string, std::string> > ForwardedHeaders(const HeaderMap& headers) {
    // Allowlist, in Rust's order.
    const char* const kForwarded[] = {"authorization", "user-agent"};
    std::vector<std::pair<std::string, std::string> > out;
    for (std::size_t i = 0; i < 2; ++i) {
        const Optional<std::string> value = headers.Get(kForwarded[i]);
        if (value.has_value()) out.push_back(std::make_pair(std::string(kForwarded[i]), *value));
    }
    return out;
}

core::Expected<std::string, std::string> ReconstructOriginUrl(const std::string& uri) {
    if (uri.empty()) return core::make_unexpected(std::string("request URI missing path"));

    std::string path = uri;
    Optional<std::string> query;
    const std::size_t question = uri.find('?');
    if (question != std::string::npos) {
        path = uri.substr(0, question);
        query = uri.substr(question + 1);
    }

    const std::string prefix = std::string(kAddressPrefix) + "/";
    if (!StartsWith(path, prefix)) {
        return core::make_unexpected(std::string("request path must start with ") +
                                     kAddressPrefix + "/");
    }
    const std::string origin = path.substr(prefix.size());
    if (origin.empty()) return core::make_unexpected(std::string("missing origin url"));

    // The query is re-attached because a presigned origin URL needs its
    // signature to be usable; only the *canonical key* drops it.
    if (query.has_value()) return origin + "?" + *query;
    return origin;
}

core::Expected<Unit, HttpFacadeError> ValidateOriginUrl(const std::string& origin_url) {
    const core::Expected<core::Url, std::string> parsed = core::Url::Parse(origin_url);
    if (!parsed.ok()) {
        return core::make_unexpected(
            HttpFacadeError(kStatusBadRequest, "invalid origin url: " + parsed.error()));
    }
    // Only these two: `file://` or similar would turn the facade into a local
    // file read primitive for anything that can reach it.
    if (parsed.value().scheme != "http" && parsed.value().scheme != "https") {
        return core::make_unexpected(HttpFacadeError(
            kStatusBadRequest, "unsupported origin url scheme " + parsed.value().scheme));
    }
    return Unit();
}

core::Expected<Unit, HttpFacadeError> ValidateDigest(const std::string& digest) {
    const std::string prefix = "sha256:";
    if (!StartsWith(digest, prefix)) {
        return core::make_unexpected(
            HttpFacadeError(kStatusBadRequest, "digest must use sha256:<hex>"));
    }
    const std::string hex = digest.substr(prefix.size());
    if (hex.size() != kSha256HexLen) {
        return core::make_unexpected(
            HttpFacadeError(kStatusBadRequest, "digest must use sha256:<64 hex chars>"));
    }
    for (std::size_t i = 0; i < hex.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(hex[i]))) {
            return core::make_unexpected(
                HttpFacadeError(kStatusBadRequest, "digest must use sha256:<64 hex chars>"));
        }
    }
    return Unit();
}

core::Expected<Unit, HttpFacadeError> ValidatePublishPath(
    const std::string& canonical_path, const std::vector<std::string>& allowed_roots) {
    // Fail closed: no roots means publishing is not configured, not that
    // everything is permitted.
    if (allowed_roots.empty()) {
        return core::make_unexpected(
            HttpFacadeError(kStatusForbidden, "no publish roots are configured"));
    }
    for (std::size_t i = 0; i < allowed_roots.size(); ++i) {
        if (PathStartsWith(canonical_path, allowed_roots[i])) return Unit();
    }
    return core::make_unexpected(
        HttpFacadeError(kStatusForbidden, "publish path is outside allowed roots"));
}

core::Expected<std::size_t, std::string> P2pFetchLen(const RequestRange& request_range,
                                                     std::size_t requested_len,
                                                     const Optional<uint64_t>& total_size) {
    // Without a known size there is nothing to clamp against; the transport
    // will return what it has.
    if (!total_size.has_value()) return requested_len;

    if (request_range.start >= *total_size) {
        return core::make_unexpected(std::string("requested range starts past layer end"));
    }
    const uint64_t available = *total_size - request_range.start;
    if (available < static_cast<uint64_t>(requested_len)) {
        return static_cast<std::size_t>(available);
    }
    return requested_len;
}

core::Expected<RangeResponseHeaders, HttpFacadeError> BuildRangeResponseHeaders(
    const RequestRange& request_range, uint64_t response_len,
    const Optional<uint64_t>& total_size) {
    // A zero-length response has no representable inclusive end offset.
    if (response_len == 0) {
        return core::make_unexpected(HttpFacadeError(kStatusRangeNotSatisfiable, "empty range"));
    }
    if (request_range.start > UINT64_MAX - response_len) {
        return core::make_unexpected(HttpFacadeError(kStatusRangeNotSatisfiable, "empty range"));
    }
    const uint64_t range_end = request_range.start + response_len - 1;

    std::ostringstream total;
    if (total_size.has_value()) {
        total << *total_size;
    } else {
        // `*` is the correct wire form for "total unknown".
        total << "*";
    }

    std::ostringstream content_range;
    content_range << "bytes " << request_range.start << "-" << range_end << "/" << total.str();

    RangeResponseHeaders headers;
    headers.status = kStatusPartialContent;
    headers.content_length = response_len;
    headers.content_range = content_range.str();
    headers.accept_ranges = "bytes";
    return headers;
}

core::Expected<uint64_t, std::string> ValidateOriginRangeResponse(
    int status, const HeaderMap& response_headers, const RequestRange& request_range,
    ContentRange* parsed_out) {
    // An origin that ignored Range would give us the whole blob while we frame
    // it as a partial response — a silent corruption, so it is rejected.
    if (status == kStatusOk) {
        return core::make_unexpected(
            std::string("origin ignored Range request and returned 200"));
    }
    if (status != kStatusPartialContent) {
        std::ostringstream oss;
        oss << "origin returned unexpected status " << status;
        return core::make_unexpected(oss.str());
    }

    const Optional<std::string> raw = response_headers.Get("content-range");
    if (!raw.has_value()) {
        return core::make_unexpected(std::string("origin response missing Content-Range"));
    }
    const core::Expected<ContentRange, std::string> parsed = ParseContentRange(*raw);
    if (!parsed.ok()) {
        return core::make_unexpected(std::string("parse Content-Range: ") + parsed.error());
    }

    if (parsed.value().start != request_range.start) {
        std::ostringstream oss;
        oss << "origin returned wrong range start: got " << parsed.value().start
            << ", expected " << request_range.start;
        return core::make_unexpected(oss.str());
    }
    if (parsed.value().end > request_range.end) {
        std::ostringstream oss;
        oss << "origin returned range past request: got " << parsed.value().end << ", max "
            << request_range.end;
        return core::make_unexpected(oss.str());
    }

    const uint64_t response_len = parsed.value().end - parsed.value().start + 1;

    // When both headers are present they must agree; a mismatch means the
    // framing cannot be trusted either way.
    const Optional<std::string> content_length = response_headers.Get("content-length");
    if (content_length.has_value()) {
        uint64_t declared = 0;
        if (!ParseU64(Trim(*content_length), &declared)) {
            return core::make_unexpected(
                std::string("origin Content-Length is not a valid integer"));
        }
        if (declared != response_len) {
            std::ostringstream oss;
            oss << "origin Content-Length does not match Content-Range: got " << declared
                << ", expected " << response_len;
            return core::make_unexpected(oss.str());
        }
    }

    if (parsed_out != NULL) *parsed_out = parsed.value();
    return response_len;
}

// ---------------------------------------------------------------------------
// PublishLayerRequest
// ---------------------------------------------------------------------------

core::Expected<PublishLayerRequest, std::string> PublishLayerRequest::ParseJson(
    const std::string& text) {
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(text);
    if (!parsed.ok()) {
        return core::make_unexpected(std::string("parse publish request: ") +
                                     parsed.error().chain());
    }
    if (parsed.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("publish request must be a JSON object"));
    }
    const core::JsonObject& members = parsed.value().as_object();

    PublishLayerRequest request;

    const core::JsonObject::const_iterator path = members.find("path");
    if (path == members.end() || path->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("publish request needs a string 'path'"));
    }
    request.path = path->second.as_string();

    const core::JsonObject::const_iterator digest = members.find("digest");
    if (digest == members.end() || digest->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("publish request needs a string 'digest'"));
    }
    request.digest = digest->second.as_string();

    const core::JsonObject::const_iterator size = members.find("size");
    if (size == members.end()) {
        return core::make_unexpected(std::string("publish request needs a numeric 'size'"));
    }
    if (size->second.kind() == core::Json::Kind::Int) {
        const int64_t value = size->second.as_int();
        if (value < 0) {
            return core::make_unexpected(
                std::string("publish request 'size' must not be negative"));
        }
        request.size = static_cast<uint64_t>(value);
    } else if (size->second.kind() == core::Json::Kind::Double) {
        const double value = size->second.as_double();
        if (value < 0) {
            return core::make_unexpected(
                std::string("publish request 'size' must not be negative"));
        }
        request.size = static_cast<uint64_t>(value);
    } else {
        return core::make_unexpected(std::string("publish request needs a numeric 'size'"));
    }

    // Rust `#[serde(default)]`: optional.
    const core::JsonObject::const_iterator source_url = members.find("source_url");
    if (source_url != members.end() && source_url->second.kind() == core::Json::Kind::String) {
        request.source_url = source_url->second.as_string();
    }
    return request;
}

std::string PublishLayerRequest::ToJson() const {
    core::JsonObject out;
    out["path"] = core::Json(path);
    out["digest"] = core::Json(digest);
    out["size"] = core::Json(static_cast<int64_t>(size));
    out["source_url"] = source_url.has_value() ? core::Json(*source_url) : core::Json();
    return core::Json(out).ToString();
}

core::Expected<std::vector<std::string>, std::string> CanonicalizePublishRoots(
    const std::vector<std::string>& roots) {
    std::vector<std::string> out;
    out.reserve(roots.size());

    for (std::size_t i = 0; i < roots.size(); ++i) {
        const core::Expected<Unit, std::string> made = fs::CreateDirAll(roots[i]);
        if (!made.ok()) {
            return core::make_unexpected(std::string("create P2P publish root ") + roots[i] +
                                         ": " + made.error());
        }
        char resolved[PATH_MAX];
        if (::realpath(roots[i].c_str(), resolved) == NULL) {
            return core::make_unexpected(std::string("canonicalize P2P publish root ") +
                                         roots[i]);
        }
        out.push_back(std::string(resolved));
    }
    return out;
}

FacadeAddresses FacadeAddresses::ForAuthority(const std::string& authority) {
    FacadeAddresses addresses;
    addresses.address = "http://" + authority + kAddressPrefix;
    addresses.uuid_address = "http://" + authority + kUuidAddressPrefix;
    addresses.publish_address = "http://" + authority + kPublishLayerPath;
    return addresses;
}

// ---------------------------------------------------------------------------
// FacadeCore
// ---------------------------------------------------------------------------

FacadeCore::FacadeCore(std::shared_ptr<agentenv::p2p::P2pTransport> transport,
                       const FacadeConfig& config,
                       std::shared_ptr<OriginRangeFetcher> origin_fetcher)
    : transport_(transport),
      origin_fetcher_(origin_fetcher),
      descriptor_cache_(config.descriptor_cache_ttl_ms, config.descriptor_miss_cache_ttl_ms,
                        config.descriptor_cache_max_entries),
      lookup_timeout_ms_(config.lookup_timeout_ms),
      fetch_range_timeout_ms_(config.fetch_range_timeout_ms),
      allowed_publish_roots_(config.allowed_publish_roots) {}

Optional<agentenv::p2p::P2pArtifactDescriptor> FacadeCore::LookupDescriptor(
    const agentenv::p2p::P2pArtifactKey& artifact_key, const CanonicalBlobIdentity& canonical) {
    const Optional<Optional<agentenv::p2p::P2pArtifactDescriptor> > cached =
        descriptor_cache_.Get(artifact_key);
    if (cached.has_value()) return *cached;

    // Porting note. Rust wraps this call in `tokio::time::timeout`; there is
    // no such wrapper for a synchronous call here, so the timeout is the
    // transport's responsibility and is surfaced as `P2pError::Timeout`. The
    // distinction below is what the Rust code uses the timeout result for.
    agentenv::p2p::P2pArtifactDescriptor found;
    const agentenv::p2p::P2pResult<bool> result = transport_->LookupWithHints(
        artifact_key, std::vector<agentenv::p2p::P2pArtifactProviderHint>(), &found);

    Optional<agentenv::p2p::P2pArtifactDescriptor> descriptor;
    if (!result.ok()) {
        if (result.error().kind == agentenv::p2p::P2pError::Timeout) {
            // Not cached: a slow peer is retried on the next read rather than
            // being written off for the whole miss TTL.
            AGENTENV_DEBUG("p2p lookup timed out; retrying on next read rather than caching a "
                           "miss key="
                           << artifact_key << " timeout_ms=" << lookup_timeout_ms_);
            return Optional<agentenv::p2p::P2pArtifactDescriptor>();
        }
        AGENTENV_DEBUG("p2p lookup failed; caching as miss key="
                       << artifact_key << " error=" << result.error().ToString());
    } else if (result.value()) {
        descriptor = found;
    }

    // A descriptor whose metadata does not match the request is returned once
    // but never cached: caching it would pin a bad answer for the hit TTL.
    bool should_cache = true;
    if (descriptor.has_value()) {
        const core::Expected<LayerMetadata, std::string> metadata =
            LayerMetadata::FromDescriptor(*descriptor);
        should_cache = metadata.ok() && metadata.value().Validate(canonical).ok();
    }
    if (!should_cache) {
        AGENTENV_DEBUG("p2p descriptor failed static metadata validation; not caching key="
                       << artifact_key);
        return descriptor;
    }

    descriptor_cache_.Insert(artifact_key, descriptor);
    return descriptor;
}

core::Expected<OriginRangeResult, std::string> FacadeCore::FetchP2pRange(
    const agentenv::p2p::P2pArtifactDescriptor& descriptor,
    const CanonicalBlobIdentity& canonical, const RequestRange& request_range,
    std::size_t len) {
    const core::Expected<LayerMetadata, std::string> metadata =
        LayerMetadata::FromDescriptor(descriptor);
    if (!metadata.ok()) return core::make_unexpected(metadata.error());

    const core::Expected<Unit, std::string> valid = metadata.value().Validate(canonical);
    if (!valid.ok()) return core::make_unexpected(valid.error());

    const core::Expected<std::size_t, std::string> fetch_len =
        P2pFetchLen(request_range, len, metadata.value().size);
    if (!fetch_len.ok()) return core::make_unexpected(fetch_len.error());

    const agentenv::p2p::P2pResult<std::shared_ptr<agentenv::p2p::P2pByteStream> > stream =
        transport_->FetchByteRange(descriptor, request_range.start, fetch_len.value());
    if (!stream.ok()) {
        if (stream.error().kind == agentenv::p2p::P2pError::Timeout) {
            std::ostringstream oss;
            oss << "p2p range fetch timed out after " << fetch_range_timeout_ms_ << "ms";
            return core::make_unexpected(oss.str());
        }
        return core::make_unexpected(std::string("fetch p2p layer range: ") +
                                     stream.error().ToString());
    }

    // Once this stream is attached to the response body the headers are
    // committed, so a mid-stream P2P failure is reported through HTTP framing
    // rather than by falling back to the origin.
    OriginRangeResult result;
    result.body = stream.value();
    result.response_len = static_cast<uint64_t>(fetch_len.value());
    result.total_size = metadata.value().size;
    return result;
}

core::Expected<OriginRangeResult, HttpFacadeError> FacadeCore::HandleOriginRequest(
    const std::string& uri, const HeaderMap& headers) {
    const core::Expected<Unit, HttpFacadeError> conditional = RejectConditionalHeaders(headers);
    if (!conditional.ok()) return core::make_unexpected(conditional.error());

    const core::Expected<std::string, std::string> origin_url = ReconstructOriginUrl(uri);
    if (!origin_url.ok()) {
        return core::make_unexpected(HttpFacadeError(kStatusBadRequest, origin_url.error()));
    }
    const core::Expected<Unit, HttpFacadeError> scheme = ValidateOriginUrl(origin_url.value());
    if (!scheme.ok()) return core::make_unexpected(scheme.error());

    const core::Expected<RequestRange, HttpFacadeError> request_range =
        ParseSingleRange(headers);
    if (!request_range.ok()) return core::make_unexpected(request_range.error());
    const core::Expected<std::size_t, HttpFacadeError> len = request_range.value().Len();
    if (!len.ok()) return core::make_unexpected(len.error());

    const core::Expected<CanonicalBlobIdentity, std::string> canonical =
        CanonicalBlobIdentity::FromOriginUrl(origin_url.value());
    if (!canonical.ok()) {
        return core::make_unexpected(HttpFacadeError(kStatusBadRequest, canonical.error()));
    }
    const agentenv::p2p::P2pArtifactKey artifact_key = LayerArtifactKey(canonical.value());

    const Optional<agentenv::p2p::P2pArtifactDescriptor> descriptor =
        LookupDescriptor(artifact_key, canonical.value());
    if (descriptor.has_value()) {
        const core::Expected<OriginRangeResult, std::string> p2p_range = FetchP2pRange(
            *descriptor, canonical.value(), request_range.value(), len.value());
        if (p2p_range.ok()) return p2p_range.value();
        // P2P is an optimization, never a requirement: any failure here falls
        // through to the origin.
        AGENTENV_WARN("p2p layer range fetch failed; falling back to origin key="
                      << artifact_key << " error=" << p2p_range.error());
    }

    if (!origin_fetcher_) {
        return core::make_unexpected(
            HttpFacadeError(kStatusBadGateway, "failed to fetch origin range"));
    }
    const core::Expected<OriginRangeResult, std::string> origin =
        origin_fetcher_->FetchRange(origin_url.value(), request_range.value(),
                                    ForwardedHeaders(headers));
    if (!origin.ok()) {
        AGENTENV_WARN("origin range fetch failed error=" << origin.error());
        return core::make_unexpected(
            HttpFacadeError(kStatusBadGateway, "failed to fetch origin range"));
    }
    return origin.value();
}

core::Expected<OriginRangeResult, HttpFacadeError> FacadeCore::HandleUuidRequest(
    const std::string& uuid, const HeaderMap& headers) {
    const core::Expected<Unit, HttpFacadeError> conditional = RejectConditionalHeaders(headers);
    if (!conditional.ok()) return core::make_unexpected(conditional.error());

    const core::Expected<RequestRange, HttpFacadeError> request_range =
        ParseSingleRange(headers);
    if (!request_range.ok()) return core::make_unexpected(request_range.error());
    const core::Expected<std::size_t, HttpFacadeError> len = request_range.value().Len();
    if (!len.ok()) return core::make_unexpected(len.error());

    if (uuid.empty()) {
        return core::make_unexpected(HttpFacadeError(kStatusBadRequest, "invalid uuid"));
    }

    const CanonicalBlobIdentity canonical = CanonicalBlobIdentity::FromUuid(uuid);
    const agentenv::p2p::P2pArtifactKey artifact_key = LayerArtifactKey(canonical);

    const Optional<agentenv::p2p::P2pArtifactDescriptor> descriptor =
        LookupDescriptor(artifact_key, canonical);
    if (!descriptor.has_value()) {
        // No origin exists for a UUID-addressed layer, so a miss is terminal.
        return core::make_unexpected(HttpFacadeError(
            kStatusNotFound, "p2p uuid layer " + uuid + " was not found"));
    }

    const core::Expected<OriginRangeResult, std::string> range_body =
        FetchP2pRange(*descriptor, canonical, request_range.value(), len.value());
    if (!range_body.ok()) {
        AGENTENV_WARN("p2p uuid layer range fetch failed key=" << artifact_key << " uuid="
                                                               << uuid << " error="
                                                               << range_body.error());
        return core::make_unexpected(
            HttpFacadeError(kStatusBadGateway, "failed to fetch p2p uuid layer range"));
    }
    return range_body.value();
}

core::Expected<Unit, HttpFacadeError> FacadeCore::HandlePublishLayer(
    bool is_loopback, const PublishLayerRequest& request) {
    // Localhost-only for the co-located ublk daemon. This is a reachability
    // restriction, not a trust decision — the checks below are the real gates.
    if (!is_loopback) {
        return core::make_unexpected(
            HttpFacadeError(kStatusForbidden, "publish endpoint only accepts localhost clients"));
    }

    const core::Expected<Unit, HttpFacadeError> digest_ok = ValidateDigest(request.digest);
    if (!digest_ok.ok()) return core::make_unexpected(digest_ok.error());

    // Canonicalize before the allowlist check: without it a symlink or `..`
    // could point outside an allowed root while still matching its prefix.
    char resolved[PATH_MAX];
    if (::realpath(request.path.c_str(), resolved) == NULL) {
        return core::make_unexpected(
            HttpFacadeError(kStatusBadRequest, "invalid layer path " + request.path));
    }
    const std::string source(resolved);

    const core::Expected<Unit, HttpFacadeError> allowed =
        ValidatePublishPath(source, allowed_publish_roots_);
    if (!allowed.ok()) return core::make_unexpected(allowed.error());

    const core::Expected<fs::FileStat, std::string> stat = fs::Stat(source);
    if (!stat.ok()) {
        return core::make_unexpected(HttpFacadeError(kStatusBadRequest, stat.error()));
    }
    if (!stat.value().is_regular) {
        return core::make_unexpected(
            HttpFacadeError(kStatusBadRequest, "publish path is not a regular file"));
    }
    if (stat.value().size != request.size) {
        std::ostringstream oss;
        oss << "layer size mismatch: got " << stat.value().size << ", expected " << request.size;
        return core::make_unexpected(HttpFacadeError(kStatusBadRequest, oss.str()));
    }

    // The daemon already verified the layer, but it is re-hashed here because
    // localhost is a process boundary, not a trust boundary.
    const core::Expected<core::FileDigest, std::string> described = core::DescribeFile(source);
    if (!described.ok()) {
        return core::make_unexpected(HttpFacadeError(
            kStatusBadRequest, "hash layer file " + source + ": " + described.error()));
    }
    if (described.value().size != request.size ||
        described.value().sha256 != request.digest) {
        return core::make_unexpected(
            HttpFacadeError(kStatusBadRequest, "layer digest mismatch"));
    }

    const CanonicalBlobIdentity canonical = CanonicalBlobIdentity::FromDigest(request.digest);
    const agentenv::p2p::P2pArtifactKey key = LayerArtifactKey(canonical);

    // Only the hash of the source URL is retained; the raw value may be
    // presigned and carry credentials.
    Optional<std::string> source_url_hash;
    if (request.source_url.has_value()) {
        source_url_hash = core::Sha256Hex(*request.source_url);
    }
    const LayerMetadata metadata = LayerMetadata::FromDigest(
        request.digest, Optional<uint64_t>(request.size), source_url_hash);

    // Reference mode: the layer is already on local disk, so copying it into
    // the P2P store would double the space used.
    agentenv::p2p::P2pPublishRequest publish =
        agentenv::p2p::P2pPublishRequest::File(key, source);
    publish.WithMetadata(metadata.ToJson());
    publish.WithPublishMode(agentenv::p2p::P2pPublishMode::Reference);

    const agentenv::p2p::P2pResult<Unit> published = transport_->Publish(publish);
    if (!published.ok()) {
        return core::make_unexpected(HttpFacadeError(
            kStatusInternalServerError,
            "publish layer artifact failed: " + published.error().ToString()));
    }

    // Drop any cached miss so the new artifact is visible immediately instead
    // of after the miss TTL.
    descriptor_cache_.Remove(key);
    return Unit();
}

}  // namespace p2p
}  // namespace overlaybd
}  // namespace agentenv
