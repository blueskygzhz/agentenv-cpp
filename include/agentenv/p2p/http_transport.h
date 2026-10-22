// SPDX-License-Identifier: MIT
// A P2pTransport over plain HTTP/1.1 — the alternative to the iroh backend.
//
// Why this exists. The iroh backend needs two things that are protocols to
// match bit-for-bit rather than libraries to call: magicsock (relay plus hole
// punching) and iroh-blobs' BLAKE3/bao verified streaming. Inside one cluster
// the scheduler already hands out directly routable peer addresses, so NAT
// traversal buys nothing, and artifact integrity is already carried by the
// digest checks the image and volume layers perform on every layer they
// accept. What is left of the iroh backend's job is moving bytes between two
// reachable hosts, which HTTP does.
//
// Why the protocol is range-only. Neither `api::HttpResponse::body` nor
// `core::http::Response::body` can stream — both are whole `std::string`s — so
// a multi-gigabyte artifact served as a single GET would be buffered twice
// over. Requiring a `Range` header on every blob request turns that
// limitation into three properties worth having anyway: a fixed memory
// ceiling, resumability, and a natural fit for the only consumer that matters,
// since `overlaybd::p2p` already issues range reads against the facade's own
// local HTTP surface.
//
// Wire format, `agentenv-p2p/v1`:
//
//   POST /p2p/v1/catalog/lookup     body is `CatalogRequest::Encode()`,
//                                   answer is `CatalogResponse::Encode()`
//   HEAD /p2p/v1/blobs/{locator}    Content-Length + digest header
//   GET  /p2p/v1/blobs/{locator}    requires `Range`, answers 206
//
// The catalog half reuses `p2p/catalog.h` verbatim, which is what that file
// was already scoped for: "the request/response types and size limits live
// here too, so a future transport (iroh or otherwise) serves the identical
// wire format". Only the framing differs — an HTTP path instead of an ALPN.
//
// Why this client never issues the HEAD. `core::http` frames a response body
// by `Content-Length`, so a bodiless HEAD reply carrying a non-zero one reads
// to it as a truncated response and fails to parse. The fetch path takes the
// artifact's total from the first range response's `Content-Range` instead,
// which removes the hazard and a round trip at once — worth having, since
// `HttpClient` opens a fresh connection per request. The route is still
// served, because it belongs to the protocol rather than to this client; see
// the note above it in `http_routes.cc`.
//
// A consequence worth knowing before changing the server: a range whose end
// runs past the artifact is *clamped*, not refused (RFC 7233). That is what
// lets a client ask for a first chunk of something whose size it does not yet
// know. Only a range that *starts* past the end is 416 — which is also how an
// empty artifact reports its size, since no range over one is satisfiable.
#ifndef AGENTENV_P2P_HTTP_TRANSPORT_H_
#define AGENTENV_P2P_HTTP_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/p2p/catalog.h"
#include "agentenv/p2p/config.h"
#include "agentenv/p2p/discovery.h"
#include "agentenv/p2p/http_blob_store.h"
#include "agentenv/p2p/transport.h"

namespace agentenv {
namespace p2p {

/// The `P2pEndpoint::backend` value this transport publishes and accepts is
/// `kHttpBackendId`, declared in `p2p/config.h` beside the iroh one.

extern const char* const kCatalogLookupPath;
extern const char* const kBlobPathPrefix;
/// Shared-secret header. `core::http` speaks plaintext TCP, so this is the
/// only credential standing between the blob surface and anyone who can reach
/// the port; see `Options::shared_token`.
extern const char* const kTokenHeader;
extern const char* const kDigestHeader;

/// Chunk size for whole-artifact pulls.
///
/// Large on purpose: `core::http::HttpClient` opens a fresh connection per
/// request, so a 4 GiB artifact at overlaybd's natural 4 MiB granularity would
/// mean a thousand connects. Range reads on behalf of the facade are
/// unaffected — those carry the caller's own length.
extern const uint64_t kFetchChunkBytes;

/// Server-side ceiling on a single range, enforced with 416.
///
/// The response body is assembled in full before it is written, so without a
/// bound a peer could ask for an artifact-sized range and charge this node the
/// memory for it.
extern const uint64_t kMaxRangeBytes;

/// Rust `MAX_CONCURRENT_CATALOG_LOOKUPS`.
extern const std::size_t kMaxConcurrentCatalogLookups;

// ---- the serving surface ----------------------------------------------------
//
// Declared here rather than using `api::Server` directly. `agentenv-api` links
// `agentenv-p2p` transitively (api -> orchestrator -> snapshot -> p2p), so a
// `p2p -> api` dependency would be a CMake cycle. This is the same constraint
// `node.h` documents for `cfg`, and it is resolved the same way: p2p names the
// contract it needs and the caller injects an adapter over its real server.

struct P2pServerRequest {
    std::string method;
    std::string path;
    /// Use `FindP2pHeader`; HTTP header names are case-insensitive and an
    /// adapter may hand them over in any casing.
    std::map<std::string, std::string> headers;
    std::string body;
};

struct P2pServerResponse {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;
};

/// Case-insensitive header lookup, per RFC 7230.
core::Optional<std::string> FindP2pHeader(const std::map<std::string, std::string>& headers,
                                          const std::string& name);

/// The subset of an HTTP server this transport needs.
class P2pBlobServer {
 public:
    typedef std::function<void(const P2pServerRequest&, P2pServerResponse*)> Handler;

    virtual ~P2pBlobServer() {}

