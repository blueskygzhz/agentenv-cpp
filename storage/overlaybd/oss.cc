// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/oss.rs
#include "agentenv/storage/overlaybd/oss.h"

#include <cstdlib>
#include <cstring>
#include <sstream>

#include "agentenv/core/digest.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

std::string Trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

/// Split `scheme://host/path?query` without a URL library.
struct SplitUrl {
    std::string scheme;
    std::string host;
    std::string path;
    std::string query;
};

bool SplitUrlParts(const std::string& raw, SplitUrl* out) {
    const std::string::size_type sep = raw.find("://");
    if (sep == std::string::npos) return false;
    out->scheme = raw.substr(0, sep);
    if (out->scheme.empty()) return false;

    const std::string rest = raw.substr(sep + 3);
    const std::string::size_type q = rest.find('?');
    const std::string before_query = q == std::string::npos ? rest : rest.substr(0, q);
    out->query = q == std::string::npos ? std::string() : rest.substr(q + 1);

    const std::string::size_type slash = before_query.find('/');
    if (slash == std::string::npos) {
        out->host = before_query;
        out->path.clear();
    } else {
        out->host = before_query.substr(0, slash);
        out->path = before_query.substr(slash);
    }
    return !out->host.empty();
}

void ForEachQueryPair(const std::string& query, std::string* endpoint, std::string* region) {
    size_t pos = 0;
    while (pos < query.size()) {
        const std::string::size_type amp = query.find('&', pos);
        const std::string pair =
            query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const std::string::size_type eq = pair.find('=');
        if (eq != std::string::npos) {
            const std::string name = pair.substr(0, eq);
            const std::string value = pair.substr(eq + 1);
            if (name == "endpoint" && endpoint) *endpoint = value;
            if (name == "region" && region) *region = value;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
}

std::string HostOfEndpoint(const std::string& endpoint) {
    SplitUrl parts;
    if (!SplitUrlParts(endpoint, &parts)) return std::string();
    return parts.host;
}

std::string SchemeOfEndpoint(const std::string& endpoint) {
    SplitUrl parts;
    if (!SplitUrlParts(endpoint, &parts)) return std::string("https");
    return parts.scheme;
}

std::string HexSha256(const uint8_t* data, size_t len) {
    core::Sha256Stream h;
    if (len > 0) h.Update(data, len);
    uint8_t digest[32];
    h.PeekFinal(digest);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 32; ++i) {
        out.push_back(kHex[(digest[i] >> 4) & 0xF]);
        out.push_back(kHex[digest[i] & 0xF]);
    }
    return out;
}

}  // namespace

core::Expected<core::Optional<AddressingStyle>, std::string> ParseAddressingStyle(
    const std::string& value) {
    const std::string trimmed = Trim(value);
    // Empty is "auto-detect", which is a third outcome rather than a default
    // style: the right one depends on the endpoint.
    if (trimmed.empty()) return core::Optional<AddressingStyle>();
    if (trimmed == "virtual") {
        return core::Optional<AddressingStyle>(kAddressingVirtual);
    }
    if (trimmed == "path") return core::Optional<AddressingStyle>(kAddressingPath);
    return core::make_unexpected("invalid oss defaultAddressingStyle '" + trimmed +
                                 "': expected 'virtual', 'path', or empty for "
                                 "auto-detection");
}

core::Expected<AddressingStyle, std::string> DetectAddressingStyle(
    const std::string& endpoint, const std::string& bucket) {
    const std::string host = HostOfEndpoint(endpoint);
    if (host.empty()) {
        return core::make_unexpected(std::string("oss endpoint host is missing"));
    }

    // A host already prefixed with the bucket is a CNAME valid only for that
    // bucket, so path style would address the wrong thing.
    if (StartsWith(host, bucket + ".")) return kAddressingVirtual;
    // Aliyun's own endpoints require virtual hosting.
    if (EndsWith(host, ".aliyuncs.com") || EndsWith(host, ".aliyun-inc.com")) {
        return kAddressingVirtual;
    }
    // Everything else: path style, which every S3-compatible server accepts.
    return kAddressingPath;
}

