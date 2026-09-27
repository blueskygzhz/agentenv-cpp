// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/{manifest,mmds,overlaybd_snapshot}.rs.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_MANIFEST_H_
#define AGENTENV_SANDBOX_FIRECRACKER_MANIFEST_H_

#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/identity.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust: firecracker/manifest.rs :: SnapshotManifest.
struct SnapshotManifest {
    std::string vm_state_path;
    std::string mem_state_path;
    std::string layout_version;
};

/// Rust: firecracker/mmds.rs :: MmdsData (opaque metadata JSON).
struct MmdsData {
    std::string json_bytes;
};

/// Rust: firecracker/mmds.rs :: MmdsMetadata — the per-VM MMDS document with the
/// e2b-compatible field names. Serializes to a flat JSON object where `extra`
/// key/value pairs are flattened into the top level next to the core fields.
class MmdsMetadata {
 public:
    /// Rust `MmdsMetadata::new(sandbox_id, snapshot_id)`.
    MmdsMetadata(const core::SandboxId& sandbox_id, const std::string& snapshot_id);

    /// Rust `with_extra` — merge/extend extra top-level key/value pairs.
    /// `json_value` must be a valid JSON fragment (caller quotes strings).
    MmdsMetadata& WithExtra(const std::string& key, const std::string& json_value);

    /// Rust `Serialize` — the flat JSON object with e2b field names:
 ///   instanceID / envID / address / accessTokenHash + flattened extras.
    std::string ToJson() const;

    const std::string& SandboxId() const { return sandbox_id_; }
    const std::string& SnapshotId() const { return snapshot_id_; }
    const std::string& AccessTokenHash() const { return access_token_hash_; }

    /// Rust `hash_access_token` — lowercase hex SHA-512 of the token.
  static std::string HashAccessToken(const std::string& token);

 private:
    std::string sandbox_id_;      // "instanceID"
    std::string snapshot_id_;          // "envID"
    std::string logs_collector_address_;  // "address"
    std::string access_token_hash_;       // "accessTokenHash"
    std::vector<std::pair<std::string, std::string> > extra_;  // flattened
};

/// Rust: firecracker/overlaybd_snapshot.rs :: OverlaybdSnapshotSpec.
struct OverlaybdSnapshotSpec {
    std::string source_path;
    std::string dest_path;
    bool        preserve_metadata = true;
};

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_MANIFEST_H_
