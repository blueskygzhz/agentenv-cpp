// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/registryfs_v2.rs (read path)
#include "agentenv/storage/overlaybd/registryfs.h"

#include "agentenv/storage/overlaybd/metrics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <sys/time.h>
#include <time.h>

#include "agentenv/core/base64.h"
#include "agentenv/core/json.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

uint64_t NowMs() {
    struct timeval tv;
    ::gettimeofday(&tv, 0);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
           static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
}

/// Rust `remaining_timeout` — what is left of the deadline.
///
/// An exhausted deadline is an error rather than a zero timeout: zero means
/// "no limit" to libcurl, so passing it through would turn a timed-out read
/// into one that blocks indefinitely.
core::Expected<long, std::string> RemainingTimeoutSecs(uint64_t deadline_ms) {
    const uint64_t now = NowMs();
    if (now >= deadline_ms) {
        return core::make_unexpected(std::string("operation timed out"));
    }
    const uint64_t left_ms = deadline_ms - now;
    // Rounded up: libcurl's timeout is in whole seconds, and rounding down
    // would floor a sub-second remainder to "no limit".
    return static_cast<long>((left_ms + 999ULL) / 1000ULL);
}

/// Rust `RETRY_BACKOFF` — 1ms between attempts.
///
/// Short on purpose: it exists to let a transient connection reset clear, not
/// to back off a loaded server.
void SleepRetryBackoff() {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000L;  // 1ms
    ::nanosleep(&ts, 0);
}

std::string Trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                     s[e - 1] == '\n')) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string LowerAscii(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
    return out;
}

bool StartsWithIgnoreCase(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

/// Rust `parse_kv_map` — the comma-separated parameter list of a
/// `WWW-Authenticate` header.
///
/// Quote-aware, because a `scope` value routinely contains commas
/// (`repository:a:pull,push`); splitting on every comma would truncate it.
std::map<std::string, std::string> ParseKvMap(const std::string& s) {
    std::vector<std::string> parts;
    std::string buf;
    bool in_quotes = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (ch == '"') {
            in_quotes = !in_quotes;
            buf.push_back(ch);
        } else if (ch == ',' && !in_quotes) {
            if (!Trim(buf).empty()) parts.push_back(Trim(buf));
            buf.clear();
        } else {
            buf.push_back(ch);
        }
    }
    if (!Trim(buf).empty()) parts.push_back(Trim(buf));

    std::map<std::string, std::string> kv;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string::size_type eq = parts[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string key = LowerAscii(Trim(parts[i].substr(0, eq)));
        std::string val = Trim(parts[i].substr(eq + 1));
        if (val.size() >= 2 && val[0] == '"' && val[val.size() - 1] == '"') {
            val = val.substr(1, val.size() - 2);
        }
        kv[key] = val;
    }
    return kv;
}

struct SplitUrl {
    std::string scheme;
    std::string host;   // may include :port
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
    const std::string before = q == std::string::npos ? rest : rest.substr(0, q);
    out->query = q == std::string::npos ? std::string() : rest.substr(q + 1);

    const std::string::size_type slash = before.find('/');
    if (slash == std::string::npos) {
        out->host = before;
        out->path.clear();
    } else {
        out->host = before.substr(0, slash);
        out->path = before.substr(slash);
    }
    return !out->host.empty();
}

std::vector<std::string> PathSegments(const std::string& path) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        if (i >= path.size()) break;
        const std::string::size_type slash = path.find('/', i);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        out.push_back(path.substr(i, end - i));
        i = end;
    }
    return out;
}

/// Rust `normalize_url` — a leading slash is stripped so the same blob is not
/// cached under two keys.
std::string NormalizeUrl(const std::string& url) {
    size_t i = 0;
    while (i < url.size() && url[i] == '/') ++i;
    return url.substr(i);
}

std::string AppendQueryPair(const std::string& url, const std::string& key,
                            const std::string& value) {
    const bool has_query = url.find('?') != std::string::npos;
    return url + (has_query ? "&" : "?") + http::EncodeQueryValue(key) + "=" +
           http::EncodeQueryValue(value);
}

/// Rust `parse_blob_segments` — the address prefixes a credential entry may be
/// keyed by.
///
/// For `https://host/v2/library/nginx/blobs/sha256:x` this yields
/// `[host, library, nginx, blobs]`: the host, then the path words between
/// `v2` and the digest. `blobs` is included because Rust slices
/// `words[2..len-1]` without filtering it out, and the prefixes built from
/// this list are compared verbatim -- dropping it here would stop matching a
/// key that a Rust-written config can legitimately contain.
std::vector<std::string> ParseBlobSegments(const std::string& remote_url) {
    std::string without_scheme;
    if (remote_url.compare(0, 7, "http://") == 0) {
        without_scheme = remote_url.substr(7);
    } else if (remote_url.compare(0, 8, "https://") == 0) {
        without_scheme = remote_url.substr(8);
    } else {
        return std::vector<std::string>();
    }

    std::vector<std::string> words;
    std::string::size_type i = 0;
    while (i < without_scheme.size()) {
        const std::string::size_type slash = without_scheme.find('/', i);
        const std::string::size_type end =
            slash == std::string::npos ? without_scheme.size() : slash;
        if (end > i) words.push_back(without_scheme.substr(i, end - i));
        i = end == without_scheme.size() ? end : end + 1;
    }
    if (words.empty()) return std::vector<std::string>();

    std::vector<std::string> seg;
    seg.push_back(words[0]);
    if (words.size() > 3) {
        for (size_t k = 2; k + 1 < words.size(); ++k) seg.push_back(words[k]);
    }
    return seg;
}

/// Rust `address_matches` — the key must equal one of the `/`-joined prefixes.
///
/// Exact equality against each prefix, never a substring test: a substring
/// match would let a key like `evil.example.com/registry.io` match a blob on
/// `registry.io`, and the credential would be sent to the wrong registry.
bool AddressMatches(const std::string& addr, const std::vector<std::string>& seg) {
    std::string prefix;
    for (size_t i = 0; i < seg.size(); ++i) {
        if (!prefix.empty()) prefix += "/";
        prefix += seg[i];
        if (addr == prefix) return true;
    }
    return false;
}

