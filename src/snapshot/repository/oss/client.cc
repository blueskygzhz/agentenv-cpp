// SPDX-License-Identifier: MIT
// Rust: src/repository/backends/oss/client.rs
#include "agentenv/snapshot/repository/oss/client.h"

#include <algorithm>
#include <sstream>

#include <cstdio>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace oss {

namespace sov = storage::overlaybd;
namespace http = storage::overlaybd::http;

namespace {

/// Rust `normalized_credential` — trimmed, blank treated as absent.
std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\n' ||
                           value[begin] == '\r')) {
        ++begin;
    }
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\n' || value[end - 1] == '\r')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string TrimSlashes(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && value[begin] == '/') ++begin;
    while (end > begin && value[end - 1] == '/') --end;
    return value.substr(begin, end - begin);
}

/// The host of an endpoint URL, with any scheme and path removed.
std::string HostOfEndpoint(const std::string& endpoint) {
    std::string rest = endpoint;
    const std::size_t scheme = rest.find("://");
    if (scheme != std::string::npos) rest = rest.substr(scheme + 3);
    const std::size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    return rest;
}

bool EndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string BodyToString(const std::vector<uint8_t>& body) {
    if (body.empty()) return std::string();
    return std::string(reinterpret_cast<const char*>(&body[0]), body.size());
}

/// Writes a streamed response body into a file.
///
/// The file is opened on the first byte, not up front: a `GET` that answers
/// 404 must leave nothing behind, or a failed fetch would be
/// indistinguishable from a published zero-length blob. `Restart` throws away
/// a previous attempt's bytes for the same reason — appending to them would
/// splice two responses.
class FileSink : public RetryableSink {
 public:
    explicit FileSink(const std::string& path) : path_(path), file_(0), written_(0), failed_(false) {}
    ~FileSink() { Close(); }

    core::Expected<core::Unit, std::string> Restart() {
        Close();
        // Removed rather than truncated in place, so a destination that was
        // never written to is never created.
        if (core::fs::Exists(path_)) {
            const core::Expected<core::Unit, std::string> removed = core::fs::RemoveFile(path_);
            if (!removed.ok()) return core::make_unexpected(removed.error());
        }
        written_ = 0;
        failed_ = false;
        return core::Unit();
    }

    bool Write(const uint8_t* data, size_t len) {
        if (failed_) return false;
        if (file_ == 0) {
            file_ = std::fopen(path_.c_str(), "wb");
            if (file_ == 0) {
                failed_ = true;
                return false;
            }
        }
        if (len == 0) return true;
        if (std::fwrite(data, 1, len, file_) != len) {
            failed_ = true;
            return false;
        }
        written_ += len;
        return true;
    }

    /// Closes the file and reports how many bytes it holds.
    core::Expected<uint64_t, std::string> Finish() {
        const bool close_failed = !Close();
        if (failed_ || close_failed) {
            return core::make_unexpected("write '" + path_ + "' failed");
        }
        return written_;
    }

    /// Closes and removes the file, for a response that must leave nothing.
    void Discard() {
        Close();
        if (core::fs::Exists(path_)) core::fs::RemoveFile(path_);
    }

 private:
    FileSink(const FileSink&);
    FileSink& operator=(const FileSink&);

    /// False when the buffered data could not be flushed, which is a write
    /// failure like any other and must not be mistaken for a complete file.
    bool Close() {
        if (file_ == 0) return true;
        const bool ok = std::fclose(file_) == 0;
        file_ = 0;
        return ok;
    }

    std::string path_;
    std::FILE* file_;
    uint64_t written_;
    bool failed_;
};

/// A server's `ETag` as sent, including its quotes.
///
/// The quotes are part of the token: a conditional request must echo the
/// value byte for byte, and stripping them produces a precondition that never
/// matches — which would look exactly like permanent contention.
core::Optional<std::string> EtagOf(const http::Response& response) {
    const http::HeaderMap::const_iterator it = response.headers.find("ETag");
    if (it == response.headers.end() || it->second.empty()) {
        return core::Optional<std::string>();
    }
    return core::Optional<std::string>(Trim(it->second));
}

