// SPDX-License-Identifier: MIT
// Rust: the pure helpers of src/snapshot/image_export/acr/{client.rs,
// source_image.rs} — URL shapes, auth challenges and Docker credentials.
//
// Separated from the client because every rule here is a parse that decides
// where a DELETE is aimed or which secret is attached to it, and those are
// worth testing without a transport in the way.
#include "agentenv/snapshot/image_export/acr.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "agentenv/core/base64.h"

namespace agentenv {
namespace snapshot {
namespace image_export {

using repository::RepositoryError;
using repository::RepositoryResult;

const int64_t kAcrDefaultTokenTtlMs = 300 * 1000;
const int64_t kAcrTokenExpirySkewMs = 30 * 1000;

namespace {

std::string ToLowerAscii(const std::string& text) {
    std::string out = text;
    for (std::string::size_type i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
    return out;
}

std::string TrimAscii(const std::string& text) {
    std::string::size_type begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    std::string::size_type end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

std::string TrimTrailing(const std::string& text, char ch) {
    std::string::size_type end = text.size();
    while (end > 0 && text[end - 1] == ch) --end;
    return text.substr(0, end);
}

/// Strips one trailing occurrence of `suffix`, Rust's `trim_end_matches` for a
/// multi-character pattern applied once.
std::string StripSuffix(const std::string& text, const std::string& suffix) {
    if (text.size() >= suffix.size() &&
        text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return text.substr(0, text.size() - suffix.size());
    }
    return text;
}

/// The slice of a URL this module needs. Deliberately not a general parser:
/// only `scheme://host[:port]/path` is recognised, and anything else is a
/// parse failure rather than a guess.
struct ParsedUrl {
    std::string scheme;
    std::string host;
    core::Optional<uint16_t> port;
    std::string path;
};

bool ParseUrl(const std::string& text, ParsedUrl* out) {
    const std::string::size_type scheme_end = text.find("://");
    if (scheme_end == std::string::npos || scheme_end == 0) return false;
    out->scheme = ToLowerAscii(text.substr(0, scheme_end));

    const std::string rest = text.substr(scheme_end + 3);
    // A `?` or `#` ends the authority too, not just `/`; without them a query
    // string would be absorbed into the host.
    const std::string::size_type path_at = rest.find_first_of("/?#");
    const std::string authority =
        path_at == std::string::npos ? rest : rest.substr(0, path_at);
    out->path = path_at == std::string::npos ? std::string()
                                            : rest.substr(path_at);
    if (!out->path.empty() && out->path[0] != '/') out->path = std::string();
    if (authority.empty()) return false;

    // Userinfo is dropped, not rejected. Rust reaches the authority through
    // `Url::host_str()`, which returns the host alone, and rebuilds
    // `repo_blob_url` from the parsed registry — so credentials embedded in a
    // recorded `repoBlobUrl` never reach the string that gets cached or
    // logged, and the publication still resolves. Refusing here instead would
    // fail the whole export or, worse, the rollback inside a snapshot delete,
    // leaving a "deleted" snapshot published in the registry.
    const std::string::size_type userinfo_end = authority.rfind('@');
    const std::string host_port = userinfo_end == std::string::npos
                                      ? authority
                                      : authority.substr(userinfo_end + 1);
    if (host_port.empty()) return false;

    const std::string::size_type colon = host_port.rfind(':');
    if (colon == std::string::npos) {
        out->host = ToLowerAscii(host_port);
    } else {
        const std::string digits = host_port.substr(colon + 1);
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        const unsigned long value = std::strtoul(digits.c_str(), NULL, 10);
        if (value == 0 || value > 65535) return false;
        out->host = ToLowerAscii(host_port.substr(0, colon));
        out->port = core::Optional<uint16_t>(static_cast<uint16_t>(value));
    }
    return !out->host.empty();
}

/// `host[:port]`, with no port rendered when absent.
std::string HostPort(const ParsedUrl& url) {
    if (!url.port.has_value()) return url.host;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(*url.port));
    return url.host + ":" + buf;
}

std::vector<std::string> PathSegments(const std::string& path) {
    std::vector<std::string> out;
    std::string::size_type at = 0;
    while (at < path.size()) {
        if (path[at] == '/') {
            ++at;
            continue;
        }
        const std::string::size_type next = path.find('/', at);
        if (next == std::string::npos) {
            out.push_back(path.substr(at));
            break;
        }
        out.push_back(path.substr(at, next - at));
        at = next + 1;
    }
    return out;
}

}  // namespace

// ---- SourceRegistryRepository ---------------------------------------------

RepositoryResult<SourceRegistryRepository> SourceRegistryRepository::Parse(
    const std::string& repo_blob_url, bool allow_loopback_http) {
    ParsedUrl url;
    if (!ParseUrl(repo_blob_url, &url)) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "invalid ACR repoBlobUrl '" + repo_blob_url + "'"));
    }