/// Rust `parse_auths` — pick the credential entry whose key addresses this
/// blob, and read either its base64 `auth` field or its explicit
/// `username`/`password` pair.
void ParseAuths(const core::Json& auths, const std::string& remote_url,
                std::string* username, std::string* password) {
    username->clear();
    password->clear();
    if (auths.kind() != core::Json::Kind::Object) return;

    const std::vector<std::string> seg = ParseBlobSegments(remote_url);

    const core::JsonObject& members = auths.as_object();
    for (core::JsonObject::const_iterator it = members.begin(); it != members.end(); ++it) {
        if (!AddressMatches(it->first, seg)) continue;
        if (it->second.kind() != core::Json::Kind::Object) continue;
        const core::JsonObject& entry = it->second.as_object();

        // `auth` first, as Rust does: a config carrying both is answered from
        // the combined field.
        const core::JsonObject::const_iterator packed = entry.find("auth");
        if (packed != entry.end() && packed->second.kind() == core::Json::Kind::String) {
            // Standard alphabet by definition of the Docker config format.
            const core::Expected<std::string, std::string> decoded =
                core::Base64StandardDecode(packed->second.as_string());
            if (decoded.ok()) {
                const std::string::size_type colon = decoded.value().find(':');
                if (colon != std::string::npos) {
                    *username = decoded.value().substr(0, colon);
                    *password = decoded.value().substr(colon + 1);
                    return;
                }
            }
        }

        // Only when `auth` was absent or unusable. A malformed `auth` falls
        // through to these rather than failing the lookup, matching Rust's
        // `if cred.is_none()`.
        const core::JsonObject::const_iterator u = entry.find("username");
        const core::JsonObject::const_iterator pw = entry.find("password");
        if (u != entry.end() && u->second.kind() == core::Json::Kind::String &&
            pw != entry.end() && pw->second.kind() == core::Json::Kind::String) {
            *username = u->second.as_string();
            *password = pw->second.as_string();
            return;
        }

        // Rust keeps scanning later keys when this entry yielded nothing.
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

RegistryFsOptions RegistryFsOptions::FromGlobalConfig(const GlobalConfig& cfg) {
    RegistryFsOptions out;
    out.timeout_secs = kRegistryDefaultTimeoutSecs;
    // Zero or negative means "unset", not "do not retry": no attempts at all
    // would make every transient failure fatal.
    out.retry_count = cfg.download.try_cnt <= 0 ? kRegistryDefaultRetryCount
                                                : static_cast<uint32_t>(cfg.download.try_cnt);
    out.user_agent = cfg.user_agent;
    out.credential = cfg.credential_config;
    // Only when p2p is on; a configured address with the feature off must not
    // silently route traffic through the accelerator.
    out.accelerate_address = cfg.p2p_config.enable ? cfg.p2p_config.address : std::string();
    out.cert_file = cfg.cert_config.cert_file;
    out.key_file = cfg.cert_config.key_file;
    return out;
}

// ---------------------------------------------------------------------------
// Challenge parsing
// ---------------------------------------------------------------------------

core::Expected<RegistryAuthChallenge, std::string> ParseRegistryAuthChallenge(
    const std::string& challenge, bool want_scope) {
    RegistryAuthChallenge out;
    if (StartsWithIgnoreCase(challenge, "basic ")) {
        out.kind = kRegistryAuthBasic;
        return out;
    }
    if (!StartsWithIgnoreCase(challenge, "bearer ")) {
        return core::make_unexpected("unsupported auth challenge: " + challenge);
    }

    const std::map<std::string, std::string> kv = ParseKvMap(Trim(challenge.substr(7)));
    const std::map<std::string, std::string>::const_iterator realm = kv.find("realm");
    if (realm == kv.end()) {
        return core::make_unexpected(std::string("auth challenge missing realm"));
    }
    const std::map<std::string, std::string>::const_iterator service = kv.find("service");
    if (service == kv.end()) {
        return core::make_unexpected(std::string("auth challenge missing service"));
    }

    out.kind = kRegistryAuthBearer;
    out.realm = realm->second;
    out.service = service->second;
    // Dropped for the registry-wide ping: that challenge is cached per origin,
    // and a scope naming one repository must not be reused for another.
    if (want_scope) {
        const std::map<std::string, std::string>::const_iterator scope = kv.find("scope");
        if (scope != kv.end() && !scope->second.empty()) out.scope = scope->second;
    }
    return out;
}

core::Expected<std::string, std::string> BuildBearerAuthUrl(const std::string& realm,
                                                               const std::string& service,
                                                               const std::string& scope) {
    SplitUrl parts;
    if (!SplitUrlParts(realm, &parts)) {
        return core::make_unexpected("invalid realm URL in auth challenge: " + realm);
    }
    std::string url = AppendQueryPair(realm, "service", service);
    // Optional per RFC 6750 section 3, so an empty scope is omitted rather
    // than sent blank -- a blank scope is a different request.
    if (!scope.empty()) url = AppendQueryPair(url, "scope", scope);
    return url;
}

std::string BuildBasicAuthHeader(const std::string& username, const std::string& password) {
    // Standard alphabet, per RFC 7617. The URL-safe one differs for any
    // credential whose encoding reaches index 62 or 63.
    return "Basic " + core::Base64StandardEncode(username + ":" + password);
}

core::Optional<BlobAuthContext> ParseBlobAuthContext(const std::string& remote_url) {
    SplitUrl parts;
    if (!SplitUrlParts(remote_url, &parts)) return core::Optional<BlobAuthContext>();
    if (parts.scheme != "http" && parts.scheme != "https") {
        return core::Optional<BlobAuthContext>();
    }

    const std::vector<std::string> segments = PathSegments(parts.path);
    // `/v2/<repo...>/blobs/<digest>` is the only shape that implies a scope.
    // Anything else (a redirected CDN URL, for instance) has no `/v2/` to
    // ping, and inventing one would send credentials to a host that never
    // asked for them.
    if (segments.size() < 4 || segments[0] != "v2") {
        return core::Optional<BlobAuthContext>();
    }
    const size_t blobs_index = segments.size() - 2;
    if (segments[blobs_index] != "blobs") return core::Optional<BlobAuthContext>();
    if (segments[segments.size() - 1].empty()) return core::Optional<BlobAuthContext>();

    std::string repo;
    for (size_t i = 1; i < blobs_index; ++i) {
        if (segments[i].empty()) return core::Optional<BlobAuthContext>();
        if (!repo.empty()) repo += "/";
        repo += segments[i];
    }
    if (repo.empty()) return core::Optional<BlobAuthContext>();

    BlobAuthContext ctx;
    ctx.origin = parts.scheme + "://" + parts.host;
    // Query and fragment are dropped: the ping is a different request, and a
    // blob's query string is often a signed credential.
    ctx.ping_url = ctx.origin + "/v2/";
    ctx.scope = "repository:" + repo + ":pull";
    return core::Optional<BlobAuthContext>(ctx);
}

// ---------------------------------------------------------------------------
// RegistryFs — construction
// ---------------------------------------------------------------------------

core::Expected<std::shared_ptr<RegistryFs>, std::string> RegistryFs::WithOptions(
    const RegistryFsOptions& options) {
    // Validated up front so a bad config fails at construction rather than on
    // the first blob read.
    const std::string mode = options.credential.mode;
    if (!mode.empty() && mode != "file" && mode != "http") {
        return core::make_unexpected("invalid credential mode " + mode);
    }
    if (mode == "http" && options.credential.path.empty()) {
        return core::make_unexpected(
            std::string("credentialConfig.path cannot be empty when mode=http"));
    }

    std::shared_ptr<RegistryFs> fs(new RegistryFs());
    fs->options_ = options;
    fs->accelerate_address_ = options.accelerate_address;
    return fs;
}

core::Expected<std::shared_ptr<RegistryFs>, std::string> RegistryFs::FromGlobalConfig(
    const GlobalConfig& cfg) {
    return WithOptions(RegistryFsOptions::FromGlobalConfig(cfg));
}

core::Expected<std::shared_ptr<RegistryFs>, std::string> RegistryFs::WithStaticCredentials(
    const RegistryFsOptions& options, const std::string& username,
    const std::string& password) {
    RegistryFsOptions copy = options;
    // Cleared so the validation above cannot reject a mode that will never be
    // consulted: static credentials replace it entirely.
    copy.credential = CredentialConfig();

    const core::Expected<std::shared_ptr<RegistryFs>, std::string> fs = WithOptions(copy);
    if (!fs.ok()) return fs;
    fs.value()->static_credentials_ = true;
    fs.value()->static_username_ = username;
    fs.value()->static_password_ = password;
    return fs;
}

// ---------------------------------------------------------------------------
// Credentials
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> RegistryFs::ResolveCredentials(
    const std::string& remote_url, std::string* username, std::string* password) {
    username->clear();
    password->clear();

    if (static_credentials_) {
        *username = static_username_;
        *password = static_password_;
        return core::Unit();
    }

    const std::string& mode = options_.credential.mode;
    // Anonymous: a public registry needs no credentials, so this is a normal
    // outcome rather than a missing configuration.
    if (mode.empty()) return core::Unit();

    if (mode == "file") {
        std::FILE* f = std::fopen(options_.credential.path.c_str(), "rb");
        if (f == 0) {
            return core::make_unexpected("read credential file " + options_.credential.path +
                                         " failed");
        }
        std::string raw;
        char buf[8192];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) raw.append(buf, n);
        std::fclose(f);

        const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(raw);
        if (!parsed.ok()) {
            return core::make_unexpected(std::string("parse credential file json failed"));
        }
        if (parsed.value().kind() != core::Json::Kind::Object) {
            return core::make_unexpected(std::string("credential file must be a JSON object"));
        }
        const core::JsonObject& root = parsed.value().as_object();
        const core::JsonObject::const_iterator auths = root.find("auths");
        if (auths != root.end()) ParseAuths(auths->second, remote_url, username, password);
        return core::Unit();
    }

    if (mode == "http") {
        http::Request request;
        request.method = "GET";
        request.url = AppendQueryPair(options_.credential.path, "remote_url", remote_url);
        // The credential endpoint's own timeout, which is normally far shorter
        // than a blob read's.
        request.timeout_secs = options_.credential.timeout <= 0
                                   ? 1
                                   : static_cast<long>(options_.credential.timeout);
        if (!options_.user_agent.empty()) request.headers["User-Agent"] = options_.user_agent;
        ApplyClientCert(&request);

        const core::Expected<http::Response, std::string> response = http::Perform(request);
        if (!response.ok()) {
            return core::make_unexpected("request credential http failed: " +
                                         response.error());
        }
        if (!response.value().IsSuccess()) {
            std::ostringstream o;
            o << "credential http request failed with status " << response.value().status;
            return core::make_unexpected(o.str());
        }

        const std::string text(
            response.value().body.empty()
                ? std::string()
                : std::string(reinterpret_cast<const char*>(&response.value().body[0]),
                              response.value().body.size()));
        const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(text);
        if (!parsed.ok() || parsed.value().kind() != core::Json::Kind::Object) {
            return core::make_unexpected(
                std::string("parse credential http response json failed"));
        }
        const core::JsonObject& root = parsed.value().as_object();

        // `success` is reported in the body rather than the status, so a 200
        // carrying a failure must still be refused.
        const core::JsonObject::const_iterator success = root.find("success");
        if (success != root.end() && success->second.kind() == core::Json::Kind::Bool &&
            !success->second.as_bool()) {
            std::string trace;
            const core::JsonObject::const_iterator tid = root.find("traceId");
            if (tid != root.end() && tid->second.kind() == core::Json::Kind::String) {
                trace = tid->second.as_string();
            }
            return core::make_unexpected("credential http response failed, traceId=" + trace);
        }

        const core::JsonObject::const_iterator data = root.find("data");
        if (data != root.end() && data->second.kind() == core::Json::Kind::Object) {
            const core::JsonObject& inner = data->second.as_object();
            const core::JsonObject::const_iterator auths = inner.find("auths");
            if (auths != inner.end()) {
                ParseAuths(auths->second, remote_url, username, password);
            }
        }
        return core::Unit();
    }

    return core::make_unexpected("invalid credential mode " + mode);
}

// ---------------------------------------------------------------------------
// Auth challenge and token
// ---------------------------------------------------------------------------

void RegistryFs::ApplyClientCert(http::Request* request) const {
    // Rust `build_http_client`: the identity is attached only when *both*
    // paths are set. One without the other cannot form a PEM identity, and
    // sending a cert with no key would fail the handshake rather than fall
    // back to anonymous TLS.
    if (options_.cert_file.empty() || options_.key_file.empty()) return;
    request->client_cert_file = options_.cert_file;
    request->client_key_file = options_.key_file;
}

void RegistryFs::MarkProbePerUrl(const std::string& origin) {
    std::lock_guard<std::mutex> lk(cache_mu_);
    CachedChallenge entry;
    entry.challenge.kind = kRegistryAuthProbePerUrl;
    // Shorter TTL than a real answer: this is a fallback after the ping
    // failed, so it should be retried sooner.
    entry.expire_at_ms = NowMs() + kRegistryProbePerUrlLifeSecs * 1000ULL;
    challenges_[origin] = entry;
}

core::Expected<RegistryAuthChallenge, std::string> RegistryFs::GetOrCreateRegistryChallenge(
    const BlobAuthContext& ctx) {
    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        const std::map<std::string, CachedChallenge>::const_iterator found =
            challenges_.find(ctx.origin);
        if (found != challenges_.end() && found->second.expire_at_ms > NowMs()) {
            return found->second.challenge;
        }
    }

    http::Request request;
    request.method = "GET";
    request.url = ctx.ping_url;
    request.timeout_secs = static_cast<long>(options_.timeout_secs);
    if (!options_.user_agent.empty()) request.headers["User-Agent"] = options_.user_agent;
    // The challenge rides on the 401 itself, so a redirect would hide it.
    request.follow_redirects = false;
    ApplyClientCert(&request);

    RegistryAuthChallenge challenge;
    const core::Expected<http::Response, std::string> response = http::Perform(request);
    if (!response.ok()) {
        // A ping that cannot even be sent falls back to per-URL probing
        // rather than failing the read: some registries answer `/v2/`
        // differently from the blob endpoint.
        MarkProbePerUrl(ctx.origin);
        RegistryAuthChallenge probe;
        probe.kind = kRegistryAuthProbePerUrl;
        return probe;
    }

    const long status = response.value().status;
    if (status != 401 && status != 403) {
        // Anything else means the registry did not ask for credentials.
        challenge.kind = kRegistryAuthNone;
    } else {
        const http::HeaderMap::const_iterator header =
            response.value().headers.find("WWW-Authenticate");
        if (header == response.value().headers.end()) {
            MarkProbePerUrl(ctx.origin);
            RegistryAuthChallenge probe;
            probe.kind = kRegistryAuthProbePerUrl;
            return probe;
        }
        // Scope dropped: this challenge is cached per origin.
        const core::Expected<RegistryAuthChallenge, std::string> parsed =
            ParseRegistryAuthChallenge(header->second, false);
        if (!parsed.ok()) {
            MarkProbePerUrl(ctx.origin);
            RegistryAuthChallenge probe;
            probe.kind = kRegistryAuthProbePerUrl;
            return probe;
        }
        challenge = parsed.value();
    }

    CachedChallenge entry;
    entry.challenge = challenge;
    entry.expire_at_ms = NowMs() + kRegistryAuthChallengeLifeSecs * 1000ULL;
    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        challenges_[ctx.origin] = entry;
    }
    return challenge;
}

