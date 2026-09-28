// SPDX-License-Identifier: MIT
// Rust: src/snapshot/types/snapshot.rs
//
// The durable snapshot record and the payload it carries once committed.
// A record exists from the moment a template build is queued, so `committed`
// is optional: an uncommitted record is a build in flight, not a broken one.
#ifndef AGENTENV_SNAPSHOT_RECORD_H_
#define AGENTENV_SNAPSHOT_RECORD_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/virtualization.h"
#include "agentenv/sandbox/types.h"
#include "agentenv/snapshot/drive.h"
#include "agentenv/snapshot/layers.h"
#include "agentenv/snapshot/startup_pack.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/snapshot/version.h"
#include "agentenv/template/build_spec.h"
#include "agentenv/volume/record.h"

namespace agentenv {
namespace snapshot {

/// Rust `SNAPSHOT_IMAGE_TAG_PREFIX`.
extern const char* const kSnapshotImageTagPrefix;

/// Rust `rootfs_snapshot_image_tag`.
std::string RootfsSnapshotImageTag(const core::SnapshotId& snapshot_id);

/// Rust `now_unix_ms` — 0 rather than an error when the clock predates the
/// epoch, matching the Rust `unwrap_or(0)`.
int64_t NowUnixMs();

/// Rust struct `SnapshotVolume`.
struct SnapshotVolume {
    std::string mount_path;
    /// Rust `#[serde(default)]` → `VolumeMode::Exclusive`.
    volume::VolumeMode mode = volume::VolumeMode::Exclusive;
    uint64_t size_mb = 0;
    std::vector<OverlaybdLayerRef> layers;

    core::Json ToJson() const;
    static core::Expected<SnapshotVolume, std::string> FromJson(const core::Json& json);

    bool operator==(const SnapshotVolume& o) const;
    bool operator!=(const SnapshotVolume& o) const { return !(*this == o); }
};

/// Rust struct `StartupCommand`.
struct StartupCommand {
    std::string start_cmd;
    std::string ready_cmd;
    CommandContext context;
    /// Rust `#[serde(default, skip_serializing_if = "Option::is_none")]` —
    /// absent for legacy templates, which get a Bash login shell.
    core::Optional<std::string> shell;

    /// Rust `StartupCommand::shell_command` — `(program, flag)`.
    void ShellCommand(std::string* program, std::string* flag) const;

    core::Json ToJson() const;
    static core::Expected<StartupCommand, std::string> FromJson(const core::Json& json);

    bool operator==(const StartupCommand& o) const;
    bool operator!=(const StartupCommand& o) const { return !(*this == o); }
};

/// Rust struct `PersistedDiskImagePublication`.
struct PersistedDiskImagePublication {
    std::string image_ref;
    std::string tag;
    std::string manifest_digest;
    std::string repo_blob_url;

    core::Json ToJson() const;
    static core::Expected<PersistedDiskImagePublication, std::string> FromJson(
        const core::Json& json);

    bool operator==(const PersistedDiskImagePublication& o) const;
    bool operator!=(const PersistedDiskImagePublication& o) const { return !(*this == o); }
};

/// Rust enum `TemplateBuildStatus`.
///
/// Note `Ready`, not `Committed`: the repository-boundary copy of this enum
/// in `repository/interfaces.h` uses a different spelling, and the two must
/// not be confused when a record is written.
enum class TemplateBuildStatus {
    Waiting,
    Building,
    Ready,
    Error,
};

const char* TemplateBuildStatusToString(TemplateBuildStatus status);
core::Optional<TemplateBuildStatus> TemplateBuildStatusParse(const std::string& raw);

/// Rust struct `TemplateBuildInfo`.
struct TemplateBuildInfo {
    TemplateBuildStatus status = TemplateBuildStatus::Waiting;
    core::Optional<int64_t> started_at_unix_ms;
    core::Optional<int64_t> finished_at_unix_ms;
    core::Optional<TemplateBuildErrorReason> error_reason;

    /// Rust `TemplateBuildInfo::waiting`.
    static TemplateBuildInfo Waiting();

    core::Json ToJson() const;
    static core::Expected<TemplateBuildInfo, std::string> FromJson(const core::Json& json);

    bool operator==(const TemplateBuildInfo& o) const;
    bool operator!=(const TemplateBuildInfo& o) const { return !(*this == o); }
};

/// Rust enum `SnapshotSourceKind`.
enum class SnapshotSourceKind {
    Template,
    Sandbox,
};

/// Rust enum `SnapshotSource`.
struct SnapshotSource {
    SnapshotSourceKind kind = SnapshotSourceKind::Template;
    /// `Template { build }`.
    TemplateBuildInfo build;
    /// `Sandbox { source_sandbox_id }`.
    std::string source_sandbox_id;

    static SnapshotSource Template(const TemplateBuildInfo& build);
    static SnapshotSource Sandbox(const std::string& source_sandbox_id);

