// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/acr/{source_image.rs, client.rs,
// publisher.rs} — the manifest-delete half that a snapshot delete needs.
//
// Two kinds of test here, deliberately separated:
//
//   * the parses, called directly. Each one decides either where a DELETE is
//     aimed or which secret is attached to it, so they are worth pinning
//     without a transport in the way.
//   * the client, driven over an injected transport that records every
//     request. The retry, challenge and token rules are only real if the
//     *sequence* of requests is asserted, not just the final answer -- a
//     client that re-fetched a token per request would pass an
//     answer-only test.
#include "agentenv/snapshot/image_export/acr.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentenv/core/base64.h"
#include "agentenv/core/digest.h"
#include "agentenv/snapshot/acr_manifest.h"
#include "agentenv/snapshot/store.h"
#include "microtest.h"

using namespace agentenv;                                  // NOLINT
using namespace agentenv::snapshot;                        // NOLINT
using namespace agentenv::snapshot::image_export;          // NOLINT
using namespace agentenv::snapshot::repository;            // NOLINT

namespace obdhttp = agentenv::storage::overlaybd::http;

namespace {

// ---- URL parsing ----------------------------------------------------------

MT_TEST(acr_parse_source_registry_repository) {
    const RepositoryResult<SourceRegistryRepository> parsed =
        SourceRegistryRepository::Parse("https://reg.example.com/v2/team/app/blobs");
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_EQ(parsed.value().registry, std::string("reg.example.com"));
    MT_EXPECT_EQ(parsed.value().repository, std::string("team/app"));
    MT_EXPECT_EQ(parsed.value().repo_blob_url,
                 std::string("https://reg.example.com/v2/team/app/blobs"));
}

/// `:443` and a bare host are the *same* registry. They must normalise to one
/// string, because the client cache and
/// `snapshot_managed_publication_for_target` both key on it: two spellings
/// would open two connections and compare unequal to themselves.
MT_TEST(acr_parse_strips_default_https_port) {
    const RepositoryResult<SourceRegistryRepository> with_port =
        SourceRegistryRepository::Parse("https://reg.example.com:443/v2/app/blobs");
    const RepositoryResult<SourceRegistryRepository> without =
        SourceRegistryRepository::Parse("https://reg.example.com/v2/app/blobs");
    MT_EXPECT_TRUE(with_port.ok() && without.ok());
    MT_EXPECT_EQ(with_port.value().registry, without.value().registry);
    MT_EXPECT_EQ(with_port.value().repo_blob_url, without.value().repo_blob_url);
}

/// Every other port is kept: `reg:5000` really is a different registry.
MT_TEST(acr_parse_keeps_explicit_non_default_port) {
    const RepositoryResult<SourceRegistryRepository> parsed =
        SourceRegistryRepository::Parse("https://reg.example.com:5000/v2/app/blobs");
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_EQ(parsed.value().registry, std::string("reg.example.com:5000"));
    MT_EXPECT_EQ(parsed.value().repo_blob_url,
                 std::string("https://reg.example.com:5000/v2/app/blobs"));
}

/// A trailing slash is not a different source.
MT_TEST(acr_parse_normalises_trailing_slash) {
    const RepositoryResult<SourceRegistryRepository> parsed =
        SourceRegistryRepository::Parse("https://reg.example.com/v2/app/blobs/");
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_EQ(parsed.value().repo_blob_url,
                 std::string("https://reg.example.com/v2/app/blobs"));
}

MT_TEST(acr_parse_rejects_plain_http) {
    MT_EXPECT_TRUE(!SourceRegistryRepository::Parse("http://reg.example.com/v2/app/blobs").ok());
}

/// The loopback exemption is Rust's `cfg!(test)` escape, and it is off unless
/// asked for: a production call site must not be able to reach it by accident.
MT_TEST(acr_parse_loopback_http_only_when_allowed) {
    MT_EXPECT_TRUE(!SourceRegistryRepository::Parse("http://127.0.0.1:5000/v2/app/blobs").ok());
    MT_EXPECT_TRUE(
        SourceRegistryRepository::Parse("http://127.0.0.1:5000/v2/app/blobs", true).ok());
    // Loopback only; a remote host over plain HTTP stays refused even then.
    MT_EXPECT_TRUE(
        !SourceRegistryRepository::Parse("http://reg.example.com/v2/app/blobs", true).ok());
}

/// The shape is required exactly, because the registry and repository parsed
/// out of it are pasted into a DELETE URL. A URL that merely looks close
/// enough would aim the delete at a path nobody intended.
MT_TEST(acr_parse_requires_v2_blobs_shape) {
    MT_EXPECT_TRUE(!SourceRegistryRepository::Parse("https://reg.example.com/app/blobs").ok());
    MT_EXPECT_TRUE(!SourceRegistryRepository::Parse("https://reg.example.com/v2/app").ok());
    MT_EXPECT_TRUE(!SourceRegistryRepository::Parse("https://reg.example.com/v2/blobs").ok());
    MT_EXPECT_TRUE(!SourceRegistryRepository::Parse("https://reg.example.com").ok());
    MT_EXPECT_TRUE(!SourceRegistryRepository::Parse("not a url").ok());
}

/// Userinfo is dropped, not refused — Rust reaches the authority through
/// `Url::host_str()`, which never includes it, and rebuilds `repo_blob_url`
/// from the parsed registry. So the credential cannot reach the string that is
/// cached and logged, *and* a recorded URL carrying one still resolves.
/// Refusing instead would fail the rollback inside a snapshot delete, leaving
/// a "deleted" snapshot published in the registry.
MT_TEST(acr_parse_drops_userinfo_instead_of_refusing_it) {
    const RepositoryResult<SourceRegistryRepository> parsed =
        SourceRegistryRepository::Parse("https://user:pw@reg.example.com/v2/app/blobs");
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_EQ(parsed.value().registry, std::string("reg.example.com"));
    MT_EXPECT_EQ(parsed.value().repo_blob_url,
                 std::string("https://reg.example.com/v2/app/blobs"));
    // It normalises onto the same source as the credential-free spelling, so
    // one image is not published twice.
    MT_EXPECT_TRUE(parsed.value() ==
                   SourceRegistryRepository::Parse("https://reg.example.com/v2/app/blobs")
                       .value());

    // The port still parses when userinfo precedes it, which it would not if
    // the port were split off the whole authority.
    const RepositoryResult<SourceRegistryRepository> with_port =
        SourceRegistryRepository::Parse("https://user:pw@reg.example.com:5000/v2/app/blobs");
    MT_EXPECT_TRUE(with_port.ok());
    MT_EXPECT_EQ(with_port.value().registry, std::string("reg.example.com:5000"));

    // `RegistryHost` goes through the same parse, so the challenge cache key
    // cannot carry a secret either.
    MT_EXPECT_EQ(*RegistryHost("https://user:pw@reg.example.com/v2/app"),
                 std::string("reg.example.com"));
}

/// The scheme for the API calls is recovered from `repo_blob_url` rather than
/// hardcoded, so a loopback HTTP source does not switch protocol mid-publish.
MT_TEST(acr_source_registry_builds_api_urls_from_the_original_scheme) {
    const SourceRegistryRepository https =
        SourceRegistryRepository::Parse("https://registry.example/v2/ns/app/blobs").value();
    MT_EXPECT_EQ(https.ImageRef("v1"), std::string("registry.example/ns/app:v1"));
    MT_EXPECT_EQ(https.UploadUrl(),
                 std::string("https://registry.example/v2/ns/app/blobs/uploads/"));
    MT_EXPECT_EQ(https.ManifestUrl("v1"),
                 std::string("https://registry.example/v2/ns/app/manifests/v1"));

    const SourceRegistryRepository http =
        SourceRegistryRepository::Parse("http://127.0.0.1:5000/v2/app/blobs", true).value();
    MT_EXPECT_EQ(http.UploadUrl(),
                 std::string("http://127.0.0.1:5000/v2/app/blobs/uploads/"));
    MT_EXPECT_EQ(http.ManifestUrl("v1"),
                 std::string("http://127.0.0.1:5000/v2/app/manifests/v1"));
}

MT_TEST(acr_registry_host_and_challenge_cache_key) {
    MT_EXPECT_EQ(*RegistryHost("https://reg.example.com/v2/app/manifests/sha256:ab"),
                 std::string("reg.example.com"));
    MT_EXPECT_EQ(*RegistryHost("https://reg.example.com:5000/v2/app"),
                 std::string("reg.example.com:5000"));
    MT_EXPECT_TRUE(!RegistryHost("garbage").has_value());

    // Keyed by host, so the challenge learned from one path is reused for
    // every other path on the registry -- one 401 round trip per registry
    // instead of one per request.
    MT_EXPECT_EQ(ChallengeCacheKey("https://reg.example.com/v2/a/manifests/x"),
                 ChallengeCacheKey("https://reg.example.com/v2/b/manifests/y"));
    // Unparsable falls back to the whole URL rather than collapsing every
    // broken URL onto one key.
    MT_EXPECT_EQ(ChallengeCacheKey("garbage"), std::string("garbage"));
}

// ---- auth challenges ------------------------------------------------------

/// A comma inside quotes does not separate fields: a realm URL may contain
/// one, and splitting there would truncate the token endpoint.
MT_TEST(acr_parse_auth_fields_ignores_quoted_commas) {
    const std::vector<std::pair<std::string, std::string> > fields =
        ParseAuthFields("realm=\"https://auth/token?a=1,2\",service=\"reg\"");
    MT_EXPECT_EQ(fields.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(fields[0].first, std::string("realm"));
    MT_EXPECT_EQ(fields[0].second, std::string("https://auth/token?a=1,2"));
    MT_EXPECT_EQ(fields[1].first, std::string("service"));
    MT_EXPECT_EQ(fields[1].second, std::string("reg"));
}

MT_TEST(acr_parse_bearer_challenge) {
    const core::Expected<BearerChallenge, AcrClientError> parsed = ParseBearerChallenge(
        "Bearer realm=\"https://auth.example.com/token\",service=\"reg.example.com\"");
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_EQ(parsed.value().realm, std::string("https://auth.example.com/token"));
    MT_EXPECT_EQ(parsed.value().service, std::string("reg.example.com"));

    // The scheme token is case-insensitive per RFC 7235, and registries do
    // vary the casing.
    MT_EXPECT_TRUE(
        ParseBearerChallenge("bEaReR realm=\"https://a/token\",service=\"s\"").ok());

    // Both fields are required: without a realm there is nowhere to ask for a
    // token, and a token issued for the wrong service is rejected.
    MT_EXPECT_TRUE(!ParseBearerChallenge("Bearer service=\"s\"").ok());
    MT_EXPECT_TRUE(!ParseBearerChallenge("Bearer realm=\"https://a/token\"").ok());
    MT_EXPECT_TRUE(!ParseBearerChallenge("Digest realm=\"x\"").ok());
}

/// A token is issued *for* a scope, so the scope has to be part of the key.
/// Without it, a token for one repository would be handed to a request
/// against another, and the 401 that follows reads as a credential problem.
MT_TEST(acr_bearer_cache_key_includes_scope) {
    BearerChallenge challenge;
    challenge.realm = "https://auth/token";
    challenge.service = "reg";
    MT_EXPECT_TRUE(challenge.CacheKey("repository:a:push,pull") !=
                   challenge.CacheKey("repository:b:push,pull"));
}

/// Standard base64, not URL-safe: RFC 7617 defines the credential that way,
/// and the two differ for any byte that maps onto index 62 or 63.
MT_TEST(acr_basic_auth_header) {
    MT_EXPECT_EQ(BuildBasicAuthHeader("user", "pw"),
                 std::string("Basic ") + core::Base64StandardEncode(std::string("user:pw")));
}

// ---- status classification ------------------------------------------------

MT_TEST(acr_retryable_status) {
    MT_EXPECT_TRUE(IsRetryableStatus(500));
    MT_EXPECT_TRUE(IsRetryableStatus(503));
    MT_EXPECT_TRUE(IsRetryableStatus(429));
    // A 4xx other than 429 will not become a different answer by being asked
    // again; retrying one only delays the real error.
    MT_EXPECT_TRUE(!IsRetryableStatus(400));
    MT_EXPECT_TRUE(!IsRetryableStatus(404));
    MT_EXPECT_TRUE(!IsRetryableStatus(202));
}

MT_TEST(acr_status_error_variants) {
    MT_EXPECT_TRUE(StatusError(401, "op").kind == AcrClientError::Kind::PermissionDenied);
    MT_EXPECT_TRUE(StatusError(403, "op").kind == AcrClientError::Kind::PermissionDenied);
    MT_EXPECT_TRUE(StatusError(503, "op").is_transient());
    MT_EXPECT_TRUE(StatusError(400, "op").kind == AcrClientError::Kind::Registry);
    // Misclassifying a permission failure as transient would turn one 403
    // into three.
    MT_EXPECT_TRUE(!StatusError(403, "op").is_transient());
}

MT_TEST(acr_retry_after_seconds) {
    MT_EXPECT_EQ(*RetryAfterDelayMs(core::Optional<std::string>(std::string("2"))),
                 static_cast<int64_t>(2000));
    MT_EXPECT_EQ(*RetryAfterDelayMs(core::Optional<std::string>(std::string(" 3 "))),
                 static_cast<int64_t>(3000));
    MT_EXPECT_TRUE(!RetryAfterDelayMs(core::Optional<std::string>()).has_value());
    // The HTTP-date form is ignored rather than mis-parsed; the caller then
    // falls back to its backoff instead of sleeping for a nonsense duration.
    MT_EXPECT_TRUE(!RetryAfterDelayMs(
                        core::Optional<std::string>(std::string("Wed, 21 Oct 2015 07:28:00 GMT")))
                        .has_value());
}

/// Only `ManifestExists` is something the caller can act on by choosing
/// another tag. `MissingCredentials` is deliberately *not* an invalid request:
/// an operator cannot fix it by changing the call.
MT_TEST(acr_error_to_repository_error) {
    MT_EXPECT_TRUE(AcrClientError::ManifestExists("v1").ToRepositoryError().kind ==
                   RepositoryErrorKind::InvalidRequest);
    MT_EXPECT_TRUE(AcrClientError::MissingCredentials("reg").ToRepositoryError().kind ==
                   RepositoryErrorKind::Backend);
    MT_EXPECT_TRUE(AcrClientError::Transient("x").ToRepositoryError().kind ==
                   RepositoryErrorKind::Backend);
}

// ---- Docker credentials ---------------------------------------------------

/// The `auths` keys are not quite registry names. Matching them is what makes
/// an existing `docker login` work here at all.
MT_TEST(acr_docker_auth_address_matches) {
    MT_EXPECT_TRUE(DockerAuthAddressMatches("reg.example.com", "reg.example.com"));
    MT_EXPECT_TRUE(DockerAuthAddressMatches("https://reg.example.com", "reg.example.com"));
    MT_EXPECT_TRUE(DockerAuthAddressMatches("https://reg.example.com/", "reg.example.com"));
    MT_EXPECT_TRUE(DockerAuthAddressMatches("https://index.docker.io/v1/", "index.docker.io"));
    MT_EXPECT_TRUE(DockerAuthAddressMatches("https://reg.example.com/v2/", "reg.example.com"));
    MT_EXPECT_TRUE(DockerAuthAddressMatches("reg.example.com:5000", "reg.example.com:5000"));
    // A port is part of the identity: credentials for one must not be handed
    // to the other.
    MT_EXPECT_TRUE(!DockerAuthAddressMatches("reg.example.com:5000", "reg.example.com"));
    MT_EXPECT_TRUE(!DockerAuthAddressMatches("other.example.com", "reg.example.com"));
}

core::Json ParseJson(const std::string& text) {
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(text);
    MT_EXPECT_TRUE(json.ok());
    return json.ok() ? json.value() : core::Json();
}

MT_TEST(acr_parse_docker_auths_prefers_auth_field) {
    const std::string encoded = core::Base64StandardEncode(std::string("alice:s3cret"));
    const core::Json auths = ParseJson("{\"https://reg.example.com\":{\"auth\":\"" + encoded +
                                       "\",\"username\":\"stale\",\"password\":\"stale\"}}");
    const core::Optional<DockerRegistryCredentials> found =
        ParseDockerAuths(auths, "reg.example.com");
    MT_EXPECT_TRUE(found.has_value());
    // `auth` is the field `docker login` writes, so a config carrying both is
    // almost always one whose explicit pair is stale.
    MT_EXPECT_EQ(found->username, std::string("alice"));
    MT_EXPECT_EQ(found->password, std::string("s3cret"));
}

/// Only the *first* colon splits. A password may contain one, and splitting
/// on the last would truncate it into a credential that silently fails.
MT_TEST(acr_parse_docker_auths_password_with_colon) {
    const std::string encoded = core::Base64StandardEncode(std::string("alice:a:b:c"));
    const core::Json auths =
        ParseJson("{\"reg.example.com\":{\"auth\":\"" + encoded + "\"}}");
    const core::Optional<DockerRegistryCredentials> found =
        ParseDockerAuths(auths, "reg.example.com");
    MT_EXPECT_TRUE(found.has_value());
    MT_EXPECT_EQ(found->username, std::string("alice"));
    MT_EXPECT_EQ(found->password, std::string("a:b:c"));
}

/// A malformed `auth` falls through to the explicit pair rather than failing
/// the lookup: the pair is still a usable credential.
MT_TEST(acr_parse_docker_auths_falls_back_to_pair) {
    const core::Json auths = ParseJson(
        "{\"reg.example.com\":{\"auth\":\"!!!not base64!!!\",\"username\":\"bob\","
        "\"password\":\"pw\"}}");
    const core::Optional<DockerRegistryCredentials> found =
        ParseDockerAuths(auths, "reg.example.com");
    MT_EXPECT_TRUE(found.has_value());
    MT_EXPECT_EQ(found->username, std::string("bob"));

    // A base64 blob with no colon is not a credential either.
    const std::string no_colon = core::Base64StandardEncode(std::string("nocolon"));
    const core::Json only_auth =
        ParseJson("{\"reg.example.com\":{\"auth\":\"" + no_colon + "\"}}");
    MT_EXPECT_TRUE(!ParseDockerAuths(only_auth, "reg.example.com").has_value());
}

MT_TEST(acr_parse_docker_auths_ignores_other_registries) {
    const core::Json auths = ParseJson(
        "{\"other.example.com\":{\"username\":\"bob\",\"password\":\"pw\"}}");
    MT_EXPECT_TRUE(!ParseDockerAuths(auths, "reg.example.com").has_value());
    // Not an object at all -- a config may carry anything.
    MT_EXPECT_TRUE(!ParseDockerAuths(ParseJson("[]"), "reg.example.com").has_value());
}

/// A per-registry `credHelpers` entry beats the global `credsStore`, and it is
/// asked about the address it was keyed by: a helper registered under
/// `https://index.docker.io/v1/` will not answer to the bare host.
MT_TEST(acr_credential_helper_prefers_cred_helpers) {
    const core::Json config = ParseJson(
        "{\"credsStore\":\"desktop\",\"credHelpers\":"
        "{\"https://index.docker.io/v1/\":\"ecr-login\"}}");
    const core::Optional<std::pair<std::string, std::string> > helper =
        DockerCredentialHelperForRegistry(config, "index.docker.io");
    MT_EXPECT_TRUE(helper.has_value());
    MT_EXPECT_EQ(helper->first, std::string("ecr-login"));
    MT_EXPECT_EQ(helper->second, std::string("https://index.docker.io/v1"));
}

/// Falling back to `credsStore`, the server URL is the matching `auths` key
/// when there is one, for the same reason.
MT_TEST(acr_credential_helper_creds_store_uses_auths_key) {
    const core::Json config = ParseJson(
        "{\"credsStore\":\"desktop\",\"auths\":{\"https://index.docker.io/v1/\":{}}}");
    const core::Optional<std::pair<std::string, std::string> > helper =
        DockerCredentialHelperForRegistry(config, "index.docker.io");
    MT_EXPECT_TRUE(helper.has_value());
    MT_EXPECT_EQ(helper->first, std::string("desktop"));
    MT_EXPECT_EQ(helper->second, std::string("https://index.docker.io/v1"));

    // With no matching `auths` key the registry name is used as-is.
    const core::Json bare = ParseJson("{\"credsStore\":\"desktop\"}");
    MT_EXPECT_EQ(DockerCredentialHelperForRegistry(bare, "reg.example.com")->second,
                 std::string("reg.example.com"));

    MT_EXPECT_TRUE(!DockerCredentialHelperForRegistry(ParseJson("{}"), "reg").has_value());
    // An empty name is not a helper; running `docker-credential-` would fail
    // with a confusing exec error.
    MT_EXPECT_TRUE(
        !DockerCredentialHelperForRegistry(ParseJson("{\"credsStore\":\"\"}"), "reg")
             .has_value());
}

MT_TEST(acr_parse_credential_helper_output) {
    const core::Expected<core::Optional<DockerRegistryCredentials>, AcrClientError> ok =
        ParseDockerCredentialHelperOutput("{\"Username\":\"alice\",\"Secret\":\"pw\"}");
    MT_EXPECT_TRUE(ok.ok() && ok.value().has_value());
    MT_EXPECT_EQ(ok.value()->username, std::string("alice"));
    MT_EXPECT_EQ(ok.value()->password, std::string("pw"));

    // serde `alias = "username"`: real helpers emit both spellings.
    const core::Expected<core::Optional<DockerRegistryCredentials>, AcrClientError> lower =
        ParseDockerCredentialHelperOutput("{\"username\":\"alice\",\"secret\":\"pw\"}");
    MT_EXPECT_TRUE(lower.ok() && lower.value().has_value());

    // Empty is how a helper says "nothing for this registry". An error here
    // would stop the search before the next candidate config.
    const core::Expected<core::Optional<DockerRegistryCredentials>, AcrClientError> empty =
        ParseDockerCredentialHelperOutput("{\"Username\":\"\",\"Secret\":\"\"}");
    MT_EXPECT_TRUE(empty.ok() && !empty.value().has_value());

    MT_EXPECT_TRUE(!ParseDockerCredentialHelperOutput("{\"Username\":\"a\"}").ok());
    MT_EXPECT_TRUE(!ParseDockerCredentialHelperOutput("not json").ok());
}

// ---- the client over an injected transport --------------------------------

/// Records every request and replies from a queued script.
///
/// A queue rather than a URL map: what these tests are about is the *order* of
/// requests -- 401 then token then retry -- and a map would let a client that
/// skipped a step still pass.
struct ScriptedTransport {
    struct Reply {
        long status;
        obdhttp::HeaderMap headers;
        std::string body;
        /// A transport-level failure instead of a response.
        std::string transport_error;

        Reply() : status(200) {}
    };

    std::vector<Reply> replies;
    std::size_t next;
    std::vector<obdhttp::Request> requests;

    ScriptedTransport() : next(0) {}

    static Reply Status(long status) {
        Reply reply;
        reply.status = status;
        return reply;
    }

    static Reply Unauthorized(const std::string& challenge) {
        Reply reply;
        reply.status = 401;
        reply.headers["WWW-Authenticate"] = challenge;
        return reply;
    }

    static Reply Token(const std::string& token, int64_t expires_in_secs) {
        Reply reply;
        reply.status = 200;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(expires_in_secs));
        reply.body = "{\"token\":\"" + token + "\",\"expires_in\":" + buf + "}";
        return reply;
    }

    static Reply Failure(const std::string& message) {
        Reply reply;
        reply.transport_error = message;
        return reply;
    }

    AcrClient::Transport Fn() {
        ScriptedTransport* self = this;
        return [self](const obdhttp::Request& request)
                   -> core::Expected<obdhttp::Response, std::string> {
            self->requests.push_back(request);
            if (self->next >= self->replies.size()) {
                return core::make_unexpected(std::string("scripted transport exhausted"));
            }
            const Reply& reply = self->replies[self->next++];
            if (!reply.transport_error.empty()) {
                return core::make_unexpected(reply.transport_error);
            }
            obdhttp::Response response;
            response.status = reply.status;
            response.headers = reply.headers;
            response.body.assign(reply.body.begin(), reply.body.end());
            return response;
        };
    }

    std::string AuthorizationOf(std::size_t index) const {
        const obdhttp::HeaderMap::const_iterator at =
            requests[index].headers.find("Authorization");
        return at == requests[index].headers.end() ? std::string() : at->second;
    }
};

const char* kChallenge =
    "Bearer realm=\"https://auth.example.com/token\",service=\"reg.example.com\"";

core::Optional<DockerRegistryCredentials> Credentials() {
    DockerRegistryCredentials creds;
    creds.username = "alice";
    creds.password = "pw";
    return core::Optional<DockerRegistryCredentials>(creds);
}

AcrClientOptions FastOptions() {
    AcrClientOptions options;
    // Backoff is only ever observed through the recorded sleeps, so the value
    // is arbitrary; keeping it small makes a real sleep harmless if the
    // sleeper is ever left unset.
    options.retry_initial_backoff_ms = 1;
    return options;
}

/// A client whose sleeps are recorded instead of taken, and whose clock is
/// manual: token expiry is otherwise untestable without waiting minutes.
struct ClientFixture {
    ScriptedTransport transport;
    std::vector<int64_t> sleeps;
    int64_t now;

