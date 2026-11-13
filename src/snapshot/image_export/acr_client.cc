// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/acr/client.rs (the manifest-delete path) and
// `AcrDiskImageExporter::rollback_publication` from acr/publisher.rs.
#include "agentenv/snapshot/image_export/acr.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include "agentenv/core/digest.h"
#include "agentenv/core/logging.h"
#include "agentenv/snapshot/acr_manifest.h"
#include "agentenv/snapshot/image_export/acr_source_image.h"
#include "agentenv/snapshot/recontainerize.h"
#include "agentenv/storage/overlaybd/dense_export.h"
#include "agentenv/storage/overlaybd/local_file.h"

namespace agentenv {
namespace snapshot {
namespace image_export {

namespace obdhttp = storage::overlaybd::http;
using repository::RepositoryError;
using repository::RepositoryResult;

namespace {

int64_t MonotonicMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void SleepMs(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

core::Optional<std::string> FindHeader(const obdhttp::HeaderMap& headers,
                                       const std::string& name) {
    // Header names are case-insensitive, and a registry's casing is not
    // something to depend on: `WWW-Authenticate` is what makes the difference
    // between re-authenticating and reporting a permission failure.
    for (obdhttp::HeaderMap::const_iterator it = headers.begin(); it != headers.end(); ++it) {
        if (it->first.size() != name.size()) continue;
        bool same = true;
        for (std::string::size_type i = 0; same && i < name.size(); ++i) {
            const char a = it->first[i] >= 'A' && it->first[i] <= 'Z'
                               ? static_cast<char>(it->first[i] - 'A' + 'a')
                               : it->first[i];
            const char b = name[i] >= 'A' && name[i] <= 'Z'
                               ? static_cast<char>(name[i] - 'A' + 'a')
                               : name[i];
            if (a != b) same = false;
        }
        if (same) return core::Optional<std::string>(it->second);
    }
    return core::Optional<std::string>();
}

std::string ResponseBodyText(const obdhttp::Response& response) {
    return std::string(response.body.begin(), response.body.end());
}

/// `Response::status` is libcurl's `long`. Narrowed here, once, so the status
/// classifiers keep their `int` signature and no call site repeats the cast.
int StatusCode(const obdhttp::Response& response) {
    return static_cast<int>(response.status);
}

std::string StatusText(int status) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", status);
    return buf;
}

bool IsHttpsUrl(const std::string& url) {
    return url.size() >= 8 && (url.compare(0, 8, "https://") == 0);
}

/// Rust `trim_end_matches('/')` — *every* trailing slash, not just one.
std::string TrimTrailingSlashes(const std::string& text) {
    std::string::size_type end = text.size();
    while (end > 0 && text[end - 1] == '/') --end;
    return text.substr(0, end);
}

std::string Decimal(uint64_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
    return buf;
}

}  // namespace

// ---- AcrRequestBody -------------------------------------------------------

AcrRequestBody AcrRequestBody::Blob(const std::vector<uint8_t>& bytes) {
    AcrRequestBody out;
    out.bytes = bytes;
    out.content_type = "application/octet-stream";
    return out;
}

AcrRequestBody AcrRequestBody::BlobRange(const std::vector<uint8_t>& bytes, uint64_t offset,
                                         uint64_t end) {
    AcrRequestBody out = Blob(bytes);
    // Bare `<start>-<end>`: the registry API's `Content-Range` for a chunked
    // `PATCH` has neither the `bytes ` prefix nor the `/<total>` suffix of the
    // HTTP range header it resembles, and adding either is a 400.
    out.content_range =
        core::Optional<std::string>(Decimal(offset) + "-" + Decimal(end));
    return out;
}

AcrRequestBody AcrRequestBody::Manifest(const std::vector<uint8_t>& bytes) {
    AcrRequestBody out;
    out.bytes = bytes;
    out.content_type = snapshot::kOciImageManifestMediaType;
    return out;
}

// ---- Docker credential loading --------------------------------------------

namespace {

/// Runs `docker-credential-<helper> get` with `server_url` on stdin.
///
/// Rust spawns the binary and writes the URL followed by a newline. A non-zero
/// exit is "no credentials", not a failure: that is how a helper reports a
/// registry it does not know, and failing would stop the search.
core::Expected<core::Optional<DockerRegistryCredentials>, AcrClientError>
RunCredentialHelper(const std::string& helper, const std::string& server_url) {
    const std::string binary = "docker-credential-" + helper;

    int in_pipe[2];
    int out_pipe[2];
    if (::pipe(in_pipe) != 0) {
        return core::make_unexpected(AcrClientError::Registry(
            "run Docker credential helper '" + binary + " get': " + std::strerror(errno)));
    }
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        return core::make_unexpected(AcrClientError::Registry(
            "run Docker credential helper '" + binary + " get': " + std::strerror(errno)));
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return core::make_unexpected(AcrClientError::Registry(
            "run Docker credential helper '" + binary + " get': " + std::strerror(errno)));
    }
    if (pid == 0) {
        ::dup2(in_pipe[0], 0);
        ::dup2(out_pipe[1], 1);
        // stderr is discarded rather than inherited: a helper's diagnostics
        // would otherwise land in this server's log as if they were ours.
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, 2);
            ::close(devnull);
        }
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        const char* argv[3];
        argv[0] = binary.c_str();
        argv[1] = "get";
        argv[2] = NULL;
        ::execvp(binary.c_str(), const_cast<char* const*>(argv));
        ::_exit(127);
    }

    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    const std::string request = server_url + "\n";
    // A helper that exits without reading would otherwise kill this process
    // with SIGPIPE; the write failure is handled through the exit status.
    const ssize_t ignored = ::write(in_pipe[1], request.data(), request.size());
    (void)ignored;
    ::close(in_pipe[1]);

    std::string output;
    char buf[4096];
    ssize_t read_bytes = 0;
    while ((read_bytes = ::read(out_pipe[0], buf, sizeof(buf))) > 0) {
        output.append(buf, static_cast<std::size_t>(read_bytes));
    }
    ::close(out_pipe[0]);

