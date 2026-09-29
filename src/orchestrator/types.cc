// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/types.rs (+ enums from mod.rs).
#include "agentenv/orchestrator/types.h"

#include <sstream>

namespace agentenv {
namespace orchestrator {

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
