// SPDX-License-Identifier: MIT
// Rust: src/snapshot/types/  and repository/  — snapshot metadata + storage.
#ifndef AGENTENV_SNAPSHOT_TYPES_H_
#define AGENTENV_SNAPSHOT_TYPES_H_

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace snapshot {

/// Metadata describing a snapshot bundle on disk.
struct SnapshotMeta {
    core::SnapshotId id;
    std::string      template_id;
    core::Digest     mem_digest;
    core::Digest     disk_digest;
    int64_t          mem_size = 0;
    int64_t          disk_size = 0;
    int64_t          created_at_ms = 0;
    std::string      guest_arch;   // "x86_64" | "aarch64"
};

/// Rust newtype `SnapshotAlias(String)` (src/snapshot/types/value.rs).
///
/// Only constructible through `Parse`, so an invalid alias cannot exist — the
/// same guarantee the Rust newtype gives by keeping its field `pub(crate)`.
class SnapshotAlias {
 public:
    /// Rust `SnapshotAlias::parse`. ASCII alphanumerics, `-` and `_` only.
    static core::Expected<SnapshotAlias, std::string> Parse(const std::string& input);

    /// Rust `impl Display`.
    const std::string& ToString() const { return value_; }

    bool operator==(const SnapshotAlias& o) const { return value_ == o.value_; }
    bool operator!=(const SnapshotAlias& o) const { return !(*this == o); }
    bool operator<(const SnapshotAlias& o) const { return value_ < o.value_; }

 private:
    explicit SnapshotAlias(std::string value) : value_(value) {}
    std::string value_;
};

std::ostream& operator<<(std::ostream& os, const SnapshotAlias& alias);

/// Rust struct `CommandContext` — the OCI-derived execution environment that
/// build steps mutate and the snapshot records.
struct CommandContext {
    std::map<std::string, std::string> env_vars;
    /// Rust `Default` sets this to "/", not the empty string.
    std::string workdir = "/";
    core::Optional<std::string> user;
    std::vector<std::string> exposed_ports;
    /// `Option<Vec<String>>`: absent and empty are distinct for OCI, since an
    /// explicitly empty entrypoint overrides the base image's.
    core::Optional<std::vector<std::string> > entrypoint;
    core::Optional<std::vector<std::string> > cmd;
    std::vector<std::string> volumes;
    std::map<std::string, std::string> labels;

    bool operator==(const CommandContext& o) const;
    bool operator!=(const CommandContext& o) const { return !(*this == o); }
};

/// Rust struct `TemplateBuildErrorReason` — a build failure message plus the
/// optional client-visible step that produced it.
struct TemplateBuildErrorReason {
    std::string message;
    core::Optional<std::string> step;

    /// Rust `TemplateBuildErrorReason::new`.
    static TemplateBuildErrorReason New(const std::string& message);
    /// Rust `TemplateBuildErrorReason::with_step`.
    static TemplateBuildErrorReason WithStep(const std::string& message, const std::string& step);

    /// Rust `impl Display` — the message only; the step is carried separately
    /// so the API can surface it as its own field.
    const std::string& ToString() const { return message; }

    bool operator==(const TemplateBuildErrorReason& o) const;
    bool operator!=(const TemplateBuildErrorReason& o) const { return !(*this == o); }
};

std::ostream& operator<<(std::ostream& os, const TemplateBuildErrorReason& reason);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_TYPES_H_