core::Expected<ParsedOssUrl, std::string> ParsedOssUrl::Parse(
    const std::string& raw, const std::string& default_endpoint,
    const std::string& default_region,
    const core::Optional<AddressingStyle>& addressing_override) {
    SplitUrl parts;
    if (!SplitUrlParts(raw, &parts)) {
        return core::make_unexpected("invalid oss url " + raw);
    }
    if (parts.scheme != "s3" && parts.scheme != "oss") {
        return core::make_unexpected("unsupported oss url scheme " + parts.scheme);
    }

    ParsedOssUrl out;
    out.bucket = parts.host;
    if (out.bucket.empty()) {
        return core::make_unexpected(std::string("oss url bucket is missing"));
    }

    out.key = parts.path;
    while (!out.key.empty() && out.key[0] == '/') out.key.erase(0, 1);
    if (out.key.empty()) {
        return core::make_unexpected(std::string("oss url object key is missing"));
    }

    // Per-URL overrides win: one image may name a bucket in another region.
    std::string endpoint, region;
    ForEachQueryPair(parts.query, &endpoint, &region);
    if (endpoint.empty()) endpoint = default_endpoint;
    if (region.empty()) region = default_region;
    if (endpoint.empty()) {
        return core::make_unexpected(std::string("oss endpoint is missing"));
    }
    if (region.empty()) {
        return core::make_unexpected(std::string("oss region is missing"));
    }
    out.endpoint = endpoint;
    out.region = region;

    // Detection runs even when overridden, because it also validates the
    // endpoint URL; the override then wins.
    const core::Expected<AddressingStyle, std::string> detected =
        DetectAddressingStyle(endpoint, out.bucket);
    if (!detected.ok()) return core::make_unexpected(detected.error());
    out.addressing_style =
        addressing_override.has_value() ? *addressing_override : detected.value();
    return out;
}

std::string ParsedOssUrl::Host() const {
    const std::string host = HostOfEndpoint(endpoint);
    if (addressing_style == kAddressingVirtual) {
        // Already bucket-prefixed (a CNAME), so prefixing again would produce
        // `bucket.bucket.host`.
        if (StartsWith(host, bucket + ".")) return host;
        return bucket + "." + host;
    }
    return host;
}

std::string ParsedOssUrl::CanonicalUri() const {
    // The signature covers the encoded path, and it must match the request
    // line byte for byte.
    if (addressing_style == kAddressingVirtual) {
        return http::EncodePath("/" + key);
    }
    return http::EncodePath("/" + bucket + "/" + key);
}

std::string ParsedOssUrl::ObjectUrl() const {
    std::ostringstream o;
    o << SchemeOfEndpoint(endpoint) << "://" << Host() << CanonicalUri();
    return o.str();
}

// ---------------------------------------------------------------------------
// OssBackend
// ---------------------------------------------------------------------------

core::Expected<std::shared_ptr<OssBackend>, std::string> OssBackend::New(
    const OssConfig& config) {
    const core::Expected<core::Optional<AddressingStyle>, std::string> style =
        ParseAddressingStyle(config.default_addressing_style);
    if (!style.ok()) return core::make_unexpected(style.error());

    // Rust `normalized_credential`: trimmed, and blank counts as absent. A
    // field of spaces is a config that forgot to fill it in, not a credential
    // whose value is whitespace.
    const std::string access_key_id = Trim(config.access_key_id);
    const std::string secret_access_key = Trim(config.secret_access_key);
    const std::string security_token = Trim(config.security_token);
    const std::string credential_process = Trim(config.credential_process);

    // Rust `credential_source_from_fields`, in the same order. Each of these
    // is a config that cannot mean one thing, so it is refused at
    // construction rather than producing a 403 on the first read.
    if (!credential_process.empty()) {
        if (!access_key_id.empty() || !secret_access_key.empty() ||
            !security_token.empty()) {
            return core::make_unexpected(
                std::string("oss credential_process cannot be combined with access_key_id, "
                            "secret_access_key, or security_token"));
        }
        // Not run here. Rust shells out to it to obtain keys; doing that from
        // this layer would make a read spawn a process, so a config relying
        // on it is refused rather than silently degrading to anonymous.
        return core::make_unexpected(
            std::string("ossConfig.credentialProcess is not supported by this "
                        "implementation; supply accessKeyId/secretAccessKey"));
    }

    if (access_key_id.empty() && secret_access_key.empty()) {
        // A token alone cannot be signed with: it is an addition to a key
        // pair, not a replacement for one.
        if (!security_token.empty()) {
            return core::make_unexpected(
                std::string("oss security_token requires access_key_id and "
                            "secret_access_key"));
        }
        // Anonymous is allowed for OSS (`allow_anonymous: true`): a public
        // bucket needs no credentials.
    } else if (access_key_id.empty() || secret_access_key.empty()) {
        return core::make_unexpected(
            std::string("oss access_key_id and secret_access_key must be set together"));
    }

    std::shared_ptr<OssBackend> backend(new OssBackend());
    backend->config_ = config;
    // Rust falls back when either is zero. Zero is "unset", not a literal
    // instruction: libcurl reads a zero timeout as "no limit", and zero
    // attempts would make every transient failure fatal.
    if (backend->config_.timeout_secs == 0) {
        backend->config_.timeout_secs = kOssDefaultTimeoutSecs;
    }
    if (backend->config_.retry_count == 0) {
        backend->config_.retry_count = kOssDefaultRetryCount;
    }
    backend->addressing_override_ = style.value();
    backend->credentials_.access_key_id = access_key_id;
    backend->credentials_.secret_access_key = secret_access_key;
    backend->credentials_.security_token = security_token;
    return backend;
}

