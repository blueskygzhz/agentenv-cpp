// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/mmds.rs — the per-VM MMDS document.
//
// MMDS is the guest-readable metadata service, so this document is a trust
// boundary: the core fields carry the sandbox's runtime identity and the hash
// the guest authenticates against, while `extra` is opaque pass-through the API
// layer fills in without the sandbox layer interpreting it.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_MMDS_H_
#define AGENTENV_SANDBOX_FIRECRACKER_MMDS_H_

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/identity.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust `RESERVED_FIELDS` — the MMDS keys that carry runtime identity and auth.
extern const char* const kMmdsReservedFields[4];

/// Rust `RESERVED_FIELDS.contains(&key)`.
bool IsReservedMmdsField(const std::string& key);

/// Rust `struct MmdsMetadata`. Serializes to a flat JSON object with the
/// e2b-compatible field names, `extra` pairs flattened next to the core fields
/// (Rust's `#[serde(flatten)]`).
class MmdsMetadata {
 public:
    /// Rust `MmdsMetadata::new(sandbox_id, snapshot_id)`.
    ///
    /// The access token hash is seeded with the hash of the *empty* token
    /// rather than an empty string, so the field is always a well-formed
    /// digest and an unset token is not mistakable for a missing one.
    MmdsMetadata(const core::SandboxId& sandbox_id, const std::string& snapshot_id);

    /// Rust `with_extra` — merge an opaque top-level key/value pair.
    ///
    /// `json_value` must be a valid JSON fragment (the caller quotes strings).
    /// Reserved keys are **dropped**, matching Rust's `extra.retain(...)`: this
    /// is what stops API-supplied extras from overriding the sandbox's identity
    /// or its access token hash.
    MmdsMetadata& WithExtra(const std::string& key, const std::string& json_value);

    /// Rust `set_access_token` — stores only the digest, never the token.
    void SetAccessToken(const std::string& token);

    /// Rust `Serialize` — `instanceID` / `envID` / `address` /
    /// `accessTokenHash`, plus flattened extras.
    std::string ToJson() const;

    const std::string& SandboxId() const { return sandbox_id_; }
    const std::string& SnapshotId() const { return snapshot_id_; }
    const std::string& AccessTokenHash() const { return access_token_hash_; }
    std::size_t ExtraCount() const { return extra_.size(); }

    /// Rust `hash_access_token` — lowercase hex SHA-512.
    static std::string HashAccessToken(const std::string& token);

 private:
    std::string sandbox_id_;              // "instanceID"
    std::string snapshot_id_;             // "envID"
    std::string logs_collector_address_;  // "address"
    std::string access_token_hash_;       // "accessTokenHash"
    std::vector<std::pair<std::string, std::string> > extra_;  // flattened
};

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_MMDS_H_
