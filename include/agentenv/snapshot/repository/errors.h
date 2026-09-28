// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/errors.rs — RepositoryError + RepositoryResult.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_ERRORS_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_ERRORS_H_

#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace snapshot {
namespace repository {

/// Rust enum `RepositoryError` variants (tag).
enum class RepositoryErrorKind {
    InvalidRequest,
    SnapshotNotFound,
    VolumeNotFound,
    VolumeNameConflict,
    AliasNotFound,
    AliasConflict,
    ArtifactNotFound,
    ManagedLayerNotFound,
    IntegrityMismatch,
    Unsupported,
    Backend,
};

/// Rust struct `RepositoryError`.
struct RepositoryError {
    RepositoryErrorKind kind = RepositoryErrorKind::Backend;
    std::string         message;
    // Extra fields used by specific variants (kept flat for C++11 simplicity).
    std::string         alias;
    std::string         existing;
    std::string         new_id;
    std::string         artifact;
    std::string         expected;
    std::string         actual;

    static RepositoryError InvalidRequest(std::string reason);
    static RepositoryError SnapshotNotFound(std::string lookup);
    static RepositoryError VolumeNotFound(std::string lookup);
    static RepositoryError VolumeNameConflict(std::string name);
    static RepositoryError AliasNotFound(std::string alias);
    static RepositoryError AliasConflict(std::string alias, std::string existing, std::string new_id);
    static RepositoryError ArtifactNotFound(std::string artifact);
    static RepositoryError ManagedLayerNotFound(std::string digest);
    static RepositoryError IntegrityMismatch(std::string artifact, std::string expected,
                                             std::string actual);
    static RepositoryError Unsupported(std::string feature);
    /// Rust `RepositoryError::backend(message, source)` — the source error is
    /// folded into the message here, since there is no `anyhow` chain to
    /// carry it.
    static RepositoryError Backend(std::string message);
    static RepositoryError Backend(std::string message, const std::string& source);

    /// Rust `Display` impl.
    std::string ToString() const;
};

/// Rust `type RepositoryResult<T> = Result<T, RepositoryError>`.
template <typename T>
using RepositoryResult = core::Expected<T, RepositoryError>;

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_ERRORS_H_