core::Expected<ParsedOssUrl, std::string> OssBackend::ParseUrl(
    const std::string& url) const {
    return ParsedOssUrl::Parse(url, config_.default_endpoint, config_.default_region,
                               addressing_override_);
}

core::Expected<http::Response, std::string> OssBackend::Send(
    const ParsedOssUrl& location, const std::string& method, bool has_range,
    uint64_t range_begin, uint64_t range_end, const uint8_t* body, size_t body_len,
    http::BodySink* sink) {
    const uint32_t attempts = config_.retry_count + 1;
    std::string last_error;

    for (uint32_t attempt = 0; attempt < attempts; ++attempt) {
        http::Request request;
        request.method = method;
        request.url = location.ObjectUrl();
        request.has_range = has_range;
        request.range_begin = range_begin;
        request.range_end = range_end;
        request.timeout_secs = static_cast<long>(config_.timeout_secs);
        if (body != 0 && body_len > 0) {
            request.body.assign(body, body + body_len);
        }
        // Only meaningful for a 2xx; `Perform` routes an error body to
        // `Response.body` regardless, so the status check above still sees
        // the message.
        request.body_sink = sink;

        S3SigningContext ctx;
        ctx.method = method;
        ctx.canonical_uri = location.CanonicalUri();
        ctx.host = location.Host();
        ctx.region = location.region;
        ctx.service = "s3";
        // Re-signed per attempt: the signature covers a timestamp the server
        // only accepts within a window, so a retry after a long stall must
        // not reuse the old one.
        ctx.amz_date = S3AmzDateNow();
        ctx.payload_sha256_hex =
            body_len > 0 ? HexSha256(body, body_len) : std::string(kS3EmptyPayloadSha256);

        S3SignRequest(credentials_, ctx, &request);

        const core::Expected<http::Response, std::string> response = http::Perform(request);
        if (!response.ok()) {
            // Transport failure: worth retrying, the request never landed.
            last_error = response.error();
            continue;
        }
        // 5xx is the server asking to be retried. A 4xx is its final answer
        // and is returned as-is, because the caller distinguishes 404 from
        // 403 from 416 and repeating it would only stall.
        if (response.value().status >= 500 && attempt + 1 < attempts) {
            std::ostringstream o;
            o << "oss " << method << " returned status " << response.value().status;
            last_error = o.str();
            continue;
        }
        return response.value();
    }

    return core::make_unexpected(last_error.empty()
                                     ? std::string("oss request failed")
                                     : last_error);
}

core::Expected<std::shared_ptr<OssFile>, std::string> OssBackend::OpenWithSizeHint(
    const std::string& url, uint64_t size_hint) {
    const core::Expected<ParsedOssUrl, std::string> location = ParseUrl(url);
    if (!location.ok()) return core::make_unexpected(location.error());

    std::shared_ptr<OssFile> file(new OssFile());
    file->backend_ = this;
    file->location_ = location.value();
    // A non-zero hint avoids the HEAD entirely: the caller knows the size
    // from a manifest, and the object cannot change under the handle.
    if (size_hint > 0) {
        file->size_known_ = true;
        file->size_ = size_hint;
    }
    return file;
}

core::Expected<core::Optional<uint64_t>, std::string> OssBackend::ObjectSize(
    const std::string& url) {
    const core::Expected<ParsedOssUrl, std::string> location = ParseUrl(url);
    if (!location.ok()) return core::make_unexpected(location.error());

    const core::Expected<http::Response, std::string> response =
        Send(location.value(), "HEAD", false, 0, 0, 0, 0, 0);
    if (!response.ok()) return core::make_unexpected(response.error());

    // Absent, not an error: callers probe for an object they may still need
    // to upload.
    if (response.value().status == 404) return core::Optional<uint64_t>();
    if (!response.value().IsSuccess()) {
        std::ostringstream o;
        o << "oss HEAD '" << location.value().key << "' returned status "
          << response.value().status;
        return core::make_unexpected(o.str());
    }

    uint64_t size = 0;
    if (!response.value().ContentLength(&size)) {
        return core::make_unexpected(std::string("oss HEAD response has no Content-Length"));
    }
    return core::Optional<uint64_t>(size);
}

