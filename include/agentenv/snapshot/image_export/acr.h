// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/acr/{client.rs,source_image.rs} and
// src/snapshot/image_export/acr/publisher.rs — the registry client, limited to
// what rolling a publication back needs.
//
// Scope, stated plainly: this is the *delete* half of the ACR exporter. Blob
// upload and manifest assembly (`publisher.rs`, `manifest.rs`) are not here,
// and neither is the policy that would select the exporter for a push in the
// first place. Rolling back is what unblocks deleting a snapshot that was
// published to a source registry, which is otherwise refused outright: a
// "deleted" snapshot that stays published in the registry is the one failure
// the caller cannot see.
//
// `SourceRegistryRepository` is the exception: it belongs to `source_image.rs`
// upstream but is shared with this half, which needs it to aim a DELETE. There
// is one of it, as upstream has — see `acr_source_image.h` for the rest of
// that module.
#ifndef AGENTENV_SNAPSHOT_IMAGE_EXPORT_ACR_H_
#define AGENTENV_SNAPSHOT_IMAGE_EXPORT_ACR_H_

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/storage/overlaybd/http_transport.h"

namespace agentenv {
namespace snapshot {
namespace image_export {

/// Rust `SourceRegistryRepository`.
struct SourceRegistryRepository {
    std::string registry;
    std::string repository;
    /// The input URL re-rendered from the *normalised* registry, so two
    /// spellings of one source compare equal.
    std::string repo_blob_url;

    /// Rust `SourceRegistryRepository::parse`.
    ///
    /// Requires the exact shape `https://<registry>/v2/<repo>/blobs`. The
    /// strictness is deliberate: the registry and repository parsed out of
    /// this are pasted into a DELETE URL, so a URL that merely *looks* close
    /// enough would aim the delete at a path nobody intended.
    ///
    /// `:443` is stripped for https because the port is implied — leaving it
    /// would make `https://r/v2/x/blobs` and `https://r:443/v2/x/blobs` two
    /// different registries, and the client cache is keyed by that string.
    /// Every other explicit port is kept.
    ///
    /// `allow_loopback_http` exists only for tests, mirroring Rust's
    /// `cfg!(test)` loopback exemption: a test registry is served over plain
    /// HTTP on 127.0.0.1. It is a parameter rather than a compile-time switch
    /// so the production call sites can be *seen* to pass false.
    static repository::RepositoryResult<SourceRegistryRepository> Parse(
        const std::string& repo_blob_url, bool allow_loopback_http = false);

    /// Rust `image_ref`.
    std::string ImageRef(const std::string& tag) const;

    /// Rust `upload_url`.
    std::string UploadUrl() const;

    /// Rust `manifest_url`.
    std::string ManifestUrl(const std::string& tag) const;

    /// Rust derives `PartialEq, Eq`.
    bool operator==(const SourceRegistryRepository& o) const {
        return registry == o.registry && repository == o.repository &&
               repo_blob_url == o.repo_blob_url;
    }
    bool operator!=(const SourceRegistryRepository& o) const { return !(*this == o); }
};

/// Rust `enum AcrClientError`.
///
/// The variants are not cosmetic: `Transient` is what the retry loop retries
/// and the others are what it gives up on immediately, so misclassifying a
/// permission failure as transient would turn one 403 into three.
struct AcrClientError {
    enum class Kind {
        MissingCredentials,
        PermissionDenied,
        ManifestExists,
        Transient,
        Registry,
    };

    Kind kind = Kind::Registry;
    /// `registry` for `MissingCredentials`, `tag` for `ManifestExists`,
    /// `message` otherwise.
    std::string detail;

    static AcrClientError MissingCredentials(const std::string& registry);
    static AcrClientError PermissionDenied(const std::string& message);
    static AcrClientError ManifestExists(const std::string& tag);
    static AcrClientError Transient(const std::string& message);
    static AcrClientError Registry(const std::string& message);

    bool is_transient() const { return kind == Kind::Transient; }

    /// The `#[error(..)]` strings, verbatim.
    std::string ToString() const;