std::string StatusMessage(const std::string& what, const http::Response& response) {
    std::ostringstream o;
    o << what << " returned status " << response.status;
    const std::string body = BodyToString(response.body);
    if (!body.empty()) {
        // The S3 error body names the condition (`PreconditionFailed`,
        // `NoSuchKey`), which is what an operator needs; the status alone
        // sends them to the wrong place.
        o << ": " << body.substr(0, 512);
    }
    return o.str();
}

/// Every occurrence of `<tag>...</tag>`, in document order.
std::vector<std::string> XmlAllElements(const std::string& xml, const std::string& tag) {
    std::vector<std::string> out;
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    std::size_t at = 0;
    while (true) {
        const std::size_t begin = xml.find(open, at);
        if (begin == std::string::npos) break;
        const std::size_t value_begin = begin + open.size();
        const std::size_t value_end = xml.find(close, value_begin);
        if (value_end == std::string::npos) break;
        out.push_back(xml.substr(value_begin, value_end - value_begin));
        at = value_end + close.size();
    }
    return out;
}

/// The five predefined XML entities, which is all an object key can contain
/// once a conforming server has escaped it.
std::string XmlUnescape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        if (value[i] != '&') {
            out.push_back(value[i]);
            ++i;
            continue;
        }
        if (value.compare(i, 5, "&amp;") == 0) {
            out.push_back('&');
            i += 5;
        } else if (value.compare(i, 4, "&lt;") == 0) {
            out.push_back('<');
            i += 4;
        } else if (value.compare(i, 4, "&gt;") == 0) {
            out.push_back('>');
            i += 4;
        } else if (value.compare(i, 6, "&quot;") == 0) {
            out.push_back('"');
            i += 6;
        } else if (value.compare(i, 6, "&apos;") == 0) {
            out.push_back('\'');
            i += 6;
        } else {
            out.push_back('&');
            ++i;
        }
    }
    return out;
}

}  // namespace

bool IsAliyunHost(const std::string& host) {
    return EndsWith(host, ".aliyuncs.com") || EndsWith(host, ".aliyun-inc.com");
}

std::vector<std::string> ParseListObjectsKeys(const std::string& xml) {
    // Scoped to `<Contents>` so a `<CommonPrefixes><Prefix>` or the
    // response's own `<Prefix>` echo cannot be mistaken for an object.
    std::vector<std::string> keys;
    const std::vector<std::string> contents = XmlAllElements(xml, "Contents");
    for (std::size_t i = 0; i < contents.size(); ++i) {
        const std::vector<std::string> key = XmlAllElements(contents[i], "Key");
        if (!key.empty()) keys.push_back(XmlUnescape(key[0]));
    }
    return keys;
}

bool ParseListObjectsTruncated(const std::string& xml) {
    const std::vector<std::string> flag = XmlAllElements(xml, "IsTruncated");
    return !flag.empty() && Trim(flag[0]) == "true";
}