core::Expected<core::Unit, std::string> OssBackend::UploadBytes(const std::string& url,
                                                                   const uint8_t* data,
                                                                   size_t len) {
    const core::Expected<ParsedOssUrl, std::string> location = ParseUrl(url);
    if (!location.ok()) return core::make_unexpected(location.error());

    const core::Expected<http::Response, std::string> response =
        Send(location.value(), "PUT", false, 0, 0, data, len, 0);
    if (!response.ok()) return core::make_unexpected(response.error());
    if (!response.value().IsSuccess()) {
        std::ostringstream o;
        o << "oss PUT '" << location.value().key << "' returned status "
          << response.value().status;
        return core::make_unexpected(o.str());
    }
    return core::Unit();
}

// ---------------------------------------------------------------------------
// OssFile
// ---------------------------------------------------------------------------

core::Expected<uint64_t, std::string> OssFile::Size() {
    if (size_known_) return size_;

    const core::Expected<http::Response, std::string> response =
        backend_->Send(location_, "HEAD", false, 0, 0, 0, 0, 0);
    if (!response.ok()) return core::make_unexpected(response.error());
    if (!response.value().IsSuccess()) {
        std::ostringstream o;
        o << "oss HEAD '" << location_.key << "' returned status "
          << response.value().status;
        return core::make_unexpected(o.str());
    }

    uint64_t size = 0;
    if (!response.value().ContentLength(&size)) {
        return core::make_unexpected(std::string("oss HEAD response has no Content-Length"));
    }
    // Cached for the life of the handle: an object is immutable, and probing
    // per read would double the request count.
    size_ = size;
    size_known_ = true;
    return size_;
}

core::Expected<ByteBuffer, std::string> OssFile::ReadAt(uint64_t offset, size_t len) {
    ByteBuffer out;
    if (len == 0) return out;

    const core::Expected<uint64_t, std::string> size = Size();
    if (!size.ok()) return core::make_unexpected(size.error());
    // Past the end is an empty read, matching a local file rather than
    // producing a 416 the caller would have to interpret.
    if (offset >= size.value()) return out;

    const uint64_t remain = size.value() - offset;
    const uint64_t want = remain < len ? remain : len;
    // HTTP ranges are inclusive at both ends.
    const uint64_t end = offset + want - 1;

    const core::Expected<http::Response, std::string> response =
        backend_->Send(location_, "GET", true, offset, end, 0, 0, 0);
    if (!response.ok()) return core::make_unexpected(response.error());
    if (!response.value().IsSuccess()) {
        std::ostringstream o;
        o << "read range " << offset << ".." << (end + 1) << " from oss object '"
          << location_.key << "' returned status " << response.value().status;
        return core::make_unexpected(o.str());
    }

    // Returned as received. Rust hands back `data.to_bytes()` and does not
    // re-window a 200 that ignored the range: such a server is
    // non-conforming, and silently slicing its answer here would differ from
    // the Rust reader against the same store.
    return response.value().body;
}

core::Expected<size_t, std::string> OssFile::ReadAtInto(uint64_t offset, uint8_t* dst,
                                                           size_t len) {
    if (len == 0) return static_cast<size_t>(0);

    const core::Expected<uint64_t, std::string> size = Size();
    if (!size.ok()) return core::make_unexpected(size.error());
    if (offset >= size.value()) return static_cast<size_t>(0);

    // Clamped to the object, as Rust's `min(offset + len, size)` does.
    const uint64_t remain = size.value() - offset;
    const size_t want = static_cast<size_t>(remain < len ? remain : len);
    const uint64_t end = offset + static_cast<uint64_t>(want) - 1;

    // Streamed into the caller's buffer rather than built up first: a
    // multi-megabyte range otherwise costs two copies of itself.
    http::BufferSink sink(dst, want);
    const core::Expected<http::Response, std::string> response =
        backend_->Send(location_, "GET", true, offset, end, 0, 0, &sink);
    if (!response.ok()) return core::make_unexpected(response.error());
    if (!response.value().IsSuccess()) {
        std::ostringstream o;
        o << "read range " << offset << ".." << (end + 1) << " from oss object '"
          << location_.key << "' returned status " << response.value().status;
        return core::make_unexpected(o.str());
    }
    // A surplus means the store ignored the range, so what arrived is not the
    // window asked for; the sink already cut the transfer off.
    if (sink.overflow()) {
        std::ostringstream o;
        o << "range response larger than requested for oss object '" << location_.key
          << "' at " << offset;
        return core::make_unexpected(o.str());
    }
    return sink.filled();
}

core::Expected<size_t, std::string> OssFile::WriteAt(uint64_t, const uint8_t*, size_t) {
    return core::make_unexpected(
        std::string("oss file backend is read-only; use OssBackend::UploadBytes for writes"));
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
