// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/types.rs — the orchestrator's lifecycle ADTs.
//
// This header is a strict 1:1 port of `types.rs` plus the operation/error
// enums declared in `mod.rs` (`SandboxOperation`, `OrchestratorError`).
#ifndef AGENTENV_ORCHESTRATOR_TYPES_H_
#define AGENTENV_ORCHESTRATOR_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/identity.h"
#include "agentenv/sandbox/types.h"

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

    // Constructors mirroring the Rust variants.
    static OrchestratorError ConfigLoadFailed(std::string source);
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
