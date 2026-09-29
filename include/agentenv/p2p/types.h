// SPDX-License-Identifier: MIT
// Rust: src/p2p/types.rs — the P2P catalog value types.
#ifndef AGENTENV_P2P_TYPES_H_
#define AGENTENV_P2P_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

namespace agentenv {
namespace p2p {

/// Rust: `type P2pArtifactKey = String`.
typedef std::string P2pArtifactKey;

/// Rust: `P2pEndpoint` — backend + backend-specific serialized address.
struct P2pEndpoint {
    std::string backend;   // e.g. "iroh" | "mock"
    std::string address;

    bool operator==(const P2pEndpoint& o) const {
        return backend == o.backend && address == o.address;
 }
};

/// Rust: `P2pPeer`.
struct P2pPeer {
    std::string node_id;
    P2pEndpoint endpoint;

bool operator==(const P2pPeer& o) const {
return node_id == o.node_id && endpoint == o.endpoint;
    }
};

/// Rust: `P2pArtifactProvider` enum { Local, Peer(P2pPeer) }.
struct P2pArtifactProvider {
    enum Kind { Local, Peer };
    Kind        kind;
    P2pPeer   peer;   // valid when kind == Peer

    P2pArtifactProvider() : kind(Local) {}
    static P2pArtifactProvider MakeLocal() {
   P2pArtifactProvider p; p.kind = Local; return p;
    }
    static P2pArtifactProvider FromPeer(P2pPeer peer) {
        P2pArtifactProvider p; p.kind = Peer; p.peer = std::move(peer); return p;
    }
    bool IsLocal() const { return kind == Local; }
};

/// Rust: `P2pArtifactDescriptor`.
struct P2pArtifactDescriptor {
  P2pArtifactKey        key;
    std::vector<P2pArtifactProvider> providers;
    // Option<String>: has_backend_locator == false means None.
    bool    has_backend_locator = false;
    std::string              backend_locator;
    // serde_json::Value metadata, kept as a raw JSON string ("null" default).
    std::string                metadata_json = "null";
};

/// Rust: `P2pPublishMode` { Copy (default), Reference }.
enum class P2pPublishMode { Copy, Reference };

/// Rust: `P2pPublishSource` enum { Path(PathBuf), Bytes(Bytes) }.
struct P2pPublishSource {
    enum Kind { Path, Bytes };
    Kind     kind;
    std::string path;   // valid when kind == Path
    std::vector<uint8_t> bytes;       // valid when kind == Bytes

    P2pPublishSource() : kind(Path) {}
    static P2pPublishSource FromPath(std::string p) {
        P2pPublishSource s; s.kind = Path; s.path = std::move(p); return s;
    }
    static P2pPublishSource FromBytes(std::vector<uint8_t> b) {
        P2pPublishSource s; s.kind = Bytes; s.bytes = std::move(b); return s;
    }
    /// Rust `Display`.
  std::string ToString() const;
};

/// Rust: `P2pPublishRequest` (+ builder helpers file/bytes/with_*).
struct P2pPublishRequest {
    P2pArtifactKey   key;
    P2pPublishSource source;
    std::string      metadata_json = "null";
    P2pPublishMode   publish_mode = P2pPublishMode::Copy;

    /// Rust `P2pPublishRequest::file(key, path)` — default mode Copy.
    static P2pPublishRequest File(P2pArtifactKey key, std::string path);
    /// Rust `P2pPublishRequest::bytes(key, bytes)` — mode Copy.
    static P2pPublishRequest FromBytes(P2pArtifactKey key, std::vector<uint8_t> bytes);

    P2pPublishRequest& WithMetadata(std::string metadata_json_value);
    P2pPublishRequest& WithPublishMode(P2pPublishMode mode);
};

/// Rust: `P2pArtifactProviderHint` (both fields optional).
struct P2pArtifactProviderHint {
    bool        has_node_id = false;
    std::string node_id;
    bool        has_endpoint = false;
    P2pEndpoint endpoint;
};

/// Rust: `P2pFetchOptions`.
///
/// `impl Default` sets `advertise: true` — note this is NOT `#[derive(Default)]`,
/// so the default is deliberately `true`, not `false`.
struct P2pFetchOptions {
    /// The transport backend should automatically advertise the fetched artifact.
    bool advertise = true;

    /// Rust `P2pFetchOptions::default()`.
    static P2pFetchOptions Default() { return P2pFetchOptions(); }

    bool operator==(const P2pFetchOptions& o) const { return advertise == o.advertise; }
    bool operator!=(const P2pFetchOptions& o) const { return !(*this == o); }
};

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_TYPES_H_
