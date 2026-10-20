// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/oss.rs
#include "agentenv/storage/overlaybd/oss.h"

#include <atomic>
#include <stdio.h>

#include "agentenv/core/credentials.h"

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

namespace {

/// Rust `normalized_credential`: trimmed, blank treated as absent.
///
/// A field of spaces is a config that was never filled in, not a credential
/// whose value is whitespace -- and sending it would produce a 403 whose
/// cause is invisible in the config file.
core::Optional<std::string> NonEmpty(const std::string& value) {
    const std::string trimmed = Trim(value);
    if (trimmed.empty()) return core::Optional<std::string>();
    return core::Optional<std::string>(trimmed);
}

}  // namespace

core::Expected<std::shared_ptr<OssBackend>, std::string> OssBackend::New(
    const OssConfig& config) {
    const core::Expected<core::Optional<AddressingStyle>, std::string> style =
        ParseAddressingStyle(config.default_addressing_style);
    if (!style.ok()) return core::make_unexpected(style.error());

    // Delegated rather than re-derived. `core::credentials::FromFields` is the
    // port of Rust's `credential_source_from_fields`, and the same rules also
    // serve the remote-image paths; duplicating them here would let the two
    // copies drift, and the failure mode would be a config accepted by one
    // and rejected by the other.
    core::credentials::CredentialFields fields;
    fields.access_key_id = NonEmpty(config.access_key_id);
    fields.secret_access_key = NonEmpty(config.secret_access_key);
    fields.security_token = NonEmpty(config.security_token);
    fields.credential_process = NonEmpty(config.credential_process);

    core::credentials::CredentialSourceOptions options;
    options.scope = "oss";
    // Public buckets must work with no credentials at all.
    options.allow_anonymous = true;
    options.required_access_key_id_label = "oss access_key_id";
    options.required_secret_access_key_label = "oss secret_access_key";

    const core::Expected<core::credentials::CredentialSource, std::string> source =
        core::credentials::FromFields(fields, options);
    if (!source.ok()) return core::make_unexpected(source.error());

    // Rust runs it; this layer refuses instead of spawning a process from a
    // read path, and refusing is deliberate -- silently degrading to
    // anonymous would fail later as a confusing 403.
    if (source.value().kind == core::credentials::CredentialSource::Kind::Process) {
        return core::make_unexpected(
            std::string("ossConfig.credentialProcess is not supported by this "
                        "implementation; supply accessKeyId/secretAccessKey"));
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
    if (source.value().kind == core::credentials::CredentialSource::Kind::Static) {
        backend->credentials_.access_key_id = source.value().credential.access_key_id;
        backend->credentials_.secret_access_key = source.value().credential.secret_access_key;
        // `Optional` -> empty string: an absent token must not be sent, and
        // the signer skips an empty one.
        if (source.value().credential.security_token.has_value()) {
            backend->credentials_.security_token = *source.value().credential.security_token;
        }
    }
    // Anonymous leaves both key fields empty, which `IsAnonymous` reports and
    // the signer treats as "send no Authorization header".
    return backend;
}

core::Expected<ParsedOssUrl, std::string> OssBackend::ParseUrl(
    const std::string& url) const {
    return ParsedOssUrl::Parse(url, config_.default_endpoint, config_.default_region,
                               addressing_override_);
}

void OssBackend::SignRequest(const S3Credentials& credentials,
                                 const ParsedOssUrl& location, const std::string& method,
                                 const uint8_t* body, size_t body_len,
                                 const std::string& canonical_query,
                                 http::Request* request) const {
    S3SigningContext ctx;
    ctx.method = method;
    ctx.canonical_uri = location.CanonicalUri();
    // Part of the signature, not decoration: SigV4 hashes the canonical query
    // string, so a `?partNumber=..&uploadId=..` that is sent but not signed is
    // rejected. It must already be sorted and percent-encoded by the caller,
    // which is why the multipart helpers below build it in one place.
    ctx.canonical_query = canonical_query;
    ctx.host = location.Host();
    ctx.region = location.region;
    ctx.service = "s3";
    // Signed afresh every attempt: the signature covers a timestamp the
    // server only accepts inside a window, so a retry after a stall must not
    // reuse the old one -- and it is the only thing that changes when a
    // static credential is retried after a 403.
    ctx.amz_date = S3AmzDateNow();
    ctx.payload_sha256_hex =
        body_len > 0 ? HexSha256(body, body_len) : std::string(kS3EmptyPayloadSha256);
    S3SignRequest(credentials, ctx, request);
}

core::Expected<http::Response, std::string> OssBackend::Send(
    const ParsedOssUrl& location, const std::string& method, bool has_range,
    uint64_t range_begin, uint64_t range_end, const uint8_t* body, size_t body_len,
    http::BodySink* sink) {
    return SendWithQuery(location, method, has_range, range_begin, range_end, body, body_len,
                         sink, std::string());
}

core::Expected<http::Response, std::string> OssBackend::SendWithQuery(
    const ParsedOssUrl& location, const std::string& method, bool has_range,
    uint64_t range_begin, uint64_t range_end, const uint8_t* body, size_t body_len,
    http::BodySink* sink, const std::string& canonical_query) {
    // Two passes at most: the operation, then the same operation again after a
    // permission-denied answer. Rust nests them the same way -- `op(...)` in
    // `run_with_refresh` is an opendal call that has already exhausted its own
    // retries, so the credential retry is *added on top* rather than taken out
    // of the transport budget. A single loop with a shared counter would cost
    // one transport attempt instead, which matters when a 5xx blip and an
    // expired credential both occur.
    const uint32_t passes = 1 + 1;
    std::string last_error;

    for (uint32_t pass = 0; pass < passes; ++pass) {
        const uint32_t attempts = config_.retry_count + 1;

        for (uint32_t attempt = 0; attempt < attempts; ++attempt) {
            http::Request request;
            request.method = method;
            request.url = location.ObjectUrl();
            if (!canonical_query.empty()) request.url += "?" + canonical_query;
            request.has_range = has_range;
            request.range_begin = range_begin;
            request.range_end = range_end;
            request.timeout_secs = static_cast<long>(config_.timeout_secs);
            if (body != 0 && body_len > 0) {
                request.body.assign(body, body + body_len);
            }
            // Only meaningful for a 2xx; `Perform` routes an error body to
            // `Response.body` regardless, so the status checks below still see
            // the message.
            request.body_sink = sink;

            SignRequest(credentials_, location, method, body, body_len, canonical_query,
                        &request);

            const core::Expected<http::Response, std::string> response =
                http::Perform(request);
            if (!response.ok()) {
                // Transport failure: worth retrying, the request never landed.
                last_error = response.error();
                continue;
            }

            // Only permission denied triggers the second pass. Rust's comment
            // says as much -- other authentication errors are terminal until
            // the contract is deliberately broadened.
            //
            // With static credentials the rebuilt operator carries the same
            // pair, so this is a retry rather than a re-authentication: what
            // changes is the signing timestamp, which is what a clock-skew or
            // just-rotated-token rejection needs.
            //
            // The key check matches Rust's `credentials.is_some()`: an
            // anonymous request has no credential to rebuild, so repeating its
            // 403 would only cost a request.
            if (response.value().status == 403 && pass + 1 < passes &&
                !credentials_.access_key_id.empty()) {
                break;
            }

            // 5xx is the server asking to be retried. A 4xx is its final
            // answer and is returned as-is, because the caller distinguishes
            // 404 from 403 from 416 and repeating it would only stall.
            if (response.value().status >= 500 && attempt + 1 < attempts) {
                std::ostringstream o;
                o << "oss " << method << " returned status " << response.value().status;
                last_error = o.str();
                continue;
            }
            return response.value();
        }
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
// Multipart upload
//
// Rust reaches this through opendal's `writer_with(key).chunk(n).concurrent(n)`,
// which performs an S3 multipart upload underneath. There is no opendal here,
// so the three calls are issued directly.
//
// Multipart rather than a single PUT for two reasons: a sealed upper layer can
// be larger than the 5 GiB single-PUT ceiling, and a single PUT would need the
// whole object resident in memory because the transport takes a pointer and a
// length.
// ---------------------------------------------------------------------------

namespace {

/// Rust `UPLOAD_READ_SIZE`-style staging buffer: how much is read from the
/// source file per `fread`.
const size_t kOssUploadReadSize = 1024 * 1024;

/// S3 requires at least 5 MiB for every part except the last, and allows at
/// most 10000 parts.
const size_t kOssMinPartSize = 5 * 1024 * 1024;
const uint32_t kOssMaxParts = 10000;

/// XML text escaping for the complete-upload body.
///
/// Only the five predefined entities. An ETag is server-supplied and normally
/// hex in quotes, but it is echoed back verbatim, so it is escaped rather than
/// trusted to be inert.
std::string XmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        switch (in[i]) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(in[i]);
        }
    }
    return out;
}

/// The text between the first `<tag>` and its `</tag>`.
///
/// A deliberate non-parser: the three responses involved have a fixed,
/// flat shape, and pulling one element out of each is all that is needed. A
/// real XML parser would be a dependency and a much larger attack surface for
/// no gain -- but it does mean a namespaced or attributed tag is not matched,
/// which is why a miss is reported as an error rather than ignored.
bool XmlFirstElement(const std::string& xml, const std::string& tag, std::string* out) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const std::string::size_type begin = xml.find(open);
    if (begin == std::string::npos) return false;
    const std::string::size_type from = begin + open.size();
    const std::string::size_type end = xml.find(close, from);
    if (end == std::string::npos) return false;
    *out = xml.substr(from, end - from);
    return true;
}

