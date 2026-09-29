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
#include "agentenv/core/json.h"
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

    /// Rust `CommandContext::new` — normalises the workdir, so a blank value
    /// from an image config becomes `/` rather than an empty `cd` target.
    static CommandContext New(const std::map<std::string, std::string>& env_vars,
                              const std::string& workdir);

    /// Rust `CommandContext::from_env_and_workdir`.
    static CommandContext FromEnvAndWorkdir(
        const std::map<std::string, std::string>& env_vars,
        const core::Optional<std::string>& workdir);

    // Rust's `with_*` builders consume and return `self`; these mutate in
    // place and return a reference so a chain reads the same way without a
    // copy per step.
    CommandContext& WithEnvVar(const std::string& key, const std::string& value);
    CommandContext& WithEnvOverrides(const std::map<std::string, std::string>& overrides);
    CommandContext& WithWorkdir(const std::string& workdir);
    CommandContext& WithUser(const core::Optional<std::string>& user);
    CommandContext& WithExposedPorts(const std::vector<std::string>& ports);
    CommandContext& WithEntrypoint(const core::Optional<std::vector<std::string> >& entrypoint);
    CommandContext& WithCmd(const core::Optional<std::vector<std::string> >& cmd);
    CommandContext& WithVolumes(const std::vector<std::string>& volumes);
    CommandContext& WithLabels(const std::map<std::string, std::string>& labels);

    /// Rust `CommandContext::effective_start_cmd`.
    ///
    /// Entrypoint and cmd concatenated and shell-quoted, suitable for
    /// `bash -lc`. Absent when both are empty, which is how the caller tells
    /// "no start command" from "an empty one".
    core::Optional<std::string> EffectiveStartCmd() const;

    core::Json ToJson() const;
    static core::Expected<CommandContext, std::string> FromJson(const core::Json& json);

    bool operator==(const CommandContext& o) const;
    bool operator!=(const CommandContext& o) const { return !(*this == o); }
};

/// Rust `normalize_workdir`.
std::string NormalizeWorkdir(const std::string& workdir);

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

    core::Json ToJson() const;

    /// Rust's hand-written `Deserialize` accepts two shapes: the structured
    /// object, and a bare string from records written before `step` existed.
    /// Dropping the legacy form would make those records unreadable.
    static core::Expected<TemplateBuildErrorReason, std::string> FromJson(
        const core::Json& json);

    bool operator==(const TemplateBuildErrorReason& o) const;
    bool operator!=(const TemplateBuildErrorReason& o) const { return !(*this == o); }
};

std::ostream& operator<<(std::ostream& os, const TemplateBuildErrorReason& reason);

/// Rust `struct SnapshotArtifactLayoutSpec` (src/snapshot/types/artifacts.rs).
///
/// The fixed file names inside a snapshot directory. Every backend derives its
/// paths from this one table so the on-disk shape cannot diverge between the
/// writer and the resolver — these names are effectively an on-disk ABI for
/// already-published snapshots.
struct SnapshotArtifactLayoutSpec {
    const char* firecracker_manifest;
    const char* vm_state;
    const char* memory_dump;
    const char* memory_image_config;
    const char* rootfs_dir;
    const char* drives_dir;
    const char* drive_layers_dir;
    const char* rootfs_image_config;
    const char* overlaybd_image_config_file;
};

/// Rust `pub const SNAPSHOT_ARTIFACT_LAYOUT`.
extern const SnapshotArtifactLayoutSpec kSnapshotArtifactLayout;

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_TYPES_H_