    // Rust's `cfg!(test)` loopback exemption, made an explicit parameter.
    const bool test_loopback_http =
        allow_loopback_http && url.scheme == "http" &&
        (url.host == "127.0.0.1" || url.host == "localhost" || url.host == "::1");
    if (url.scheme != "https" && !test_loopback_http) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "ACR repoBlobUrl must use https: " + repo_blob_url));
    }

    // Strip the default HTTPS :443 so equivalent source URLs normalize to one
    // registry; keep every other explicit port intact.
    std::string registry;
    if (url.port.has_value() && *url.port == 443 && url.scheme == "https") {
        registry = url.host;
    } else {
        registry = HostPort(url);
    }

    const std::vector<std::string> segments = PathSegments(url.path);
    if (segments.size() < 3 || segments.front() != "v2" || segments.back() != "blobs") {
        return core::make_unexpected(RepositoryError::Unsupported(
            "ACR repoBlobUrl must have shape https://<registry>/v2/<repo>/blobs: " +
            repo_blob_url));
    }
    std::string repository;
    for (std::vector<std::string>::size_type i = 1; i + 1 < segments.size(); ++i) {
        if (!repository.empty()) repository += "/";
        repository += segments[i];
    }
    if (repository.empty()) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "ACR repoBlobUrl repository is empty: " + repo_blob_url));
    }

    SourceRegistryRepository out;
    out.registry = registry;
    out.repository = repository;
    // Re-rendered from the *normalised* registry, which is what makes two
    // spellings of one source compare equal in
    // `snapshot_managed_publication_for_target`.
    out.repo_blob_url = url.scheme + "://" + registry + TrimTrailing(url.path, '/');
    return out;
}

std::string SourceRegistryRepository::ImageRef(const std::string& tag) const {
    return registry + "/" + repository + ":" + tag;
}

namespace {

/// Rust `registry_api_url` — recovers the scheme from `repo_blob_url` rather
/// than hardcoding https, so a loopback HTTP source keeps speaking HTTP for
/// the upload and manifest calls instead of switching protocol mid-publish.
std::string RegistryApiUrl(const SourceRegistryRepository& source) {
    const std::string::size_type separator = source.repo_blob_url.find("://");
    const std::string scheme = separator == std::string::npos
                                   ? std::string("https")
                                   : source.repo_blob_url.substr(0, separator);
    return scheme + "://" + source.registry;
}

}  // namespace

std::string SourceRegistryRepository::UploadUrl() const {
    return RegistryApiUrl(*this) + "/v2/" + repository + "/blobs/uploads/";
}

std::string SourceRegistryRepository::ManifestUrl(const std::string& tag) const {
    return RegistryApiUrl(*this) + "/v2/" + repository + "/manifests/" + tag;
}

// ---- AcrClientError -------------------------------------------------------

AcrClientError AcrClientError::MissingCredentials(const std::string& registry) {
    AcrClientError e;
    e.kind = Kind::MissingCredentials;
    e.detail = registry;
    return e;
}
AcrClientError AcrClientError::PermissionDenied(const std::string& message) {
    AcrClientError e;
    e.kind = Kind::PermissionDenied;
    e.detail = message;
    return e;
}
AcrClientError AcrClientError::ManifestExists(const std::string& tag) {
    AcrClientError e;
    e.kind = Kind::ManifestExists;
    e.detail = tag;
    return e;
}
AcrClientError AcrClientError::Transient(const std::string& message) {
    AcrClientError e;
    e.kind = Kind::Transient;
    e.detail = message;
    return e;
}
AcrClientError AcrClientError::Registry(const std::string& message) {
    AcrClientError e;
    e.kind = Kind::Registry;
    e.detail = message;
    return e;
}

