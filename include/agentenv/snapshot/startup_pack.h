// SPDX-License-Identifier: MIT
// Rust: src/snapshot/startup_pack.rs — the two descriptor types only.
//
// These live in a library of their own because `sandbox::SandboxSnapshotManifest`
// carries a `ResolvedStartupPack` while `agentenv-snapshot` already depends on
// `agentenv-sandbox`; Rust has one crate and no such constraint. The recording
// and upload machinery stays in the snapshot library.
#ifndef AGENTENV_SNAPSHOT_STARTUP_PACK_H_
#define AGENTENV_SNAPSHOT_STARTUP_PACK_H_

#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace snapshot {

/// Rust `struct MemoryStartupPackInfo`.
///
/// Persisted in a committed record only when the pack was both recorded and
/// uploaded, so its absence is the normal case for older and POSIX-backend
/// snapshots rather than an error.
struct MemoryStartupPackInfo {
    uint64_t pack_size = 0;
    uint64_t mem_virtual_size = 0;
    /// Hex sha256 of the pack's index section. The consumer verifies the
    /// received index against this before trusting any entry in it, which is
    /// what keeps a tampered pack from injecting guest memory.
    std::string index_sha256;

    core::Json ToJson() const;
    static core::Expected<MemoryStartupPackInfo, std::string> FromJson(const core::Json& json);

    bool operator==(const MemoryStartupPackInfo& o) const {
        return pack_size == o.pack_size && mem_virtual_size == o.mem_virtual_size &&
               index_sha256 == o.index_sha256;
    }
    bool operator!=(const MemoryStartupPackInfo& o) const { return !(*this == o); }
};

/// Rust `struct ResolvedStartupPack`.
///
/// Runtime-only: everything the ublk daemon needs to register the pack's
/// prefetch. Carrying `index_sha256` along with the URL is deliberate — the
/// reference is never trusted on its own.
struct ResolvedStartupPack {
    std::string url;
    uint64_t pack_size = 0;
    std::string index_sha256;
    uint64_t mem_virtual_size = 0;

    core::Json ToJson() const;
    static core::Expected<ResolvedStartupPack, std::string> FromJson(const core::Json& json);

    bool operator==(const ResolvedStartupPack& o) const {
        return url == o.url && pack_size == o.pack_size && index_sha256 == o.index_sha256 &&
               mem_virtual_size == o.mem_virtual_size;
    }
    bool operator!=(const ResolvedStartupPack& o) const { return !(*this == o); }
};

/// Rust `resolve_startup_pack_ref`.
///
/// The URL is passed as an already-built string rather than Rust's
/// `FnOnce() -> String`; the closure exists there only to avoid formatting a
/// URL that the disabled/absent paths would discard.
core::Optional<ResolvedStartupPack> ResolveStartupPackRef(
    const core::Optional<MemoryStartupPackInfo>& info, bool consume_enabled,
    const std::string& url);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_STARTUP_PACK_H_