    /// `path_pattern` uses the tree's existing `{name}` placeholder
    /// convention, matched segment-wise: `/p2p/v1/blobs/{locator}` matches
    /// exactly one more segment. Chosen over a prefix wildcard so an adapter
    /// over `api::Server` can hand the pattern straight through — that matcher
    /// requires the segment counts to agree, which is what stops one route
    /// from swallowing a longer path and dispatching it with a truncated id.
    ///
    /// Handlers read the locator back out of `P2pServerRequest::path` rather
    /// than from captured parameters, so the validation that it is a plausible
    /// digest stays in one place.
    virtual void Route(const std::string& method, const std::string& path_pattern,
                       Handler handler) = 0;
    /// Blocking; returns once `Stop()` has been called.
    virtual core::Expected<core::Unit, std::string>
        ListenAndServe(const std::string& bind_addr) = 0;
    virtual void Stop() = 0;
};

/// Builds the server the `Http` transport serves on. Injected for the cycle
/// reason above; `SchedulerWireTransport` exists for the same kind of reason.
typedef std::function<std::shared_ptr<P2pBlobServer>()> P2pBlobServerFactory;

// ---- the transport ----------------------------------------------------------

/// Rust: `IrohBlobsP2pTransport`, over HTTP.
class HttpP2pTransport : public P2pTransport {
 public:
    struct Options {
        ResolvedP2pConfig config;
        std::string       node_id;

        /// What peers are told to dial, as `host:port`.
        ///
        /// Deliberately separate from `config.listen_addr`: a node normally
        /// binds a wildcard, and advertising `0.0.0.0:p` would hand every peer
        /// an address that resolves to itself. Rust gets the equivalent from
        /// `wait_for_dialable_addr`, which asks iroh what it ended up
        /// reachable on; without that machinery it has to be configured.
        std::string       advertise_addr;

        /// Shared secret both halves of the protocol require.
        ///
        /// Empty disables the check, which is only defensible when the port is
        /// unreachable from outside the cluster by other means.
        std::string       shared_token;

        /// The server to serve on. Required.
        std::shared_ptr<P2pBlobServer> server;
    };

    /// Builds the store and catalog, registers the routes, and starts serving.
    ///
    /// A failure here is returned rather than downgraded to a disabled
    /// transport, for the reason `node.cc` already states for iroh: a node
    /// explicitly configured for P2P must not come up silently without it.
    static core::Expected<std::shared_ptr<HttpP2pTransport>, P2pError>
        Start(Options options, std::shared_ptr<P2pPeerDiscovery> discovery);

    ~HttpP2pTransport();

    // ---- required ----

    P2pResult<bool> LookupWithHints(const P2pArtifactKey& key,
                                    const std::vector<P2pArtifactProviderHint>& hints,
                                    P2pArtifactDescriptor* out) override;
    P2pResult<uint64_t> FetchWithOptions(const P2pArtifactDescriptor& descriptor,
                                         const std::string& destination,
                                         P2pFetchOptions options) override;
    P2pResult<std::vector<uint8_t> >
        FetchBytesWithOptions(const P2pArtifactDescriptor& descriptor,
                              P2pFetchOptions options) override;
    P2pResult<std::shared_ptr<P2pByteStream> >
        FetchByteRange(const P2pArtifactDescriptor& descriptor,
                       uint64_t offset, size_t len) override;
    P2pResult<core::Unit> Publish(const P2pPublishRequest& request) override;
    P2pResult<bool> Unpublish(const P2pArtifactKey& key) override;

    // ---- provided, overridden ----

    bool LocalEndpoint(P2pEndpoint* out) const override;
    P2pResult<core::Unit> Shutdown() override;

    // ---- exposed for tests ----

    std::shared_ptr<PublishedArtifactCatalog> catalog() const;
    std::shared_ptr<HttpBlobStore> store() const;

    class Impl;

 private:
    explicit HttpP2pTransport(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

    std::shared_ptr<Impl> impl_;
};

/// Registers the three `agentenv-p2p/v1` routes.
///
/// The caller is expected to hand over a server of its own rather than share
/// the public API's: `api::Server::Use` layers run before routing for every
/// request, so the API's credential check would also gate this surface with
/// the wrong credentials, and `p2p.listen_addr` is a separate bind by
/// configuration, which is what lets the blob port stay off any public
/// interface.
void RegisterHttpP2pRoutes(P2pBlobServer* server,
                           std::shared_ptr<PublishedArtifactCatalog> catalog,
                           std::shared_ptr<HttpBlobStore> store,
                           const std::string& shared_token);

// ---- pieces exposed for testing --------------------------------------------

/// Parses a single-range `Range: bytes=a-b` header.
///
/// Only the closed form is accepted. An open-ended `bytes=a-` asks for "the
/// rest", whose size the client has not agreed to, and that is exactly what
/// `kMaxRangeBytes` exists to prevent.
bool ParseByteRangeHeader(const std::string& value, uint64_t* start, uint64_t* end);

/// Renders `Content-Range: bytes <start>-<end>/<total>`.
std::string FormatContentRange(uint64_t start, uint64_t end, uint64_t total);

/// Parses `bytes <start>-<end>/<total>`, or the unsatisfied form
/// `bytes */<total>`.
///
/// `*out_satisfied` distinguishes the two. The unsatisfied form is how a
/// client learns the size of an artifact it asked for the wrong part of --
/// including the size being zero, which is the only way an empty artifact can
/// be reported, since every range over it is unsatisfiable.
bool ParseContentRangeHeader(const std::string& value, bool* out_satisfied,
                             uint64_t* start, uint64_t* end, uint64_t* total);

/// Splits `host:port`. Rejects an empty host or an out-of-range port.
bool ParseHostPort(const std::string& address, std::string* host, uint16_t* port);

/// The locator a descriptor says to dial, which for this backend is the
/// content digest. Rust `blob_hash_from_descriptor`.
P2pResult<std::string> LocatorFromDescriptor(const P2pArtifactDescriptor& descriptor);

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_HTTP_TRANSPORT_H_
