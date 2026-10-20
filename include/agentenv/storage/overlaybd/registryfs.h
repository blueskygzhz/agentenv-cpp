// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/registryfs_v2.rs — reading blobs from an
// OCI/Docker registry.
//
// The read half only. Rust's `RegistryUploader` / `UploadSession` (the chunked
// push path, roughly 800 lines) is not here: nothing in the image read path
// pushes a blob, and a half-ported uploader would be untested surface.
#ifndef AGENTENV_STORAGE_OVERLAYBD_REGISTRYFS_H_
#define AGENTENV_STORAGE_OVERLAYBD_REGISTRYFS_H_

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/http_transport.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust's TTL constants. All of them are short on purpose: every cached value
/// here is something the registry may revoke or re-point at any time, so the
/// cache is about collapsing bursts, not about holding state.
const uint64_t kRegistryMinTokenLifeSecs = 30;
const uint64_t kRegistryMinUrlInfoLifeSecs = 300;
const uint64_t kRegistryMinMetaLifeSecs = 300;
const uint64_t kRegistryAuthChallengeLifeSecs = 300;
/// Shorter than the others: `ProbePerUrl` is a fallback recorded after the
/// registry-wide ping failed, so it should be retried sooner than a real
/// answer.
const uint64_t kRegistryProbePerUrlLifeSecs = 30;
const uint64_t kRegistryDefaultTimeoutSecs = 30;
const uint32_t kRegistryDefaultRetryCount = 3;

/// Rust `RegistryFsV2Options`.
struct RegistryFsOptions {
    uint64_t timeout_secs;
    uint32_t retry_count;
    std::string user_agent;
    CredentialConfig credential;
    /// A P2P accelerator prefixed in front of the resolved blob URL. Empty
    /// disables it.
    std::string accelerate_address;
    std::string cert_file;
    std::string key_file;

    RegistryFsOptions()
        : timeout_secs(kRegistryDefaultTimeoutSecs),
          retry_count(kRegistryDefaultRetryCount) {}

    /// Rust `from_global_config`.
    ///
    /// `download.try_cnt <= 0` falls back to the default rather than meaning
    /// "no retries": zero attempts would make every transient failure fatal.
    static RegistryFsOptions FromGlobalConfig(const GlobalConfig& cfg);
};

/// Rust `UrlMode` — how a resolved blob URL is reached.
enum RegistryUrlMode {
    /// The registry answered with a redirect; `info` is the target URL and it
    /// carries its own credentials in the query string, so no `Authorization`
    /// must be sent (and sending one breaks some CDNs).
    kRegistryUrlRedirect,
    /// The registry serves the blob itself; `info` is the `Authorization`
    /// header value to repeat on each range request.
    kRegistryUrlSelfAuth
};

/// Rust `RegistryAuthChallenge`.
enum RegistryAuthKind {
    kRegistryAuthNone,
    kRegistryAuthBasic,
    kRegistryAuthBearer,
    /// The registry-wide `/v2/` ping did not yield a usable challenge, so each
    /// URL is probed on its own. A distinct state rather than an error: some
    /// registries answer the ping differently from the blob endpoint.
    kRegistryAuthProbePerUrl
};

/// Rust `BearerChallengeParts` / `RegistryAuthChallenge::Bearer`.
struct RegistryAuthChallenge {
    RegistryAuthKind kind;
    std::string realm;
    std::string service;
    /// Only present on a per-URL challenge; the registry ping never scopes.
    std::string scope;

    RegistryAuthChallenge() : kind(kRegistryAuthNone) {}
};

/// Rust `parse_registry_auth_challenge` / `parse_auth_challenge`.
///
/// `want_scope` keeps the challenge's own `scope` parameter. The registry-wide
/// ping deliberately drops it: that challenge is cached per origin, and a
/// scope belonging to one repository must not be reused for another.
core::Expected<RegistryAuthChallenge, std::string> ParseRegistryAuthChallenge(
    const std::string& challenge, bool want_scope);

/// Rust `build_bearer_auth_url`.
///
/// `scope` is optional per RFC 6750 §3, so an empty one is omitted rather than
/// sent blank -- a blank scope is not the same request.
core::Expected<std::string, std::string> BuildBearerAuthUrl(const std::string& realm,
                                                               const std::string& service,
                                                               const std::string& scope);

/// Rust `build_basic_auth_header`.
std::string BuildBasicAuthHeader(const std::string& username, const std::string& password);

/// Rust `BlobAuthContext` — what a `/v2/<repo>/blobs/<digest>` URL implies.
struct BlobAuthContext {
    /// `scheme://host[:port]`, the key the registry-wide challenge is cached
    /// under.
    std::string origin;
    /// `<origin>/v2/`, the endpoint whose 401 carries the challenge.
    std::string ping_url;
    /// `repository:<repo>:pull`.
    std::string scope;
};

