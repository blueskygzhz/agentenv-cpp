// SPDX-License-Identifier: MIT
// Rust: src/sandbox/backend.rs — SandboxBackend, SandboxBackendFactory,
//       PausedSandboxState, CapturedSandboxSnapshot, RuntimeArtifactSet,
//       SandboxCaptureError, SandboxForkSpec, SandboxRuntimeInfo.
//
// Concurrency model: synchronous equivalents of Rust async fn.
// `async fn -> Result<T>` maps to blocking `Expected<T, AnyError>`.
#ifndef AGENTENV_SANDBOX_BACKEND_H_
#define AGENTENV_SANDBOX_BACKEND_H_

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/json.h"
#include "agentenv/sandbox/access.h"
#include "agentenv/sandbox/custom_extension.h"
#include "agentenv/sandbox/extra_drive.h"
#include "agentenv/sandbox/network/policy.h"
#include "agentenv/sandbox/types.h"
// Header-only use: `CommandContext` is a plain struct, so naming it here does
// not make the sandbox library depend on the snapshot library.
#include "agentenv/snapshot/types.h"

namespace agentenv {
namespace sandbox {

// Declared, not included: `firecracker/config.h` includes this header, so
// naming these by definition would be circular. The factory only passes them
// by reference, which a declaration is enough for.
namespace firecracker {
struct RunnableSnapshotView;
struct SnapshotManifestView;
}  // namespace firecracker

// ---------------------------------------------------------------------------
// Rust `RuntimeArtifactSet`
// ---------------------------------------------------------------------------

/// Opaque set of local runtime artifacts a sandbox needs while it is alive.
struct RuntimeArtifactSet {
    std::vector<std::string> overlaybd_image_config_paths;

    /// Rust `RuntimeArtifactSet::empty()`.
    static RuntimeArtifactSet Empty() { return RuntimeArtifactSet(); }

    bool IsEmpty() const { return overlaybd_image_config_paths.empty(); }

