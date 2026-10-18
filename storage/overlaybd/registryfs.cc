// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/registryfs_v2.rs (read path)
#include "agentenv/storage/overlaybd/registryfs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <sys/time.h>

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

/// Rust `parse_auths` — pick the entry of a Docker-style `auths` map that
/// matches the blob's host, and split its base64 `auth` field.
void ParseAuths(const core::Json& auths, const std::string& remote_url,
                std::string* username, std::string* password) {
    username->clear();
    password->clear();
    if (auths.kind() != core::Json::Kind::Object) return;

    SplitUrl parts;
    if (!SplitUrlParts(remote_url, &parts)) return;

    const core::JsonObject& members = auths.as_object();
    for (core::JsonObject::const_iterator it = members.begin(); it != members.end(); ++it) {
        // The key may be a bare host or a URL; both have to match the blob's
        // host, which is why it is compared by substring of the registry key
        // rather than parsed as a URL.
        const std::string key = it->first;
        if (key != parts.host && key.find(parts.host) == std::string::npos) continue;
        if (it->second.kind() != core::Json::Kind::Object) continue;

        const core::JsonObject& entry = it->second.as_object();
        const core::JsonObject::const_iterator found = entry.find("auth");
        if (found == entry.end() || found->second.kind() != core::Json::Kind::String) continue;

        // The field is `user:password` in base64, standard alphabet.
        const core::Expected<std::string, std::string> decoded =
            core::Base64UrlDecode(found->second.as_string());
        std::string raw;
        if (decoded.ok()) {
            raw = decoded.value();
        } else {
            // Docker writes the standard alphabet; the URL-safe decoder only
            // accepts it when no byte landed on index 62/63. Translating and
            // retrying covers the rest without a second decoder.
            std::string translated = found->second.as_string();
            for (size_t i = 0; i < translated.size(); ++i) {
                if (translated[i] == '+') translated[i] = '-';
                if (translated[i] == '/') translated[i] = '_';
            }
            const core::Expected<std::string, std::string> second =
                core::Base64UrlDecode(translated);
            if (!second.ok()) continue;
            raw = second.value();
        }

        const std::string::size_type colon = raw.find(':');
        if (colon == std::string::npos) continue;
        *username = raw.substr(0, colon);
        *password = raw.substr(colon + 1);
        return;
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

void RegistryFs::MarkProbePerUrl(const std::string& origin) {
    CachedChallenge entry;
    entry.challenge.kind = kRegistryAuthProbePerUrl;
    // Shorter TTL than a real answer: this is a fallback after the ping
    // failed, so it should be retried sooner.
    entry.expire_at_ms = NowMs() + kRegistryProbePerUrlLifeSecs * 1000ULL;
    challenges_[origin] = entry;
}

core::Expected<RegistryAuthChallenge, std::string> RegistryFs::GetOrCreateRegistryChallenge(
    const BlobAuthContext& ctx) {
    const std::map<std::string, CachedChallenge>::const_iterator found =
        challenges_.find(ctx.origin);
    if (found != challenges_.end() && found->second.expire_at_ms > NowMs()) {
        return found->second.challenge;
    }

    http::Request request;
    request.method = "GET";
    request.url = ctx.ping_url;
    request.timeout_secs = static_cast<long>(options_.timeout_secs);
    if (!options_.user_agent.empty()) request.headers["User-Agent"] = options_.user_agent;
    // The challenge rides on the 401 itself, so a redirect would hide it.
    request.follow_redirects = false;

    RegistryAuthChallenge challenge;
    const core::Expected<http::Response, std::string> response = http::Perform(request);
    if (!response.ok()) {
        // A ping that cannot even be sent falls back to per-URL probing
        // rather than failing the read: some registries answer `/v2/`
        // differently from the blob endpoint.
        MarkProbePerUrl(ctx.origin);
        return challenges_[ctx.origin].challenge;
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
            return challenges_[ctx.origin].challenge;
        }
        // Scope dropped: this challenge is cached per origin.
        const core::Expected<RegistryAuthChallenge, std::string> parsed =
            ParseRegistryAuthChallenge(header->second, false);
        if (!parsed.ok()) {
            MarkProbePerUrl(ctx.origin);
            return challenges_[ctx.origin].challenge;
        }
        challenge = parsed.value();
    }

    CachedChallenge entry;
    entry.challenge = challenge;
    entry.expire_at_ms = NowMs() + kRegistryAuthChallengeLifeSecs * 1000ULL;
    challenges_[ctx.origin] = entry;
    return challenge;
}

core::Expected<std::string, std::string> RegistryFs::GetOrCreateToken(
    const std::string& remote_url, const std::string& auth_url) {
    const std::map<std::string, CachedToken>::const_iterator found = tokens_.find(auth_url);
    if (found != tokens_.end() && found->second.expire_at_ms > NowMs()) {
        return found->second.token;
    }

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
    std::string token;
    core::JsonObject::const_iterator field = root.find("token");
    if (field != root.end() && field->second.kind() == core::Json::Kind::String) {
        token = field->second.as_string();
    } else {
        field = root.find("access_token");
        if (field != root.end() && field->second.kind() == core::Json::Kind::String) {
            token = field->second.as_string();
        }
    }
    if (token.empty()) {
        return core::make_unexpected(
            std::string("token response missing token/access_token"));
    }

    CachedToken entry;
    entry.token = token;
    entry.expire_at_ms = NowMs() + kRegistryMinTokenLifeSecs * 1000ULL;
    tokens_[auth_url] = entry;
    return token;
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
    // A one-byte range rather than a HEAD: some registries answer HEAD without
    // the challenge, and this is also the cheapest GET that still reaches the
    // blob handler.
    http::Request request;
    request.method = "GET";
    request.url = url;
    request.has_range = true;
    request.range_begin = 0;
    request.range_end = 0;
    request.timeout_secs = static_cast<long>(options_.timeout_secs);
    if (!options_.user_agent.empty()) request.headers["User-Agent"] = options_.user_agent;
    request.follow_redirects = false;

    PreparedAuth out;
    const core::Expected<http::Response, std::string> response = http::Perform(request);
    if (!response.ok()) {
        return core::make_unexpected("challenge request failed for " + url + ": " +
                                     response.error());
    }

    const long status = response.value().status;
    if (status != 401 && status != 403) {
        out.kind = kRegistryAuthNone;
        return out;
    }

    const http::HeaderMap::const_iterator header =
        response.value().headers.find("WWW-Authenticate");
    if (header == response.value().headers.end()) {
        return core::make_unexpected(std::string("no WWW-Authenticate in response"));
    }
    // Scope kept here: a per-URL challenge names the repository being read,
    // and the token must be requested for exactly it.
    const core::Expected<RegistryAuthChallenge, std::string> parsed =
        ParseRegistryAuthChallenge(header->second, true);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());

    if (parsed.value().kind == kRegistryAuthBasic) {
        std::string username, password;
        const core::Expected<core::Unit, std::string> creds =
            ResolveCredentials(url, &username, &password);
        if (!creds.ok()) return core::make_unexpected(creds.error());
        if (username.empty()) {
            out.kind = kRegistryAuthNone;
            return out;
        }
        out.kind = kRegistryAuthBasic;
        out.header = BuildBasicAuthHeader(username, password);
        return out;
    }

    const core::Expected<std::string, std::string> auth_url = BuildBearerAuthUrl(
        parsed.value().realm, parsed.value().service, parsed.value().scope);
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
    const std::map<std::string, UrlInfo>::const_iterator found = url_infos_.find(url);
    if (found != url_infos_.end() && found->second.expire_at_ms > NowMs()) {
        return found->second;
    }
    const core::Expected<UrlInfo, std::string> info = GetActualUrl(url);
    if (!info.ok()) return info;
    url_infos_[url] = info.value();
    return info.value();
}