    int status = 0;
    (void)::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return core::Optional<DockerRegistryCredentials>();
    }
    return ParseDockerCredentialHelperOutput(output);
}

}  // namespace

core::Expected<core::Optional<DockerRegistryCredentials>, AcrClientError>
LoadDockerCredentials(const std::string& registry) {
    const std::vector<std::string> candidates = DockerConfigCandidates();
    for (std::vector<std::string>::size_type i = 0; i < candidates.size(); ++i) {
        const std::string& path = candidates[i];
        std::ifstream file(path.c_str(), std::ios::binary);
        // A missing candidate is skipped, not an error: having no
        // `$DOCKER_CONFIG` is the normal case.
        if (!file.good()) continue;
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const std::string bytes = buffer.str();

        const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(bytes);
        if (!json.ok()) {
            // A config that exists but cannot be parsed *is* an error: silently
            // skipping it would fall back to anonymous access and report the
            // eventual 401 as a permission problem, hiding the typo.
            return core::make_unexpected(AcrClientError::Registry(
                "parse Docker config '" + path + "': " + json.error().chain()));
        }

        if (json.value().kind() == core::Json::Kind::Object) {
            const core::JsonObject& fields = json.value().as_object();
            const core::JsonObject::const_iterator auths_at = fields.find("auths");
            if (auths_at != fields.end()) {
                const core::Optional<DockerRegistryCredentials> direct =
                    ParseDockerAuths(auths_at->second, registry);
                if (direct.has_value()) return direct;
            }
        }

        const core::Optional<std::pair<std::string, std::string> > helper =
            DockerCredentialHelperForRegistry(json.value(), registry);
        if (helper.has_value()) {
            const core::Expected<core::Optional<DockerRegistryCredentials>, AcrClientError>
                from_helper = RunCredentialHelper(helper->first, helper->second);
            if (!from_helper.ok()) return from_helper;
            if (from_helper.value().has_value()) return from_helper.value();
        }
    }
    return core::Optional<DockerRegistryCredentials>();
}

// ---- AcrClient ------------------------------------------------------------

AcrClient::Transport AcrClient::CurlTransport() {
    return [](const obdhttp::Request& request) -> core::Expected<obdhttp::Response, std::string> {
        if (!obdhttp::Available()) {
            return core::make_unexpected(
                std::string("this build has no HTTP transport (libcurl was unavailable)"));
        }
        return obdhttp::Perform(request);
    };
}

AcrClient::AcrClient(const core::Optional<DockerRegistryCredentials>& credentials,
                     const AcrClientOptions& options, const Transport& transport)
    : credentials_(credentials),
      options_(options),
      transport_(transport),
      clock_(&MonotonicMs),
      sleeper_(&SleepMs) {}

core::Expected<AcrClient, AcrClientError> AcrClient::FromDockerConfig(
    const std::string& registry, const Transport& transport, const AcrClientOptions& options) {
    const core::Expected<core::Optional<DockerRegistryCredentials>, AcrClientError> loaded =
        LoadDockerCredentials(registry);
    if (!loaded.ok()) return core::make_unexpected(loaded.error());
    if (!loaded.value().has_value()) {
        return core::make_unexpected(AcrClientError::MissingCredentials(registry));
    }
    return AcrClient(loaded.value(), options, transport);
}

core::Expected<core::Unit, AcrClientError> AcrClient::ValidateHttpsUrl(
    const std::string& url, const std::string& description) const {
    if (IsHttpsUrl(url) || options_.allow_insecure_http) return core::Unit();
    // Checked on every request and on the token realm, because the realm comes
    // from a server-supplied header: a downgraded realm would send the
    // registry password over plaintext.
    return core::make_unexpected(
        AcrClientError::Registry(description + " must use https: " + url));
}

core::Expected<AcrClient::HttpResponse, AcrClientError> AcrClient::Request(
    const std::string& method, const std::string& url,
    const core::Optional<std::string>& auth_header,
    const core::Optional<AcrRequestBody>& body) {
    const core::Expected<core::Unit, AcrClientError> valid =
        ValidateHttpsUrl(url, "ACR registry request");
    if (!valid.ok()) return core::make_unexpected(valid.error());

    obdhttp::Request request;
    request.method = method;
    request.url = url;
    if (auth_header.has_value()) request.headers["Authorization"] = *auth_header;
    if (body.has_value()) {
        request.body = body->bytes;
        request.headers["Content-Type"] = body->content_type;
        if (body->content_range.has_value()) {
            request.headers["Content-Range"] = *body->content_range;
        }
        // `Content-Length` is left to the transport, which sets it from the
        // body size. Rust writes it explicitly because its client does not;
        // writing it here as well would send the header twice.
    } else if (method == "POST") {
        // The bodyless `POST` that opens an upload session. libcurl sends no
        // `Content-Length` for a custom method without a payload, and a
        // registry behind a proxy answers that with `411 Length Required`.
        // Rust's client emits `content-length: 0` for an empty body on a
        // method that takes one, so this keeps the two equivalent on the wire.
        request.headers["Content-Length"] = "0";
    }
    request.timeout_secs = static_cast<long>((options_.timeout_ms + 999) / 1000);
    // Rust's redirect policy also pins same-origin and an https target. This
    // transport's own policy is what applies here; the difference is recorded
    // in the header rather than silently dropped.
    request.follow_redirects = true;

    const core::Expected<obdhttp::Response, std::string> response = transport_(request);
    if (!response.ok()) {
        // A transport failure is transient by construction: no connection, a
        // TLS error or a timeout may all succeed on the next attempt, and
        // classifying them otherwise would skip the retry.
        return core::make_unexpected(AcrClientError::Transient(
            "ACR HTTP request failed for " + url + ": " + response.error()));
    }
    return response.value();
}