core::Expected<RegistryAuthChallenge, std::string> RegistryFs::GetScopeAuth(
    const std::string& url, bool push) {
    http::Request request;
    if (push) {
        // Rust probes the push path with a POST so the challenge comes back
        // scoped for `pull,push`; a GET would only ever be answered with a
        // pull scope, and a token minted from it could not upload.
        request.method = "POST";
        request.headers["Content-Type"] = "application/octet-stream";
    } else {
        // A one-byte range: the cheapest GET that still reaches the blob
        // handler and provokes its challenge.
        request.method = "GET";
        request.has_range = true;
        request.range_begin = 0;
        request.range_end = 0;
    }
    request.url = url;
    request.timeout_secs = static_cast<long>(options_.timeout_secs);
    if (!options_.user_agent.empty()) request.headers["User-Agent"] = options_.user_agent;
    // The challenge rides on the 401 itself, so a followed redirect would
    // replace it with the CDN's answer.
    request.follow_redirects = false;
    ApplyClientCert(&request);

    const core::Expected<http::Response, std::string> response = http::Perform(request);
    if (!response.ok()) {
        return core::make_unexpected("challenge request failed for " + url + ": " +
                                     response.error());
    }

    RegistryAuthChallenge out;
    const long status = response.value().status;
    if (status != 401 && status != 403) {
        // Anything else means the endpoint did not ask for credentials.
        out.kind = kRegistryAuthNone;
        return out;
    }

    const http::HeaderMap::const_iterator header =
        response.value().headers.find("WWW-Authenticate");
    if (header == response.value().headers.end()) {
        return core::make_unexpected(std::string("no WWW-Authenticate in response"));
    }
    // Scope kept: a per-URL challenge names the repository being accessed, and
    // the token has to be requested for exactly it.
    return ParseRegistryAuthChallenge(header->second, true);
}