// ---------------------------------------------------------------------------
// Range reads
// ---------------------------------------------------------------------------

core::Expected<http::Response, std::string> RegistryFs::FetchRange(const std::string& url,
                                                                      uint64_t offset,
                                                                      size_t count) {
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
    request.timeout_secs = static_cast<long>(options_.timeout_secs);
    if (!options_.user_agent.empty()) request.headers["User-Agent"] = options_.user_agent;
    if (!auth_header.empty()) request.headers["Authorization"] = auth_header;

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

core::Expected<ByteBuffer, std::string> RegistryFs::ReadRange(const std::string& url,
                                                                 uint64_t offset,
                                                                 size_t count) {
    std::string last_error;
    for (uint32_t attempt = 0; attempt <= options_.retry_count; ++attempt) {
        const core::Expected<http::Response, std::string> response =
            FetchRange(url, offset, count);
        if (response.ok()) {
            ByteBuffer body = response.value().body;
            // A server that ignored the range answers 200 with the whole
            // blob; the window is taken out of it rather than returned whole.
            if (response.value().status == 200 && body.size() > count) {
                if (offset >= body.size()) return ByteBuffer();
                const size_t avail = body.size() - static_cast<size_t>(offset);
                const size_t take = avail < count ? avail : count;
                return ByteBuffer(body.begin() + static_cast<size_t>(offset),
                                  body.begin() + static_cast<size_t>(offset) + take);
            }
            if (body.size() > count) body.resize(count);
            return body;
        }
        last_error = response.error();
    }
    return core::make_unexpected(last_error.empty()
                                     ? std::string("range request failed")
                                     : last_error);
}

core::Expected<uint64_t, std::string> RegistryFs::GetLength(const std::string& url) {
    const std::map<std::string, CachedSize>::const_iterator found = sizes_.find(url);
    if (found != sizes_.end() && found->second.expire_at_ms > NowMs()) {
        return found->second.size;
    }

    std::string last_error;
    for (uint32_t attempt = 0; attempt <= options_.retry_count; ++attempt) {
        const core::Expected<http::Response, std::string> response = FetchRange(url, 0, 1);
        if (!response.ok()) {
            last_error = response.error();
            continue;
        }

        uint64_t size = 0;
        bool known = false;
        // `Content-Range` first: on a 206 the `Content-Length` is the length
        // of the one-byte slice, so only the `/total` suffix gives the blob's
        // size.
        if (response.value().ContentRangeTotal(&size)) {
            known = true;
        } else if (response.value().status == 200 &&
                   response.value().ContentLength(&size)) {
            // A 200 means the range was ignored and the whole blob was sent,
            // so its Content-Length *is* the size.
            known = true;
        } else {
            // Last resort: whatever arrived. Only correct because the range
            // was ignored, which is the only way to get here.
            size = static_cast<uint64_t>(response.value().body.size());
            known = true;
        }

        if (known) {
            CachedSize entry;
            entry.size = size;
            entry.expire_at_ms = NowMs() + kRegistryMinMetaLifeSecs * 1000ULL;
            sizes_[url] = entry;
            return size;
        }
    }

    InvalidateSize(url);
    return core::make_unexpected(last_error.empty()
                                     ? std::string("failed to get blob length")
                                     : last_error);
}

core::Expected<core::Unit, std::string> RegistryFs::RefreshToken(const std::string& url) {
    const std::string normalized = NormalizeUrl(url);
    const core::Optional<BlobAuthContext> ctx = ParseBlobAuthContext(normalized);
    if (!ctx.has_value()) {
        return core::make_unexpected("not a registry blob url: " + url);
    }

    // The cached challenge is kept: only the token is stale. Dropping the
    // challenge too would force an extra ping on every refresh.
    const std::map<std::string, CachedChallenge>::const_iterator found =
        challenges_.find(ctx->origin);
    if (found != challenges_.end() && found->second.challenge.kind == kRegistryAuthBearer) {
        const core::Expected<std::string, std::string> auth_url = BuildBearerAuthUrl(
            found->second.challenge.realm, found->second.challenge.service, ctx->scope);
        if (auth_url.ok()) InvalidateToken(auth_url.value());
    }
    // The resolved URL goes too: a self-auth entry holds the old header.
    InvalidateUrlInfo(normalized);
    return core::Unit();
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
    const core::Expected<ByteBuffer, std::string> data = ReadAt(offset, len);
    if (!data.ok()) return core::make_unexpected(data.error());
    const size_t n = data.value().size() < len ? data.value().size() : len;
    if (n > 0) std::memcpy(dst, &data.value()[0], n);
    return n;
}

core::Expected<size_t, std::string> RegistryFile::WriteAt(uint64_t, const uint8_t*, size_t) {
    return core::make_unexpected(
        std::string("registry blob is read-only; a blob is immutable once pushed"));
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
