// SPDX-License-Identifier: MIT
// Rust: src/snapshot/p2p.rs — bridges the snapshot manager to the p2p transport.
//
// Two kinds of artifact key live here:
//   * fixed keys  — `snapshot/v1/artifacts/<snapshot-id>/<name>`, for the
//     per-snapshot files (vm state, manifest, ...). They are addressed by
//     snapshot identity, so a re-publish of the same snapshot overwrites.
//   * layer keys  — content-addressed by digest, or namespaced by overlaybd
//     layer uuid. These are shared across snapshots, which is the whole point:
//     a peer that already has the layer can serve it regardless of which
//     snapshot asked for it.
#ifndef AGENTENV_SNAPSHOT_P2P_H_
#define AGENTENV_SNAPSHOT_P2P_H_

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/p2p/transport.h"
#include "agentenv/p2p/types.h"

namespace agentenv {
namespace snapshot {

/// Rust `SNAPSHOT_P2P_KEY_PREFIX`.
extern const char* const kSnapshotP2pKeyPrefix;

/// Rust `fixed_artifact_key`.
p2p::P2pArtifactKey FixedArtifactKey(const core::SnapshotId& snapshot_id,
                                     const std::string& name);

/// Rust `struct SnapshotP2pArtifact` — one publishable unit: a key, where the
/// bytes come from, and the metadata a consumer validates before trusting
/// them.
struct SnapshotP2pArtifact {
    p2p::P2pArtifactKey    key;
    p2p::P2pPublishSource  source;
    p2p::P2pPublishMode    publish_mode = p2p::P2pPublishMode::Copy;
    /// Serialised `LayerMetadata`, or empty for fixed artifacts (Rust's
    /// `serde_json::Value::Null`).
    std::string            metadata;

    /// Rust `SnapshotP2pArtifact::fixed` — a per-snapshot file on disk.
    static SnapshotP2pArtifact Fixed(const core::SnapshotId& snapshot_id,
                                     const std::string& name,
                                     const std::string& source_path);

    /// Rust `SnapshotP2pArtifact::bytes` — a per-snapshot in-memory blob.
    static SnapshotP2pArtifact Bytes(const core::SnapshotId& snapshot_id,
                                     const std::string& name,
                                     const std::vector<uint8_t>& source);

    /// Rust `content_addressed_overlaybd_layer`.
    static SnapshotP2pArtifact ContentAddressedOverlaybdLayer(
        const std::string& source_path, const std::string& sha256, uint64_t size);

    /// Rust `uuid_overlaybd_layer`.
    static SnapshotP2pArtifact UuidOverlaybdLayer(const std::string& source_path,
                                                  const std::string& uuid,
                                                  uint64_t size);

    /// Rust `local_overlaybd_layers`.
    ///
    /// Collects the local overlaybd layers an image config references into
    /// publishable artifacts.
    ///
    /// `committed_digests` holds every layer digest the committed record
    /// carries for this config's subject (memory, rootfs, or one attached
    /// drive). A local layer is published under its descriptor digest only
    /// when that digest is what the record references: publish-time
    /// compression recontainerizes raw local layers as zfile before upload, so
    /// the record names the compressed bytes and a raw-digest key would never
    /// be looked up.
    ///
    /// `committed_uuids` gates uuid-keyed publication. zfile layers record no
    /// uuid, so recontainerized layers drop out of it naturally.
    ///
    /// Never fails: an unreadable config or layer is logged and skipped,
    /// because P2P publication is an optional acceleration path.
    static std::vector<SnapshotP2pArtifact> LocalOverlaybdLayers(
        const std::string& image_config_path,
        const std::set<std::string>& committed_digests,
        const std::set<std::string>& committed_uuids);

    /// Rust `publish`.
    core::Expected<core::Unit, std::string>
        Publish(const std::shared_ptr<p2p::P2pTransport>& transport) const;
};

/// Rust `fetch_artifact` — returns the fetched byte count. A missing key is an
/// error, not an empty result: the caller asked for a specific artifact.
core::Expected<uint64_t, std::string>
    FetchArtifact(const std::shared_ptr<p2p::P2pTransport>& transport,
                  const p2p::P2pArtifactKey& key,
                  const std::string& destination);

/// Rust `fetch_artifact_bytes`.
core::Expected<std::vector<uint8_t>, std::string>
    FetchArtifactBytes(const std::shared_ptr<p2p::P2pTransport>& transport,
                       const p2p::P2pArtifactKey& key);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_P2P_H_