core::Expected<std::string, std::string> RegistryFs::GetToken(const std::string& remote_url,
                                                                 const std::string& auth_url) {
    std::string username, password;
    const core::Expected<core::Unit, std::string> creds =
        ResolveCredentials(remote_url, &username, &password);
    if (!creds.ok()) return core::make_unexpected(creds.error());
    const bool has_credentials = !username.empty();

    std::string token_url = auth_url;
    if (has_credentials) {
        // Per the Docker Distribution token spec the `account` parameter names
        // the user the token is for; Harbor requires it alongside the Basic
        // header rather than deriving it from one.
        token_url = AppendQueryPair(token_url, "account", username);
    }

    http::Request request;
    request.method = "GET";
    request.url = token_url;
    request.timeout_secs = static_cast<long>(options_.timeout_secs);
    if (!options_.user_agent.empty()) request.headers["User-Agent"] = options_.user_agent;
    if (has_credentials) {
        request.headers["Authorization"] = BuildBasicAuthHeader(username, password);
    }
    ApplyClientCert(&request);

    const core::Expected<http::Response, std::string> response = http::Perform(request);
    if (!response.ok()) {
        return core::make_unexpected("token request failed for " + auth_url + ": " +
                                     response.error());
    }
    if (response.value().status != 200) {
        std::ostringstream o;
        o << "token request returned non-200 for " << auth_url << ": status "
          << response.value().status;
        return core::make_unexpected(o.str());
    }

    const std::string text(
        response.value().body.empty()
            ? std::string()
            : std::string(reinterpret_cast<const char*>(&response.value().body[0]),
                          response.value().body.size()));
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(text);
    if (!parsed.ok() || parsed.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("parse token response json failed"));
    }
    const core::JsonObject& root = parsed.value().as_object();

    // `token` is the Docker Distribution spelling, `access_token` the OAuth2
    // one; registries differ, so both are accepted.
    core::JsonObject::const_iterator field = root.find("token");
    if (field != root.end() && field->second.kind() == core::Json::Kind::String) {
        return field->second.as_string();
    }
    field = root.find("access_token");
    if (field != root.end() && field->second.kind() == core::Json::Kind::String) {
        return field->second.as_string();
    }
    return core::make_unexpected(std::string("token response missing token/access_token"));
}