    ClientFixture() : now(1000) {}

    AcrClient Build(const AcrClientOptions& options = FastOptions()) {
        AcrClient client(Credentials(), options, transport.Fn());
        std::vector<int64_t>* sleeps = &this->sleeps;
        client.SetSleeper([sleeps](int64_t ms) { sleeps->push_back(ms); });
        const int64_t* clock = &this->now;
        client.SetClock([clock]() { return *clock; });
        return client;
    }
};

const char* kManifestUrl = "https://reg.example.com/v2/team/app/manifests/sha256:abc";

MT_TEST(acr_delete_manifest_accepted) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(fixture.transport.requests[0].method, std::string("DELETE"));
    MT_EXPECT_EQ(fixture.transport.requests[0].url, std::string(kManifestUrl));
    // No challenge learned yet, so the first request goes out unauthenticated
    // and the 401 is what teaches one. Guessing instead would send the
    // password to an endpoint that never asked for it.
    MT_EXPECT_EQ(fixture.transport.AuthorizationOf(0), std::string());
}

/// 404 is success. The manifest being gone is the goal, and a rollback that
/// failed on an already-deleted manifest could never finish a retried delete.
MT_TEST(acr_delete_manifest_treats_404_as_success) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(404));
    AcrClient client = fixture.Build();
    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
}