    bool operator==(const RuntimeArtifactSet& o) const {
        return overlaybd_image_config_paths == o.overlaybd_image_config_paths;
    }
    bool operator!=(const RuntimeArtifactSet& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------
// Rust `SandboxRuntimeInfo`
// ---------------------------------------------------------------------------

struct SandboxRuntimeInfo {
    core::Optional<uint64_t> rootfs_virtual_size;
    RuntimeArtifactSet       runtime_artifacts;

    bool operator==(const SandboxRuntimeInfo& o) const {
        // core::Optional has no operator==; compare presence then payload.
        const bool a = rootfs_virtual_size.has_value();
        const bool b = o.rootfs_virtual_size.has_value();
        if (a != b) return false;
        if (a && *rootfs_virtual_size != *o.rootfs_virtual_size) return false;
        return runtime_artifacts == o.runtime_artifacts;
    }
};

// ---------------------------------------------------------------------------
// Rust `SandboxCaptureError`
// ---------------------------------------------------------------------------

/// Rust enum `SandboxCaptureError { Recoverable(Error), Terminal(Error) }`.
///
/// `Recoverable` guarantees the sandbox has already been restored to a
/// running state before the error is returned.
/// `Terminal` means the runtime was mutated and is no longer safe to keep.
struct SandboxCaptureError {
    enum class Kind { Recoverable, Terminal };
    Kind        kind = Kind::Recoverable;
    std::string message;

    static SandboxCaptureError Recoverable(const std::string& msg) {
        SandboxCaptureError e; e.kind = Kind::Recoverable; e.message = msg; return e;
    }
    static SandboxCaptureError Terminal(const std::string& msg) {
        SandboxCaptureError e; e.kind = Kind::Terminal; e.message = msg; return e;
    }
    bool IsTerminal() const { return kind == Kind::Terminal; }
    const std::string& Message() const { return message; }
};

template <typename T>
using SandboxCaptureResult = core::Expected<T, SandboxCaptureError>;

// ---------------------------------------------------------------------------
// Rust `PausedSandboxState` trait
// ---------------------------------------------------------------------------

/// Rust trait `PausedSandboxState`.
///
/// The orchestrator treats this as completely opaque: it stores the value
/// after pause and passes it back to `SandboxBackendFactory::BuildFromPausedState`
/// on resume.
class PausedSandboxState {
 public:
    virtual ~PausedSandboxState() {}
    /// Rust `encode(&self) -> Result<Value>` — serialised form for persistence.
    virtual std::string Encode() const = 0;
    /// Rust `runtime_artifacts(&self) -> RuntimeArtifactSet`.
    virtual RuntimeArtifactSet RuntimeArtifacts() const = 0;
    /// Rust `control_plane_port(&self) -> Option<u16>`.
    virtual core::Optional<uint16_t> ControlPlanePort() const {
        return core::Optional<uint16_t>();
    }
};

// ---------------------------------------------------------------------------
// Rust `SandboxForkSpec`
// ---------------------------------------------------------------------------

struct SandboxForkSpec {
    core::SandboxId          sandbox_id;
    core::Optional<EnvdAccessToken> envd_access_token;
    /// Rust `Vec<ExtraDrive>` — the full drives, because a fork's replacement
    /// drives are swapped into the captured state by value (image config,
    /// mount path, size), not looked up by id.
    std::vector<ExtraDrive>  extra_drives;
    /// Pairs of `(source_drive_id, replacement_drive_id)`.
    std::vector<std::pair<std::string, std::string> > replace_drive_ids;
};

using SandboxForkResult = core::Expected<std::unique_ptr<class SandboxBackend>, core::AnyError>;

// ---------------------------------------------------------------------------
// Rust `SandboxBackend` trait
// ---------------------------------------------------------------------------

/// Lifecycle interface for a single sandbox instance.
///
/// Rust's `async fn` methods map to plain blocking equivalents.
/// `SandboxCaptureError` variants map as documented.
class SandboxBackend {
 public:
    virtual ~SandboxBackend() {}

    /// Rust `start` — start and block until ready.
    virtual core::Expected<core::Unit, core::AnyError> Start() = 0;

    /// Rust `start_nowait` — start without waiting.
    virtual core::Expected<core::Unit, core::AnyError> StartNowait() = 0;

    /// Rust `wait_for_ready`.
    virtual core::Expected<core::Unit, core::AnyError> WaitForReady() = 0;

    /// Rust `pause` — snapshot state for later resume; caller must call Stop()
    /// afterwards to release resources.
    virtual SandboxCaptureResult<std::shared_ptr<PausedSandboxState> >
        Pause(const core::Optional<std::string>& artifact_root) = 0;

    /// Rust `resume` — idempotent.
    virtual core::Expected<core::Unit, core::AnyError> Resume() = 0;

    /// Rust `snapshot` — persistent snapshot, sandbox keeps running.
    virtual SandboxCaptureResult<std::string> Snapshot() = 0;

    /// Rust `snapshot_volumes`.
    virtual SandboxCaptureResult<core::Unit> SnapshotVolumes() = 0;

    /// Rust `freeze_and_snapshot_volumes`.
    virtual SandboxCaptureResult<core::Unit> FreezeAndSnapshotVolumes() = 0;

    /// Rust `thaw_volumes`.
    virtual core::Expected<core::Unit, core::AnyError> ThawVolumes() = 0;

    /// Rust `fork`.
    virtual SandboxCaptureResult<std::vector<SandboxForkResult> >
        Fork(const std::vector<SandboxForkSpec>& specs) = 0;

    /// Rust `stop` — idempotent.
    virtual core::Expected<core::Unit, core::AnyError> Stop() = 0;

    /// Rust `host_interaction_ip`.
    virtual core::Optional<std::string> HostInteractionIp() const = 0;

    /// Rust `runtime_info`.
    virtual SandboxRuntimeInfo RuntimeInfoOf() const = 0;

    /// Rust `startup_artifacts`.
    virtual RuntimeArtifactSet StartupArtifacts() const = 0;

    /// Rust `update_network_policy`.
    virtual core::Expected<core::Unit, core::AnyError>
        UpdateNetworkPolicy(const core::Optional<std::string>& policy) = 0;

    /// Rust `update_custom_extension_params`.
    virtual void UpdateCustomExtensionParams(
        const core::Optional<std::string>& params) = 0;
};

// ---------------------------------------------------------------------------
// Rust `SandboxBackendFactory` trait
// ---------------------------------------------------------------------------

/// Rust `FreshSandboxBuildSpec` (sandbox/mod.rs).
///
/// Everything a backend needs to boot an image it has never seen: the user
/// rootfs, the image's command context (env, workdir, user — baked into the
/// guest's defaults), the machine shape, and every attached drive in full.
struct FreshSandboxBuildSpec {
    std::string              image_config_path;
    /// Rust `context: CommandContext`.
    snapshot::CommandContext context;
    SandboxResources         resources;
    /// Rust `Vec<ExtraDrive>`. On a fresh boot *all* drives travel here —
    /// the image's own and the launch-time ones — because nothing is bound
    /// yet; `SandboxLaunchConfig::extra_drives` stays empty on this path.
    std::vector<ExtraDrive>  extra_drives;
    core::Optional<std::string> extra_boot_args;
};

/// Rust `SandboxLaunchConfig` (sandbox/mod.rs) — launch-time inputs from the
/// orchestrator, layered over whatever the source (image or snapshot) carries.
struct SandboxLaunchConfig {
    core::SandboxId          sandbox_id;
    std::string              snapshot_id;
    /// Rust `Option<HashMap<String, String>>` — overrides applied *on top of*
    /// the source defaults; a key present in both takes this value.
    core::Optional<std::map<std::string, std::string> > env_vars;
    /// Rust `Option<SandboxNetworkPolicy>` — the policy itself, set only when
    /// the guest needs rules installed (`SandboxNetworkPolicy::RuntimePolicy`).
    core::Optional<network::SandboxNetworkPolicy> network;
    /// Rust `extra_mmds: serde_json::Map` — opaque pairs merged into the MMDS
    /// document (the orchestrator puts `imageConfigs` here). Reserved MMDS
    /// keys are dropped when merged, never here.
    core::JsonObject         extra_mmds;
    /// Rust `Vec<ExtraDrive>` — drives supplied for this launch (volumes).
    std::vector<ExtraDrive>  extra_drives;
    /// Whether `extra_drives` already occupy reserved slots in the source
    /// Firecracker state and can be bound before snapshot load.
    bool                     extra_drives_in_snapshot = false;
    /// Rust `Option<CustomExtensionParams>`; takes precedence over any value
    /// persisted in the source snapshot.
    core::Optional<custom_extension::Params> custom_extension_params;
    /// Runtime-only credential for envd. Never serialized.
    core::Optional<EnvdAccessToken> envd_access_token;
};

/// Rust trait `SandboxBackendFactory`.
class SandboxBackendFactory {
 public:
    virtual ~SandboxBackendFactory() {}

    /// Rust `build` — brand-new sandbox from a fresh build spec.
    virtual core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
        Build(const FreshSandboxBuildSpec& spec,
              const SandboxLaunchConfig& config) = 0;

    /// Rust `build_from_snapshot(&RunnableSnapshot, SandboxLaunchConfig)`.
    ///
    /// Takes the *resolved* snapshot, not an id. Rust resolves it upstream and
    /// hands the bundle down; this used to take an id, which left the backend
    /// with a key and no way to look it up — so the method could only refuse.
    ///
    /// The bundle arrives as the two views the projection reads, because the
    /// library dependency runs snapshot → sandbox and this layer cannot name
    /// `RunnableSnapshot`.
    virtual core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
        BuildFromSnapshot(const firecracker::RunnableSnapshotView& snapshot,
                          const firecracker::SnapshotManifestView& manifest,
                          const SandboxLaunchConfig& config) = 0;

    /// Rust `build_from_paused_state`.
    virtual core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
        BuildFromPausedState(const core::SandboxId& id,
                             const PausedSandboxState& state,
                             const core::Optional<EnvdAccessToken>& token) = 0;

    /// Rust `decode_paused_state` — rebuilds the backend-specific paused state
    /// from what `PausedSandboxState::Encode` produced, so a persisted record
    /// can be resumed after a restart.
    ///
    /// `artifact_root` is the directory the persister allocated for this
    /// sandbox; a backend that stores paths relative to it resolves them here.
    /// `state` is the encoded JSON payload.
    virtual core::Expected<std::shared_ptr<PausedSandboxState>, core::AnyError>
        DecodePausedState(const std::string& artifact_root,
                          const std::string& state) = 0;
};

// ---------------------------------------------------------------------------
// Legacy `Backend` shim — still used by MockBackend + FirecrackerBackend.
// New orchestrator code uses SandboxBackend + SandboxBackendFactory instead.
// ---------------------------------------------------------------------------

class Backend {
 public:
    virtual ~Backend() = default;

    virtual core::Expected<Handle, core::AnyError>
        Boot(LaunchPlan plan) = 0;
    virtual core::Expected<core::Unit, core::AnyError>
        Shutdown(core::SandboxId id) = 0;
    virtual core::Expected<core::Unit, core::AnyError>
        Pause(core::SandboxId id) = 0;
    virtual core::Expected<core::Unit, core::AnyError>
        Resume(core::SandboxId id) = 0;
    virtual core::Expected<std::string, core::AnyError>
        Snapshot(core::SandboxId id, const std::string& out_dir) = 0;
    virtual core::Expected<Handle, core::AnyError>
        Restore(LaunchPlan plan, const std::string& snapshot_dir) = 0;
    virtual core::Expected<ExecResult, core::AnyError>
        Exec(core::SandboxId id, ExecSpec spec) = 0;
};

std::unique_ptr<Backend> MakeBackend(const std::string& kind);

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_BACKEND_H_