std::string BodyToString(const std::vector<uint8_t>& body) {
    if (body.empty()) return std::string();
    return std::string(reinterpret_cast<const char*>(&body[0]), body.size());
}

/// Percent-encode one query *value* per RFC 3986 unreserved set.
///
/// SigV4 hashes the canonical query string, so the encoding used here has to
/// be byte-identical to what is sent -- which is why the value is encoded once
/// and the same string is used for both.
std::string EncodeQueryComponent(const std::string& in) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace

core::Expected<core::Unit, std::string> OssBackend::UploadPath(const std::string& url,
                                                                  const std::string& path,
                                                                  size_t part_size,
                                                                  size_t concurrency,
                                                                  std::atomic<uint64_t>* progress) {
    // Reset first, as Rust does, so the counter always describes the attempt
    // now underway rather than carrying the previous one's total.
    if (progress != 0) progress->store(0);

    const core::Expected<ParsedOssUrl, std::string> location = ParseUrl(url);
    if (!location.ok()) return core::make_unexpected(location.error());

    // Raised, not rejected: `part_size` is a tuning hint, and a value below
    // the protocol floor would make every part but the last invalid.
    if (part_size < kOssMinPartSize) part_size = kOssMinPartSize;

    FILE* file = ::fopen(path.c_str(), "rb");
    if (file == 0) {
        return core::make_unexpected("open oss upload source file " + path);
    }
    // Deviation: Rust passes `concurrency` to opendal, which uploads that many
    // parts at once. Parts are uploaded sequentially here. The resulting
    // object is identical -- part order is given by the part number, not by
    // completion order -- so this costs throughput on a large upload and
    // nothing else. Overlapping them would need a thread per part plus its
    // own staging buffer, which is a separate piece of work.
    (void)concurrency;

    const core::Expected<std::string, std::string> upload_id =
        CreateMultipartUpload(location.value());
    if (!upload_id.ok()) {
        ::fclose(file);
        return core::make_unexpected(upload_id.error());
    }

    std::vector<std::string> etags;
    std::vector<uint8_t> part;
    part.reserve(part_size);
    std::vector<uint8_t> chunk(kOssUploadReadSize);
    bool read_failed = false;
    std::string failure;

    for (;;) {
        // Filled to exactly `part_size` before sending, because only the final
        // part may be shorter.
        while (part.size() < part_size) {
            const size_t want = part_size - part.size() < chunk.size()
                                    ? part_size - part.size()
                                    : chunk.size();
            const size_t got = ::fread(&chunk[0], 1, want, file);
            if (got > 0) part.insert(part.end(), chunk.begin(), chunk.begin() + (long)got);
            if (got < want) {
                if (::ferror(file) != 0) read_failed = true;
                break;
            }
        }
        if (read_failed) {
            failure = "read oss upload source file " + path;
            break;
        }
        if (part.empty()) break;

        if (etags.size() + 1 > kOssMaxParts) {
            std::ostringstream o;
            o << "oss upload needs more than " << kOssMaxParts << " parts; raise part_size";
            failure = o.str();
            break;
        }

        const core::Expected<std::string, std::string> etag = UploadPart(
            location.value(), upload_id.value(), static_cast<uint32_t>(etags.size() + 1),
            part.empty() ? 0 : &part[0], part.size());
        if (!etag.ok()) {
            failure = etag.error();
            break;
        }
        etags.push_back(etag.value());
        // After the part is acknowledged, unlike Rust's "handed off to the
        // writer": there is no writer queue here, so acknowledged is the only
        // point that exists.
        if (progress != 0) progress->fetch_add(static_cast<uint64_t>(part.size()));

        const bool last = part.size() < part_size;
        part.clear();
        if (last) break;
    }
    ::fclose(file);

    if (!failure.empty()) {
        // Abandoned explicitly. An incomplete multipart upload is invisible as
        // an object but still billed for its parts, so leaving it would charge
        // for storage nobody can read.
        AbortMultipartUpload(location.value(), upload_id.value());
        return core::make_unexpected("upload oss object '" + url + "' from " + path + ": " +
                                     failure);
    }

    // Zero-length source: there is no valid empty multipart upload, so the
    // object is written with a plain PUT instead.
    if (etags.empty()) {
        AbortMultipartUpload(location.value(), upload_id.value());
        return UploadBytes(url, 0, 0);
    }

    const core::Expected<core::Unit, std::string> completed =
        CompleteMultipartUpload(location.value(), upload_id.value(), etags);
    if (!completed.ok()) {
        AbortMultipartUpload(location.value(), upload_id.value());
        return core::make_unexpected("upload oss object '" + url + "' from " + path + ": " +
                                     completed.error());
    }
    return core::Unit();
}

