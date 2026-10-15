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

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/access.h"

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
///
/// The document is built as a `core::Json` value rather than by pasting
/// strings together: extras are API-supplied, and a value spliced in as raw
/// text could close the object early and smuggle in a second `accessTokenHash`
/// — the exact override the reserved-key filter exists to stop.
class MmdsMetadata {
 public:
    /// Rust `MmdsMetadata::new(sandbox_id, snapshot_id)`.
    ///
    /// The access token hash is seeded with the hash of the *empty* token
    /// rather than an empty string, so the field is always a well-formed
    /// digest and an unset token is not mistakable for a missing one.
    MmdsMetadata(const core::SandboxId& sandbox_id, const std::string& snapshot_id);

    /// Rust `with_extra(Map)` — merge opaque top-level pairs.
    ///
    /// Reserved keys are **dropped** (`extra.retain(...)`), so API-supplied
    /// extras cannot override the sandbox's identity or its access token
    /// hash. A key already present is *replaced*, as `Map::extend` does —
    /// one key, one value, so the guest never sees a duplicate.
    MmdsMetadata& WithExtra(const core::JsonObject& extra);

    /// Rust `with_access_token`.
    MmdsMetadata& WithAccessToken(const core::Optional<EnvdAccessToken>& token);

    /// Rust `set_access_token` — stores only the digest, never the token. An
    /// absent token hashes the empty string, as upstream.
    void SetAccessToken(const core::Optional<EnvdAccessToken>& token);

    /// Rust `metadata.sandbox_id = id.to_string()` on the resume path.
    ///
    /// A resumed sandbox gets a new id while the document it inherits still
    /// names the one that was captured. The guest reads its own identity from
    /// here, so leaving the old value makes it report as the wrong sandbox —
    /// and the logs collector then files its output under that id.
    void SetSandboxId(const core::SandboxId& sandbox_id);

    /// Rust `Serialize`.
    core::Json ToValue() const;
    /// `ToValue().ToString()` — the body sent to `PUT /mmds`.
    std::string ToJson() const;

    /// Rust `Deserialize`. The four core fields are required; every other
    /// key is collected into `extra` (`#[serde(flatten)]`), so a document
    /// written without extras (the legacy shape) reads back with none.
    static core::Expected<MmdsMetadata, std::string> FromValue(const core::Json& value);

    const std::string& SandboxId() const { return sandbox_id_; }
    const std::string& SnapshotId() const { return snapshot_id_; }
    const std::string& LogsCollectorAddress() const { return logs_collector_address_; }
    const std::string& AccessTokenHash() const { return access_token_hash_; }
    const core::JsonObject& Extra() const { return extra_; }
    std::size_t ExtraCount() const { return extra_.size(); }

    /// Rust `#[derive(PartialEq)]`.
    bool operator==(const MmdsMetadata& o) const;
    bool operator!=(const MmdsMetadata& o) const { return !(*this == o); }

    /// Rust `hash_access_token` — lowercase hex SHA-512.
    static std::string HashAccessToken(const std::string& token);

 private:
    MmdsMetadata() {}

    std::string sandbox_id_;              // "instanceID"
    std::string snapshot_id_;             // "envID"
    std::string logs_collector_address_;  // "address"
    std::string access_token_hash_;       // "accessTokenHash"
    core::JsonObject extra_;              // flattened
};

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_MMDS_H_
