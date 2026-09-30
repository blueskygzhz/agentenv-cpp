// SPDX-License-Identifier: MIT
// Rust: src/p2p/iroh/catalog.rs — the published-artifact catalog and the
// catalog request/response wire format.
//
// Scope. `catalog.rs` has two halves: the catalog itself (a KV-persisted map
// from artifact key to descriptor) and `CatalogProtocol`, the iroh
// `ProtocolHandler` that serves lookups over a QUIC bi-stream. Only the first
// half is portable — the handler needs `iroh::endpoint::Connection`. The
// request/response types and size limits live here too, so a future transport
// (iroh or otherwise) serves the identical wire format.
//
// Why the catalog is persisted. A publish is a promise that this node can
// serve those bytes. If the in-memory map were the only record, a restart
// would silently retract every promise while peers still hold descriptors
// pointing here. The KV store makes the promise survive the process.
#ifndef AGENTENV_P2P_CATALOG_H_
#define AGENTENV_P2P_CATALOG_H_

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/local_store.h"
#include "agentenv/p2p/error.h"
#include "agentenv/p2p/types.h"

namespace agentenv {
namespace p2p {

/// Rust `CATALOG_ALPN` — the ALPN a transport must offer for this protocol.
extern const char* const kCatalogAlpn;
/// Rust `MAX_CATALOG_RESPONSE_BYTES`.
extern const std::size_t kMaxCatalogResponseBytes;
/// Rust `MAX_CATALOG_REQUEST_BYTES`.
extern const std::size_t kMaxCatalogRequestBytes;
/// Rust `CONNECTION_CLOSE_TIMEOUT`, in milliseconds. Bounds how long a slow
/// peer can keep a handler task alive after the response is sent.
extern const int64_t kCatalogConnectionCloseTimeoutMs;

// ---- descriptor encoding ----------------------------------------------------
//
// Rust gets these from `#[derive(Serialize, Deserialize)]`; the C++ port needs
// them spelled out because the catalog persists descriptors as JSON.

core::Json EndpointToJson(const P2pEndpoint& endpoint);
core::Expected<P2pEndpoint, std::string> EndpointFromJson(const core::Json& json);

core::Json PeerToJson(const P2pPeer& peer);
core::Expected<P2pPeer, std::string> PeerFromJson(const core::Json& json);

/// Rust `P2pArtifactProvider` is an externally-tagged enum: `"local"` for the
/// unit variant, `{"peer": {..}}` for the peer variant.
core::Json ProviderToJson(const P2pArtifactProvider& provider);
core::Expected<P2pArtifactProvider, std::string> ProviderFromJson(const core::Json& json);

core::Json DescriptorToJson(const P2pArtifactDescriptor& descriptor);
core::Expected<P2pArtifactDescriptor, std::string>
    DescriptorFromJson(const core::Json& json);

// ---- wire format ------------------------------------------------------------

/// Rust `struct CatalogRequest`.
struct CatalogRequest {
    P2pArtifactKey key;

    std::string Encode() const;
    /// Rejects a payload over `kMaxCatalogRequestBytes` before parsing it, so a
    /// peer cannot force an unbounded allocation.
    static core::Expected<CatalogRequest, std::string> Decode(const std::string& bytes);
};

/// Rust `struct CatalogResponse` — an absent descriptor is a miss, not an error.
struct CatalogResponse {
    core::Optional<P2pArtifactDescriptor> descriptor;

    std::string Encode() const;
    static core::Expected<CatalogResponse, std::string> Decode(const std::string& bytes);
};

// ---- catalog ----------------------------------------------------------------

/// Rust `struct PublishedArtifactCatalog`.
///
/// Rust guards the map with a `tokio::sync::RwLock`; without a reactor a plain
/// mutex is equivalent, since every critical section is a map operation.
class PublishedArtifactCatalog {
 public:
    /// Rust `load` — opens the store and replays every persisted descriptor
    /// into the in-memory index. A corrupt entry fails the load rather than
    /// being skipped: serving a partial catalog would silently drop promises
    /// this node already made.
    static core::Expected<std::shared_ptr<PublishedArtifactCatalog>, P2pError>
        Load(const std::string& db_path, const std::string& node_id,
             const P2pEndpoint& local_endpoint);

    /// Rust `descriptor_for`.
    core::Optional<P2pArtifactDescriptor> DescriptorFor(const P2pArtifactKey& key) const;

    /// Rust `upsert` — persists first, then updates the index, so a failed
    /// write never leaves the index claiming something the store lost.
    core::Expected<core::Unit, P2pError> Upsert(const P2pArtifactDescriptor& descriptor);

    /// Rust `remove` — returns the descriptor that was dropped, if any.
    core::Expected<core::Optional<P2pArtifactDescriptor>, P2pError>
        Remove(const P2pArtifactKey& key);

    /// Rust `CatalogProtocol::descriptor_for_response` — the served copy always
    /// advertises *this* node as the sole provider, because a requesting peer
    /// is asking what we can serve, not what we have heard about.
    core::Optional<P2pArtifactDescriptor>
        DescriptorForResponse(const P2pArtifactKey& key) const;

    const P2pPeer& local_provider() const { return local_provider_; }
    std::size_t size() const;

 private:
    PublishedArtifactCatalog(std::shared_ptr<local_store::KvStore> store, P2pPeer provider)
        : store_(std::move(store)), local_provider_(std::move(provider)) {}

    std::shared_ptr<local_store::KvStore>            store_;
    P2pPeer                                          local_provider_;
    mutable std::mutex                               mutex_;
    std::map<P2pArtifactKey, P2pArtifactDescriptor>  entries_;
};

/// Rust `impl P2pEndpoint { to_iroh_addr }` (iroh/endpoint.rs) — the backend
/// check, which is the part that does not need the iroh address type. The
/// opaque `address` payload is returned for the caller's backend to parse.
core::Expected<std::string, P2pError>
    EndpointAddressForBackend(const P2pEndpoint& endpoint, const std::string& backend);

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_CATALOG_H_