MT_TEST(acr_delete_manifest_by_digest_builds_url) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(
        client.DeleteManifestByDigest("reg.example.com", "team/app", "sha256:abc").ok());
    MT_EXPECT_EQ(fixture.transport.requests[0].url, std::string(kManifestUrl));
}

/// An empty digest never reaches the wire: the URL would end in
/// `/manifests/`, and a registry's answer to that is not a no-op.
MT_TEST(acr_delete_manifest_refuses_empty_digest) {
    ClientFixture fixture;
    AcrClient client = fixture.Build();
    const core::Expected<core::Unit, AcrClientError> deleted =
        client.DeleteManifestByDigest("reg.example.com", "team/app", "");
    MT_EXPECT_TRUE(!deleted.ok());
    MT_EXPECT_TRUE(deleted.error().kind == AcrClientError::Kind::Registry);
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(0));
}

MT_TEST(acr_delete_manifest_authenticates_after_401) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Unauthorized(kChallenge));
    fixture.transport.replies.push_back(ScriptedTransport::Token("tok-1", 300));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(3));

    // The token request: Basic-authenticated, at the challenge's realm, with
    // the service and scope the challenge asked for.
    MT_EXPECT_EQ(fixture.transport.requests[1].method, std::string("GET"));
    MT_EXPECT_TRUE(fixture.transport.requests[1].url.find("https://auth.example.com/token?") ==
                   0);
    MT_EXPECT_TRUE(fixture.transport.requests[1].url.find("service=reg.example.com") !=
                   std::string::npos);
    // `push,pull` even for a delete: ACR issues one token for the repository,
    // and a pull-only scope cannot remove a manifest. The percent-encoded
    // comma is held in a variable because `MT_EXPECT_TRUE` stringifies its
    // condition into a printf format, where a literal `%2C` is a format spec.
    const std::string encoded_scope = "scope=repository%3Ateam%2Fapp%3Apush%2Cpull";
    const bool scope_present =
        fixture.transport.requests[1].url.find(encoded_scope) != std::string::npos;
    MT_EXPECT_TRUE(scope_present);
    MT_EXPECT_EQ(fixture.transport.AuthorizationOf(1),
                 BuildBasicAuthHeader("alice", "pw"));

    // The retried DELETE carries the bearer token.
    MT_EXPECT_EQ(fixture.transport.requests[2].method, std::string("DELETE"));
    MT_EXPECT_EQ(fixture.transport.AuthorizationOf(2), std::string("Bearer tok-1"));
    // No retry was spent on the 401: re-authenticating happens inside one
    // attempt, so a genuine transient failure still gets its full budget.
    MT_EXPECT_EQ(fixture.sleeps.size(), static_cast<std::size_t>(0));
}

