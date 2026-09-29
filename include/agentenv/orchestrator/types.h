// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/types.rs — the orchestrator's lifecycle ADTs.
//
// This header is a strict 1:1 port of `types.rs` plus the operation/error
// enums declared in `mod.rs` (`SandboxOperation`, `OrchestratorError`).
#ifndef AGENTENV_ORCHESTRATOR_TYPES_H_
#define AGENTENV_ORCHESTRATOR_TYPES_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/virtualization.h"
#include "agentenv/sandbox/custom_extension.h"
#include "agentenv/sandbox/extra_drive.h"
#include "agentenv/sandbox/network/policy.h"
#include "agentenv/sandbox/types.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/snapshot/version.h"
#include "agentenv/template/build_spec.h"

namespace agentenv {
namespace orchestrator {

/// Rust enum `SandboxState` (src/orchestrator/types.rs).
///
/// The set of runtime states a sandbox can be in. Note this is intentionally
/// different from a boot "phase": there is no terminal `Stopped`/`Failed`
/// state here — a killed sandbox is removed from the store instead.
enum class SandboxState {
    Creating,
    Resuming,
    Running,
    Snapshotting,
    Forking,
    Pausing,
    Paused,
    Killing,
};

/// Rust `impl Display for SandboxState` — lowercase names ("creating", ...).
const char* SandboxStateName(SandboxState s);

/// Rust enum `SandboxTimeoutAction` (declared in store/metadata.rs).
///
/// Hoisted here because `CreateSandboxRequest` below needs it and `store.h`
/// includes this header; see the note in `store.h`.
enum class SandboxTimeoutAction {
    Pause,
    Delete,
};

/// Rust enum `SandboxLaunchSource` (types.rs).
///
/// Deviation: Rust's `Snapshot` variant holds `Box<RunnableSnapshot>`, whose
/// loader is not ported. The variant therefore carries the fields
/// `create_sandbox_inner` actually reads off the snapshot's committed record,
/// so the inheritance rules (custom params, image configs, context, ...) port
/// faithfully; only resolving a snapshot id into those fields is out of scope.
struct SandboxLaunchSource {
    enum class Kind { Snapshot, Image };

    Kind kind = Kind::Image;

    // ---- Kind::Snapshot ----
    /// Rust `snapshot.record().id`.
    std::string snapshot_id;
    /// Rust `snapshot.record().alias`.
    core::Optional<std::string> snapshot_alias;
    /// Rust `snapshot.committed().virtualization_mode`.
    core::VirtualizationMode snapshot_virtualization_mode = core::VirtualizationMode::Kvm;
    /// Rust `snapshot.committed().runtime_versions`.
    snapshot::SnapshotRuntimeVersions snapshot_runtime_versions;
    /// Rust `snapshot.committed().startup`.
    core::Optional<snapshot::StartupCommand> snapshot_startup;
    /// Rust `snapshot.committed().custom_extension_params` — inherited unless
    /// the create request overrides it.
    core::Optional<sandbox::custom_extension::Params> snapshot_custom_extension_params;
    /// Rust `*snapshot.resources()`.
    sandbox::SandboxResources snapshot_resources;

    // ---- Kind::Image ----
    /// Rust `image_ref`.
    std::string image_ref;
    /// Rust `overlaybd_config_path`.
    std::string overlaybd_config_path;
    /// Rust `resources: Option<SandboxResources>` — unset means the node's
    /// configured defaults (`default_fresh_sandbox_resources`).
    core::Optional<sandbox::SandboxResources> image_resources;
    /// Rust `extra_boot_args`.
    core::Optional<std::string> extra_boot_args;

    // ---- shared by both variants ----
    /// Rust `context` (Image) / `committed().context` (Snapshot).
    snapshot::CommandContext context;
    /// Rust `image_configs` (Image) / `committed().image_configs` (Snapshot).
    tpl::ImageConfigs image_configs;
    /// Rust `extra_drives` on the Image variant; the Snapshot variant takes
    /// its drives from the request only.
    std::vector<sandbox::ExtraDrive> extra_drives;