std::string AcrClientError::ToString() const {
    switch (kind) {
        case Kind::MissingCredentials:
            return "ACR credentials are missing for registry '" + detail + "'";
        case Kind::PermissionDenied:
            return "ACR registry rejected push/pull credentials: " + detail;
        case Kind::ManifestExists:
            return "ACR manifest already exists for tag '" + detail + "'";
        case Kind::Transient:
            return "transient ACR registry error: " + detail;
        case Kind::Registry:
        default:
            return "ACR registry error: " + detail;
    }
}

RepositoryError AcrClientError::ToRepositoryError() const {
    if (kind == Kind::ManifestExists) {
        return RepositoryError::InvalidRequest("ACR snapshot tag '" + detail +
                                               "' already exists");
    }
    return RepositoryError::Backend("ACR snapshot export", ToString());
}

// ---- status classification ------------------------------------------------

bool IsRetryableStatus(int status) {
    // 5xx or 429. A 4xx other than 429 will not become a different answer by
    // being asked again, and retrying one would only delay the real error.
    return status >= 500 || status == 429;
}

AcrClientError StatusError(int status, const std::string& operation) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", status);
    if (status == 401 || status == 403) {
        return AcrClientError::PermissionDenied(operation + " returned " + buf);
    }
    if (status >= 500) {
        return AcrClientError::Transient(operation + " returned " + buf);
    }
    return AcrClientError::Registry(operation + " returned " + buf);
}

core::Optional<int64_t> RetryAfterDelayMs(const core::Optional<std::string>& header) {
    if (!header.has_value()) return core::Optional<int64_t>();
    const std::string trimmed = TrimAscii(*header);
    if (trimmed.empty() || trimmed.find_first_not_of("0123456789") != std::string::npos) {
        // The HTTP-date form is ignored rather than mis-parsed; the caller
        // falls back to its backoff.
        return core::Optional<int64_t>();
    }
    return core::Optional<int64_t>(std::strtoll(trimmed.c_str(), NULL, 10) * 1000);
}

// ---- auth challenges ------------------------------------------------------

std::string BearerChallenge::CacheKey(const std::string& scope) const {
    return realm + "|" + service + "|" + scope;
}

namespace {

/// Rust `push_auth_field` — `key=value`, key lowercased and trimmed, value
/// trimmed then unquoted. A fragment with no `=` is dropped.
void PushAuthField(std::vector<std::pair<std::string, std::string> >* out,
                   const std::string& raw) {
    const std::string::size_type at = raw.find('=');
    if (at == std::string::npos) return;
    const std::string key = ToLowerAscii(TrimAscii(raw.substr(0, at)));
    std::string value = TrimAscii(raw.substr(at + 1));
    // Rust `trim_matches('"')`: quotes stripped from both ends.
    std::string::size_type begin = 0;
    std::string::size_type end = value.size();
    while (begin < end && value[begin] == '"') ++begin;
    while (end > begin && value[end - 1] == '"') --end;
    out->push_back(std::make_pair(key, value.substr(begin, end - begin)));
}

bool StartsWithIgnoreAsciiCase(const std::string& text, const std::string& prefix) {
    if (text.size() < prefix.size()) return false;
    return ToLowerAscii(text.substr(0, prefix.size())) == ToLowerAscii(prefix);
}

}  // namespace

std::vector<std::pair<std::string, std::string> > ParseAuthFields(const std::string& input) {
    std::vector<std::pair<std::string, std::string> > out;
    std::string buf;
    bool in_quotes = false;
    for (std::string::size_type i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '"') {
            in_quotes = !in_quotes;
            buf.push_back(ch);
        } else if (ch == ',' && !in_quotes) {
            // Only an unquoted comma separates fields: a realm URL may
            // legitimately contain one inside its quotes, and splitting there
            // would truncate the token endpoint.
            PushAuthField(&out, buf);
            buf.clear();
        } else {
            buf.push_back(ch);
        }
    }
    PushAuthField(&out, buf);
    return out;
}