/// A `Basic` challenge is answered directly; no token service is involved.
MT_TEST(acr_delete_manifest_answers_basic_challenge) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(
        ScriptedTransport::Unauthorized("Basic realm=\"reg.example.com\""));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(fixture.transport.AuthorizationOf(1), BuildBasicAuthHeader("alice", "pw"));
}

/// The learned challenge and the token are both cached, so a second delete in
/// the same repository costs one request. This is the whole point of the two
/// caches: without them every delete would pay a 401 and a token round trip.
MT_TEST(acr_reuses_cached_challenge_and_token) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Unauthorized(kChallenge));
    fixture.transport.replies.push_back(ScriptedTransport::Token("tok-1", 300));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    MT_EXPECT_TRUE(client
                       .DeleteManifestUrl(
                           "https://reg.example.com/v2/team/app/manifests/sha256:def",
                           "team/app")
                       .ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(4));
    // Authenticated on the first try, from the cached challenge.
    MT_EXPECT_EQ(fixture.transport.AuthorizationOf(3), std::string("Bearer tok-1"));
}

/// The skew is subtracted from the TTL so a token is replaced slightly before
/// the registry stops honouring it. Without it, a token that expires in
/// flight produces a 401 that looks like a credential failure.
MT_TEST(acr_token_expiry_applies_skew) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Unauthorized(kChallenge));
    fixture.transport.replies.push_back(ScriptedTransport::Token("tok-1", 60));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Token("tok-2", 60));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());

    // 60s TTL less the 30s skew: still valid just under 30s later.
    fixture.now += kAcrTokenExpirySkewMs - 1;
    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(4));
    MT_EXPECT_EQ(fixture.transport.AuthorizationOf(3), std::string("Bearer tok-1"));

    // Past the skewed expiry, while the registry would still accept the old
    // token: a fresh one is fetched anyway.
    fixture.now += 2;
    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(6));
    MT_EXPECT_EQ(fixture.transport.AuthorizationOf(5), std::string("Bearer tok-2"));
}

/// A TTL shorter than the skew saturates to zero, so such a token is never
/// cached -- the alternative, a negative expiry, would cache it forever.
MT_TEST(acr_token_shorter_than_skew_is_not_cached) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Unauthorized(kChallenge));
    fixture.transport.replies.push_back(ScriptedTransport::Token("tok-1", 5));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Token("tok-2", 5));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    // Same instant, and the token is already gone.
    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(5));
    MT_EXPECT_EQ(fixture.transport.AuthorizationOf(4), std::string("Bearer tok-2"));
}

/// A token response missing its token is a registry error, not an empty
/// bearer header: sending `Authorization: Bearer ` would earn a 401 reported
/// as a permission failure, hiding the malformed response.
MT_TEST(acr_token_response_without_token_fails) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Unauthorized(kChallenge));
    ScriptedTransport::Reply empty;
    empty.status = 200;
    empty.body = "{\"expires_in\":300}";
    fixture.transport.replies.push_back(empty);
    AcrClient client = fixture.Build();

    const core::Expected<core::Unit, AcrClientError> deleted =
        client.DeleteManifestUrl(kManifestUrl, "team/app");
    MT_EXPECT_TRUE(!deleted.ok());
    MT_EXPECT_TRUE(deleted.error().kind == AcrClientError::Kind::Registry);
}

/// `access_token` is the other spelling in use; `token` wins when both are
/// present, as Rust's `body.token.or(body.access_token)` does.
MT_TEST(acr_token_response_access_token_spelling) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Unauthorized(kChallenge));
    ScriptedTransport::Reply reply;
    reply.status = 200;
    reply.body = "{\"access_token\":\"tok-a\"}";
    fixture.transport.replies.push_back(reply);
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    MT_EXPECT_EQ(fixture.transport.AuthorizationOf(2), std::string("Bearer tok-a"));
}

/// Absent `expires_in` falls back to the default TTL rather than to zero, so
/// the token is still cached.
MT_TEST(acr_token_response_without_expires_in_uses_default_ttl) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Unauthorized(kChallenge));
    ScriptedTransport::Reply reply;
    reply.status = 200;
    reply.body = "{\"token\":\"tok-1\"}";
    fixture.transport.replies.push_back(reply);
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    fixture.now += kAcrDefaultTokenTtlMs - kAcrTokenExpirySkewMs - 1;
    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(4));
}

/// A wrongly typed field is a parse failure for the whole response, as serde's
/// `Option<u64>` is: reading past it would silently use the default TTL and
/// cache the token for the wrong length of time.
MT_TEST(acr_token_response_rejects_bad_expires_in) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Unauthorized(kChallenge));
    ScriptedTransport::Reply reply;
    reply.status = 200;
    reply.body = "{\"token\":\"tok-1\",\"expires_in\":\"300\"}";
    fixture.transport.replies.push_back(reply);
    AcrClient client = fixture.Build();

    const core::Expected<core::Unit, AcrClientError> deleted =
        client.DeleteManifestUrl(kManifestUrl, "team/app");
    MT_EXPECT_TRUE(!deleted.ok());
    MT_EXPECT_TRUE(deleted.error().kind == AcrClientError::Kind::Registry);
}

/// A 403 means the credentials were understood and refused; re-authenticating
/// or retrying would produce the same 403, so it is terminal and costs exactly
/// one request.
MT_TEST(acr_forbidden_is_terminal) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(403));
    AcrClient client = fixture.Build();

    const core::Expected<core::Unit, AcrClientError> deleted =
        client.DeleteManifestUrl(kManifestUrl, "team/app");
    MT_EXPECT_TRUE(!deleted.ok());
    MT_EXPECT_TRUE(deleted.error().kind == AcrClientError::Kind::PermissionDenied);
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(fixture.sleeps.size(), static_cast<std::size_t>(0));
}

/// Authenticated and still refused: terminal rather than transient.
MT_TEST(acr_401_after_authenticating_is_terminal) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Unauthorized(kChallenge));
    fixture.transport.replies.push_back(ScriptedTransport::Token("tok-1", 300));
    fixture.transport.replies.push_back(ScriptedTransport::Status(401));
    AcrClient client = fixture.Build();

    const core::Expected<core::Unit, AcrClientError> deleted =
        client.DeleteManifestUrl(kManifestUrl, "team/app");
    MT_EXPECT_TRUE(!deleted.ok());
    MT_EXPECT_TRUE(deleted.error().kind == AcrClientError::Kind::PermissionDenied);
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(3));
}

/// A 401 with no challenge leaves nothing to answer, so it is a permission
/// failure rather than a retry against a server that will say the same thing.
MT_TEST(acr_401_without_challenge_is_permission_denied) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(401));
    AcrClient client = fixture.Build();

    const core::Expected<core::Unit, AcrClientError> deleted =
        client.DeleteManifestUrl(kManifestUrl, "team/app");
    MT_EXPECT_TRUE(!deleted.ok());
    MT_EXPECT_TRUE(deleted.error().kind == AcrClientError::Kind::PermissionDenied);
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(1));
}

/// `0..=retry_count`, so the default of 2 is three attempts; the last one is
/// not followed by a sleep.
MT_TEST(acr_retries_5xx_then_gives_up) {
    ClientFixture fixture;
    for (int i = 0; i < 3; ++i) {
        fixture.transport.replies.push_back(ScriptedTransport::Status(503));
    }
    AcrClient client = fixture.Build();

    const core::Expected<core::Unit, AcrClientError> deleted =
        client.DeleteManifestUrl(kManifestUrl, "team/app");
    MT_EXPECT_TRUE(!deleted.ok());
    MT_EXPECT_TRUE(deleted.error().is_transient());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(fixture.sleeps.size(), static_cast<std::size_t>(2));
    // Exponential: the second wait is twice the first.
    MT_EXPECT_EQ(fixture.sleeps[1], fixture.sleeps[0] * 2);
}

MT_TEST(acr_retries_then_succeeds) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(503));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(2));
}

/// `Retry-After` wins over the backoff. The registry knows when it will be
/// ready, and ignoring it is how a throttled client gets throttled harder.
MT_TEST(acr_honours_retry_after_over_backoff) {
    ClientFixture fixture;
    ScriptedTransport::Reply throttled;
    throttled.status = 429;
    throttled.headers["Retry-After"] = "2";
    fixture.transport.replies.push_back(throttled);
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    MT_EXPECT_EQ(fixture.sleeps.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(fixture.sleeps[0], static_cast<int64_t>(2000));
}

/// A transport failure -- no connection, TLS error, timeout -- is transient by
/// construction and gets the retry budget.
MT_TEST(acr_retries_transport_failure) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Failure("connection refused"));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(client.DeleteManifestUrl(kManifestUrl, "team/app").ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(fixture.sleeps.size(), static_cast<std::size_t>(1));
}

/// A non-4xx/5xx failure the delete cannot treat as success.
MT_TEST(acr_unexpected_status_is_registry_error) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(400));
    AcrClient client = fixture.Build();

    const core::Expected<core::Unit, AcrClientError> deleted =
        client.DeleteManifestUrl(kManifestUrl, "team/app");
    MT_EXPECT_TRUE(!deleted.ok());
    MT_EXPECT_TRUE(deleted.error().kind == AcrClientError::Kind::Registry);
    MT_EXPECT_EQ(fixture.sleeps.size(), static_cast<std::size_t>(0));
}