    /// Rust `impl From<AcrClientError> for RepositoryError`.
    ///
    /// Only `ManifestExists` becomes `InvalidRequest` — it is the one variant
    /// the caller can act on by choosing another tag. Everything else is a
    /// backend failure, including `MissingCredentials`: an operator cannot fix
    /// it by changing the request.
    repository::RepositoryError ToRepositoryError() const;
};

/// Rust `DockerRegistryCredentials`.
///
/// No printable form on purpose: `password` is a registry secret, and a
/// `ToString` here is how one reaches a log.
struct DockerRegistryCredentials {
    std::string username;
    std::string password;
};

/// Rust `docker_auth_address_matches`.
///
/// A Docker config keys `auths` by strings that are not quite registry names:
/// `https://index.docker.io/v1/`, a bare host, a host with a port. Matching
/// them is what makes an existing `docker login` work here.
bool DockerAuthAddressMatches(const std::string& address, const std::string& registry);

/// Rust `parse_docker_auths` — the `auth` field first, then an explicit
/// username/password pair.
///
/// `auth` wins because that is the field `docker login` writes; a config
/// carrying both is almost always one where the pair is stale.
core::Optional<DockerRegistryCredentials> ParseDockerAuths(const core::Json& auths,
                                                           const std::string& registry);

/// Rust `docker_credential_helper_for_registry` — a per-registry `credHelpers`
/// entry, else the global `credsStore`.
///
/// Returns the helper name and the server URL to ask it about. For
/// `credsStore` the URL is the matching `auths` key when there is one, because
/// a helper keyed by `https://index.docker.io/v1/` will not answer to
/// `index.docker.io`.
core::Optional<std::pair<std::string, std::string> > DockerCredentialHelperForRegistry(
    const core::Json& docker_config, const std::string& registry);

/// Rust `parse_docker_credential_helper_output`.
///
/// Absent — not an error — when either field is empty: that is how a helper
/// reports "I have nothing for this registry", and treating it as a failure
/// would stop the search before the next candidate config.
core::Expected<core::Optional<DockerRegistryCredentials>, AcrClientError>
ParseDockerCredentialHelperOutput(const std::string& output);

/// Rust `docker_config_candidates` — `$DOCKER_CONFIG/config.json`, then
/// `$HOME/.docker/config.json`.
std::vector<std::string> DockerConfigCandidates();

/// Rust `load_docker_credentials`.
core::Expected<core::Optional<DockerRegistryCredentials>, AcrClientError>
LoadDockerCredentials(const std::string& registry);

/// Rust `BearerChallenge`.
struct BearerChallenge {
    std::string realm;
    std::string service;

    /// Rust `BearerChallenge::cache_key` — `realm|service|scope`.
    ///
    /// The scope is part of the key because a token is issued *for* a scope; a
    /// key without it would hand a token for one repository to a request
    /// against another, which the registry then rejects as a 401 the client
    /// reads as a credential problem.
    std::string CacheKey(const std::string& scope) const;
};

/// Rust `parse_auth_fields` — comma-separated `key=value`, commas inside
/// quotes ignored, keys lowercased, values unquoted.
std::vector<std::pair<std::string, std::string> > ParseAuthFields(const std::string& input);

/// Rust `parse_bearer_challenge`. Both `realm` and `service` are required.
core::Expected<BearerChallenge, AcrClientError> ParseBearerChallenge(
    const std::string& challenge);

/// Rust `build_basic_auth_header`.
std::string BuildBasicAuthHeader(const std::string& username, const std::string& password);

/// Rust `registry_host` — `host[:port]` from a URL, or absent.
core::Optional<std::string> RegistryHost(const std::string& url);

/// Rust `challenge_cache_key` — the registry host, falling back to the whole
/// URL.
///
/// Keyed by host rather than by URL so the challenge learned from one request
/// is reused for every other path on the same registry; that is what keeps the
/// 401 round trip to once per registry instead of once per request.
std::string ChallengeCacheKey(const std::string& url);

/// Rust `is_retryable_status`.
bool IsRetryableStatus(int status);

/// Rust `retry_after_delay` — a `Retry-After` given in seconds.
///
/// Only the integer form is read, as upstream does. The HTTP-date form is
/// ignored rather than mis-parsed, which falls back to the backoff.
core::Optional<int64_t> RetryAfterDelayMs(const core::Optional<std::string>& header);

/// Rust `status_error` — maps a status onto the variant that decides whether
/// the retry loop tries again.
AcrClientError StatusError(int status, const std::string& operation);

/// Rust `AcrClientOptions`.
struct AcrClientOptions {
    int64_t timeout_ms = 30 * 1000;
    /// Rust `DEFAULT_RETRY_COUNT` — attempts are `0..=retry_count`, so this is
    /// three tries in total.
    int retry_count = 2;
    int64_t retry_initial_backoff_ms = 500;
    /// Rust's `#[cfg(test)] allow_insecure_http`.
    bool allow_insecure_http = false;
};

/// Rust `DEFAULT_TOKEN_TTL` / `TOKEN_EXPIRY_SKEW`.
///
/// The skew is subtracted from every TTL so a token is replaced slightly
/// before the registry stops honouring it; without it, a token that expires
/// in flight produces a 401 that looks like a credential failure.
extern const int64_t kAcrDefaultTokenTtlMs;
extern const int64_t kAcrTokenExpirySkewMs;

/// Rust `AcrClient`, limited to the manifest-delete path.
class AcrClient {
 public:
    /// Sends one HTTP request. Injected so the retry, challenge and token
    /// logic above can be driven without a network.
    ///
    /// `storage::overlaybd::http` rather than `core::http`: this one is a
    /// libcurl transport, so it speaks TLS and follows redirects, both of
    /// which a registry client needs and `core::http` does not have. A non-2xx
    /// arrives as a `Response`, not an error — which is required here, since
    /// the status is exactly what decides between retrying, re-authenticating
    /// and giving up.
    typedef std::function<core::Expected<storage::overlaybd::http::Response, std::string>(
        const storage::overlaybd::http::Request&)>
        Transport;

