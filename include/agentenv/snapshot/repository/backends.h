// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/{common,posixfs,oss}/ — concrete
// SnapshotRepository backends.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_BACKENDS_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_BACKENDS_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "agentenv/cfg.h"
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
    RepositoryResult<core::Optional<core::SnapshotId> >
        ResolveAlias(const std::string& alias) override;
    RepositoryResult<SnapshotRecord> TryStartBuild(const core::SnapshotId& id) override;
    RepositoryResult<core::Unit>
        MarkBuildError(const core::SnapshotId& id,
                       const TemplateBuildErrorReason& reason) override;
 private:
    core::Optional<SnapshotRecord> FindLocked(const std::string& id_or_alias) const;
    mutable std::mutex mu_;
    std::map<std::string, SnapshotRecord> by_id_;
    std::map<std::string, std::string>    alias_to_id_;
};

// Rust: backends/posixfs/ — construct `posixfs::PosixFsSnapshotRepository`
// directly (see `posixfs/backend.h`). There is deliberately no
// `MakePosixFsRepository(root_dir)` factory: the backend needs a catalog store
// *and* an artifact store, and the one that used to be declared here took only
// a root, ignored it, and handed back an in-memory stub.

/// Rust `OssRepository::from_config` — an object-storage repository.
///
/// Takes the configuration section rather than a URL, as upstream does: a
/// bucket URL alone cannot carry the endpoint, region and credentials, so the
/// previous signature could never have constructed one.
///
/// Fails rather than degrading: an incomplete `[backend.oss]` section, a
/// `credential_process`, or a build without libcurl are all reported here
/// instead of at the first request. The returned repository serves documents
/// and blobs; its snapshot *record* methods report `Unsupported` (see
/// `oss/repository.h`).
RepositoryResult<std::shared_ptr<SnapshotRepository> > MakeOssRepository(
    const cfg::OssBackendConfig& config);

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_BACKENDS_H_