core::Optional<std::string> AcrClient::CachedToken(const std::string& cache_key) {
    const std::map<std::string, CachedTokenEntry>::iterator at = token_cache_.find(cache_key);
    if (at == token_cache_.end()) return core::Optional<std::string>();
    if (at->second.expires_at_ms > clock_()) {
        return core::Optional<std::string>(at->second.token);
    }
    // Evicted rather than left to be re-checked: a stale entry would otherwise
    // be compared on every request for the life of the process.
    token_cache_.erase(at);
    return core::Optional<std::string>();
}

core::Expected<std::string, AcrClientError> AcrClient::AuthHeader(
    const std::string& registry_url, const std::string& repository,
    const std::string& challenge) {
    if (!credentials_.has_value()) {
        const core::Optional<std::string> host = RegistryHost(registry_url);
        return core::make_unexpected(
            AcrClientError::MissingCredentials(host.has_value() ? *host : registry_url));
    }

    // A `Basic` challenge is answered directly; only `Bearer` needs a token.
    if (challenge.size() >= 6) {
        std::string prefix = challenge.substr(0, 6);
        for (std::string::size_type i = 0; i < prefix.size(); ++i) {
            if (prefix[i] >= 'A' && prefix[i] <= 'Z') {
                prefix[i] = static_cast<char>(prefix[i] - 'A' + 'a');
            }
        }
        if (prefix == "basic ") {
            return BuildBasicAuthHeader(credentials_->username, credentials_->password);
        }
    }

    const core::Expected<BearerChallenge, AcrClientError> bearer =
        ParseBearerChallenge(challenge);
    if (!bearer.ok()) return core::make_unexpected(bearer.error());

    // `push,pull` even for a delete: ACR issues one token for the repository
    // and a pull-only scope cannot remove a manifest.
    const std::string scope = "repository:" + repository + ":push,pull";
    const std::string cache_key = bearer.value().CacheKey(scope);
    const core::Optional<std::string> cached = CachedToken(cache_key);
    if (cached.has_value()) return "Bearer " + *cached;

    // Rust `Url::parse(&bearer.realm)?` before the https check. Kept because
    // the realm is server-supplied: an unparsable one would otherwise reach
    // the transport and come back as a vague "request failed" that looks
    // transient and gets retried three times.
    if (!RegistryHost(bearer.value().realm).has_value()) {
        return core::make_unexpected(AcrClientError::Registry("parse ACR token realm '" +
                                                              bearer.value().realm + "'"));
    }
    const core::Expected<core::Unit, AcrClientError> realm_ok =
        ValidateHttpsUrl(bearer.value().realm, "ACR token realm");
    if (!realm_ok.ok()) return core::make_unexpected(realm_ok.error());

    const std::string separator =
        bearer.value().realm.find('?') == std::string::npos ? "?" : "&";
    const std::string token_url = bearer.value().realm + separator + "service=" +
                                  obdhttp::EncodeQueryValue(bearer.value().service) +
                                  "&scope=" + obdhttp::EncodeQueryValue(scope);

    obdhttp::Request request;
    request.method = "GET";
    request.url = token_url;
    request.headers["Authorization"] =
        BuildBasicAuthHeader(credentials_->username, credentials_->password);
    request.timeout_secs = static_cast<long>((options_.timeout_ms + 999) / 1000);
    const core::Expected<obdhttp::Response, std::string> response = transport_(request);
    if (!response.ok()) {
        return core::make_unexpected(
            AcrClientError::Transient("ACR token request failed: " + response.error()));
    }

    const int token_status = StatusCode(response.value());
    if (token_status == 401 || token_status == 403) {
        return core::make_unexpected(AcrClientError::PermissionDenied(
            "ACR token service returned " + StatusText(token_status)));
    }
    if (token_status >= 500) {
        return core::make_unexpected(AcrClientError::Transient(
            "ACR token service returned " + StatusText(token_status)));
    }
    if (token_status < 200 || token_status >= 300) {
        return core::make_unexpected(AcrClientError::Registry(
            "ACR token service returned " + StatusText(token_status)));
    }

    const core::Expected<core::Json, core::AnyError> body =
        core::Json::Parse(ResponseBodyText(response.value()));
    if (!body.ok()) {
        return core::make_unexpected(
            AcrClientError::Registry("parse ACR token response: " + body.error().chain()));
    }
    // Rust deserialises the body into `TokenResponse { token: Option<String>,
    // access_token: Option<String>, expires_in: Option<u64> }` behind a single
    // `?`. So a *wrongly typed* field is a parse failure for the whole
    // response, not a field to skip -- only `null`/absent is optional. Reading
    // past a bad type instead would silently fall back to the default TTL and
    // cache a token for the wrong length of time.
    std::string token;
    int64_t ttl_ms = kAcrDefaultTokenTtlMs;
    if (body.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(
            AcrClientError::Registry("parse ACR token response: expected an object"));
    }
    const core::JsonObject& fields = body.value().as_object();
    for (int field = 0; field < 2; ++field) {
        // `token` first, then `access_token`: Rust's `body.token.or(..)`.
        const core::JsonObject::const_iterator at =
            fields.find(field == 0 ? "token" : "access_token");
        if (at == fields.end() || at->second.kind() == core::Json::Kind::Null) continue;
        if (at->second.kind() != core::Json::Kind::String) {
            return core::make_unexpected(AcrClientError::Registry(
                std::string("parse ACR token response: ") +
                (field == 0 ? "token" : "access_token") + " is not a string"));
        }
        if (token.empty()) token = at->second.as_string();
    }
    const core::JsonObject::const_iterator expires = fields.find("expires_in");
    if (expires != fields.end() && expires->second.kind() != core::Json::Kind::Null) {
        // `u64`, so a float or a negative is a deserialisation error upstream.
        if (expires->second.kind() != core::Json::Kind::Int || expires->second.as_int() < 0) {
            return core::make_unexpected(AcrClientError::Registry(
                "parse ACR token response: expires_in is not a non-negative integer"));
        }
        ttl_ms = expires->second.as_int() * 1000;
    }
    if (token.empty()) {
        // Rust `ok_or_else` on `token.or(access_token)`. An empty *string* also
        // lands here: an empty bearer token is not a credential, and sending
        // `Authorization: Bearer ` would earn a 401 reported as a permission
        // failure rather than as this malformed response.
        return core::make_unexpected(
            AcrClientError::Registry("ACR token response did not contain token"));
    }

    // `saturating_sub`: a TTL shorter than the skew becomes zero rather than
    // negative, so such a token is simply never cached.
    const int64_t effective =
        ttl_ms > kAcrTokenExpirySkewMs ? ttl_ms - kAcrTokenExpirySkewMs : 0;
    CachedTokenEntry entry;
    entry.token = token;
    entry.expires_at_ms = clock_() + effective;
    token_cache_[cache_key] = entry;
    return "Bearer " + token;
}