core::Expected<std::string, std::string> RegistryFs::GetOrCreateToken(
    const std::string& remote_url, const std::string& auth_url) {
    // Keyed by the full token-endpoint URL, so tokens for different
    // registries or repositories never collide -- even when a challenge omits
    // `scope` and the key is therefore realm+service alone.
    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        const std::map<std::string, CachedToken>::const_iterator found =
            tokens_.find(auth_url);
        if (found != tokens_.end() && found->second.expire_at_ms > NowMs()) {
            return found->second.token;
        }
    }

    // The fetch happens outside the lock. Rust serialises it per key so a
    // burst issues one request; holding this mutex across a blocking HTTP
    // call would instead stall every other blob, so a duplicate token
    // request is accepted as the cheaper outcome.
    const core::Expected<std::string, std::string> token = GetToken(remote_url, auth_url);
    if (!token.ok()) return token;

    CachedToken entry;
    entry.token = token.value();
    entry.expire_at_ms = NowMs() + kRegistryMinTokenLifeSecs * 1000ULL;
    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        tokens_[auth_url] = entry;
    }
    return token.value();
}

core::Expected<RegistryFs::PreparedAuth, std::string> RegistryFs::PrepareCachedAuth(
    const std::string& url, const BlobAuthContext& ctx) {
    const core::Expected<RegistryAuthChallenge, std::string> challenge =
        GetOrCreateRegistryChallenge(ctx);
    if (!challenge.ok()) return core::make_unexpected(challenge.error());

    PreparedAuth out;
    switch (challenge.value().kind) {
        case kRegistryAuthNone:
            out.kind = kRegistryAuthNone;
            return out;

        case kRegistryAuthBasic: {
            std::string username, password;
            const core::Expected<core::Unit, std::string> creds =
                ResolveCredentials(url, &username, &password);
            if (!creds.ok()) return core::make_unexpected(creds.error());
            // No credentials for a Basic challenge is anonymous access, not a
            // failure: the registry may still serve public blobs.
            if (username.empty()) {
                out.kind = kRegistryAuthNone;
                return out;
            }
            out.kind = kRegistryAuthBasic;
            out.header = BuildBasicAuthHeader(username, password);
            return out;
        }

        case kRegistryAuthBearer: {
            // The blob's own scope, not the ping's: the cached challenge is
            // per origin and carries none.
            const core::Expected<std::string, std::string> auth_url = BuildBearerAuthUrl(
                challenge.value().realm, challenge.value().service, ctx.scope);
            if (!auth_url.ok()) return core::make_unexpected(auth_url.error());
            const core::Expected<std::string, std::string> token =
                GetOrCreateToken(url, auth_url.value());
            if (!token.ok()) return core::make_unexpected(token.error());
            out.kind = kRegistryAuthBearer;
            out.header = "Bearer " + token.value();
            out.cache_key = auth_url.value();
            return out;
        }

        case kRegistryAuthProbePerUrl:
        default:
            out.kind = kRegistryAuthProbePerUrl;
            return out;
    }
}

core::Expected<RegistryFs::PreparedAuth, std::string> RegistryFs::PrepareLegacyAuth(
    const std::string& url) {
    // Pull flavour: this is the read path, so a GET probe is what Rust's
    // `get_scope_auth` (i.e. `get_scope_auth_for(url, false)`) sends.
    const core::Expected<RegistryAuthChallenge, std::string> challenge =
        GetScopeAuth(url, false);
    if (!challenge.ok()) return core::make_unexpected(challenge.error());

    PreparedAuth out;
    if (challenge.value().kind == kRegistryAuthNone) {
        out.kind = kRegistryAuthNone;
        return out;
    }

    if (challenge.value().kind == kRegistryAuthBasic) {
        std::string username, password;
        const core::Expected<core::Unit, std::string> creds =
            ResolveCredentials(url, &username, &password);
        if (!creds.ok()) return core::make_unexpected(creds.error());
        // No credentials for a Basic challenge is anonymous access, not a
        // failure: the registry may still serve public blobs.
        if (username.empty()) {
            out.kind = kRegistryAuthNone;
            return out;
        }
        out.kind = kRegistryAuthBasic;
        out.header = BuildBasicAuthHeader(username, password);
        return out;
    }

    const core::Expected<std::string, std::string> auth_url = BuildBearerAuthUrl(
        challenge.value().realm, challenge.value().service, challenge.value().scope);
    if (!auth_url.ok()) return core::make_unexpected(auth_url.error());
    const core::Expected<std::string, std::string> token =
        GetOrCreateToken(url, auth_url.value());
    if (!token.ok()) return core::make_unexpected(token.error());
    out.kind = kRegistryAuthBearer;
    out.header = "Bearer " + token.value();
    out.cache_key = auth_url.value();
    return out;
}

// ---------------------------------------------------------------------------
// URL resolution
// ---------------------------------------------------------------------------

core::Expected<RegistryFs::UrlInfo, std::string> RegistryFs::ResolveWithAuth(
    const std::string& url, const PreparedAuth& auth) {
    http::Request request;
    request.method = "GET";
    request.url = url;
    // A one-byte range: the cheapest request that still reaches the blob
    // handler and reveals whether it redirects.
    request.has_range = true;
    request.range_begin = 0;
    request.range_end = 0;
    request.timeout_secs = static_cast<long>(options_.timeout_secs);
    if (!options_.user_agent.empty()) request.headers["User-Agent"] = options_.user_agent;
    if (!auth.header.empty()) request.headers["Authorization"] = auth.header;
    // Not followed: a redirect *is* the answer here, and following it would
    // also forward the Authorization header to the CDN.
    request.follow_redirects = false;
    ApplyClientCert(&request);

    const core::Expected<http::Response, std::string> response = http::Perform(request);
    if (!response.ok()) return core::make_unexpected(response.error());

    const long status = response.value().status;
    if (status >= 300 && status < 400) {
        const http::HeaderMap::const_iterator location =
            response.value().headers.find("Location");
        if (location == response.value().headers.end()) {
            return core::make_unexpected(std::string("redirect without Location"));
        }
        UrlInfo info;
        info.mode = kRegistryUrlRedirect;
        info.info = location->second;
        info.expire_at_ms = NowMs() + kRegistryMinUrlInfoLifeSecs * 1000ULL;
        return info;
    }

    if (status == 200 || status == 206) {
        UrlInfo info;
        info.mode = kRegistryUrlSelfAuth;
        // The header is stored, not the URL: range requests go to the same
        // blob endpoint and must repeat exactly this credential.
        info.info = auth.header;
        info.expire_at_ms = NowMs() + kRegistryMinUrlInfoLifeSecs * 1000ULL;
        return info;
    }

    std::ostringstream o;
    o << "failed to resolve actual url for " << url << ": http status " << status;
    return core::make_unexpected(o.str());
}