/// Plaintext is refused before the request is made, so a registry password
/// cannot leave over HTTP.
MT_TEST(acr_refuses_plaintext_url) {
    ClientFixture fixture;
    AcrClient client = fixture.Build();
    const core::Expected<core::Unit, AcrClientError> deleted = client.DeleteManifestUrl(
        "http://reg.example.com/v2/team/app/manifests/sha256:abc", "team/app");
    MT_EXPECT_TRUE(!deleted.ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(0));
}

/// The token realm is server-supplied, so it is checked too: a downgraded
/// realm would send the registry password over plaintext.
MT_TEST(acr_refuses_plaintext_token_realm) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Unauthorized(
        "Bearer realm=\"http://auth.example.com/token\",service=\"reg.example.com\""));
    AcrClient client = fixture.Build();

    const core::Expected<core::Unit, AcrClientError> deleted =
        client.DeleteManifestUrl(kManifestUrl, "team/app");
    MT_EXPECT_TRUE(!deleted.ok());
    MT_EXPECT_TRUE(deleted.error().kind == AcrClientError::Kind::Registry);
    // Only the original DELETE went out; no token request followed.
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(1));
}

/// Without credentials there is nothing to answer a challenge with, and the
/// error names the registry so an operator knows where to `docker login`.
MT_TEST(acr_missing_credentials_names_the_registry) {
    ScriptedTransport transport;
    transport.replies.push_back(ScriptedTransport::Unauthorized(kChallenge));
    AcrClient client(core::Optional<DockerRegistryCredentials>(), FastOptions(),
                     transport.Fn());
    client.SetSleeper([](int64_t) {});

    const core::Expected<core::Unit, AcrClientError> deleted =
        client.DeleteManifestUrl(kManifestUrl, "team/app");
    MT_EXPECT_TRUE(!deleted.ok());
    MT_EXPECT_TRUE(deleted.error().kind == AcrClientError::Kind::MissingCredentials);
    MT_EXPECT_EQ(deleted.error().detail, std::string("reg.example.com"));
}

// ---- the exporter ---------------------------------------------------------

PersistedDiskImagePublication Publication(const std::string& repo_blob_url,
                                          const std::string& digest) {
    PersistedDiskImagePublication publication;
    publication.image_ref = "reg.example.com/team/app:v1";
    publication.tag = "v1";
    publication.manifest_digest = digest;
    publication.repo_blob_url = repo_blob_url;
    return publication;
}

/// Builds clients over one scripted transport and counts how often it is
/// asked, so the per-registry cache can be observed.
struct ExporterFixture {
    ScriptedTransport transport;
    std::vector<std::string> built_for;
    bool fail_build;

    ExporterFixture() : fail_build(false) {}

    AcrDiskImageExporter::ClientBuilder Builder() {
        ExporterFixture* self = this;
        return [self](const std::string& registry)
                   -> core::Expected<AcrClient, AcrClientError> {
            self->built_for.push_back(registry);
            if (self->fail_build) {
                return core::make_unexpected(AcrClientError::MissingCredentials(registry));
            }
            AcrClient client(Credentials(), FastOptions(), self->transport.Fn());
            client.SetSleeper([](int64_t) {});
            return client;
        };
    }
};

MT_TEST(acr_rollback_publication_deletes_manifest) {
    ExporterFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrDiskImageExporter exporter(fixture.Builder());

    MT_EXPECT_TRUE(exporter
                       .RollbackPublication(Publication(
                           "https://reg.example.com/v2/team/app/blobs", "sha256:abc"))
                       .ok());
    MT_EXPECT_EQ(fixture.built_for.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(fixture.built_for[0], std::string("reg.example.com"));
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(fixture.transport.requests[0].method, std::string("DELETE"));
    MT_EXPECT_EQ(fixture.transport.requests[0].url, std::string(kManifestUrl));
}

/// An unparsable `repoBlobUrl` succeeds with a warning. This runs inside a
/// snapshot delete, and such a publication can never be rolled back by any
/// number of retries -- failing would wedge the delete forever on a record
/// that is already unreachable.
MT_TEST(acr_rollback_publication_tolerates_invalid_url) {
    ExporterFixture fixture;
    AcrDiskImageExporter exporter(fixture.Builder());

    MT_EXPECT_TRUE(exporter.RollbackPublication(Publication("not a url", "sha256:abc")).ok());
    // No client was even built: there is no registry to build one for.
    MT_EXPECT_EQ(fixture.built_for.size(), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(0));
}

/// Cached per registry, so a snapshot with several publications in one
/// registry pays the credential load and the 401 round trip once.
MT_TEST(acr_rollback_reuses_client_per_registry) {
    ExporterFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrDiskImageExporter exporter(fixture.Builder());

    MT_EXPECT_TRUE(exporter
                       .RollbackPublication(Publication(
                           "https://reg.example.com/v2/team/app/blobs", "sha256:a"))
                       .ok());
    MT_EXPECT_TRUE(exporter
                       .RollbackPublication(Publication(
                           "https://reg.example.com/v2/team/other/blobs", "sha256:b"))
                       .ok());
    MT_EXPECT_EQ(fixture.built_for.size(), static_cast<std::size_t>(1));

    // A different registry does get its own client.
    MT_EXPECT_TRUE(exporter
                       .RollbackPublication(Publication(
                           "https://other.example.com/v2/team/app/blobs", "sha256:c"))
                       .ok());
    MT_EXPECT_EQ(fixture.built_for.size(), static_cast<std::size_t>(2));
}

/// Two spellings of one registry share a client, because `Parse` normalises
/// the host before it is used as the cache key.
MT_TEST(acr_rollback_client_cache_keyed_by_normalised_registry) {
    ExporterFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrDiskImageExporter exporter(fixture.Builder());

    MT_EXPECT_TRUE(exporter
                       .RollbackPublication(Publication(
                           "https://reg.example.com/v2/team/app/blobs", "sha256:a"))
                       .ok());
    MT_EXPECT_TRUE(exporter
                       .RollbackPublication(Publication(
                           "https://reg.example.com:443/v2/team/app/blobs", "sha256:b"))
                       .ok());
    MT_EXPECT_EQ(fixture.built_for.size(), static_cast<std::size_t>(1));
}

/// A credential failure *is* returned: unlike an unparsable URL, this one may
/// well succeed after an operator logs in, so the delete must not pretend the
/// publication is gone.
MT_TEST(acr_rollback_reports_client_build_failure) {
    ExporterFixture fixture;
    fixture.fail_build = true;
    AcrDiskImageExporter exporter(fixture.Builder());

    const RepositoryResult<core::Unit> rolled_back = exporter.RollbackPublication(
        Publication("https://reg.example.com/v2/team/app/blobs", "sha256:abc"));
    MT_EXPECT_TRUE(!rolled_back.ok());
    MT_EXPECT_TRUE(rolled_back.error().kind == RepositoryErrorKind::Backend);
}

MT_TEST(acr_rollback_reports_delete_failure) {
    ExporterFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(403));
    AcrDiskImageExporter exporter(fixture.Builder());

    const RepositoryResult<core::Unit> rolled_back = exporter.RollbackPublication(
        Publication("https://reg.example.com/v2/team/app/blobs", "sha256:abc"));
    MT_EXPECT_TRUE(!rolled_back.ok());
}

// ---- delete_snapshot's use of the rollback --------------------------------

/// Records what the blob delete was asked to remove, so `DeleteSnapshot` can
/// be checked to still reach it after a rollback failure.
///
/// The record half is refused rather than faked: `DeleteSnapshot` must not
/// touch it, and an "unsupported" answer is what turns a stray call into a
/// visible failure instead of a silent pass.
struct BlobDeleteRecorder : public SnapshotRepository {
    std::vector<std::string> deleted;

    RepositoryResult<core::Unit> DeleteSnapshotBlobs(const core::SnapshotId& id) override {
        deleted.push_back(id.ToString());
        return core::Unit();
    }

    RepositoryResult<SnapshotRecord> Create(const SnapshotRecord&) override {
        return core::make_unexpected(RepositoryError::Unsupported("Create"));
    }
    RepositoryResult<core::Optional<SnapshotRecord> > Get(const std::string&) override {
        return core::make_unexpected(RepositoryError::Unsupported("Get"));
    }
    RepositoryResult<std::vector<SnapshotRecord> > List(const SnapshotListFilter&) override {
        return core::make_unexpected(RepositoryError::Unsupported("List"));
    }
    RepositoryResult<core::Unit> Delete(const std::string&) override {
        return core::make_unexpected(RepositoryError::Unsupported("Delete"));
    }
    RepositoryResult<core::Optional<core::SnapshotId> > ResolveAlias(
        const std::string&) override {
        return core::make_unexpected(RepositoryError::Unsupported("ResolveAlias"));
    }
    RepositoryResult<SnapshotRecord> TryStartBuild(const core::SnapshotId&) override {
        return core::make_unexpected(RepositoryError::Unsupported("TryStartBuild"));
    }
    RepositoryResult<core::Unit> MarkBuildError(const core::SnapshotId&,
                                                const TemplateBuildErrorReason&) override {
        return core::make_unexpected(RepositoryError::Unsupported("MarkBuildError"));
    }
};

core::SnapshotId SomeId() {
    const core::Expected<core::SnapshotId, std::string> id =
        core::SnapshotId::Parse("11111111-2222-3333-4444-555555555555");
    MT_EXPECT_TRUE(id.ok());
    return id.ok() ? id.value() : core::SnapshotId();
}