core::Expected<core::Optional<std::string>, AcrClientError> AcrClient::CachedAuthHeader(
    const std::string& registry_url, const std::string& repository) {
    const std::map<std::string, std::string>::const_iterator at =
        challenge_cache_.find(ChallengeCacheKey(registry_url));
    // No challenge learned yet: the first request goes out unauthenticated and
    // the 401 it earns is what teaches one. Guessing a challenge instead would
    // send the password to an endpoint that never asked for it.
    if (at == challenge_cache_.end()) return core::Optional<std::string>();

    const core::Expected<std::string, AcrClientError> header =
        AuthHeader(registry_url, repository, at->second);
    if (!header.ok()) return core::make_unexpected(header.error());
    return core::Optional<std::string>(header.value());
}

core::Expected<AcrClient::HttpResponse, AcrClientError> AcrClient::SendOnce(
    const std::string& method, const std::string& url, const std::string& repository,
    const core::Optional<AcrRequestBody>& body) {
    const core::Expected<core::Optional<std::string>, AcrClientError> cached =
        CachedAuthHeader(url, repository);
    if (!cached.ok()) return core::make_unexpected(cached.error());

    core::Expected<HttpResponse, AcrClientError> response =
        Request(method, url, cached.value(), body);
    if (!response.ok()) return response;

    if (StatusCode(response.value()) != 401) {
        // A 403 is distinguished from other failures here because it means the
        // credentials were understood and refused -- re-authenticating would
        // produce the same 403.
        if (StatusCode(response.value()) == 403) {
            return core::make_unexpected(
                AcrClientError::PermissionDenied(method + " " + url + " returned 403"));
        }
        return response;
    }

    const core::Optional<std::string> challenge =
        FindHeader(response.value().headers, "WWW-Authenticate");
    if (!challenge.has_value()) {
        return core::make_unexpected(AcrClientError::PermissionDenied(
            "registry returned 401 without WWW-Authenticate"));
    }
    challenge_cache_[ChallengeCacheKey(url)] = *challenge;

    const core::Expected<std::string, AcrClientError> header =
        AuthHeader(url, repository, *challenge);
    if (!header.ok()) return core::make_unexpected(header.error());

    // The body goes out again with the retry. Omitting it would turn an
    // authenticated chunk upload into an empty one that the registry accepts,
    // leaving a blob whose content does not match its digest.
    response = Request(method, url, core::Optional<std::string>(header.value()), body);
    if (!response.ok()) return response;
    const int retried_status = StatusCode(response.value());
    if (retried_status == 401 || retried_status == 403) {
        // Authenticated and still refused: retrying cannot help, so this is
        // terminal rather than transient.
        return core::make_unexpected(AcrClientError::PermissionDenied(
            method + " " + url + " returned " + StatusText(retried_status)));
    }
    return response;
}

core::Expected<AcrClient::HttpResponse, AcrClientError> AcrClient::SendWithRetries(
    const std::string& method, const std::string& url, const std::string& repository,
    const core::Optional<AcrRequestBody>& body) {
    core::Optional<AcrClientError> last_transient;
    int64_t backoff = options_.retry_initial_backoff_ms;

    // `0..=retry_count`, so the default of 2 is three attempts.
    for (int attempt = 0; attempt <= options_.retry_count; ++attempt) {
        // Rust clones the body per attempt. Held by reference here instead,
        // which has the same effect and does not copy a chunk-sized buffer
        // once per retry.
        const core::Expected<HttpResponse, AcrClientError> response =
            SendOnce(method, url, repository, body);
        if (response.ok()) {
            const int status = StatusCode(response.value());
            if (!IsRetryableStatus(status)) return response;

            last_transient = core::Optional<AcrClientError>(AcrClientError::Transient(
                method + " " + url + " returned " + StatusText(status)));
            if (attempt < options_.retry_count) {
                // `Retry-After` wins over the backoff: the registry knows when
                // it will be ready and ignoring it is how a throttled client
                // gets throttled harder.
                const core::Optional<int64_t> retry_after =
                    RetryAfterDelayMs(FindHeader(response.value().headers, "Retry-After"));
                sleeper_(retry_after.has_value() ? *retry_after : backoff);
                backoff = backoff * 2;
            }
            continue;
        }
        // Only a transient error is retried; everything else is returned at
        // once so a 403 does not cost three round trips.
        if (!response.error().is_transient()) return response;
        last_transient = core::Optional<AcrClientError>(response.error());

        if (attempt < options_.retry_count) {
            sleeper_(backoff);
            backoff = backoff * 2;
        }
    }
    if (last_transient.has_value()) return core::make_unexpected(*last_transient);
    return core::make_unexpected(
        AcrClientError::Transient(method + " " + url + " exhausted retry budget"));
}

core::Expected<core::Unit, AcrClientError> AcrClient::DeleteManifestByDigest(
    const std::string& registry, const std::string& repository, const std::string& digest) {
    if (digest.empty()) {
        // Sent anyway, the URL would end in `/manifests/` -- which is not a
        // no-op at a registry, and whose answer this code would read as
        // success.
        return core::make_unexpected(AcrClientError::Registry(
            "cannot roll back ACR manifest because manifest digest is unknown"));
    }
    const std::string url =
        "https://" + registry + "/v2/" + repository + "/manifests/" + digest;
    return DeleteManifestUrl(url, repository);
}

