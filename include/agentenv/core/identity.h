// SPDX-License-Identifier: MIT
// Rust: identity primitives (SandboxId, TemplateId, Version...).
// C++11: strong-typed uuid-shaped id + free-standing generator.
#ifndef AGENTENV_CORE_IDENTITY_H_
#define AGENTENV_CORE_IDENTITY_H_

#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace core {

/// A 128-bit UUID v7 (time-ordered), matching Rust's `uuid::Uuid` usage.
///
/// The layout follows RFC 9562 §5.7: 48 bits ms unix time + 4 bits ver + 12
/// bits rand_a + 2 bits var + 62 bits rand_b.
class Uuid {
 public:
    Uuid() { std::memset(bytes_, 0, sizeof(bytes_)); }
    explicit Uuid(const uint8_t bytes[16]) { std::memcpy(bytes_, bytes, 16); }

    /// Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
    static bool Parse(const std::string& s, Uuid* out);

    /// Format as canonical dashed hex.
    std::string ToString() const;

    /// Generate a fresh UUID v7 seeded by the current clock + a rand stream.
    static Uuid GenV7();

    /// The all-zero (nil) UUID. Mirrors Rust `Uuid::nil()`.
    static Uuid Nil() { return Uuid(); }
    bool is_nil() const {
        for (int i = 0; i < 16; ++i) { if (bytes_[i] != 0) return false; }
        return true;
    }

    bool operator==(const Uuid& o) const { return std::memcmp(bytes_, o.bytes_, 16) == 0; }
    bool operator!=(const Uuid& o) const { return !(*this == o); }
    bool operator<(const Uuid& o) const  { return std::memcmp(bytes_, o.bytes_, 16) < 0; }

    const uint8_t* bytes() const { return bytes_; }

 private:
    uint8_t bytes_[16];
};

std::ostream& operator<<(std::ostream& os, const Uuid& u);

/// Strong-typed wrapper: only the same tag is comparable. Rust newtype pattern.
template <typename Tag>
class TaggedUuid {
 public:
    TaggedUuid() = default;
    explicit TaggedUuid(Uuid u) : inner_(u) {}
    const Uuid& inner() const { return inner_; }
    static TaggedUuid Fresh() { return TaggedUuid(Uuid::GenV7()); }
    /// Rust `SnapshotId::parse`. Fails rather than yielding a nil id, because
    /// callers use the failure to decide a string is an alias instead.
    static Expected<TaggedUuid, std::string> Parse(const std::string& text) {
        Uuid parsed;
        if (!Uuid::Parse(text, &parsed)) {
            return make_unexpected(std::string("invalid id '") + text + "'");
        }
        return TaggedUuid(parsed);
    }
    bool operator==(const TaggedUuid& o) const { return inner_ == o.inner_; }
    bool operator!=(const TaggedUuid& o) const { return !(*this == o); }
    bool operator<(const TaggedUuid& o) const  { return inner_ < o.inner_; }
    std::string ToString() const { return inner_.ToString(); }
 private:
    Uuid inner_;
};

// Tags for common identity types (Rust file: src/identity.rs).
struct SandboxIdTag  {};
struct TemplateIdTag {};
struct SnapshotIdTag {};

using SandboxId  = TaggedUuid<SandboxIdTag>;
using TemplateId = TaggedUuid<TemplateIdTag>;
using SnapshotId = TaggedUuid<SnapshotIdTag>;

/// Config inputs for node identity resolution.
///
/// Rust: `crate::cfg::NodeIdentityConfig` (src/cfg.rs). Each field maps to an
/// optional TOML/env override (`AENV_NODE_ID` / `AENV_CLUSTER_ID` /
/// `AENV_SERVICE_INSTANCE_ID`); env resolution itself lives in the config
/// layer, so here we only take the already-resolved optionals.
struct NodeIdentityConfig {
    Optional<std::string> node_id;
    Optional<std::string> cluster_id;
    Optional<std::string> service_instance_id;
};

/// Stable node identity fields used by the node/admin APIs.
///
/// Rust: `crate::identity::NodeIdentity` (src/identity.rs). Values come from
/// environment/config overrides when present, otherwise from hostname- or
/// process-derived fallbacks. The build commit is injected at compile time
/// (`AENV_GIT_COMMIT`) rather than read from runtime configuration.
struct NodeIdentity {
    std::string id;
    Uuid        cluster_id;
    std::string service_instance_id;
    std::string commit;
    std::string version;

    /// Resolves the stable node identity from config + host fallbacks.
    /// Mirrors `NodeIdentity::from_config`.
    static NodeIdentity FromConfig(const NodeIdentityConfig& config);
};

/// Build commit injected at compile time; "unknown" when unset.
/// Mirrors Rust `build_commit()` (uses `option_env!("AENV_GIT_COMMIT")`).
const char* BuildCommit();

/// Reads the host name via `$HOSTNAME`, then `/proc/sys/kernel/hostname`,
/// then `/etc/hostname`. Mirrors Rust `read_hostname()`.
Optional<std::string> ReadHostname();

}  // namespace core
}  // namespace agentenv

namespace std {
template <> struct hash<::agentenv::core::Uuid> {
    size_t operator()(const ::agentenv::core::Uuid& u) const noexcept {
        // Fowler-Noll-Vo 1a, since we can't rely on <bit>.
        size_t h = 1469598103934665603ull;
        const uint8_t* p = u.bytes();
        for (int i = 0; i < 16; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }
};
}  // namespace std
#endif  // AGENTENV_CORE_IDENTITY_H_