RepositoryResult<OssClientConfig> OssClientConfig::FromBackendConfig(
    const cfg::OssBackendConfig& cfg) {
    OssClientConfig out;

    out.bucket = Trim(cfg.bucket);
    if (out.bucket.empty()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("backend.oss.bucket cannot be empty"));
    }
    out.endpoint = Trim(cfg.endpoint);
    if (out.endpoint.empty()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("backend.oss.endpoint cannot be empty"));
    }
    // Rust validates the region here even though only overlaybd's global
    // config consumes it, so an incomplete OSS section fails during backend
    // construction rather than at the first signed request.
    out.region = cfg.region.has_value() ? Trim(*cfg.region) : std::string();
    if (out.region.empty()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("backend.oss.region is required"));
    }
    out.prefix = cfg.prefix.has_value() ? TrimSlashes(Trim(*cfg.prefix)) : std::string();

    // Rust runs a `credential_process`. Refused rather than ignored: silently
    // degrading to anonymous fails later as a 403 that looks like a
    // permissions problem rather than a missing credential.
    if (cfg.credential_process.has_value() && !Trim(*cfg.credential_process).empty()) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "backend.oss.credential_process; supply access_key_id/access_key_secret"));
    }

    const std::string key_id =
        cfg.access_key_id.has_value() ? Trim(*cfg.access_key_id) : std::string();
    const std::string key_secret =
        cfg.access_key_secret.has_value() ? Trim(*cfg.access_key_secret) : std::string();
    // Rust passes `allow_anonymous: false` for `backend.oss`: the catalog is
    // private, and an unsigned request would 403 with no indication that the
    // credential was simply absent.
    if (key_id.empty()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("backend.oss.access_key_id is required"));
    }
    if (key_secret.empty()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("backend.oss.access_key_secret is required"));
    }
    out.credentials.access_key_id = key_id;
    out.credentials.secret_access_key = key_secret;
    if (cfg.security_token.has_value()) {
        out.credentials.security_token = Trim(*cfg.security_token);
    }

    if (cfg.addressing_style.has_value()) {
        out.addressing_style = core::Optional<sov::AddressingStyle>(
            *cfg.addressing_style == cfg::OssAddressingStyle::Virtual ? sov::kAddressingVirtual
                                                                      : sov::kAddressingPath);
    }
    return out;
}

RepositoryResult<OssClient*> OssClient::New(const OssClientConfig& config) {
    return NewWithTransport(config, Transport());
}

RepositoryResult<OssClient*> OssClient::NewWithTransport(const OssClientConfig& config,
                                                         const Transport& transport) {
    if (!transport && !http::Available()) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "object storage requires libcurl, which this build was compiled without"));
    }

    sov::AddressingStyle style = sov::kAddressingPath;
    if (config.addressing_style.has_value()) {
        style = *config.addressing_style;
    } else {
        const core::Expected<sov::AddressingStyle, std::string> detected =
            sov::DetectAddressingStyle(config.endpoint, config.bucket);
        if (!detected.ok()) {
            return core::make_unexpected(RepositoryError::InvalidRequest(
                "backend.oss.endpoint: " + detected.error()));
        }
        style = detected.value();
    }

    OssClient* client = new OssClient();
    client->config_ = config;
    client->style_ = style;
    client->transport_ = transport;
    // Rust `aliyun_service_endpoint(..).is_some()`.
    client->can_append_ = IsAliyunHost(HostOfEndpoint(config.endpoint));
    return client;
}

std::string OssClient::FullKey(const std::string& key) const {
    if (config_.prefix.empty()) return key;
    return config_.prefix + "/" + key;
}

sov::ParsedOssUrl OssClient::LocationOf(const std::string& key) const {
    sov::ParsedOssUrl location;
    location.bucket = config_.bucket;
    location.key = key;
    location.region = config_.region;
    location.endpoint = config_.endpoint;
    location.addressing_style = style_;
    return location;
}