core::Expected<core::Unit, AcrClientError> AcrClient::DeleteManifestUrl(
    const std::string& manifest_digest_url, const std::string& repository) {
    const core::Expected<HttpResponse, AcrClientError> response =
        SendWithRetries("DELETE", manifest_digest_url, repository);
    if (!response.ok()) return core::make_unexpected(response.error());

    const int status = StatusCode(response.value());
    // 404 is success: the manifest being gone is the goal, and a rollback that
    // failed on an already-deleted manifest could never finish a retried
    // delete. 202 is what a registry returns for an accepted async delete.
    if (status == 202 || status == 404 || (status >= 200 && status < 300)) {
        return core::Unit();
    }
    return core::make_unexpected(StatusError(status, "delete ACR image manifest"));
}

// ---- push half ------------------------------------------------------------

core::Expected<core::Unit, AcrClientError> AcrClient::EnsureManifestAbsent(
    const std::string& manifest_url, const std::string& repository, const std::string& tag) {
    const core::Expected<HttpResponse, AcrClientError> response =
        SendWithRetries("HEAD", manifest_url, repository);
    if (!response.ok()) return core::make_unexpected(response.error());

    const int status = StatusCode(response.value());
    // 404 is the success case. Every other 2xx means something answers to the
    // tag, which is refused rather than overwritten: a published tag is what a
    // caller may already have pulled, and replacing its manifest silently
    // changes what that reference means.
    if (status == 404) return core::Unit();
    if (status >= 200 && status < 300) {
        return core::make_unexpected(AcrClientError::ManifestExists(tag));
    }
    return core::make_unexpected(StatusError(status, "check ACR manifest existence"));
}

core::Expected<bool, AcrClientError> AcrClient::BlobExists(const std::string& repo_blob_url,
                                                           const std::string& repository,
                                                           const std::string& digest) {
    const std::string url = TrimTrailingSlashes(repo_blob_url) + "/" + digest;
    const core::Expected<HttpResponse, AcrClientError> response =
        SendWithRetries("HEAD", url, repository);
    if (!response.ok()) return core::make_unexpected(response.error());

    const int status = StatusCode(response.value());
    if (status == 404) return false;
    if (status >= 200 && status < 300) return true;
    // Not treated as "absent": an error status says nothing about whether the
    // blob is there, and guessing absent would re-upload a layer the registry
    // already holds while hiding the failure.
    return core::make_unexpected(StatusError(status, "check ACR blob existence"));
}

core::Expected<std::string, AcrClientError> AcrClient::StartBlobUpload(
    const std::string& upload_url, const std::string& repository) {
    const core::Expected<HttpResponse, AcrClientError> response =
        SendWithRetries("POST", upload_url, repository);
    if (!response.ok()) return core::make_unexpected(response.error());

    const int status = StatusCode(response.value());
    if (status != 202 && !(status >= 200 && status < 300)) {
        return core::make_unexpected(StatusError(status, "start ACR blob upload"));
    }
    const core::Optional<std::string> location =
        FindHeader(response.value().headers, "Location");
    if (!location.has_value()) {
        // There is nowhere to send the chunks. Falling back to `upload_url`
        // would post them to the session-opening endpoint, which opens a new
        // session per chunk and leaves them all incomplete.
        return core::make_unexpected(
            AcrClientError::Registry("ACR blob upload response missing Location header"));
    }
    return AbsolutizeLocation(upload_url, *location);
}

core::Expected<core::Unit, AcrClientError> AcrClient::CompleteBlobUpload(
    const std::string& session_url, const std::string& repository,
    const std::vector<uint8_t>& bytes, const std::string& digest) {
    const std::string complete_url = AppendDigestQuery(session_url, digest);
    const core::Expected<HttpResponse, AcrClientError> response = SendWithRetries(
        "PUT", complete_url, repository,
        core::Optional<AcrRequestBody>(AcrRequestBody::Blob(bytes)));
    if (!response.ok()) return core::make_unexpected(response.error());

    const int status = StatusCode(response.value());
    if (status == 201 || (status >= 200 && status < 300)) return core::Unit();
    return core::make_unexpected(StatusError(status, "complete ACR blob upload"));
}

core::Expected<core::Unit, AcrClientError> AcrClient::UploadBlob(
    const std::string& upload_url, const std::string& repository, const std::string& path,
    const std::string& digest) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.good()) {
        return core::make_unexpected(AcrClientError::Registry(
            "open ACR blob '" + path + "': " + std::strerror(errno)));
    }
    struct stat info;
    if (::stat(path.c_str(), &info) != 0) {
        return core::make_unexpected(AcrClientError::Registry(
            "stat ACR blob '" + path + "': " + std::strerror(errno)));
    }
    const uint64_t size = static_cast<uint64_t>(info.st_size);

    const core::Expected<std::string, AcrClientError> session =
        StartBlobUpload(upload_url, repository);
    if (!session.ok()) return core::make_unexpected(session.error());
    std::string session_url = session.value();

    // Clamped to at least 1: a zero would make the loop read nothing and
    // never advance past the first chunk.
    const std::size_t chunk_size = options_.upload_chunk_size > 0
                                       ? options_.upload_chunk_size
                                       : static_cast<std::size_t>(1);
    uint64_t offset = 0;
    while (offset < size) {
        const uint64_t remaining = size - offset;
        const std::size_t count = remaining < static_cast<uint64_t>(chunk_size)
                                      ? static_cast<std::size_t>(remaining)
                                      : chunk_size;
        std::vector<uint8_t> buffer(count);
        file.read(reinterpret_cast<char*>(&buffer[0]), static_cast<std::streamsize>(count));
        if (static_cast<std::size_t>(file.gcount()) != count) {
            // Rust's `read_exact`. A short read means the file shrank under
            // us, and sending the partial chunk would complete an upload
            // whose content does not match the digest being claimed.
            return core::make_unexpected(
                AcrClientError::Registry("read ACR blob '" + path + "': unexpected end of file"));
        }
        const uint64_t end = offset + count - 1;

        // A retried `PATCH` reuses the same session and byte range. OCI
        // registries are expected to tolerate that idempotent retry; one that
        // does not would need this path to restart the session from offset 0.
        const core::Expected<HttpResponse, AcrClientError> response = SendWithRetries(
            "PATCH", session_url, repository,
            core::Optional<AcrRequestBody>(AcrRequestBody::BlobRange(buffer, offset, end)));
        if (!response.ok()) return core::make_unexpected(response.error());

        const int status = StatusCode(response.value());
        if (status != 202 && !(status >= 200 && status < 300)) {
            return core::make_unexpected(StatusError(status, "upload ACR blob chunk"));
        }
        // The session URL may move between chunks, and a registry that
        // relocates it will not accept the old one again.
        const core::Optional<std::string> location =
            FindHeader(response.value().headers, "Location");
        if (location.has_value()) {
            const core::Expected<std::string, AcrClientError> moved =
                AbsolutizeLocation(session_url, *location);
            if (!moved.ok()) return core::make_unexpected(moved.error());
            session_url = moved.value();
        }
        offset += count;
    }

    // The completing `PUT` carries no further bytes: everything went out as
    // chunks, and repeating the tail here would append it twice.
    return CompleteBlobUpload(session_url, repository, std::vector<uint8_t>(), digest);
}