/// Reverse order, as upstream: publications are appended as they are created,
/// so unwinding from the end undoes the most recent first.
MT_TEST(delete_snapshot_rolls_publications_back_in_reverse) {
    BlobDeleteRecorder repository;
    std::vector<std::string> rolled;
    const PublicationRollback rollback =
        [&rolled](const PersistedDiskImagePublication& publication)
            -> RepositoryResult<core::Unit> {
        rolled.push_back(publication.manifest_digest);
        return core::Unit();
    };

    std::vector<PersistedDiskImagePublication> publications;
    publications.push_back(Publication("https://reg.example.com/v2/a/blobs", "sha256:first"));
    publications.push_back(Publication("https://reg.example.com/v2/a/blobs", "sha256:second"));

    MT_EXPECT_TRUE(
        DeleteSnapshot(repository, SomeId(), publications, rollback).ok());
    MT_EXPECT_EQ(rolled.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(rolled[0], std::string("sha256:second"));
    MT_EXPECT_EQ(rolled[1], std::string("sha256:first"));
    MT_EXPECT_EQ(repository.deleted.size(), static_cast<std::size_t>(1));
}

/// A rollback failure is warned about and stepped over. Both halves matter: a
/// registry that is down must not pin the record forever, yet the attempt has
/// to happen first, because once the record is gone nothing names the
/// publications.
MT_TEST(delete_snapshot_continues_past_rollback_failure) {
    BlobDeleteRecorder repository;
    std::vector<std::string> rolled;
    const PublicationRollback rollback =
        [&rolled](const PersistedDiskImagePublication& publication)
            -> RepositoryResult<core::Unit> {
        rolled.push_back(publication.manifest_digest);
        return core::make_unexpected(RepositoryError::Backend("delete manifest", "503"));
    };

    std::vector<PersistedDiskImagePublication> publications;
    publications.push_back(Publication("https://reg.example.com/v2/a/blobs", "sha256:first"));
    publications.push_back(Publication("https://reg.example.com/v2/a/blobs", "sha256:second"));

    // Still succeeds, and every publication was attempted -- one failure does
    // not abandon the rest.
    MT_EXPECT_TRUE(
        DeleteSnapshot(repository, SomeId(), publications, rollback).ok());
    MT_EXPECT_EQ(rolled.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(repository.deleted.size(), static_cast<std::size_t>(1));
}

/// With nothing published the rollback is never consulted, which is why the
/// two-argument overload can pass an empty functor at all.
MT_TEST(delete_snapshot_without_publications_skips_rollback) {
    BlobDeleteRecorder repository;
    MT_EXPECT_TRUE(DeleteSnapshot(repository, SomeId()).ok());
    MT_EXPECT_EQ(repository.deleted.size(), static_cast<std::size_t>(1));
}

// ---------------------------------------------------------------------------
// the push half
// ---------------------------------------------------------------------------
//
// Driven over the same scripted transport, and for the same reason: an upload
// is a *sequence* -- POST, PATCH per chunk, PUT with the digest -- and only
// asserting the recorded requests can tell a correct upload from one that
// sent the right total number of bytes in the wrong shape.

const char* kUploadUrl = "https://reg.example.com/v2/team/app/blobs/uploads/";
const char* kRepoBlobUrl = "https://reg.example.com/v2/team/app/blobs";
const char* kPutManifestUrl = "https://reg.example.com/v2/team/app/manifests/snap-1";

ScriptedTransport::Reply WithLocation(long status, const std::string& location) {
    ScriptedTransport::Reply reply;
    reply.status = status;
    reply.headers["Location"] = location;
    return reply;
}

std::string HeaderOf(const ScriptedTransport& transport, std::size_t index,
                     const std::string& name) {
    const obdhttp::HeaderMap::const_iterator at = transport.requests[index].headers.find(name);
    return at == transport.requests[index].headers.end() ? std::string() : at->second;
}

std::string BodyOf(const ScriptedTransport& transport, std::size_t index) {
    const std::vector<uint8_t>& body = transport.requests[index].body;
    return std::string(body.begin(), body.end());
}

std::vector<uint8_t> Bytes(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

AcrClientOptions ChunkedOptions(std::size_t chunk_size) {
    AcrClientOptions options = FastOptions();
    options.upload_chunk_size = chunk_size;
    return options;
}

/// A real file, because `upload_blob` stats and reads one. The chunking is
/// driven off the size on disk, so a fake would not exercise it.
struct TempBlob {
    std::string path;

    explicit TempBlob(const std::string& contents) {
        char name[] = "/tmp/agentenv-acr-blob-XXXXXX";
        const int fd = ::mkstemp(name);
        path = name;
        MT_EXPECT_TRUE(fd >= 0);
        if (fd >= 0) {
            const ssize_t written = ::write(fd, contents.data(), contents.size());
            MT_EXPECT_EQ(static_cast<std::size_t>(written), contents.size());
            ::close(fd);
        }
    }

    ~TempBlob() { ::unlink(path.c_str()); }
};

// ---- append_digest_query / absolutize_location ----------------------------

MT_TEST(acr_append_digest_query_picks_the_right_delimiter) {
    MT_EXPECT_EQ(AppendDigestQuery("https://r/v2/x/blobs/uploads/abc", "sha256:ff"),
                 std::string("https://r/v2/x/blobs/uploads/abc?digest=sha256:ff"));
    // An upload session `Location` is opaque and commonly already carries a
    // query, which is the case a `?` would corrupt into a second one.
    MT_EXPECT_EQ(AppendDigestQuery("https://r/v2/x/blobs/uploads/abc?_state=y", "sha256:ff"),
                 std::string("https://r/v2/x/blobs/uploads/abc?_state=y&digest=sha256:ff"));
}

/// The colon stays literal, matching upstream: it is legal in a query value,
/// and a percent-encoded `sha256%3A...` is rejected by some registries.
MT_TEST(acr_append_digest_query_does_not_encode_the_digest) {
    const std::string url = AppendDigestQuery("https://r/v2/x/blobs/uploads/a", "sha256:ff00");
    MT_EXPECT_TRUE(url.find("digest=sha256:ff00") != std::string::npos);
    MT_EXPECT_TRUE(url.find("%3A") == std::string::npos);
}

MT_TEST(acr_absolutize_location_passes_an_absolute_url_through) {
    const core::Expected<std::string, AcrClientError> out =
        AbsolutizeLocation(kUploadUrl, "https://cdn.example.com/upload/1?token=t");
    MT_EXPECT_TRUE(out.ok());
    if (out.ok()) {
        MT_EXPECT_EQ(out.value(), std::string("https://cdn.example.com/upload/1?token=t"));
    }
}

MT_TEST(acr_absolutize_location_resolves_a_path_against_the_origin) {
    const core::Expected<std::string, AcrClientError> absolute_path =
        AbsolutizeLocation(kUploadUrl, "/v2/team/app/blobs/uploads/sess?_state=x");
    MT_EXPECT_TRUE(absolute_path.ok());
    if (absolute_path.ok()) {
        MT_EXPECT_EQ(
            absolute_path.value(),
            std::string("https://reg.example.com/v2/team/app/blobs/uploads/sess?_state=x"));
    }

    // A relative path joins onto the *directory* of the base, not onto the
    // origin: resolving it against the origin would send the chunks to
    // `https://reg.example.com/sess`.
    const core::Expected<std::string, AcrClientError> relative =
        AbsolutizeLocation(kUploadUrl, "sess");
    MT_EXPECT_TRUE(relative.ok());
    if (relative.ok()) {
        MT_EXPECT_EQ(relative.value(),
                     std::string("https://reg.example.com/v2/team/app/blobs/uploads/sess"));
    }
}

/// Dot segments are removed, as `Url::join` does upstream. Left in, the
/// completing `PUT` would go to a path the registry has no session for.
MT_TEST(acr_absolutize_location_removes_dot_segments) {
    const core::Expected<std::string, AcrClientError> out = AbsolutizeLocation(
        "https://reg.example.com/v2/team/app/blobs/uploads/sess", "../uploads/sess2");
    MT_EXPECT_TRUE(out.ok());
    if (out.ok()) {
        MT_EXPECT_EQ(out.value(),
                     std::string("https://reg.example.com/v2/team/app/blobs/uploads/sess2"));
    }
}

/// A protocol-relative `Location` inherits the base's scheme rather than
/// being upgraded to https, so a plain-HTTP test registry keeps working.
MT_TEST(acr_absolutize_location_inherits_the_scheme) {
    const core::Expected<std::string, AcrClientError> out =
        AbsolutizeLocation("http://127.0.0.1:5000/v2/x/blobs/uploads/", "//other:5000/u/1");
    MT_EXPECT_TRUE(out.ok());
    if (out.ok()) MT_EXPECT_EQ(out.value(), std::string("http://other:5000/u/1"));
}

MT_TEST(acr_absolutize_location_refuses_a_relative_url_without_a_base) {
    const core::Expected<std::string, AcrClientError> out =
        AbsolutizeLocation("not a url", "/v2/x/blobs/uploads/sess");
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) MT_EXPECT_TRUE(out.error().kind == AcrClientError::Kind::Registry);
}

// ---- ensure_manifest_absent ----------------------------------------------

/// A `HEAD`, not a `GET`: this runs before any blob is uploaded, so the cost
/// of discovering a taken tag has to stay at one empty round trip.
MT_TEST(acr_ensure_manifest_absent_accepts_404) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(404));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(client.EnsureManifestAbsent(kPutManifestUrl, "team/app", "snap-1").ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(fixture.transport.requests[0].method, std::string("HEAD"));
    MT_EXPECT_EQ(fixture.transport.requests[0].url, std::string(kPutManifestUrl));
    MT_EXPECT_TRUE(fixture.transport.requests[0].body.empty());
}

MT_TEST(acr_ensure_manifest_absent_refuses_an_existing_tag) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(200));
    AcrClient client = fixture.Build();

    const core::Expected<core::Unit, AcrClientError> out =
        client.EnsureManifestAbsent(kPutManifestUrl, "team/app", "snap-1");
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        // `ManifestExists` and not a generic failure: it is the one variant
        // the caller can act on, by choosing another tag.
        MT_EXPECT_TRUE(out.error().kind == AcrClientError::Kind::ManifestExists);
        MT_EXPECT_EQ(out.error().detail, std::string("snap-1"));
    }
}