core::Expected<RegistryFs::UrlInfo, std::string> RegistryFs::GetActualUrl(
    const std::string& url) {
    const core::Optional<BlobAuthContext> ctx = ParseBlobAuthContext(url);

    // Not a registry blob URL (a redirected CDN target, say): there is no
    // `/v2/` to ping, so probe this URL on its own.
    PreparedAuth auth;
    if (!ctx.has_value()) {
        const core::Expected<PreparedAuth, std::string> legacy = PrepareLegacyAuth(url);
        if (!legacy.ok()) return core::make_unexpected(legacy.error());
        auth = legacy.value();
    } else {
        const core::Expected<PreparedAuth, std::string> cached =
            PrepareCachedAuth(url, *ctx);
        if (!cached.ok()) return core::make_unexpected(cached.error());
        auth = cached.value();
        if (auth.kind == kRegistryAuthProbePerUrl) {
            const core::Expected<PreparedAuth, std::string> legacy = PrepareLegacyAuth(url);
            if (!legacy.ok()) return core::make_unexpected(legacy.error());
            auth = legacy.value();
        }
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        http::Request request;
        request.method = "GET";
        request.url = url;
        request.has_range = true;
        request.range_begin = 0;
        request.range_end = 0;
        request.timeout_secs = static_cast<long>(options_.timeout_secs);
        if (!options_.user_agent.empty()) {
            request.headers["User-Agent"] = options_.user_agent;
        }
        if (!auth.header.empty()) request.headers["Authorization"] = auth.header;
        // Not followed: a redirect *is* the answer here, and following it
        // would also forward the Authorization header to the CDN.
        request.follow_redirects = false;

        const core::Expected<http::Response, std::string> response = http::Perform(request);
        if (!response.ok()) return core::make_unexpected(response.error());

        const long status = response.value().status;
        if (status >= 300 && status < 400) {
            const http::HeaderMap::const_iterator location =
                response.value().headers.find("Location");
            if (location == response.value().headers.end()) {
                return core::make_unexpected(std::string("redirect without Location"));
            }
            UrlInfo info;
            info.mode = kRegistryUrlRedirect;
            info.info = location->second;
            info.expire_at_ms = NowMs() + kRegistryMinUrlInfoLifeSecs * 1000ULL;
            return info;
        }

        if (status == 200 || status == 206) {
            UrlInfo info;
            info.mode = kRegistryUrlSelfAuth;
            // The header is stored, not the URL: range requests go to the
            // same blob endpoint and must repeat exactly this credential.
            info.info = auth.header;
            info.expire_at_ms = NowMs() + kRegistryMinUrlInfoLifeSecs * 1000ULL;
            return info;
        }

        if (status == 401 || status == 403) {
            if (!auth.cache_key.empty()) InvalidateToken(auth.cache_key);
            // One retry, and only for Bearer: a cached token may simply have
            // expired, whereas a rejected Basic credential will be rejected
            // again. A second 401 after a fresh token means the credential is
            // wrong, not stale.
            if (attempt == 0 && auth.kind == kRegistryAuthBearer && ctx.has_value()) {
                const core::Expected<PreparedAuth, std::string> refreshed =
                    PrepareCachedAuth(url, *ctx);
                if (refreshed.ok() && refreshed.value().kind != kRegistryAuthProbePerUrl) {
                    auth = refreshed.value();
                    continue;
                }
            }

            // Last resort: probe this URL directly. The registry-wide
            // challenge can be wrong for an individual blob -- a repository
            // may demand Basic where the ping offered Bearer, or name a scope
            // the ping cannot know -- and the blob's own 401 carries the
            // challenge that actually applies. Failing here instead would
            // make those registries unreadable.
            if (ctx.has_value() && auth.kind != kRegistryAuthProbePerUrl) {
                const core::Expected<PreparedAuth, std::string> legacy =
                    PrepareLegacyAuth(url);
                if (legacy.ok() && legacy.value().kind != kRegistryAuthProbePerUrl) {
                    const core::Expected<UrlInfo, std::string> probed =
                        ResolveWithAuth(url, legacy.value());
                    if (probed.ok()) {
                        // Remember that this origin needs per-URL probing, so
                        // the next blob skips the ping that just misled us.
                        MarkProbePerUrl(ctx->origin);
                        return probed.value();
                    }
                }
            }

            std::ostringstream o;
            o << "failed to resolve actual url for " << url << ": http status " << status;
            return core::make_unexpected(o.str());
        }

        if (!auth.cache_key.empty()) InvalidateToken(auth.cache_key);
        std::ostringstream o;
        o << "failed to resolve actual url for " << url << ": http status " << status;
        return core::make_unexpected(o.str());
    }

    return core::make_unexpected("failed to resolve actual url for " + url);
}

core::Expected<RegistryFs::UrlInfo, std::string> RegistryFs::GetOrCreateUrlInfo(
    const std::string& url) {
    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        const std::map<std::string, UrlInfo>::const_iterator found = url_infos_.find(url);
        if (found != url_infos_.end() && found->second.expire_at_ms > NowMs()) {
            return found->second;
        }
    }
    const core::Expected<UrlInfo, std::string> info = GetActualUrl(url);
    if (!info.ok()) return info;
    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        url_infos_[url] = info.value();
    }
    return info.value();
}

// ---------------------------------------------------------------------------
// Range reads
// ---------------------------------------------------------------------------

