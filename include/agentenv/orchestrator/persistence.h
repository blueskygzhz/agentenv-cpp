// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/persistence/{mod,file_backed}.rs
#ifndef AGENTENV_ORCHESTRATOR_PERSISTENCE_H_
#define AGENTENV_ORCHESTRATOR_PERSISTENCE_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/orchestrator/store.h"

namespace agentenv {
namespace orchestrator {

/// Rust enum `SandboxPersistenceError` (persistence/mod.rs).
struct SandboxPersistenceError {
    enum class Kind { Io, InvalidRecord, RuntimeState, Store };

    Kind        kind      = Kind::Io;
    std::string operation;  // Io / Store
    std::string path;       // Io
    std::string reason;     // InvalidRecord / RuntimeState
    std::string source;     // underlying error text

    /// Rust `SandboxPersistenceError::io`.
    static SandboxPersistenceError Io(const std::string& operation,
                                      const std::string& path,
                                      const std::string& source);
    /// Rust `SandboxPersistenceError::store`.
    static SandboxPersistenceError Store(const std::string& operation,
                                         const std::string& source);
    static SandboxPersistenceError InvalidRecord(const std::string& reason,
                                                 const std::string& source);
    static SandboxPersistenceError RuntimeState(const std::string& reason);

    /// Reproduces the Rust `#[error(...)]` format strings.
    std::string Message() const;
};

/// Rust `type PersistenceResult<T>`.
template <typename T>
using PersistenceResult = core::Expected<T, SandboxPersistenceError>;

/// Rust trait `SandboxPersister` (persistence/mod.rs).
///
/// The Rust trait is generic over `SandboxBackendFactory` in `load_all`; the
/// factory is not yet ported, so the C++ signature drops that parameter.
class SandboxPersister {
 public:
    virtual ~SandboxPersister() {}

    /// Rust `load_all` — every persisted sandbox record from the last run.
    virtual PersistenceResult<std::vector<SandboxMetadata> > LoadAll() = 0;

    /// Rust `allocate_artifact_root` — an unset result means persistence is
    /// disabled and the backend owns its temporary artifacts' lifecycle.
    virtual PersistenceResult<core::Optional<std::string> >
        AllocateArtifactRoot(const core::SandboxId& id) = 0;

    /// Rust `persist_paused` — metadata plus runtime state for a paused sandbox.
    ///
    /// `paused_state` is the backend's opaque capture; Rust passes it
    /// alongside the metadata so the record and the runtime state are written
    /// together.
    virtual PersistenceResult<core::Unit>
        PersistPaused(const SandboxMetadata& metadata,
                      const core::Optional<std::string>& artifact_root,
                      const sandbox::PausedSandboxState* paused_state) = 0;

    /// Rust `mark_resuming`.
    virtual PersistenceResult<core::Unit> MarkResuming(const core::SandboxId& id) = 0;
    /// Rust `rollback_resuming` — undoes the mark after a failed resume.
    virtual PersistenceResult<core::Unit> RollbackResuming(const core::SandboxId& id) = 0;
    /// Rust `delete_record`.
    virtual PersistenceResult<core::Unit> DeleteRecord(const core::SandboxId& id) = 0;
    /// Rust `delete_record_and_artifacts`.
    virtual PersistenceResult<core::Unit>
        DeleteRecordAndArtifacts(const core::SandboxId& id) = 0;
};

/// Rust struct `DisabledSandboxPersister` — every operation is a no-op and
/// `load_all` yields nothing.
class DisabledSandboxPersister : public SandboxPersister {
 public:
    PersistenceResult<std::vector<SandboxMetadata> > LoadAll() override;
    PersistenceResult<core::Optional<std::string> >
        AllocateArtifactRoot(const core::SandboxId& id) override;
    PersistenceResult<core::Unit>
        PersistPaused(const SandboxMetadata& metadata,
                      const core::Optional<std::string>& artifact_root,
                      const sandbox::PausedSandboxState* paused_state) override;
    PersistenceResult<core::Unit> MarkResuming(const core::SandboxId& id) override;
    PersistenceResult<core::Unit> RollbackResuming(const core::SandboxId& id) override;
    PersistenceResult<core::Unit> DeleteRecord(const core::SandboxId& id) override;
    PersistenceResult<core::Unit>
        DeleteRecordAndArtifacts(const core::SandboxId& id) override;
};

/// "disabled" — Rust `DisabledSandboxPersister`.
/// "file"     — Rust `FileBackedSandboxPersister` (not yet ported; falls back
///              to the disabled persister so wiring stays valid).
std::unique_ptr<SandboxPersister> MakePersister(const std::string& kind,
                                                const std::string& path);

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_PERSISTENCE_H_