/// Any other 2xx also means something answers to the tag. A `204` read as
/// "absent" would overwrite a published manifest.
MT_TEST(acr_ensure_manifest_absent_treats_any_success_as_existing) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(204));
    AcrClient client = fixture.Build();
    const core::Expected<core::Unit, AcrClientError> out =
        client.EnsureManifestAbsent(kPutManifestUrl, "team/app", "snap-1");
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) MT_EXPECT_TRUE(out.error().kind == AcrClientError::Kind::ManifestExists);
}

// ---- blob_exists ----------------------------------------------------------

MT_TEST(acr_blob_exists_reports_a_present_blob) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(200));
    AcrClient client = fixture.Build();

    const core::Expected<bool, AcrClientError> out =
        client.BlobExists(kRepoBlobUrl, "team/app", "sha256:ff");
    MT_EXPECT_TRUE(out.ok());
    if (out.ok()) MT_EXPECT_TRUE(out.value());
    MT_EXPECT_EQ(fixture.transport.requests[0].method, std::string("HEAD"));
    MT_EXPECT_EQ(fixture.transport.requests[0].url,
                 std::string("https://reg.example.com/v2/team/app/blobs/sha256:ff"));
}

/// `trim_end_matches('/')` strips *every* trailing slash. One pass would
/// leave `blobs//sha256:ff`, which is a different path at the registry.
MT_TEST(acr_blob_exists_collapses_trailing_slashes) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(200));
    AcrClient client = fixture.Build();
    MT_EXPECT_TRUE(client.BlobExists(std::string(kRepoBlobUrl) + "///", "team/app",
                                     "sha256:ff").ok());
    MT_EXPECT_EQ(fixture.transport.requests[0].url,
                 std::string("https://reg.example.com/v2/team/app/blobs/sha256:ff"));
}

MT_TEST(acr_blob_exists_reports_an_absent_blob) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(404));
    AcrClient client = fixture.Build();
    const core::Expected<bool, AcrClientError> out =
        client.BlobExists(kRepoBlobUrl, "team/app", "sha256:ff");
    MT_EXPECT_TRUE(out.ok());
    if (out.ok()) MT_EXPECT_TRUE(!out.value());
}

/// A failure is not "absent". Collapsing it would re-upload a layer the
/// registry already holds and hide the error while doing it.
MT_TEST(acr_blob_exists_propagates_a_failure) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(400));
    AcrClient client = fixture.Build();
    const core::Expected<bool, AcrClientError> out =
        client.BlobExists(kRepoBlobUrl, "team/app", "sha256:ff");
    MT_EXPECT_TRUE(!out.ok());
}

// ---- upload_blob ----------------------------------------------------------

/// The whole protocol in one assertion: a `POST` to open the session, one
/// `PATCH` per chunk with a bare `<start>-<end>` range and that chunk's
/// bytes, then a bodyless `PUT ?digest=`.
MT_TEST(acr_upload_blob_sends_one_patch_per_chunk) {
    TempBlob blob("abcde");
    ClientFixture fixture;
    fixture.transport.replies.push_back(WithLocation(202, "/v2/team/app/blobs/uploads/sess"));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(201));
    AcrClient client = fixture.Build(ChunkedOptions(2));

    MT_EXPECT_TRUE(
        client.UploadBlob(kUploadUrl, "team/app", blob.path, "sha256:ff").ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(5));

    MT_EXPECT_EQ(fixture.transport.requests[0].method, std::string("POST"));
    MT_EXPECT_EQ(fixture.transport.requests[0].url, std::string(kUploadUrl));

    const char* const ranges[] = {"0-1", "2-3", "4-4"};
    const char* const chunks[] = {"ab", "cd", "e"};
    for (std::size_t i = 0; i < 3; ++i) {
        MT_EXPECT_EQ(fixture.transport.requests[1 + i].method, std::string("PATCH"));
        MT_EXPECT_EQ(fixture.transport.requests[1 + i].url,
                     std::string("https://reg.example.com/v2/team/app/blobs/uploads/sess"));
        // Bare `<start>-<end>`: no `bytes ` prefix and no `/<total>`.
        MT_EXPECT_EQ(HeaderOf(fixture.transport, 1 + i, "Content-Range"),
                     std::string(ranges[i]));
        MT_EXPECT_EQ(BodyOf(fixture.transport, 1 + i), std::string(chunks[i]));
        MT_EXPECT_EQ(HeaderOf(fixture.transport, 1 + i, "Content-Type"),
                     std::string("application/octet-stream"));
    }

    // The completing PUT carries no further bytes -- everything went out as
    // chunks, and repeating the tail here would append it twice.
    MT_EXPECT_EQ(fixture.transport.requests[4].method, std::string("PUT"));
    MT_EXPECT_EQ(
        fixture.transport.requests[4].url,
        std::string("https://reg.example.com/v2/team/app/blobs/uploads/sess?digest=sha256:ff"));
    MT_EXPECT_TRUE(fixture.transport.requests[4].body.empty());
}

/// One chunk larger than the file is one `PATCH`, covering the whole range.
MT_TEST(acr_upload_blob_sends_one_patch_when_the_chunk_covers_the_file) {
    TempBlob blob("abcde");
    ClientFixture fixture;
    fixture.transport.replies.push_back(WithLocation(202, "/v2/team/app/blobs/uploads/sess"));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(201));
    AcrClient client = fixture.Build(ChunkedOptions(1024));

    MT_EXPECT_TRUE(client.UploadBlob(kUploadUrl, "team/app", blob.path, "sha256:ff").ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(HeaderOf(fixture.transport, 1, "Content-Range"), std::string("0-4"));
    MT_EXPECT_EQ(BodyOf(fixture.transport, 1), std::string("abcde"));
}

/// A chunk size of zero is clamped to one rather than dividing by it: left
/// alone, the loop would read nothing and never advance.
MT_TEST(acr_upload_blob_clamps_a_zero_chunk_size) {
    TempBlob blob("ab");
    ClientFixture fixture;
    fixture.transport.replies.push_back(WithLocation(202, "/v2/team/app/blobs/uploads/sess"));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(201));
    AcrClient client = fixture.Build(ChunkedOptions(0));

    MT_EXPECT_TRUE(client.UploadBlob(kUploadUrl, "team/app", blob.path, "sha256:ff").ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(4));
    MT_EXPECT_EQ(HeaderOf(fixture.transport, 1, "Content-Range"), std::string("0-0"));
    MT_EXPECT_EQ(HeaderOf(fixture.transport, 2, "Content-Range"), std::string("1-1"));
}

/// A registry may relocate the session between chunks, and will not accept
/// the old URL again. Ignoring the new `Location` is how an upload fails
/// halfway through with a 404 the client reports as a registry error.
MT_TEST(acr_upload_blob_follows_a_relocated_session) {
    TempBlob blob("abcd");
    ClientFixture fixture;
    fixture.transport.replies.push_back(WithLocation(202, "/v2/team/app/blobs/uploads/one"));
    fixture.transport.replies.push_back(WithLocation(202, "/v2/team/app/blobs/uploads/two"));
    fixture.transport.replies.push_back(WithLocation(202, "/v2/team/app/blobs/uploads/three"));
    fixture.transport.replies.push_back(ScriptedTransport::Status(201));
    AcrClient client = fixture.Build(ChunkedOptions(2));

    MT_EXPECT_TRUE(client.UploadBlob(kUploadUrl, "team/app", blob.path, "sha256:ff").ok());
    MT_EXPECT_EQ(fixture.transport.requests[1].url,
                 std::string("https://reg.example.com/v2/team/app/blobs/uploads/one"));
    MT_EXPECT_EQ(fixture.transport.requests[2].url,
                 std::string("https://reg.example.com/v2/team/app/blobs/uploads/two"));
    MT_EXPECT_EQ(
        fixture.transport.requests[3].url,
        std::string("https://reg.example.com/v2/team/app/blobs/uploads/three?digest=sha256:ff"));
}

/// Without a `Location` there is nowhere to send the chunks. Falling back to
/// the upload endpoint would open a fresh session per chunk and leave every
/// one of them incomplete.
MT_TEST(acr_upload_blob_refuses_a_session_without_a_location) {
    TempBlob blob("abcd");
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    AcrClient client = fixture.Build();

    const core::Expected<core::Unit, AcrClientError> out =
        client.UploadBlob(kUploadUrl, "team/app", blob.path, "sha256:ff");
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        MT_EXPECT_TRUE(out.error().kind == AcrClientError::Kind::Registry);
        MT_EXPECT_TRUE(out.error().detail.find("missing Location") != std::string::npos);
    }
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(1));
}

MT_TEST(acr_upload_blob_reports_a_missing_file_without_opening_a_session) {
    ClientFixture fixture;
    AcrClient client = fixture.Build();
    const core::Expected<core::Unit, AcrClientError> out =
        client.UploadBlob(kUploadUrl, "team/app", "/tmp/agentenv-acr-no-such-blob",
                          "sha256:ff");
    MT_EXPECT_TRUE(!out.ok());
    // Nothing was sent: an orphaned session would otherwise sit on the
    // registry until its own timeout.
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(0));
}

