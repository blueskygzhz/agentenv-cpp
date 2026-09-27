// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/{common,posixfs,oss}/ — concrete
// SnapshotRepository backends.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_BACKENDS_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_BACKENDS_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "agentenv/snapshot/repository/interfaces.h"

namespace agentenv {
namespace snapshot {
namespace repository {

/// Rust: backends/common/ — an in-memory SnapshotRepository used as the base
/// for tests and as the reference implementation of the trait semantics.
class InMemorySnapshotRepository : public SnapshotRepository {
 public:
    RepositoryResult<SnapshotRecord> Create(const SnapshotRecord& record) override;
    RepositoryResult<core::Optional<SnapshotRecord> > Get(const std::string& id_or_alias) override;
    RepositoryResult<std::vector<SnapshotRecord> > List(const SnapshotListFilter& filter) override;
    RepositoryResult<core::Unit> Delete(const std::string& id_or_alias) override;
    RepositoryResult<core::Optional<std::string> > ResolveAlias(const std::string& alias) override;
    RepositoryResult<SnapshotRecord> TryStartBuild(const std::string& id) override;
    RepositoryResult<core::Unit> MarkBuildError(const std::string& id,
                                                const std::string& reason) override;
 private:
    core::Optional<SnapshotRecord> FindLocked(const std::string& id_or_alias) const;
    mutable std::mutex mu_;
    std::map<std::string, SnapshotRecord> by_id_;
    std::map<std::string, std::string>    alias_to_id_;
};

/// Rust: backends/posixfs/ — a shared-POSIX filesystem repository.
std::unique_ptr<SnapshotRepository> MakePosixFsRepository(const std::string& root_dir);

/// Rust: backends/oss/ — an object-storage repository (gated on libcurl).
std::unique_ptr<SnapshotRepository> MakeOssRepository(const std::string& bucket_url);

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_BACKENDS_H_