core::Expected<BearerChallenge, AcrClientError> ParseBearerChallenge(
    const std::string& challenge) {
    if (!StartsWithIgnoreAsciiCase(challenge, "bearer ")) {
        return core::make_unexpected(
            AcrClientError::Registry("unsupported registry auth challenge: " + challenge));
    }
    const std::vector<std::pair<std::string, std::string> > fields =
        ParseAuthFields(challenge.substr(7));

    BearerChallenge out;
    bool has_realm = false;
    bool has_service = false;
    for (std::vector<std::pair<std::string, std::string> >::size_type i = 0;
         i < fields.size(); ++i) {
        // The *first* occurrence wins, matching Rust's `.find(..)`.
        if (!has_realm && fields[i].first == "realm") {
            out.realm = fields[i].second;
            has_realm = true;
        }
        if (!has_service && fields[i].first == "service") {
            out.service = fields[i].second;
            has_service = true;
        }
    }
    if (!has_realm) {
        return core::make_unexpected(
            AcrClientError::Registry("bearer challenge missing realm"));
    }
    if (!has_service) {
        return core::make_unexpected(
            AcrClientError::Registry("bearer challenge missing service"));
    }
    return out;
}

std::string BuildBasicAuthHeader(const std::string& username, const std::string& password) {
    // Standard base64, not URL-safe: the two differ for any credential
    // containing a byte that maps to `+` or `/`, and a URL-safe encoding here
    // would be rejected as a bad password.
    return "Basic " + core::Base64StandardEncode(username + ":" + password);
}

core::Optional<std::string> RegistryHost(const std::string& url) {
    ParsedUrl parsed;
    if (!ParseUrl(url, &parsed)) return core::Optional<std::string>();
    return core::Optional<std::string>(HostPort(parsed));
}

std::string ChallengeCacheKey(const std::string& url) {
    const core::Optional<std::string> host = RegistryHost(url);
    return host.has_value() ? *host : url;
}

// ---- Docker credentials ---------------------------------------------------

bool DockerAuthAddressMatches(const std::string& address, const std::string& registry) {
    // `trim_end_matches` strips repeatedly; the `/v1` and `/v2` passes run
    // after the slash pass, which is how `https://index.docker.io/v1/`
    // reduces to the host.
    std::string trimmed = TrimTrailing(TrimAscii(address), '/');
    trimmed = TrimTrailing(StripSuffix(trimmed, "/v1"), '/');
    trimmed = TrimTrailing(StripSuffix(trimmed, "/v2"), '/');
    if (trimmed == registry) return true;

    ParsedUrl url;
    if (!ParseUrl(trimmed, &url)) return false;
    return HostPort(url) == registry;
}

namespace {

const core::Json* FindField(const core::Json& object, const std::string& name) {
    if (object.kind() != core::Json::Kind::Object) return NULL;
    const core::JsonObject& fields = object.as_object();
    const core::JsonObject::const_iterator at = fields.find(name);
    return at == fields.end() ? NULL : &at->second;
}

core::Optional<std::string> StringField(const core::Json& object, const std::string& name) {
    const core::Json* found = FindField(object, name);
    if (found == NULL || found->kind() != core::Json::Kind::String) {
        return core::Optional<std::string>();
    }
    return core::Optional<std::string>(found->as_string());
}

}  // namespace

core::Optional<DockerRegistryCredentials> ParseDockerAuths(const core::Json& auths,
                                                           const std::string& registry) {
    if (auths.kind() != core::Json::Kind::Object) {
        return core::Optional<DockerRegistryCredentials>();
    }
    const core::JsonObject& entries = auths.as_object();
    for (core::JsonObject::const_iterator it = entries.begin(); it != entries.end(); ++it) {
        if (!DockerAuthAddressMatches(it->first, registry)) continue;

        // `auth` first: it is the field `docker login` writes, so a config
        // carrying both is almost always one whose explicit pair is stale.
        const core::Optional<std::string> auth = StringField(it->second, "auth");
        if (auth.has_value()) {
            const core::Expected<std::string, std::string> decoded =
                core::Base64StandardDecode(*auth);
            if (decoded.ok()) {
                const std::string::size_type colon = decoded.value().find(':');
                if (colon != std::string::npos) {
                    DockerRegistryCredentials out;
                    out.username = decoded.value().substr(0, colon);
                    // Only the *first* colon splits: a password may contain
                    // one, and splitting on the last would truncate it.
                    out.password = decoded.value().substr(colon + 1);
                    return core::Optional<DockerRegistryCredentials>(out);
                }
            }
            // A malformed `auth` falls through to the explicit pair rather
            // than failing the whole lookup.
        }
        const core::Optional<std::string> username = StringField(it->second, "username");
        const core::Optional<std::string> password = StringField(it->second, "password");
        if (username.has_value() && password.has_value()) {
            DockerRegistryCredentials out;
            out.username = *username;
            out.password = *password;
            return core::Optional<DockerRegistryCredentials>(out);
        }
    }
    return core::Optional<DockerRegistryCredentials>();
}