core::Expected<http::Response, std::string> OssClient::Send(
    const std::string& method, const std::string& key, const std::string& canonical_query,
    const http::HeaderMap& extra_headers, const std::string& body, RetryableSink* sink) const {
    const sov::ParsedOssUrl location = LocationOf(key);

    // Two passes, as the overlaybd backend does: the operation, then the same
    // operation again after a permission-denied answer. The credential retry
    // is added on top of the transport budget rather than taken out of it, so
    // a 5xx blip and an expired credential can both be absorbed.
    const uint32_t passes = 2;
    std::string last_error;

    for (uint32_t pass = 0; pass < passes; ++pass) {
        const uint32_t attempts = config_.retry_count + 1;
        for (uint32_t attempt = 0; attempt < attempts; ++attempt) {
            // Before the request, not after a failure: an attempt that died
            // mid-body has already written part of the file, and the next one
            // must not continue from there.
            if (sink != 0) {
                const core::Expected<core::Unit, std::string> restarted = sink->Restart();
                if (!restarted.ok()) return core::make_unexpected(restarted.error());
            }

            http::Request request;
            request.method = method;
            request.url = location.ObjectUrl();
            if (!canonical_query.empty()) request.url += "?" + canonical_query;
            request.timeout_secs = config_.timeout_secs;
            // A catalog document is not a blob read; redirects are off so a
            // misconfigured endpoint fails visibly instead of following a
            // redirect to somewhere unsigned.
            request.follow_redirects = false;
            request.headers = extra_headers;
            request.body_sink = sink;
            if (!body.empty()) {
                request.body.assign(body.begin(), body.end());
            }

            sov::S3SigningContext ctx;
            ctx.method = method;
            ctx.canonical_uri = location.CanonicalUri();
            // Part of the signature: SigV4 hashes the canonical query string,
            // so a query that is sent but not signed is rejected. It must
            // already be sorted and encoded, which the callers below do.
            ctx.canonical_query = canonical_query;
            ctx.host = location.Host();
            ctx.region = config_.region;
            ctx.service = "s3";
            // Re-signed every attempt: the signature covers a timestamp the
            // server only accepts inside a window, so a retry after a stall
            // must not reuse the old one.
            ctx.amz_date = sov::S3AmzDateNow();
            ctx.payload_sha256_hex =
                body.empty() ? std::string(sov::kS3EmptyPayloadSha256)
                             : core::Sha256Hex(body);
            // The conditional headers are deliberately outside the signed
            // set, which is `host`, `x-amz-date`, `x-amz-content-sha256` and
            // the token. S3 does not require them to be signed, and adding
            // them would make the signature depend on a header set that
            // varies per call.
            sov::S3SignRequest(config_.credentials, ctx, &request);

            const core::Expected<http::Response, std::string> response =
                transport_ ? transport_(request) : http::Perform(request);
            if (!response.ok()) {
                // The request never landed, so retrying it is safe.
                last_error = response.error();
                continue;
            }

            // Only a 403 earns the second pass, and only when there is a
            // credential to re-sign with. What changes is the timestamp,
            // which is what clock skew or a just-rotated token needs.
            if (response.value().status == 403 && pass + 1 < passes &&
                !config_.credentials.access_key_id.empty()) {
                last_error = StatusMessage("oss " + method + " '" + key + "'", response.value());
                break;
            }

            // 5xx is the server asking to be retried. A 4xx is its final
            // answer and is handed back as-is: the callers distinguish 404
            // from 412, and repeating it would only stall.
            if (response.value().status >= 500 && attempt + 1 < attempts) {
                last_error = StatusMessage("oss " + method + " '" + key + "'", response.value());
                continue;
            }
            return response.value();
        }
    }

    if (last_error.empty()) last_error = "oss " + method + " '" + key + "' failed";
    return core::make_unexpected(last_error);
}

RepositoryResult<ObjectRead> OssClient::GetWithEtag(const std::string& key) const {
    const std::string full = FullKey(key);
    const core::Expected<http::Response, std::string> response =
        Send("GET", full, std::string(), http::HeaderMap(), std::string());
    if (!response.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("oss get '" + full + "'", response.error()));
    }
    // 404 is an answer, not a failure: an update probes for a document it may
    // still have to create.
    if (response.value().status == 404) return ObjectRead::NotFound();
    if (!response.value().IsSuccess()) {
        return core::make_unexpected(
            RepositoryError::Backend(StatusMessage("oss get '" + full + "'", response.value())));
    }
    // Bytes and ETag come from the same response, so they describe the same
    // version by construction. An absent ETag is reported as such rather than
    // invented; the document layer refuses to update without one.
    return ObjectRead::Found(BodyToString(response.value().body), EtagOf(response.value()));
}