    bool is_template() const { return kind == SnapshotSourceKind::Template; }

    core::Json ToJson() const;
    static core::Expected<SnapshotSource, std::string> FromJson(const core::Json& json);

    bool operator==(const SnapshotSource& o) const;
    bool operator!=(const SnapshotSource& o) const { return !(*this == o); }
};

/// Rust enum `SnapshotPublishSource`.
struct SnapshotPublishSource {
    SnapshotSourceKind kind = SnapshotSourceKind::Template;
    std::string source_sandbox_id;

    static SnapshotPublishSource Template();
    static SnapshotPublishSource Sandbox(const std::string& source_sandbox_id);

    bool operator==(const SnapshotPublishSource& o) const {
        return kind == o.kind && source_sandbox_id == o.source_sandbox_id;
    }
    bool operator!=(const SnapshotPublishSource& o) const { return !(*this == o); }
};

/// Rust struct `CommittedSnapshot` — the artifact payload of a published
/// snapshot. Never contains node-local paths: those are resolved per node.
struct CommittedSnapshot {
    CommandContext context;
    core::Optional<StartupCommand> startup;
    SnapshotRuntimeVersions runtime_versions;
    /// Rust `#[serde(default)]` — the ABI the capture was taken under, so a
    /// node with a different one can refuse to resume it.
    core::VirtualizationMode virtualization_mode = core::VirtualizationMode::Kvm;
    /// Rust `#[serde(default, skip_serializing_if = "ImageConfigs::is_empty")]`.
    tpl::ImageConfigs image_configs;
    std::vector<OverlaybdLayerRef> rootfs_layers;
    std::vector<CommittedAttachedDrive> attached_drives;
    /// Rust `#[serde(default)]`.
    std::vector<SnapshotVolume> volume_snapshots;
    /// Managed overlaybd layers for the memory image, ordered bottom-up.
    std::vector<ManagedLayer> memory_layers;
    /// Rust `#[serde(default)]`.
    std::vector<PersistedDiskImagePublication> disk_publications;
    /// Rust `#[serde(default, skip_serializing_if = "Option::is_none")]` —
    /// opaque JSON forwarded to the custom extension hooks.
    core::Optional<core::Json> custom_extension_params;
    /// Present only when the pack was recorded *and* uploaded; absent on
    /// older snapshots and on POSIX backends.
    core::Optional<MemoryStartupPackInfo> memory_startup;

    core::Json ToJson() const;
    static core::Expected<CommittedSnapshot, std::string> FromJson(const core::Json& json);

    bool operator==(const CommittedSnapshot& o) const;
    bool operator!=(const CommittedSnapshot& o) const { return !(*this == o); }
};

/// Rust struct `SnapshotRecord`.
struct SnapshotRecord {
    core::SnapshotId id;
    core::Optional<SnapshotAlias> alias;
    SnapshotSource source;
    sandbox::SandboxResources resources;
    int64_t created_at_unix_ms = 0;
    int64_t updated_at_unix_ms = 0;
    /// Absent while a template build is still in flight.
    core::Optional<CommittedSnapshot> committed;

    /// Rust `SnapshotRecord::template_waiting`.
    static SnapshotRecord TemplateWaiting(const core::SnapshotId& id,
                                          const core::Optional<SnapshotAlias>& alias,
                                          const sandbox::SandboxResources& resources);

    /// Rust `SnapshotRecord::mark_committed`.
    void MarkCommitted(const core::Optional<SnapshotAlias>& alias,
                       const sandbox::SandboxResources& resources,
                       const CommittedSnapshot& committed, const SnapshotPublishSource& source,
                       int64_t now_unix_ms);

    /// Rust `SnapshotRecord::published_rootfs_image_ref`.
    core::Optional<std::string> PublishedRootfsImageRef() const;

    core::Json ToJson() const;
    static core::Expected<SnapshotRecord, std::string> FromJson(const core::Json& json);

    bool operator==(const SnapshotRecord& o) const;
    bool operator!=(const SnapshotRecord& o) const { return !(*this == o); }
};

/// Rust struct `SnapshotPublishMetadata`.
///
/// Not serialised: it is the in-flight description of a publish, which
/// `commit_publish` folds into a `SnapshotRecord`.
struct SnapshotPublishMetadata {
    core::SnapshotId id;
    core::Optional<SnapshotAlias> alias;
    SnapshotPublishSource source;
    CommandContext context;
    core::Optional<StartupCommand> startup;
    sandbox::SandboxResources resources;
    SnapshotRuntimeVersions runtime_versions;
    core::VirtualizationMode virtualization_mode = core::VirtualizationMode::Kvm;
    tpl::ImageConfigs image_configs;
    std::vector<SnapshotVolume> volume_snapshots;
    core::Optional<core::Json> custom_extension_params;
};

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_RECORD_H_