core::Expected<std::pair<std::string, uint64_t>, AcrClientError>
AcrClient::UploadBlobWithDescriptor(const std::string& upload_url,
                                    const std::string& repo_blob_url,
                                    const std::string& repository, const std::string& path,
                                    const std::string& digest, uint64_t size) {
    if (digest.empty()) {
        // The digest is what the manifest will claim and nothing downstream
        // re-derives it, so an empty one would publish a manifest pointing at
        // `sha256:` and the layer would be unreachable.
        return core::make_unexpected(
            AcrClientError::Registry("ACR blob '" + path + "' is missing digest"));
    }
    if (size == 0) {
        return core::make_unexpected(
            AcrClientError::Registry("ACR blob '" + path + "' is empty"));
    }
    struct stat info;
    if (::stat(path.c_str(), &info) != 0) {
        return core::make_unexpected(AcrClientError::Registry(
            "stat ACR blob '" + path + "': " + std::strerror(errno)));
    }
    const uint64_t source_size = static_cast<uint64_t>(info.st_size);
    if (source_size != size) {
        // Checked before uploading rather than after: a descriptor that
        // disagrees with the file means one of the two is stale, and either
        // way the manifest would describe a layer that is not what was sent.
        return core::make_unexpected(AcrClientError::Registry(
            "ACR blob descriptor size mismatch for '" + path + "': descriptor says " +
            Decimal(size) + ", file has " + Decimal(source_size)));
    }

    const core::Expected<bool, AcrClientError> exists =
        BlobExists(repo_blob_url, repository, digest);
    if (!exists.ok()) return core::make_unexpected(exists.error());
    // Content addressed, so a digest the registry already has needs no
    // upload. This is what makes a re-publish of an unchanged layer free.
    if (exists.value()) return std::make_pair(digest, size);

    const core::Expected<core::Unit, AcrClientError> uploaded =
        UploadBlob(upload_url, repository, path, digest);
    if (!uploaded.ok()) return core::make_unexpected(uploaded.error());
    return std::make_pair(digest, size);
}

core::Expected<core::Unit, AcrClientError> AcrClient::UploadBlobBytes(
    const std::string& upload_url, const std::string& repository,
    const std::vector<uint8_t>& bytes, const std::string& digest) {
    const core::Expected<std::string, AcrClientError> session =
        StartBlobUpload(upload_url, repository);
    if (!session.ok()) return core::make_unexpected(session.error());
    // No `PATCH` phase: the config blob is a few hundred bytes, and chunking
    // something smaller than one chunk only adds a round trip.
    return CompleteBlobUpload(session.value(), repository, bytes, digest);
}

core::Expected<std::string, AcrClientError> AcrClient::PutManifest(
    const std::string& manifest_url, const std::string& repository,
    const std::vector<uint8_t>& manifest_bytes) {
    // Computed before the request, so a response without
    // `Docker-Content-Digest` is answered rather than failed.
    const std::string fallback_digest = core::Sha256Digest(
        manifest_bytes.empty() ? static_cast<const void*>("") : &manifest_bytes[0],
        manifest_bytes.size());

    const core::Expected<HttpResponse, AcrClientError> response = SendWithRetries(
        "PUT", manifest_url, repository,
        core::Optional<AcrRequestBody>(AcrRequestBody::Manifest(manifest_bytes)));
    if (!response.ok()) return core::make_unexpected(response.error());

    const int status = StatusCode(response.value());
    if (!(status == 201 || (status >= 200 && status < 300))) {
        return core::make_unexpected(StatusError(status, "put ACR image manifest"));
    }
    // The registry's digest wins: that is the one it will answer to later.
    // They differ only when the registry re-serialised the manifest, and in
    // that case the local digest names nothing.
    const core::Optional<std::string> reported =
        FindHeader(response.value().headers, "Docker-Content-Digest");
    return reported.has_value() ? *reported : fallback_digest;
}

// ---- AcrDiskImageExporter -------------------------------------------------

AcrDiskImageExporter::AcrDiskImageExporter(
    const ClientBuilder& client_builder,
    const overlaybd::OverlaybdCompactOutput& publish_compression)
    : client_builder_(client_builder), publish_compression_(publish_compression) {}

AcrDiskImageExporter::ClientBuilder AcrDiskImageExporter::DockerConfigClientBuilder(
    const AcrClientOptions& options) {
    return [options](const std::string& registry) -> core::Expected<AcrClient, AcrClientError> {
        return AcrClient::FromDockerConfig(registry, AcrClient::CurlTransport(), options);
    };
}