/// The bodyless `POST` still declares a length. libcurl sends no
/// `Content-Length` for a custom method with no payload, and a registry
/// behind a proxy answers that with `411 Length Required`.
MT_TEST(acr_upload_blob_declares_a_zero_length_on_the_session_post) {
    TempBlob blob("ab");
    ClientFixture fixture;
    fixture.transport.replies.push_back(WithLocation(202, "/v2/team/app/blobs/uploads/sess"));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(201));
    AcrClient client = fixture.Build(ChunkedOptions(1024));

    MT_EXPECT_TRUE(client.UploadBlob(kUploadUrl, "team/app", blob.path, "sha256:ff").ok());
    MT_EXPECT_EQ(HeaderOf(fixture.transport, 0, "Content-Length"), std::string("0"));
    // The chunk's length is the transport's business, derived from the body;
    // writing it here as well would send the header twice.
    MT_EXPECT_EQ(HeaderOf(fixture.transport, 1, "Content-Length"), std::string());
}

// ---- upload_blob_with_descriptor -----------------------------------------

/// The deduplication that makes a re-publish cheap: one `HEAD`, no session.
MT_TEST(acr_upload_blob_with_descriptor_skips_an_existing_blob) {
    TempBlob blob("abcde");
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(200));
    AcrClient client = fixture.Build();

    const core::Expected<std::pair<std::string, uint64_t>, AcrClientError> out =
        client.UploadBlobWithDescriptor(kUploadUrl, kRepoBlobUrl, "team/app", blob.path,
                                        "sha256:ff", 5);
    MT_EXPECT_TRUE(out.ok());
    if (out.ok()) {
        MT_EXPECT_EQ(out.value().first, std::string("sha256:ff"));
        MT_EXPECT_EQ(out.value().second, static_cast<uint64_t>(5));
    }
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(fixture.transport.requests[0].method, std::string("HEAD"));
}

MT_TEST(acr_upload_blob_with_descriptor_uploads_an_absent_blob) {
    TempBlob blob("abcde");
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(404));
    fixture.transport.replies.push_back(WithLocation(202, "/v2/team/app/blobs/uploads/sess"));
    fixture.transport.replies.push_back(ScriptedTransport::Status(202));
    fixture.transport.replies.push_back(ScriptedTransport::Status(201));
    AcrClient client = fixture.Build(ChunkedOptions(1024));

    MT_EXPECT_TRUE(client
                       .UploadBlobWithDescriptor(kUploadUrl, kRepoBlobUrl, "team/app",
                                                 blob.path, "sha256:ff", 5)
                       .ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(4));
    MT_EXPECT_EQ(BodyOf(fixture.transport, 2), std::string("abcde"));
}

/// The descriptor is what the manifest will claim and nothing downstream
/// re-derives it, so a disagreement with the file is refused *before* any
/// request -- publishing it would name a layer that is not what was sent.
MT_TEST(acr_upload_blob_with_descriptor_refuses_a_size_mismatch) {
    TempBlob blob("abcde");
    ClientFixture fixture;
    AcrClient client = fixture.Build();

    const core::Expected<std::pair<std::string, uint64_t>, AcrClientError> out =
        client.UploadBlobWithDescriptor(kUploadUrl, kRepoBlobUrl, "team/app", blob.path,
                                        "sha256:ff", 9);
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        MT_EXPECT_TRUE(out.error().detail.find("descriptor says 9") != std::string::npos);
        MT_EXPECT_TRUE(out.error().detail.find("file has 5") != std::string::npos);
    }
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(0));
}

MT_TEST(acr_upload_blob_with_descriptor_refuses_an_empty_digest_or_size) {
    TempBlob blob("abcde");
    ClientFixture fixture;
    AcrClient client = fixture.Build();

    // An empty digest would publish a manifest pointing at `sha256:`, leaving
    // the layer unreachable.
    MT_EXPECT_TRUE(!client
                        .UploadBlobWithDescriptor(kUploadUrl, kRepoBlobUrl, "team/app",
                                                  blob.path, "", 5)
                        .ok());
    MT_EXPECT_TRUE(!client
                        .UploadBlobWithDescriptor(kUploadUrl, kRepoBlobUrl, "team/app",
                                                  blob.path, "sha256:ff", 0)
                        .ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(0));
}

// ---- upload_blob_bytes ---------------------------------------------------

/// No `PATCH` phase at all: the config blob is a few hundred bytes, and
/// chunking something smaller than one chunk only adds a round trip.
MT_TEST(acr_upload_blob_bytes_puts_the_whole_body_at_once) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(WithLocation(202, "/v2/team/app/blobs/uploads/sess"));
    fixture.transport.replies.push_back(ScriptedTransport::Status(201));
    AcrClient client = fixture.Build();

    MT_EXPECT_TRUE(
        client.UploadBlobBytes(kUploadUrl, "team/app", Bytes("{\"config\":1}"), "sha256:cc")
            .ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(fixture.transport.requests[0].method, std::string("POST"));
    MT_EXPECT_EQ(fixture.transport.requests[1].method, std::string("PUT"));
    MT_EXPECT_EQ(
        fixture.transport.requests[1].url,
        std::string("https://reg.example.com/v2/team/app/blobs/uploads/sess?digest=sha256:cc"));
    MT_EXPECT_EQ(BodyOf(fixture.transport, 1), std::string("{\"config\":1}"));
    MT_EXPECT_EQ(HeaderOf(fixture.transport, 1, "Content-Type"),
                 std::string("application/octet-stream"));
    MT_EXPECT_TRUE(HeaderOf(fixture.transport, 1, "Content-Range").empty());
}

MT_TEST(acr_upload_blob_bytes_reports_a_refused_completion) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(WithLocation(202, "/v2/team/app/blobs/uploads/sess"));
    fixture.transport.replies.push_back(ScriptedTransport::Status(400));
    AcrClient client = fixture.Build();
    MT_EXPECT_TRUE(
        !client.UploadBlobBytes(kUploadUrl, "team/app", Bytes("x"), "sha256:cc").ok());
}

// ---- put_manifest --------------------------------------------------------

/// The registry's `Docker-Content-Digest` wins over the locally computed one:
/// it is the digest the registry will answer to later, and they differ only
/// when the registry re-serialised the manifest.
MT_TEST(acr_put_manifest_prefers_the_registry_digest) {
    ClientFixture fixture;
    ScriptedTransport::Reply created;
    created.status = 201;
    created.headers["Docker-Content-Digest"] = "sha256:fromregistry";
    fixture.transport.replies.push_back(created);
    AcrClient client = fixture.Build();

    const core::Expected<std::string, AcrClientError> out =
        client.PutManifest(kPutManifestUrl, "team/app", Bytes("{\"schemaVersion\":2}"));
    MT_EXPECT_TRUE(out.ok());
    if (out.ok()) MT_EXPECT_EQ(out.value(), std::string("sha256:fromregistry"));

    MT_EXPECT_EQ(fixture.transport.requests[0].method, std::string("PUT"));
    MT_EXPECT_EQ(BodyOf(fixture.transport, 0), std::string("{\"schemaVersion\":2}"));
    // The OCI manifest media type, not `application/octet-stream`: a registry
    // stores the body under the type it was declared as, and an octet-stream
    // manifest is not resolvable as one.
    MT_EXPECT_EQ(HeaderOf(fixture.transport, 0, "Content-Type"),
                 std::string(kOciImageManifestMediaType));
}

/// A missing header is not an error, which is why the local digest is
/// computed up front rather than only on demand.
MT_TEST(acr_put_manifest_falls_back_to_the_local_digest) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(201));
    AcrClient client = fixture.Build();

    const std::string manifest = "{\"schemaVersion\":2}";
    const core::Expected<std::string, AcrClientError> out =
        client.PutManifest(kPutManifestUrl, "team/app", Bytes(manifest));
    MT_EXPECT_TRUE(out.ok());
    if (out.ok()) MT_EXPECT_EQ(out.value(), core::Sha256Digest(manifest));
}

MT_TEST(acr_put_manifest_reports_a_refused_manifest) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(400));
    AcrClient client = fixture.Build();
    MT_EXPECT_TRUE(!client.PutManifest(kPutManifestUrl, "team/app", Bytes("{}")).ok());
}

/// The body goes out again with the post-401 retry. Omitted, an authenticated
/// upload becomes an empty one the registry accepts, leaving a blob whose
/// content does not match the digest it was stored under -- a corruption no
/// status code reports.
MT_TEST(acr_resends_the_body_on_the_authenticated_retry) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Unauthorized(kChallenge));
    fixture.transport.replies.push_back(ScriptedTransport::Token("tok-1", 300));
    fixture.transport.replies.push_back(ScriptedTransport::Status(201));
    AcrClient client = fixture.Build();

    const std::string manifest = "{\"schemaVersion\":2}";
    MT_EXPECT_TRUE(client.PutManifest(kPutManifestUrl, "team/app", Bytes(manifest)).ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(BodyOf(fixture.transport, 0), manifest);
    // The retry, with both the token and the body.
    MT_EXPECT_EQ(fixture.transport.AuthorizationOf(2), std::string("Bearer tok-1"));
    MT_EXPECT_EQ(BodyOf(fixture.transport, 2), manifest);
    MT_EXPECT_EQ(HeaderOf(fixture.transport, 2, "Content-Type"),
                 std::string(kOciImageManifestMediaType));
}

/// And again across a transient retry, where the whole attempt is repeated
/// rather than re-authenticated.
MT_TEST(acr_resends_the_body_on_a_transient_retry) {
    ClientFixture fixture;
    fixture.transport.replies.push_back(ScriptedTransport::Status(503));
    fixture.transport.replies.push_back(ScriptedTransport::Status(201));
    AcrClient client = fixture.Build();

    const std::string manifest = "{\"schemaVersion\":2}";
    MT_EXPECT_TRUE(client.PutManifest(kPutManifestUrl, "team/app", Bytes(manifest)).ok());
    MT_EXPECT_EQ(fixture.transport.requests.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(BodyOf(fixture.transport, 1), manifest);
    MT_EXPECT_EQ(fixture.sleeps.size(), static_cast<std::size_t>(1));
}

}  // namespace

MT_MAIN