RepositoryResult<bool> OssClient::PutConditionally(const std::string& key,
                                                   const std::string& bytes,
                                                   const core::Optional<std::string>& etag) {
    const std::string full = FullKey(key);
    http::HeaderMap headers;
    if (etag.has_value()) {
        // Replace only this exact version.
        headers["If-Match"] = *etag;
    } else {
        // Create only if absent. `*` matches any existing object, so the
        // precondition fails exactly when something is already there.
        headers["If-None-Match"] = "*";
    }

    const core::Expected<http::Response, std::string> response =
        Send("PUT", full, std::string(), headers, bytes);
    if (!response.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("oss put '" + full + "'", response.error()));
    }
    // 412 is a lost race, not an error — the caller re-reads and decides
    // again. 409 is the same answer from stores that report a create-only
    // collision that way.
    if (response.value().status == 412 || response.value().status == 409) return false;
    if (!response.value().IsSuccess()) {
        return core::make_unexpected(
            RepositoryError::Backend(StatusMessage("oss put '" + full + "'", response.value())));
    }
    return true;
}

RepositoryResult<core::Unit> OssClient::Put(const std::string& key, const std::string& bytes) {
    const std::string full = FullKey(key);
    const core::Expected<http::Response, std::string> response =
        Send("PUT", full, std::string(), http::HeaderMap(), bytes);
    if (!response.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("oss put '" + full + "'", response.error()));
    }
    if (!response.value().IsSuccess()) {
        return core::make_unexpected(
            RepositoryError::Backend(StatusMessage("oss put '" + full + "'", response.value())));
    }
    return core::Unit();
}

RepositoryResult<core::Unit> OssClient::Delete(const std::string& key) {
    const std::string full = FullKey(key);
    const core::Expected<http::Response, std::string> response =
        Send("DELETE", full, std::string(), http::HeaderMap(), std::string());
    if (!response.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("oss delete '" + full + "'", response.error()));
    }
    // A missing object is success, as it is on every backend: S3 answers 204
    // either way, but a store that reports 404 must not turn an idempotent
    // delete into a failure.
    if (response.value().status == 404) return core::Unit();
    if (!response.value().IsSuccess()) {
        return core::make_unexpected(RepositoryError::Backend(
            StatusMessage("oss delete '" + full + "'", response.value())));
    }
    return core::Unit();
}

RepositoryResult<std::vector<std::string> > OssClient::ListKeysPage(
    const std::string& prefix, const core::Optional<std::string>& start_after,
    std::size_t limit) const {
    if (limit == 0) return std::vector<std::string>();

    const std::string full_prefix = FullKey(prefix);
    // ListObjectsV2 caps a page at 1000; a larger limit is satisfied by the
    // server's own maximum rather than by asking for something it will
    // silently clamp.
    const std::size_t page = limit > 1000 ? 1000 : limit;

    // The canonical query string must be sorted by key, which SigV4 hashes.
    // `list-type` < `max-keys` < `prefix` < `start-after` already holds, so
    // the order below is the sorted one.
    std::ostringstream query;
    query << "list-type=2";
    query << "&max-keys=" << page;
    query << "&prefix=" << http::EncodeQueryValue(full_prefix);
    if (start_after.has_value()) {
        query << "&start-after=" << http::EncodeQueryValue(FullKey(*start_after));
    }

    // A bucket-level request: the key is empty, which makes the canonical URI
    // `/` for virtual-hosted style and `/<bucket>/` for path style.
    const core::Expected<http::Response, std::string> response =
        Send("GET", std::string(), query.str(), http::HeaderMap(), std::string());
    if (!response.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("oss list '" + full_prefix + "'", response.error()));
    }
    if (!response.value().IsSuccess()) {
        return core::make_unexpected(RepositoryError::Backend(
            StatusMessage("oss list '" + full_prefix + "'", response.value())));
    }

    const std::vector<std::string> full_keys =
        ParseListObjectsKeys(BodyToString(response.value().body));

    // The prefix is stripped back off, so a caller sees the same keys it
    // would on a repository with no prefix configured.
    std::vector<std::string> keys;
    keys.reserve(full_keys.size());
    const std::string strip = config_.prefix.empty() ? std::string() : config_.prefix + "/";
    for (std::size_t i = 0; i < full_keys.size(); ++i) {
        if (!strip.empty()) {
            if (full_keys[i].size() <= strip.size() ||
                full_keys[i].compare(0, strip.size(), strip) != 0) {
                // Outside this repository's prefix: the server answered with
                // something that was not asked for, and inventing a relative
                // key from it would corrupt the listing.
                return core::make_unexpected(RepositoryError::Backend(
                    "oss list '" + full_prefix + "' returned key outside the configured prefix: '" +
                    full_keys[i] + "'"));
            }
            keys.push_back(full_keys[i].substr(strip.size()));
        } else {
            keys.push_back(full_keys[i]);
        }
        if (keys.size() >= limit) break;
    }
    return keys;
}