core::Expected<http::Response, std::string> RegistryFs::FetchRange(const std::string& url,
                                                                      uint64_t offset,
                                                                      size_t count,
                                                                      long timeout_secs,
                                                                      http::BodySink* sink) {
    const core::Expected<UrlInfo, std::string> info = GetOrCreateUrlInfo(url);
    if (!info.ok()) return core::make_unexpected(info.error());

    std::string request_url =
        info.value().mode == kRegistryUrlRedirect ? info.value().info : url;
    // Only in self-auth mode. A redirect target carries its credentials in the
    // query string, and some CDNs reject a request that also bears an
    // Authorization header.
    const std::string auth_header =
        info.value().mode == kRegistryUrlSelfAuth ? info.value().info : std::string();

    if (!accelerate_address_.empty()) {
        std::string prefix = accelerate_address_;
        while (!prefix.empty() && prefix[prefix.size() - 1] == '/') {
            prefix.erase(prefix.size() - 1);
        }
        request_url = prefix + "/" + request_url;
    }

    http::Request request;
    request.method = "GET";
    request.url = request_url;
    request.has_range = true;
    request.range_begin = offset;
    // Inclusive, so the last byte is `offset + count - 1`.
    request.range_end = offset + static_cast<uint64_t>(count) - 1;
    // The deadline's remainder, so the retry loop as a whole stays inside
    // `options_.timeout_secs`.
    request.timeout_secs = timeout_secs;
    if (!options_.user_agent.empty()) request.headers["User-Agent"] = options_.user_agent;
    if (!auth_header.empty()) request.headers["Authorization"] = auth_header;
    ApplyClientCert(&request);
    // Streamed into the caller's buffer when one was given, so a large range
    // does not cost a second copy of itself.
    request.body_sink = sink;

    const core::Expected<http::Response, std::string> response = http::Perform(request);
    if (!response.ok()) {
        // The resolved URL is dropped on failure: a redirect target is
        // short-lived and signed, so the next attempt should re-resolve rather
        // than retry a dead URL.
        InvalidateUrlInfo(url);
        return core::make_unexpected(response.error());
    }
    if (response.value().status != 200 && response.value().status != 206) {
        InvalidateUrlInfo(url);
        std::ostringstream o;
        o << "range request returned non-2xx: status " << response.value().status;
        return core::make_unexpected(o.str());
    }
    return response.value();
}

core::Expected<ByteBuffer, std::string> RegistryFs::ReadRangeInner(const std::string& url,
                                                                      uint64_t offset,
                                                                      size_t count) {
    // One deadline for the whole retry loop, not per attempt. Rust takes
    // `remaining_timeout(deadline)` before each try, so `timeout` bounds the
    // entire read -- otherwise N retries could take N * timeout, and a caller
    // that set 30s would wait 120s.
    const uint64_t deadline_ms = NowMs() + options_.timeout_secs * 1000ULL;

    std::string last_error;
    for (uint32_t attempt = 0; attempt <= options_.retry_count; ++attempt) {
        const core::Expected<long, std::string> remaining = RemainingTimeoutSecs(deadline_ms);
        if (!remaining.ok()) return core::make_unexpected(remaining.error());

        const core::Expected<http::Response, std::string> response =
            FetchRange(url, offset, count, remaining.value(), 0);
        if (response.ok()) {
            // Returned as received. Rust hands back `resp.bytes()` whole and
            // does not slice a 200 that ignored the range: a registry that
            // does that is non-conforming, and silently re-windowing its
            // answer here would differ from the Rust reader on the same
            // server.
            return response.value().body;
        }
        last_error = response.error();
        if (attempt != options_.retry_count) SleepRetryBackoff();
    }
    return core::make_unexpected(last_error.empty()
                                     ? std::string("range request failed")
                                     : last_error);
}

core::Expected<size_t, std::string> RegistryFs::ReadRangeIntoInner(const std::string& url,
                                                                       uint64_t offset,
                                                                       uint8_t* dst,
                                                                       size_t len) {
    const uint64_t deadline_ms = NowMs() + options_.timeout_secs * 1000ULL;

    std::string last_error;
    for (uint32_t attempt = 0; attempt <= options_.retry_count; ++attempt) {
        const core::Expected<long, std::string> remaining = RemainingTimeoutSecs(deadline_ms);
        if (!remaining.ok()) return core::make_unexpected(remaining.error());

        // Written chunk by chunk straight into `dst`, as Rust's
        // `while let Some(chunk) = resp.chunk()` loop does. The body is never
        // held whole, so a multi-megabyte range costs one chunk rather than
        // twice the window.
        http::BufferSink sink(dst, len);
        const core::Expected<http::Response, std::string> response =
            FetchRange(url, offset, len, remaining.value(), &sink);

        if (response.ok()) {
            // Rust bails with this exact condition while streaming: a surplus
            // means the server ignored the range, so what arrived is not the
            // window that was asked for. The sink already stopped the
            // transfer, so nothing beyond `len` was even downloaded.
            if (sink.overflow()) {
                std::ostringstream o;
                o << "range response larger than requested for " << url << " at " << offset;
                return core::make_unexpected(o.str());
            }
            return sink.filled();
        }
        last_error = response.error();
        if (attempt != options_.retry_count) SleepRetryBackoff();
    }
    return core::make_unexpected(last_error.empty()
                                     ? std::string("range request failed")
                                     : last_error);
}

core::Expected<uint64_t, std::string> RegistryFs::GetLengthInner(const std::string& url) {
    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        const std::map<std::string, CachedSize>::const_iterator found = sizes_.find(url);
        if (found != sizes_.end() && found->second.expire_at_ms > NowMs()) {
            return found->second.size;
        }
    }

    const uint64_t deadline_ms = NowMs() + options_.timeout_secs * 1000ULL;

    std::string last_error;
    for (uint32_t attempt = 0; attempt <= options_.retry_count; ++attempt) {
        const core::Expected<long, std::string> remaining = RemainingTimeoutSecs(deadline_ms);
        if (!remaining.ok()) {
            InvalidateSize(url);
            return core::make_unexpected(remaining.error());
        }

        // A one-byte range: the size arrives in `Content-Range` without
        // transferring the blob.
        const core::Expected<http::Response, std::string> response =
            FetchRange(url, 0, 1, remaining.value(), 0);
        if (!response.ok()) {
            last_error = response.error();
            if (attempt == options_.retry_count) break;
            SleepRetryBackoff();
            continue;
        }

        uint64_t size = 0;
        // `Content-Range` first: on a 206 the `Content-Length` is the length
        // of the one-byte slice, so only the `/total` suffix gives the blob's
        // size.
        if (!response.value().ContentRangeTotal(&size)) {
            if (response.value().status == 200 && response.value().ContentLength(&size)) {
                // A 200 means the range was ignored and the whole blob was
                // sent, so its `Content-Length` is the size.
            } else {
                // Rust's final fallback: the body that arrived. Only
                // meaningful because reaching here means the range was
                // ignored, so what arrived is the whole blob.
                size = static_cast<uint64_t>(response.value().body.size());
            }
        }

        CachedSize entry;
        entry.size = size;
        entry.expire_at_ms = NowMs() + kRegistryMinMetaLifeSecs * 1000ULL;
        {
            std::lock_guard<std::mutex> lk(cache_mu_);
            sizes_[url] = entry;
        }
        return size;
    }

    // Dropped on failure so a later call re-probes rather than serving a
    // stale entry that happens to still be inside its TTL.
    InvalidateSize(url);
    return core::make_unexpected(last_error.empty()
                                     ? std::string("failed to get blob length")
                                     : last_error);
}