core::Expected<std::string, std::string> OssBackend::CreateMultipartUpload(
    const ParsedOssUrl& location) {
    // `uploads` is a valueless sub-resource; SigV4 still expects the `=`.
    const core::Expected<http::Response, std::string> response =
        SendWithQuery(location, "POST", false, 0, 0, 0, 0, 0, "uploads=");
    if (!response.ok()) return core::make_unexpected(response.error());
    if (!response.value().IsSuccess()) {
        std::ostringstream o;
        o << "oss create multipart upload '" << location.key << "' returned status "
          << response.value().status;
        return core::make_unexpected(o.str());
    }

    std::string upload_id;
    if (!XmlFirstElement(BodyToString(response.value().body), "UploadId", &upload_id) ||
        upload_id.empty()) {
        return core::make_unexpected(
            std::string("oss create multipart upload response has no UploadId"));
    }
    return upload_id;
}

core::Expected<std::string, std::string> OssBackend::UploadPart(const ParsedOssUrl& location,
                                                                   const std::string& upload_id,
                                                                   uint32_t part_number,
                                                                   const uint8_t* body,
                                                                   size_t body_len) {
    // Sorted by key, as the canonical query string requires: `partNumber`
    // before `uploadId`.
    std::ostringstream query;
    query << "partNumber=" << part_number << "&uploadId=" << EncodeQueryComponent(upload_id);

    const core::Expected<http::Response, std::string> response =
        SendWithQuery(location, "PUT", false, 0, 0, body, body_len, 0, query.str());
    if (!response.ok()) return core::make_unexpected(response.error());
    if (!response.value().IsSuccess()) {
        std::ostringstream o;
        o << "oss upload part " << part_number << " of '" << location.key
          << "' returned status " << response.value().status;
        return core::make_unexpected(o.str());
    }

    const http::HeaderMap::const_iterator etag = response.value().headers.find("ETag");
    if (etag == response.value().headers.end() || etag->second.empty()) {
        // The completion request has to name every part's ETag, so a part
        // whose ETag is missing cannot be completed -- failing here keeps the
        // upload abortable instead of producing a rejected completion.
        std::ostringstream o;
        o << "oss upload part " << part_number << " of '" << location.key
          << "' returned no ETag";
        return core::make_unexpected(o.str());
    }
    return etag->second;
}

