// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/persistence/{mod,file_backed}.rs
#include "agentenv/orchestrator/persistence.h"

#include <sstream>

namespace agentenv {
namespace orchestrator {

// ---- SandboxPersistenceError -----------------------------------------------

SandboxPersistenceError SandboxPersistenceError::Io(const std::string& operation,
                                                    const std::string& path,
                                                    const std::string& source) {
    SandboxPersistenceError e;
    e.kind      = Kind::Io;
    e.operation = operation;
    e.path      = path;
    e.source    = source;
    return e;
}

SandboxPersistenceError SandboxPersistenceError::Store(const std::string& operation,
                                                       const std::string& source) {
    SandboxPersistenceError e;
    e.kind      = Kind::Store;
    e.operation = operation;
    e.source    = source;
    return e;
}

SandboxPersistenceError SandboxPersistenceError::InvalidRecord(
    const std::string& reason, const std::string& source) {
    SandboxPersistenceError e;
    e.kind   = Kind::InvalidRecord;
    e.reason = reason;
    e.source = source;
    return e;
}

SandboxPersistenceError SandboxPersistenceError::RuntimeState(
    const std::string& reason) {
    SandboxPersistenceError e;
    e.kind   = Kind::RuntimeState;
    e.reason = reason;
    return e;
}

std::string SandboxPersistenceError::Message() const {
    std::ostringstream os;
    switch (kind) {
        case Kind::Io:
            os << "failed to " << operation << " " << path << ": " << source;
            return os.str();
        case Kind::InvalidRecord:
            os << "invalid sandbox record: " << reason;
            return os.str();
        case Kind::RuntimeState:
            os << "invalid paused sandbox runtime state: " << reason;
            return os.str();
        case Kind::Store:
            os << "paused sandbox store operation failed: " << operation
               << ": " << source;
            return os.str();
    }
    return "unknown sandbox persistence error";
}

// ---- DisabledSandboxPersister ----------------------------------------------

PersistenceResult<std::vector<SandboxMetadata> >
DisabledSandboxPersister::LoadAll() {
    return std::vector<SandboxMetadata>();
}

PersistenceResult<core::Optional<std::string> >
DisabledSandboxPersister::AllocateArtifactRoot(const core::SandboxId&) {
    // Rust returns Ok(None): persistence disabled, backend owns its artifacts.
    return core::Optional<std::string>(core::nullopt);
}

PersistenceResult<core::Unit>
DisabledSandboxPersister::PersistPaused(const SandboxMetadata&,
                                        const core::Optional<std::string>&) {
    return core::Unit{};
}

PersistenceResult<core::Unit>
DisabledSandboxPersister::MarkResuming(const core::SandboxId&) {
    return core::Unit{};
}

PersistenceResult<core::Unit>
DisabledSandboxPersister::RollbackResuming(const core::SandboxId&) {
    return core::Unit{};
}

PersistenceResult<core::Unit>
DisabledSandboxPersister::DeleteRecord(const core::SandboxId&) {
    return core::Unit{};
}

PersistenceResult<core::Unit>
DisabledSandboxPersister::DeleteRecordAndArtifacts(const core::SandboxId&) {
    return core::Unit{};
}

// ---- factory ---------------------------------------------------------------

std::unique_ptr<SandboxPersister> MakePersister(const std::string& /*kind*/,
                                                const std::string& /*path*/) {
    // Rust `FileBackedSandboxPersister` is not ported yet; the disabled
    // persister keeps the orchestrator wiring valid and side-effect free.
    return std::unique_ptr<SandboxPersister>(new DisabledSandboxPersister());
}

}  // namespace orchestrator
}  // namespace agentenv