core::Expected<AcrClient*, AcrClientError> AcrDiskImageExporter::ClientForRegistry(
    const std::string& registry) {
    const std::map<std::string, AcrClient>::iterator at = clients_.find(registry);
    // Cached per registry so a snapshot with several publications in one
    // registry pays the credential load and the 401 round trip once.
    if (at != clients_.end()) return &at->second;

    const core::Expected<AcrClient, AcrClientError> built = client_builder_(registry);
    if (!built.ok()) return core::make_unexpected(built.error());
    const std::pair<std::map<std::string, AcrClient>::iterator, bool> inserted =
        clients_.insert(std::make_pair(registry, built.value()));
    return &inserted.first->second;
}

namespace {

/// Rust `AcrDiskImageExporter::upload_local_delta`.
///
/// A free function rather than a method: the only thing it needs from the
/// exporter is `publish_compression`, and taking that as an argument keeps
/// `LocalSnapshotDelta` — declared in `acr_source_image.h`, which includes
/// `acr.h` — out of the class declaration.
RepositoryResult<PreparedLayerUpload> UploadLocalDelta(
    AcrClient& client, const std::string& upload_url, const std::string& repo_blob_url,
    const std::string& repository, const LocalSnapshotDelta& local,
    const overlaybd::OverlaybdCompactOutput& publish_compression) {
    std::string prepare_from = local.path;
    core::Optional<std::string> trusted_digest;
    uint64_t trusted_size = 0;
    OwnedTempPath dense_temp;

    if (local.descriptor.is_raw()) {
        // Publish compression recontainerizes a raw delta as zfile, which
        // changes the physical bytes; `PrepareLayerUpload` therefore re-hashes
        // its output, and the digest and size below always describe the bytes
        // that were uploaded. An input that is already zfile is passed
        // through, so its descriptor stays valid.
        trusted_digest = core::Optional<std::string>(local.descriptor.digest);
        trusted_size = local.descriptor.size;
    } else {
        // `DenseOverlaybd`: the holes have to be materialised before anything
        // can be hashed. The planner deliberately left this one undescribed,
        // because hashing the sparse file would record a digest for bytes the
        // registry never receives.
        //
        // Rust stages this in `NamedTempFile::new()`, i.e. `$TMPDIR`;
        // `OwnedTempPath::Create` is the same placement and also gives the
        // unlink-on-last-reference that `retain_source` relies on.
        const core::Expected<OwnedTempPath, std::string> temp =
            OwnedTempPath::Create("agentenv-acr-dense-delta-");
        if (!temp.ok()) {
            return core::make_unexpected(RepositoryError::Backend(
                "create temp dense ACR delta for '" + local.path + "'", temp.error()));
        }
        dense_temp = temp.value();

        const core::Expected<std::shared_ptr<storage::overlaybd::LocalFile>, std::string> dest =
            storage::overlaybd::LocalFile::OpenRw(dense_temp.path(), false);
        if (!dest.ok()) {
            return core::make_unexpected(RepositoryError::Backend(
                "dense-export overlaybd ACR delta '" + local.path + "'", dest.error()));
        }
        const core::Expected<core::Unit, std::string> written =
            storage::overlaybd::WriteDenseLayerToFile(local.path, dest.value());
        if (!written.ok()) {
            return core::make_unexpected(RepositoryError::Backend(
                "dense-export overlaybd ACR delta '" + local.path + "'", written.error()));
        }
        // Rust takes the descriptor `write_dense_overlaybd_layer_to_file`
        // returns; it is the digest and size of exactly these dense bytes.
        const core::Expected<core::FileDigest, std::string> descriptor =
            core::DescribeFile(dense_temp.path());
        if (!descriptor.ok()) {
            return core::make_unexpected(RepositoryError::Backend(
                "dense-export overlaybd ACR delta '" + local.path + "'", descriptor.error()));
        }
        prepare_from = dense_temp.path();
        trusted_digest = core::Optional<std::string>(descriptor.value().sha256);
        trusted_size = descriptor.value().size;
    }

    const RepositoryResult<PreparedLayerUpload> prepared =
        PrepareLayerUpload(prepare_from, publish_compression, trusted_digest, trusted_size);
    if (!prepared.ok()) return prepared;

    PreparedLayerUpload upload = prepared.value();
    if (dense_temp.valid()) {
        // Rust `upload.retain_source(dense_temp)`. Without this the dense file
        // is unlinked when `dense_temp` goes out of scope, and in the
        // uncompressed case that file *is* what the upload reads. A
        // recontainerized output owns its own temporary, which `RetainSource`
        // deliberately does not displace.
        upload.RetainSource(dense_temp);
    }

    const core::Expected<std::pair<std::string, uint64_t>, AcrClientError> uploaded =
        client.UploadBlobWithDescriptor(upload_url, repo_blob_url, repository, upload.Path(),
                                        upload.Digest(), upload.Size());
    if (!uploaded.ok()) {
        return core::make_unexpected(uploaded.error().ToRepositoryError());
    }
    return upload;
}

}  // namespace