core::Expected<core::Unit, std::string> OssBackend::CompleteMultipartUpload(
    const ParsedOssUrl& location, const std::string& upload_id,
    const std::vector<std::string>& etags) {
    std::ostringstream xml;
    xml << "<CompleteMultipartUpload>";
    for (size_t i = 0; i < etags.size(); ++i) {
        xml << "<Part><PartNumber>" << (i + 1) << "</PartNumber><ETag>"
            << XmlEscape(etags[i]) << "</ETag></Part>";
    }
    xml << "</CompleteMultipartUpload>";
    const std::string body = xml.str();

    const core::Expected<http::Response, std::string> response = SendWithQuery(
        location, "POST", false, 0, 0, reinterpret_cast<const uint8_t*>(body.data()),
        body.size(), 0, "uploadId=" + EncodeQueryComponent(upload_id));
    if (!response.ok()) return core::make_unexpected(response.error());
    if (!response.value().IsSuccess()) {
        std::ostringstream o;
        o << "oss complete multipart upload '" << location.key << "' returned status "
          << response.value().status;
        return core::make_unexpected(o.str());
    }

    // A 200 is not enough: S3 streams the completion and may report a failure
    // inside the body after the headers are already sent. Treating the status
    // alone as success would report an object that does not exist.
    const std::string text = BodyToString(response.value().body);
    std::string code;
    if (XmlFirstElement(text, "Error", &code) || XmlFirstElement(text, "Code", &code)) {
        return core::make_unexpected("oss complete multipart upload '" + location.key +
                                     "' failed after 200: " + code);
    }
    return core::Unit();
}

void OssBackend::AbortMultipartUpload(const ParsedOssUrl& location,
                                      const std::string& upload_id) {
    // Best-effort: the caller is already failing, and a failed abort only
    // leaves the parts for the bucket's own lifecycle rule to clean up.
    SendWithQuery(location, "DELETE", false, 0, 0, 0, 0, 0,
                  "uploadId=" + EncodeQueryComponent(upload_id));
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
