// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/types.rs (+ enums from mod.rs).
#include "agentenv/orchestrator/types.h"

#include <sstream>

namespace agentenv {
namespace orchestrator {

// Rust `SandboxLaunchSource::Snapshot { .. }`.
SandboxLaunchSource SandboxLaunchSource::FromSnapshot(const std::string& snapshot_id) {
    SandboxLaunchSource s;
    s.kind = Kind::Snapshot;
    s.snapshot_id = snapshot_id;
    return s;
}

// Rust `SandboxLaunchSource::Image { .. }`.
SandboxLaunchSource SandboxLaunchSource::FromImage(
    const std::string& image_ref, const std::string& overlaybd_config_path) {
    SandboxLaunchSource s;
    s.kind = Kind::Image;
    s.image_ref = image_ref;
    s.overlaybd_config_path = overlaybd_config_path;
    return s;
}

core::Expected<SandboxLaunchSource, std::string>
SandboxLaunchSource::FromRunnableSnapshot(const snapshot::RunnableSnapshot& snapshot) {
    const snapshot::SnapshotRecord& record = snapshot.record;
    if (!record.committed.has_value()) {
        return core::make_unexpected(std::string("snapshot '") + record.id.ToString() +
                                     "' has no committed payload");
    }
    const snapshot::CommittedSnapshot& committed = *record.committed;

    SandboxLaunchSource s;
    s.kind = Kind::Snapshot;
    s.snapshot_id = record.id.ToString();
    if (record.alias.has_value()) {
        s.snapshot_alias = core::Optional<std::string>(record.alias->ToString());
    }
    s.snapshot_virtualization_mode = committed.virtualization_mode;
    s.snapshot_runtime_versions    = committed.runtime_versions;
    s.snapshot_startup             = committed.startup;
    // Rust `*snapshot.resources()`, which reads the **record**, not the
    // committed payload: resources are what the snapshot was published with
    // and may be overridden on republish, so they live one level up.
    s.snapshot_resources           = record.resources;
    // Rust carries `CustomExtensionParams` (a JSON map) end to end; this port
    // stores the params as their text (`custom_extension::Params::json_bytes`)
    // so the caller's bytes reach the extension unchanged. `ToString` is the
    // only conversion, and it is lossless for a document that was parsed from
    // text in the first place.
    if (committed.custom_extension_params.has_value()) {
        sandbox::custom_extension::Params params;
        params.json_bytes = committed.custom_extension_params->ToString();
        s.snapshot_custom_extension_params =
            core::Optional<sandbox::custom_extension::Params>(params);
    }
    s.context       = committed.context;
    s.image_configs = committed.image_configs;

    // Rust reads these off `snapshot.manifest()` inside
    // `create_sandbox_inner`. The manifest is the *hydrated* one the resolver
    // produced, so every path here is node-local and absolute -- which is the
    // whole reason the resolver has to run before this projection.
    const sandbox::SandboxSnapshotManifest& manifest = snapshot.manifest;
    s.snapshot_manifest.backend                  = manifest.backend;
    s.snapshot_manifest.vm_state_path            = manifest.vm_state.path;
    s.snapshot_manifest.memory_image_config_path = manifest.memory.image_config_path;
    s.snapshot_manifest.memory_virtual_size      = manifest.memory.virtual_size;
    s.snapshot_manifest.rootfs_image_config_path = manifest.rootfs.image_config_path;
    s.snapshot_manifest.rootfs_virtual_size      = manifest.rootfs.virtual_size;
    s.snapshot_manifest.extra_drives             = manifest.ExtraDrives();
    s.snapshot_manifest.volume_drive_slots       = manifest.volume_drive_slots;
    s.snapshot_manifest.physical_extra_drive_count = manifest.physical_extra_drive_count;
    return s;
}

// Rust `impl Display for SandboxState` — lowercase.
const char* SandboxStateName(SandboxState s) {
    switch (s) {
        case SandboxState::Creating:     return "creating";
        case SandboxState::Resuming:     return "resuming";
        case SandboxState::Running:      return "running";
        case SandboxState::Snapshotting: return "snapshotting";
        case SandboxState::Forking:      return "forking";
        case SandboxState::Pausing:      return "pausing";
        case SandboxState::Paused:       return "paused";
        case SandboxState::Killing:      return "killing";
    }
    return "?";
}

namespace {

// Rust `{:?}` (Debug) rendering of SandboxState — the PascalCase variant name.
const char* SandboxStateDebug(SandboxState s) {
    switch (s) {
        case SandboxState::Creating:     return "Creating";
        case SandboxState::Resuming:     return "Resuming";
        case SandboxState::Running:      return "Running";
        case SandboxState::Snapshotting: return "Snapshotting";
        case SandboxState::Forking:      return "Forking";
        case SandboxState::Pausing:      return "Pausing";
        case SandboxState::Paused:       return "Paused";
        case SandboxState::Killing:      return "Killing";
    }
    return "?";
}

}  // namespace

// Rust `{:?}` (Debug) rendering of SandboxOperation — the PascalCase name.
const char* SandboxOperationName(SandboxOperation op) {
    switch (op) {
        case SandboxOperation::Build:                      return "Build";
        case SandboxOperation::Start:                      return "Start";
        case SandboxOperation::WaitReady:                  return "WaitReady";
        case SandboxOperation::Pause:                      return "Pause";
        case SandboxOperation::Resume:                     return "Resume";
        case SandboxOperation::Snapshot:                   return "Snapshot";
        case SandboxOperation::SnapshotVolumes:            return "SnapshotVolumes";
        case SandboxOperation::Fork:                       return "Fork";
        case SandboxOperation::UpdateNetwork:              return "UpdateNetwork";
        case SandboxOperation::PatchCustomExtensionParams: return "PatchCustomExtensionParams";
        case SandboxOperation::Stop:                       return "Stop";
    }
    return "?";
}

// --- OrchestratorError constructors ----------------------------------------

OrchestratorError OrchestratorError::ConfigLoadFailed(std::string source) {
    OrchestratorError e;
    e.kind = OrchestratorErrorKind::ConfigLoadFailed;
    e.detail = std::move(source);
    return e;
}
OrchestratorError OrchestratorError::VirtualizationModeMismatch(
    std::string resource, core::VirtualizationMode resource_mode,
    core::VirtualizationMode node_mode) {
    OrchestratorError e;
    e.kind = OrchestratorErrorKind::VirtualizationModeMismatch;
    e.detail = std::move(resource);
    e.resource_mode = resource_mode;
    e.node_mode = node_mode;
    return e;
}
OrchestratorError OrchestratorError::ShuttingDown() {
    OrchestratorError e;
    e.kind = OrchestratorErrorKind::ShuttingDown;
    return e;
}
OrchestratorError OrchestratorError::SandboxNotFound(core::SandboxId id) {
    OrchestratorError e;
    e.kind = OrchestratorErrorKind::SandboxNotFound;
    e.sandbox_id = id;
    return e;
}
OrchestratorError OrchestratorError::InvalidSandboxState(core::SandboxId id,
                                                         SandboxState st) {
    OrchestratorError e;
    e.kind = OrchestratorErrorKind::InvalidSandboxState;
    e.sandbox_id = id;
    e.state = st;
    return e;
}
OrchestratorError OrchestratorError::SandboxOperationFailed(core::SandboxId id,
                                                            SandboxOperation op,
                                                            std::string source) {
    OrchestratorError e;
    e.kind = OrchestratorErrorKind::SandboxOperationFailed;
    e.sandbox_id = id;
    e.operation = op;
    e.detail = std::move(source);
    return e;
}
OrchestratorError OrchestratorError::SandboxOperationConflict(core::SandboxId id,
                                                              SandboxOperation op) {
    OrchestratorError e;
    e.kind = OrchestratorErrorKind::SandboxOperationConflict;
    e.sandbox_id = id;
    e.operation = op;
    return e;
}
OrchestratorError OrchestratorError::StoreOperationFailed(std::string detail) {
    OrchestratorError e;
    e.kind = OrchestratorErrorKind::StoreOperationFailed;
    e.detail = std::move(detail);
    return e;
}
OrchestratorError OrchestratorError::SandboxPersistenceFailed(std::string detail) {
    OrchestratorError e;
    e.kind = OrchestratorErrorKind::SandboxPersistenceFailed;
    e.detail = std::move(detail);
    return e;
}
OrchestratorError OrchestratorError::InvalidTimeout(core::SandboxId id,
                                                    std::string timeout) {
    OrchestratorError e;
    e.kind = OrchestratorErrorKind::InvalidTimeout;
    e.sandbox_id = id;
    e.detail = std::move(timeout);
    return e;
}
OrchestratorError OrchestratorError::InternalError(std::string message) {
    OrchestratorError e;
    e.kind = OrchestratorErrorKind::InternalError;
    e.detail = std::move(message);
    return e;
}

// Reproduces the Rust `#[error(...)]` format strings.
std::string OrchestratorError::Message() const {
    std::ostringstream os;
    switch (kind) {
        case OrchestratorErrorKind::ConfigLoadFailed:
            return "failed to load sandbox config";
        case OrchestratorErrorKind::VirtualizationModeMismatch:
            // Rust: "{resource} uses virtualization mode '{resource_mode}',
            //        but this node runs in mode '{node_mode}'"
            os << detail << " uses virtualization mode '"
               << core::VirtualizationModeToString(resource_mode)
               << "', but this node runs in mode '"
               << core::VirtualizationModeToString(node_mode) << "'";
            return os.str();
        case OrchestratorErrorKind::ShuttingDown:
            return "orchestrator is shutting down";
        case OrchestratorErrorKind::SandboxNotFound:
            os << "sandbox " << sandbox_id.ToString() << " not found";
            return os.str();
        case OrchestratorErrorKind::InvalidSandboxState:
            os << "sandbox " << sandbox_id.ToString()
               << " is in invalid state " << SandboxStateDebug(state);
            return os.str();
        case OrchestratorErrorKind::SandboxOperationFailed:
            os << "sandbox " << sandbox_id.ToString() << " operation "
               << SandboxOperationName(operation) << " failed: " << detail;
            return os.str();
        case OrchestratorErrorKind::SandboxOperationConflict:
            os << "sandbox " << sandbox_id.ToString() << " operation "
               << SandboxOperationName(operation)
               << " conflicted with another operation";
            return os.str();
        case OrchestratorErrorKind::StoreOperationFailed:
            os << "store operation failed: " << detail;
            return os.str();
        case OrchestratorErrorKind::SandboxPersistenceFailed:
            os << "sandbox persistence failed: " << detail;
            return os.str();
        case OrchestratorErrorKind::InvalidTimeout:
            os << "invalid timeout for " << sandbox_id.ToString() << ": " << detail;
            return os.str();
        case OrchestratorErrorKind::InternalError:
            os << "internal error: " << detail;
            return os.str();
    }
    return "unknown orchestrator error";
}

}  // namespace orchestrator
}  // namespace agentenv