namespace {

/// Rust `docker_helper_server_url`.
std::string DockerHelperServerUrl(const std::string& address) {
    return TrimTrailing(TrimAscii(address), '/');
}

}  // namespace

core::Optional<std::pair<std::string, std::string> > DockerCredentialHelperForRegistry(
    const core::Json& docker_config, const std::string& registry) {
    const core::Json* helpers = FindField(docker_config, "credHelpers");
    if (helpers != NULL && helpers->kind() == core::Json::Kind::Object) {
        const core::JsonObject& entries = helpers->as_object();
        for (core::JsonObject::const_iterator it = entries.begin(); it != entries.end(); ++it) {
            if (!DockerAuthAddressMatches(it->first, registry)) continue;
            if (it->second.kind() != core::Json::Kind::String) continue;
            const std::string helper = TrimAscii(it->second.as_string());
            if (helper.empty()) continue;
            // A per-registry helper is asked about the address it was keyed
            // by, not the registry name: a helper registered under
            // `https://index.docker.io/v1/` will not answer to the host.
            return core::Optional<std::pair<std::string, std::string> >(
                std::make_pair(helper, DockerHelperServerUrl(it->first)));
        }
    }

    const core::Json* store_field = FindField(docker_config, "credsStore");
    if (store_field == NULL || store_field->kind() != core::Json::Kind::String) {
        return core::Optional<std::pair<std::string, std::string> >();
    }
    const std::string store = TrimAscii(store_field->as_string());
    if (store.empty()) return core::Optional<std::pair<std::string, std::string> >();

    // Rust indexes `docker_config["auths"]`, which yields Null when absent --
    // and `matching_docker_auth_address` then finds nothing. Same here.
    std::string server_url = registry;
    const core::Json* auths = FindField(docker_config, "auths");
    if (auths != NULL && auths->kind() == core::Json::Kind::Object) {
        const core::JsonObject& entries = auths->as_object();
        for (core::JsonObject::const_iterator it = entries.begin(); it != entries.end(); ++it) {
            if (DockerAuthAddressMatches(it->first, registry)) {
                server_url = DockerHelperServerUrl(it->first);
                break;
            }
        }
    }
    return core::Optional<std::pair<std::string, std::string> >(
        std::make_pair(store, server_url));
}

core::Expected<core::Optional<DockerRegistryCredentials>, AcrClientError>
ParseDockerCredentialHelperOutput(const std::string& output) {
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(output);
    if (!json.ok()) {
        return core::make_unexpected(AcrClientError::Registry(
            "parse Docker credential helper output: " + json.error().chain()));
    }
    // serde `rename = "Username", alias = "username"`: the capitalised form is
    // what the documented protocol uses, and real helpers emit both.
    core::Optional<std::string> username = StringField(json.value(), "Username");
    if (!username.has_value()) username = StringField(json.value(), "username");
    core::Optional<std::string> secret = StringField(json.value(), "Secret");
    if (!secret.has_value()) secret = StringField(json.value(), "secret");

    if (!username.has_value() || !secret.has_value()) {
        return core::make_unexpected(AcrClientError::Registry(
            "parse Docker credential helper output: missing Username or Secret"));
    }
    // Empty is how a helper says "nothing for this registry". An error here
    // would stop the search before the next candidate config.
    if (username->empty() || secret->empty()) {
        return core::Optional<DockerRegistryCredentials>();
    }
    DockerRegistryCredentials out;
    out.username = *username;
    out.password = *secret;
    return core::Optional<DockerRegistryCredentials>(out);
}

std::vector<std::string> DockerConfigCandidates() {
    std::vector<std::string> out;
    const char* docker_config = ::getenv("DOCKER_CONFIG");
    if (docker_config != NULL && docker_config[0] != '\0') {
        out.push_back(std::string(docker_config) + "/config.json");
    }
    const char* home = ::getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        out.push_back(std::string(home) + "/.docker/config.json");
    }
    // `DOCKER_CONFIG` first, so an explicit override wins over the default.
    return out;
}

}  // namespace image_export
}  // namespace snapshot
}  // namespace agentenv
