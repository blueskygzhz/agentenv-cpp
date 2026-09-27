// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/errors.rs
#include "agentenv/snapshot/repository/errors.h"

namespace agentenv {
namespace snapshot {
namespace repository {

RepositoryError RepositoryError::InvalidRequest(std::string reason) {
    RepositoryError e; e.kind = RepositoryErrorKind::InvalidRequest; e.message = std::move(reason); return e;
}
RepositoryError RepositoryError::SnapshotNotFound(std::string lookup) {
    RepositoryError e; e.kind = RepositoryErrorKind::SnapshotNotFound; e.message = std::move(lookup); return e;
}
RepositoryError RepositoryError::AliasNotFound(std::string alias) {
    RepositoryError e; e.kind = RepositoryErrorKind::AliasNotFound; e.alias = std::move(alias); return e;
}
RepositoryError RepositoryError::AliasConflict(std::string alias, std::string existing, std::string new_id) {
    RepositoryError e; e.kind = RepositoryErrorKind::AliasConflict;
    e.alias = std::move(alias); e.existing = std::move(existing); e.new_id = std::move(new_id);
    return e;
}
RepositoryError RepositoryError::Unsupported(std::string feature) {
    RepositoryError e; e.kind = RepositoryErrorKind::Unsupported; e.message = std::move(feature); return e;
}
RepositoryError RepositoryError::Backend(std::string message) {
    RepositoryError e; e.kind = RepositoryErrorKind::Backend; e.message = std::move(message); return e;
}

std::string RepositoryError::ToString() const {
    switch (kind) {
        case RepositoryErrorKind::InvalidRequest:
            return "invalid repository request: " + message;
        case RepositoryErrorKind::SnapshotNotFound:
            return "snapshot not found: " + message;
        case RepositoryErrorKind::AliasNotFound:
            return "snapshot alias not found: " + alias;
        case RepositoryErrorKind::AliasConflict:
            return "alias '" + alias + "' already points to '" + existing +
                   "', cannot rebind to '" + new_id + "'";
        case RepositoryErrorKind::ArtifactNotFound:
            return "artifact not found: " + artifact;
        case RepositoryErrorKind::ManagedLayerNotFound:
            return "managed layer not found: " + message;
        case RepositoryErrorKind::IntegrityMismatch:
            return "integrity mismatch for " + artifact + ": expected " + expected +
                   ", got " + actual;
        case RepositoryErrorKind::Unsupported:
            return "unsupported operation: " + message;
        case RepositoryErrorKind::Backend:
        default:
            return "backend error: " + message;
    }
}

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