    /// The production transport: `storage::overlaybd::http::Perform`.
    static Transport CurlTransport();

    /// Monotonic milliseconds, injected so token expiry is testable without
    /// sleeping.
    typedef std::function<int64_t()> Clock;

    AcrClient(const core::Optional<DockerRegistryCredentials>& credentials,
              const AcrClientOptions& options, const Transport& transport);

    void SetClock(const Clock& clock) { clock_ = clock; }
    /// Called between retry attempts. Default sleeps; tests replace it.
    void SetSleeper(const std::function<void(int64_t)>& sleeper) { sleeper_ = sleeper; }

    /// Rust `AcrClient::from_docker_config` — credentials from the Docker
    /// config, failing with `MissingCredentials` when there are none.
    static core::Expected<AcrClient, AcrClientError> FromDockerConfig(
        const std::string& registry, const Transport& transport,
        const AcrClientOptions& options = AcrClientOptions());

    /// Rust `AcrClient::delete_manifest_by_digest`.
    ///
    /// An empty digest is refused rather than sent: the URL would become
    /// `.../manifests/` and the registry's answer to that is not a no-op.
    core::Expected<core::Unit, AcrClientError> DeleteManifestByDigest(
        const std::string& registry, const std::string& repository, const std::string& digest);

    /// Rust `AcrClient::delete_manifest_url`.
    ///
    /// **404 counts as success.** The manifest being gone is the goal, and a
    /// rollback that fails on an already-deleted manifest could never finish a
    /// retried delete.
    core::Expected<core::Unit, AcrClientError> DeleteManifestUrl(
        const std::string& manifest_digest_url, const std::string& repository);

 private:
    typedef storage::overlaybd::http::Response HttpResponse;

    core::Expected<HttpResponse, AcrClientError> SendWithRetries(const std::string& method,
                                                                 const std::string& url,
                                                                 const std::string& repository);
    core::Expected<HttpResponse, AcrClientError> SendOnce(const std::string& method,
                                                          const std::string& url,
                                                          const std::string& repository);
    core::Expected<HttpResponse, AcrClientError> Request(
        const std::string& method, const std::string& url,
        const core::Optional<std::string>& auth_header);
    core::Expected<std::string, AcrClientError> AuthHeader(const std::string& registry_url,
                                                           const std::string& repository,
                                                           const std::string& challenge);
    core::Expected<core::Optional<std::string>, AcrClientError> CachedAuthHeader(
        const std::string& registry_url, const std::string& repository);
    core::Optional<std::string> CachedToken(const std::string& cache_key);
    core::Expected<core::Unit, AcrClientError> ValidateHttpsUrl(
        const std::string& url, const std::string& description) const;

    struct CachedTokenEntry {
        std::string token;
        int64_t expires_at_ms = 0;
    };

    core::Optional<DockerRegistryCredentials> credentials_;
    AcrClientOptions options_;
    Transport transport_;
    Clock clock_;
    std::function<void(int64_t)> sleeper_;
    std::map<std::string, std::string> challenge_cache_;
    std::map<std::string, CachedTokenEntry> token_cache_;
};

/// Rust `AcrDiskImageExporter::rollback_publication`, with its client cache.
class AcrDiskImageExporter {
 public:
    /// Builds a client for one registry. Injected because the production
    /// builder reads the Docker config and talks to the network.
    typedef std::function<core::Expected<AcrClient, AcrClientError>(const std::string& registry)>
        ClientBuilder;

    explicit AcrDiskImageExporter(const ClientBuilder& client_builder);

    /// The production builder: Docker-config credentials over real HTTP.
    static ClientBuilder DockerConfigClientBuilder(
        const AcrClientOptions& options = AcrClientOptions());

    /// Rust `rollback_publication`.
    ///
    /// **An unparsable `repoBlobUrl` succeeds with a warning.** That is not
    /// leniency for its own sake: this runs inside a snapshot delete, and a
    /// publication whose URL cannot be parsed can never be rolled back by any
    /// number of retries. Failing would wedge the delete forever on a record
    /// that is already unreachable, so the warning is the only useful outcome.
    repository::RepositoryResult<core::Unit> RollbackPublication(
        const PersistedDiskImagePublication& publication, bool allow_loopback_http = false);

 private:
    core::Expected<AcrClient*, AcrClientError> ClientForRegistry(const std::string& registry);

    ClientBuilder client_builder_;
    std::map<std::string, AcrClient> clients_;
};

}  // namespace image_export
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_IMAGE_EXPORT_ACR_H_