core::Expected<core::Optional<std::string>, std::string> RegistryFs::RefreshToken(
    const std::string& url) {
    // Probed with the push flavour, as Rust does. A push challenge names the
    // `pull,push` scope, so the token this returns is usable for an upload --
    // which is the only reason a caller asks for a refresh rather than
    // letting the read path renew its own token lazily.
    const core::Expected<RegistryAuthChallenge, std::string> challenge =
        GetScopeAuth(url, true);
    if (!challenge.ok()) return core::make_unexpected(challenge.error());

    // Nothing to refresh: neither anonymous nor Basic access involves a
    // token, so this reports absence rather than failing.
    if (challenge.value().kind != kRegistryAuthBearer) {
        return core::Optional<std::string>();
    }

    const core::Expected<std::string, std::string> auth_url = BuildBearerAuthUrl(
        challenge.value().realm, challenge.value().service, challenge.value().scope);
    if (!auth_url.ok()) return core::make_unexpected(auth_url.error());

    // Fetched directly, bypassing the cache: the point of a refresh is to
    // replace whatever is cached, so consulting it first would return the
    // stale token this call exists to displace.
    const core::Expected<std::string, std::string> token = GetToken(url, auth_url.value());
    if (!token.ok()) return core::make_unexpected(token.error());
    if (token.value().empty()) {
        return core::make_unexpected(std::string("failed to get token"));
    }

    CachedToken entry;
    entry.token = token.value();
    entry.expire_at_ms = NowMs() + kRegistryMinTokenLifeSecs * 1000ULL;
    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        tokens_[auth_url.value()] = entry;
    }
    return core::Optional<std::string>(token.value());
}

// ---------------------------------------------------------------------------
// Open / RegistryFile
// ---------------------------------------------------------------------------

core::Expected<std::shared_ptr<RegistryFile>, std::string> RegistryFs::OpenFile(
    const std::string& url, uint64_t size_hint) {
    std::shared_ptr<RegistryFile> file(new RegistryFile());
    file->backend_ = this;
    // Normalized so the same blob is not cached under two keys.
    file->url_ = NormalizeUrl(url);
    if (size_hint > 0) {
        file->size_known_ = true;
        file->size_ = size_hint;
    }
    return file;
}

core::Expected<uint64_t, std::string> RegistryFile::Size() {
    if (size_known_) return size_;
    const core::Expected<uint64_t, std::string> size = backend_->GetLength(url_);
    if (!size.ok()) return size;
    // Cached for the handle's life: a blob is immutable once pushed, so
    // re-probing it could only cost requests.
    size_ = size.value();
    size_known_ = true;
    return size_;
}

core::Expected<uint64_t, std::string> RegistryFs::GetLength(const std::string& url) {
    const metrics::RemoteSource source =
        metrics::RemoteSource::Registry(accelerate_address_, url);
    const uint64_t start = NowMs();
    const core::Expected<uint64_t, std::string> result = GetLengthInner(url);
    metrics::RecordRemoteMetadata(source, result.ok() ? std::string() : result.error(),
                                  static_cast<double>(NowMs() - start) / 1000.0);
    return result;
}

// ---------------------------------------------------------------------------
// Metric wrappers
// ---------------------------------------------------------------------------
//
// Rust splits each of these into an outer function that measures and an
// `_inner` that does the work; the split is kept because it is what guarantees
// every return path is timed, including the early ones.

core::Expected<ByteBuffer, std::string> RegistryFs::ReadRange(const std::string& url,
                                                                 uint64_t offset,
                                                                 size_t count) {
    const metrics::RemoteSource source =
        metrics::RemoteSource::Registry(accelerate_address_, url);
    const uint64_t start = NowMs();
    const core::Expected<ByteBuffer, std::string> result = ReadRangeInner(url, offset, count);
    metrics::RecordRemoteRead(
        source, metrics::kReadRange, result.ok() ? std::string() : result.error(),
        result.ok() ? static_cast<uint64_t>(result.value().size()) : 0,
        static_cast<double>(NowMs() - start) / 1000.0);
    return result;
}

core::Expected<size_t, std::string> RegistryFs::ReadRangeInto(const std::string& url,
                                                                 uint64_t offset, uint8_t* dst,
                                                                 size_t len) {
    const metrics::RemoteSource source =
        metrics::RemoteSource::Registry(accelerate_address_, url);
    const uint64_t start = NowMs();
    const core::Expected<size_t, std::string> result =
        ReadRangeIntoInner(url, offset, dst, len);
    metrics::RecordRemoteRead(
        source, metrics::kReadRangeInto, result.ok() ? std::string() : result.error(),
        result.ok() ? static_cast<uint64_t>(result.value()) : 0,
        static_cast<double>(NowMs() - start) / 1000.0);
    return result;
}

core::Expected<ByteBuffer, std::string> RegistryFile::ReadAt(uint64_t offset, size_t len) {
    ByteBuffer out;
    if (len == 0) return out;

    const core::Expected<uint64_t, std::string> size = Size();
    if (!size.ok()) return core::make_unexpected(size.error());
    // Past the end reads empty, matching a local file rather than surfacing
    // the registry's 416.
    if (offset >= size.value()) return out;

    const uint64_t remain = size.value() - offset;
    const size_t want = static_cast<size_t>(remain < len ? remain : len);
    return backend_->ReadRange(url_, offset, want);
}

core::Expected<size_t, std::string> RegistryFile::ReadAtInto(uint64_t offset, uint8_t* dst,
                                                                size_t len) {
    if (len == 0) return static_cast<size_t>(0);

    const core::Expected<uint64_t, std::string> size = Size();
    if (!size.ok()) return core::make_unexpected(size.error());
    if (offset >= size.value()) return static_cast<size_t>(0);

    // Clamped to the blob, then handed straight to the backend. Rust fills
    // `dst[..count]` by streaming rather than materialising a buffer first,
    // so this must not route through `ReadAt`.
    const uint64_t remain = size.value() - offset;
    const size_t count = static_cast<size_t>(remain < len ? remain : len);
    return backend_->ReadRangeInto(url_, offset, dst, count);
}

core::Expected<size_t, std::string> RegistryFile::WriteAt(uint64_t, const uint8_t*, size_t) {
    return core::make_unexpected(
        std::string("registry blob is read-only; a blob is immutable once pushed"));
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