    static SandboxLaunchSource FromSnapshot(const std::string& snapshot_id);
    static SandboxLaunchSource FromImage(const std::string& image_ref,
                                         const std::string& overlaybd_config_path);
};

/// Rust struct `SandboxForkChildSpec` (types.rs).
struct SandboxForkChildSpec {
    core::SandboxId sandbox_id;
    /// Volume mounts the child gets, keyed by guest path.
    std::unordered_map<std::string, std::string> volume_mounts;
    std::vector<sandbox::ExtraDrive> extra_drives;
    /// Pairs of `(source_drive_id, replacement_drive_id)`.
    std::vector<std::pair<std::string, std::string> > replace_drive_ids;
};

/// Rust struct `CreateSandboxRequest` (types.rs).
struct CreateSandboxRequest {
    SandboxLaunchSource source;
    /// Launch-time drives that are not part of the source snapshot.
    std::vector<sandbox::ExtraDrive> extra_drives;
    /// Whether `extra_drives` already occupy reserved slots in the source
    /// Firecracker state. Restored volume snapshots can be bound before load;
    /// newly requested volumes must replace placeholders after load.
    bool extra_drives_in_snapshot = false;
    /// Rust `timeout: Option<Duration>` — unset applies the node default.
    core::Optional<int64_t> timeout_ms;
    SandboxTimeoutAction timeout_action = SandboxTimeoutAction::Pause;
    bool auto_resume = false;
    std::unordered_map<std::string, std::string> user_metadata;
    std::unordered_map<std::string, std::string> env_vars;
    sandbox::network::SandboxNetworkPolicy network_policy;
    bool secure = false;
    /// Opaque user-provided JSON passed through to the custom extension hooks.
    core::Optional<sandbox::custom_extension::Params> custom_extension_params;
    /// Volume mounts requested for this sandbox, keyed by guest path.
    std::unordered_map<std::string, std::string> volume_mounts;
};

/// Rust enum `SandboxLifecycleEventType` (types.rs).
enum class SandboxLifecycleEventType {
    Create,
    Delete,
    Pause,
    Resume,
    Fork,
};

/// Rust struct `SandboxLifecycleEvent` (types.rs).
struct SandboxLifecycleEvent {
    SandboxLifecycleEventType event_type;
    core::SandboxId           sandbox_id;
    sandbox::SandboxResources resources;

    bool operator==(const SandboxLifecycleEvent& o) const {
        return event_type == o.event_type && sandbox_id == o.sandbox_id &&
               resources == o.resources;
    }
    bool operator!=(const SandboxLifecycleEvent& o) const { return !(*this == o); }
};

/// Rust enum `SandboxOperation` (src/orchestrator/mod.rs).
///
/// The set of operations the orchestrator serializes per-sandbox.
enum class SandboxOperation {
    Build,
    Start,
    WaitReady,
    Pause,
    Resume,
    Snapshot,
    SnapshotVolumes,
    Fork,
    UpdateNetwork,
    PatchCustomExtensionParams,
    Stop,
};

const char* SandboxOperationName(SandboxOperation op);

/// Rust enum `OrchestratorError` (src/orchestrator/mod.rs).
///
/// The variant tag. Detail fields (sandbox id, offending state, operation,
/// message) live on `OrchestratorError` below, matching the Rust struct-like
/// enum variants.
enum class OrchestratorErrorKind {
    ConfigLoadFailed,
    VirtualizationModeMismatch,
    ShuttingDown,
    SandboxNotFound,
    InvalidSandboxState,
    SandboxOperationFailed,
    SandboxOperationConflict,
    StoreOperationFailed,
    SandboxPersistenceFailed,
    InvalidTimeout,
    InternalError,
};

/// Rust enum `OrchestratorError` (thiserror). Modeled as a tagged struct so
/// each variant's payload (id / state / operation / message) is preserved and
/// `Message()` reproduces the Rust `#[error(...)]` format strings.
struct OrchestratorError {
    OrchestratorErrorKind kind = OrchestratorErrorKind::InternalError;
    core::SandboxId       sandbox_id;      // for id-carrying variants
    SandboxState          state = SandboxState::Creating;  // InvalidSandboxState
    SandboxOperation      operation = SandboxOperation::Build;  // op variants
    std::string           detail;          // source/message/timeout string

    /// `VirtualizationModeMismatch { resource, resource_mode, node_mode }` —
    /// `detail` carries `resource`.
    core::VirtualizationMode resource_mode = core::VirtualizationMode::Kvm;
    core::VirtualizationMode node_mode     = core::VirtualizationMode::Kvm;

    // Constructors mirroring the Rust variants.
    static OrchestratorError ConfigLoadFailed(std::string source);
    static OrchestratorError VirtualizationModeMismatch(
        std::string resource, core::VirtualizationMode resource_mode,
        core::VirtualizationMode node_mode);
    static OrchestratorError ShuttingDown();
    static OrchestratorError SandboxNotFound(core::SandboxId id);
    static OrchestratorError InvalidSandboxState(core::SandboxId id, SandboxState st);
    static OrchestratorError SandboxOperationFailed(core::SandboxId id,
                                                    SandboxOperation op,
                                                    std::string source);
    static OrchestratorError SandboxOperationConflict(core::SandboxId id,
                                                      SandboxOperation op);
    static OrchestratorError StoreOperationFailed(std::string detail);
    static OrchestratorError SandboxPersistenceFailed(std::string detail);
    static OrchestratorError InvalidTimeout(core::SandboxId id, std::string timeout);
    static OrchestratorError InternalError(std::string message);

    /// Reproduces the Rust `Display`/`#[error(...)]` message.
    std::string Message() const;
};

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_TYPES_H_