/// Rust `blob_auth_context` — recognise a blob URL and derive its auth
/// context.
///
/// Reports nothing for anything that is not a registry blob URL. That is the
/// signal to fall back to per-URL probing: a redirected CDN URL has no `/v2/`
/// endpoint to ping, and guessing one would send credentials to a host that
/// never asked for them.
core::Optional<BlobAuthContext> ParseBlobAuthContext(const std::string& remote_url);

class RegistryFs;

/// Rust `RegistryFileImplV2` — one blob, read-only.
class RegistryFile : public VirtualFile {
 public:
    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override;
    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst,
                                                     size_t len) override;
    /// Always refused: a blob is immutable once pushed.
    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                  size_t len) override;
    core::Expected<uint64_t, std::string> Size() override;

 private:
    friend class RegistryFs;

    RegistryFile() : backend_(0), size_known_(false), size_(0) {}
    RegistryFile(const RegistryFile&);
    RegistryFile& operator=(const RegistryFile&);

    RegistryFs* backend_;
    std::string url_;
    bool size_known_;
    uint64_t size_;
};

/// Rust `RegistryFsV2`.
class RegistryFs {
 public:
    static core::Expected<std::shared_ptr<RegistryFs>, std::string> WithOptions(
        const RegistryFsOptions& options);

    /// Rust `from_global_config`.
    static core::Expected<std::shared_ptr<RegistryFs>, std::string> FromGlobalConfig(
        const GlobalConfig& cfg);

    /// Rust `with_static_credentials` — bypass `CredentialConfig` entirely.
    static core::Expected<std::shared_ptr<RegistryFs>, std::string> WithStaticCredentials(
        const RegistryFsOptions& options, const std::string& username,
        const std::string& password);

    /// Rust `set_accelerate_address`.
    void SetAccelerateAddress(const std::string& addr) { accelerate_address_ = addr; }

    /// Rust `open_file` / `open_file_with_size_hint`.
    ///
    /// A non-zero hint avoids the size probe. Unlike the OSS backend this is
    /// the common case: a manifest already lists every blob's size.
    core::Expected<std::shared_ptr<RegistryFile>, std::string> OpenFile(
        const std::string& url, uint64_t size_hint);

    /// Rust `refresh_token` — mint a new token for a URL and cache it.
    ///
    /// Reports absence rather than failing when the endpoint needs no token
    /// (anonymous or Basic): there is nothing to refresh, which is not an
    /// error.
    ///
    /// Probed with the *push* flavour, as Rust does, so the challenge comes
    /// back scoped for `pull,push` and the token is usable for an upload. A
    /// pull probe would yield a pull-only token, defeating the purpose.
    core::Expected<core::Optional<std::string>, std::string> RefreshToken(
        const std::string& url);

 private:
    friend class RegistryFile;

    struct CachedToken {
        std::string token;
        uint64_t expire_at_ms;
        CachedToken() : expire_at_ms(0) {}
    };
    struct CachedChallenge {
        RegistryAuthChallenge challenge;
        uint64_t expire_at_ms;
        CachedChallenge() : expire_at_ms(0) {}
    };
    struct UrlInfo {
        RegistryUrlMode mode;
        /// The redirect target, or the `Authorization` header to repeat.
        std::string info;
        uint64_t expire_at_ms;
        UrlInfo() : mode(kRegistryUrlSelfAuth), expire_at_ms(0) {}
    };
    struct CachedSize {
        uint64_t size;
        uint64_t expire_at_ms;
        CachedSize() : size(0), expire_at_ms(0) {}
    };

    /// What to put on a request, once the challenge is known.
    struct PreparedAuth {
        RegistryAuthKind kind;
        std::string header;
        /// The token-endpoint URL the token was cached under, so a rejection
        /// can invalidate exactly that entry.
        std::string cache_key;
        PreparedAuth() : kind(kRegistryAuthNone) {}
    };

    RegistryFs() : static_credentials_(false) {}
    RegistryFs(const RegistryFs&);
    RegistryFs& operator=(const RegistryFs&);

    /// Rust `resolve_credentials` — `""` / `file` / `http` / static.
    core::Expected<core::Unit, std::string> ResolveCredentials(const std::string& remote_url,
                                                                 std::string* username,
                                                                 std::string* password);

    core::Expected<RegistryAuthChallenge, std::string> GetOrCreateRegistryChallenge(
        const BlobAuthContext& ctx);

    /// Rust `get_scope_auth_for` — probe one endpoint for its challenge.
    ///
    /// `push` selects a POST probe instead of a ranged GET, because a
    /// registry only answers with a `pull,push` scope when asked about a
    /// write.
    core::Expected<RegistryAuthChallenge, std::string> GetScopeAuth(const std::string& url,
                                                                       bool push);