RepositoryResult<DiskImageExportOutcome> AcrDiskImageExporter::Export(
    const core::SnapshotId& snapshot_id, const DiskImageSubject& subject,
    const std::string& image_config_path,
    const core::Optional<SnapshotOciConfigInput>& config, bool allow_loopback_http) {
    const RepositoryResult<SourceRegistryImage> image =
        LoadSourceRegistryImage(image_config_path, allow_loopback_http);
    if (!image.ok()) return core::make_unexpected(image.error());
    const SourceRegistryRepository& target = image.value().target;

    // Before any upload: a tag rejected afterwards would leave the blobs
    // pushed with no manifest naming them.
    const std::string tag = SnapshotTag(snapshot_id, subject);
    const RepositoryResult<core::Unit> valid_tag = ValidateSnapshotTag(tag);
    if (!valid_tag.ok()) return core::make_unexpected(valid_tag.error());

    const core::Expected<AcrClient*, AcrClientError> client =
        ClientForRegistry(target.registry);
    if (!client.ok()) return core::make_unexpected(client.error().ToRepositoryError());

    const std::string manifest_url = target.ManifestUrl(tag);
    const core::Expected<core::Unit, AcrClientError> absent =
        client.value()->EnsureManifestAbsent(manifest_url, target.repository, tag);
    if (!absent.ok()) return core::make_unexpected(absent.error().ToRepositoryError());

    const std::string upload_url = target.UploadUrl();
    std::vector<OciDescriptor> descriptors;
    std::vector<OverlaybdLayerRef> committed_layers;
    descriptors.reserve(image.value().layers.size());
    committed_layers.reserve(image.value().layers.size());
    // Rust returns these in `DiskImageExportOutcome::local_layers` for the
    // post-commit P2P announcement. Each one owns the temporary file its
    // upload read from, so they are also what keeps those bytes on disk until
    // the announcement has copied them.
    std::vector<PreparedLayerUpload> local_layers;

    for (std::vector<SourceRegistryLayer>::size_type i = 0; i < image.value().layers.size();
         ++i) {
        const SourceRegistryLayer& layer = image.value().layers[i];
        std::string digest;
        uint64_t size = 0;
        if (layer.is_remote()) {
            // Already in the registry, so publishing only has to name it.
            digest = layer.digest;
            size = layer.size;
        } else {
            const RepositoryResult<PreparedLayerUpload> uploaded =
                UploadLocalDelta(*client.value(), upload_url, target.repo_blob_url,
                                 target.repository, layer.local_delta, publish_compression_);
            if (!uploaded.ok()) return core::make_unexpected(uploaded.error());
            // The *uploaded* bytes, which is the whole point of routing
            // through `PreparedLayerUpload`: a recontainerized delta no
            // longer matches the planner's descriptor.
            digest = uploaded.value().Digest();
            size = uploaded.value().Size();
            local_layers.push_back(uploaded.value());
        }
        descriptors.push_back(OciDescriptor::OverlaybdLayer(digest, size));
        ExternalLayer external;
        external.digest = digest;
        external.repo_blob_url = target.repo_blob_url;
        external.size = size;
        committed_layers.push_back(OverlaybdLayerRef::External(external));
    }

    const OciConfigBlob config_blob =
        config.has_value()
            ? SnapshotOciConfigBlob(HostArchitectureForOci(), config->context,
                                    config->raw_config)
            : MinimalOciConfigBlob(HostArchitectureForOci());

    // Content addressed like any other blob, so an unchanged configuration
    // costs one `HEAD` on a re-publish.
    const core::Expected<bool, AcrClientError> config_exists = client.value()->BlobExists(
        target.repo_blob_url, target.repository, config_blob.digest);
    if (!config_exists.ok()) {
        return core::make_unexpected(config_exists.error().ToRepositoryError());
    }
    if (!config_exists.value()) {
        const core::Expected<core::Unit, AcrClientError> uploaded =
            client.value()->UploadBlobBytes(
                upload_url, target.repository,
                std::vector<uint8_t>(config_blob.bytes.begin(), config_blob.bytes.end()),
                config_blob.digest);
        if (!uploaded.ok()) return core::make_unexpected(uploaded.error().ToRepositoryError());
    }

    // Last, because the manifest is what makes the rest resolvable: until it
    // lands the blobs are unreferenced, so a failure above leaves nothing a
    // puller can see.
    const std::string manifest = BuildOciImageManifest(
        OciDescriptor::Config(config_blob.digest, config_blob.size), descriptors, tag);
    const core::Expected<std::string, AcrClientError> manifest_digest =
        client.value()->PutManifest(manifest_url, target.repository,
                                    std::vector<uint8_t>(manifest.begin(), manifest.end()));
    if (!manifest_digest.ok()) {
        return core::make_unexpected(manifest_digest.error().ToRepositoryError());
    }

    DiskImageExportOutcome outcome;
    outcome.layers = committed_layers;
    // Rust `local_layers: self.local_layers`. Handing these back is what keeps
    // each recontainerized/dense temporary alive past this function, so the
    // post-commit P2P announcement can still read the exact uploaded bytes.
    outcome.local_layers = local_layers;
    PersistedDiskImagePublication publication;
    publication.image_ref = target.ImageRef(tag);
    publication.tag = tag;
    // The registry's digest when it reported one -- that is what a rollback
    // will aim its DELETE at.
    publication.manifest_digest = manifest_digest.value();
    publication.repo_blob_url = target.repo_blob_url;
    outcome.publication = core::Optional<PersistedDiskImagePublication>(publication);
    return outcome;
}

RepositoryResult<core::Unit> AcrDiskImageExporter::RollbackPublication(
    const PersistedDiskImagePublication& publication, bool allow_loopback_http) {
    const RepositoryResult<SourceRegistryRepository> parsed =
        SourceRegistryRepository::Parse(publication.repo_blob_url, allow_loopback_http);
    if (!parsed.ok()) {
        // Success with a warning. This runs inside a snapshot delete, and a
        // publication whose URL cannot be parsed can never be rolled back by
        // any number of retries -- failing would wedge the delete forever on a
        // record that is already unreachable.
        AGENTENV_WARN("cannot roll back ACR publication with invalid repoBlobUrl "
                      << publication.repo_blob_url << ": " << parsed.error().ToString());
        return core::Unit();
    }

    const core::Expected<AcrClient*, AcrClientError> client =
        ClientForRegistry(parsed.value().registry);
    if (!client.ok()) return core::make_unexpected(client.error().ToRepositoryError());

    const core::Expected<core::Unit, AcrClientError> deleted =
        client.value()->DeleteManifestByDigest(parsed.value().registry,
                                               parsed.value().repository,
                                               publication.manifest_digest);
    if (!deleted.ok()) return core::make_unexpected(deleted.error().ToRepositoryError());
    return core::Unit();
}

}  // namespace image_export
}  // namespace snapshot
}  // namespace agentenv