// ---- the blob half --------------------------------------------------------

RepositoryResult<bool> OssClient::Exists(const std::string& key) const {
    const std::string full = FullKey(key);
    const core::Expected<http::Response, std::string> response =
        Send("HEAD", full, std::string(), http::HeaderMap(), std::string());
    if (!response.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("oss head '" + full + "'", response.error()));
    }
    // 404 is the answer the caller asked for, not a failure.
    if (response.value().status == 404) return false;
    if (!response.value().IsSuccess()) {
        // Anything else -- 403 above all -- is *not* absence. Reporting it as
        // "no" would make a publish skip an upload it never verified, or a
        // reader conclude a layer was never published.
        return core::make_unexpected(
            RepositoryError::Backend(StatusMessage("oss head '" + full + "'", response.value())));
    }
    return true;
}

std::string OssClient::ObjectUrl(const std::string& key) const {
    return "s3://" + config_.bucket + "/" + FullKey(key);
}

RepositoryResult<uint64_t> OssClient::GetToFile(const std::string& key,
                                                const std::string& destination) const {
    const std::string full = FullKey(key);

    const core::Optional<std::string> parent = core::fs::Parent(destination);
    if (parent.has_value() && !parent->empty()) {
        const core::Expected<core::Unit, std::string> made = core::fs::CreateDirAll(*parent);
        if (!made.ok()) {
            return core::make_unexpected(
                RepositoryError::Backend("create '" + *parent + "'", made.error()));
        }
    }

    FileSink sink(destination);
    const core::Expected<http::Response, std::string> response =
        Send("GET", full, std::string(), http::HeaderMap(), std::string(), &sink);
    if (!response.ok()) {
        sink.Discard();
        return core::make_unexpected(
            RepositoryError::Backend("oss download '" + full + "'", response.error()));
    }
    if (response.value().status == 404) {
        // Nothing was written -- the transport only feeds the sink on a 2xx --
        // so there is no file to remove, and the caller can tell "not
        // published yet" from a transport failure.
        sink.Discard();
        return core::make_unexpected(RepositoryError::ArtifactNotFound("'" + full + "'"));
    }
    if (!response.value().IsSuccess()) {
        sink.Discard();
        return core::make_unexpected(RepositoryError::Backend(
            StatusMessage("oss download '" + full + "'", response.value())));
    }

    const core::Expected<uint64_t, std::string> written = sink.Finish();
    if (!written.ok()) {
        sink.Discard();
        return core::make_unexpected(
            RepositoryError::Backend("oss download '" + full + "'", written.error()));
    }

    // A response shorter than its own `Content-Length` is a truncated
    // download. Returning its size as the blob's would publish a layer whose
    // tail is missing, which the reader only discovers as a corrupt image.
    uint64_t declared = 0;
    if (response.value().ContentLength(&declared) && declared != written.value()) {
        sink.Discard();
        std::ostringstream o;
        o << "oss download '" << full << "' delivered " << written.value()
          << " bytes against a declared length of " << declared;
        return core::make_unexpected(RepositoryError::Backend(o.str()));
    }
    return written.value();
}

}  // namespace oss
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