    /// Rust `get_token` — fetch a token, bypassing the cache.
    core::Expected<std::string, std::string> GetToken(const std::string& remote_url,
                                                         const std::string& auth_url);
    /// Rust `get_or_create_token` — cached variant of the above.
    core::Expected<std::string, std::string> GetOrCreateToken(const std::string& remote_url,
                                                                 const std::string& auth_url);

    /// Attach `cert_file`/`key_file` to a request, when both are configured.
    void ApplyClientCert(http::Request* request) const;

    core::Expected<PreparedAuth, std::string> PrepareCachedAuth(const std::string& url,
                                                                   const BlobAuthContext& ctx);
    /// Rust `prepare_legacy_auth` — probe the blob URL itself for a challenge.
    core::Expected<PreparedAuth, std::string> PrepareLegacyAuth(const std::string& url);

    core::Expected<UrlInfo, std::string> GetOrCreateUrlInfo(const std::string& url);
    core::Expected<UrlInfo, std::string> GetActualUrl(const std::string& url);
    /// One resolution attempt with a given credential. Shared by the main
    /// path and the per-URL probe fallback so the two cannot drift.
    core::Expected<UrlInfo, std::string> ResolveWithAuth(const std::string& url,
                                                            const PreparedAuth& auth);

    /// One ranged GET against the resolved URL, with the accelerator applied
    /// when configured.
    /// `sink` null accumulates the body into the response; non-null streams
    /// it there instead.
    core::Expected<http::Response, std::string> FetchRange(const std::string& url,
                                                              uint64_t offset, size_t count,
                                                              long timeout_secs,
                                                              http::BodySink* sink);

    /// Rust `read_range` / `read_range_into` / `get_length` — each is a thin
    /// measuring wrapper over the matching `*Inner`.
    ///
    /// The split is Rust's and is kept for the same reason: it puts the
    /// metric on the single exit of the wrapper, so no early return inside
    /// the retry loop can escape untimed.
    core::Expected<ByteBuffer, std::string> ReadRange(const std::string& url, uint64_t offset,
                                                         size_t count);
    /// Rust `read_range_into` — stream into a caller-owned buffer.
    ///
    /// Separate from `ReadRange` because Rust consumes `resp.chunk()` into
    /// `dst` rather than materialising the body first, and refuses an
    /// over-long response outright instead of truncating it.
    core::Expected<size_t, std::string> ReadRangeInto(const std::string& url, uint64_t offset,
                                                         uint8_t* dst, size_t len);

    core::Expected<ByteBuffer, std::string> ReadRangeInner(const std::string& url,
                                                              uint64_t offset, size_t count);
    core::Expected<size_t, std::string> ReadRangeIntoInner(const std::string& url,
                                                              uint64_t offset, uint8_t* dst,
                                                              size_t len);
    core::Expected<uint64_t, std::string> GetLengthInner(const std::string& url);
    core::Expected<uint64_t, std::string> GetLength(const std::string& url);

    void InvalidateUrlInfo(const std::string& url) {
        std::lock_guard<std::mutex> lk(cache_mu_);
        url_infos_.erase(url);
    }
    void InvalidateToken(const std::string& key) {
        std::lock_guard<std::mutex> lk(cache_mu_);
        tokens_.erase(key);
    }
    void InvalidateSize(const std::string& url) {
        std::lock_guard<std::mutex> lk(cache_mu_);
        sizes_.erase(url);
    }
    void MarkProbePerUrl(const std::string& origin);

    RegistryFsOptions options_;
    std::string accelerate_address_;
    /// Set only by `WithStaticCredentials`; takes precedence over
    /// `options_.credential`.
    bool static_credentials_;
    std::string static_username_;
    std::string static_password_;

    /// Guards the four caches below.
    ///
    /// Rust reaches for `DashMap`, which is concurrent by construction; a bare
    /// `std::map` is not, and these are read on every blob request. One mutex
    /// rather than four because the critical sections are map lookups, so the
    /// contention saved by splitting them would not pay for the risk of
    /// taking two in the wrong order.
    ///
    /// Note this is *not* Rust's per-key `bearer_token_locks` /
    /// `registry_auth_locks`: those serialise the token *fetch* so a burst of
    /// readers issues one network request instead of N. Holding a mutex
    /// across a blocking HTTP call here would serialise unrelated blobs, so
    /// the duplicate-fetch collapsing is deliberately not ported -- a
    /// redundant token request is cheaper than a convoyed reader.
    mutable std::mutex cache_mu_;

    /// Keyed by the *full* token-endpoint URL (realm + service + scope), so
    /// tokens for different registries or repositories never collide -- even
    /// when a challenge omits `scope`.
    std::map<std::string, CachedToken> tokens_;
    /// Keyed by origin.
    std::map<std::string, CachedChallenge> challenges_;
    std::map<std::string, UrlInfo> url_infos_;
    std::map<std::string, CachedSize> sizes_;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_REGISTRYFS_H_
