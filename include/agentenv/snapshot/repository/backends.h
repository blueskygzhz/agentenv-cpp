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

namespace oss {
class OssClient;
}  // namespace oss

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
///
/// `publish_compression` is `AppConfig.snapshot.publish_compression`, and is
/// **required** rather than defaulted, matching Rust's `from_config`. It is
/// the switch that decides whether published layers are recontainerized as
/// zfile, so a caller that does not name it has not decided what this
/// repository uploads -- and the safe-looking default (the config struct's
/// own, which has `enabled = true`) would turn compression *on* for a caller
/// that said nothing. The `OssSnapshotRepository` constructor does default
/// its policy, because there the fallback is `PublishPolicy()`, which is
/// passthrough.
/// `client`, when non-null, also receives the `OssClient` the repository was
/// built over. Added for `SnapshotImageService`, which has to read managed
/// layer objects *directly* — it downloads them to feed `regctl blob put`,
/// which the repository interface has no operation for. It must be the **same
/// client**, not a second one built from the same config: two would each
/// carry their own credential refresh and connection state, so a token
/// rotation observed by one would leave the other signing with a stale one.
RepositoryResult<std::shared_ptr<SnapshotRepository> > MakeOssRepository(
    const cfg::OssBackendConfig& config,
    const cfg::SnapshotPublishCompressionConfig& publish_compression,
    std::shared_ptr<oss::OssClient>* client = 0);

/// Rust `build_snapshot_backend_from_config` (`snapshot/backends/mod.rs:57`)
/// — the repository the node runs on, selected by
/// `snapshot.repository_backend`.
///
/// **Selected by the declared backend, not by which sections happen to be
/// present.** That distinction is the bug this function exists to fix: the
/// startup path used to branch on `backend.posix_fs.has_value()`, and config
/// normalisation only auto-inserts that section when the backend *is*
/// posix-fs (`cfg.cc:578`). A node declaring `repository_backend = "oss"`
/// that still carried a `[backend.posix_fs]` section -- the ordinary state of
/// a node migrated from posix-fs -- therefore published to its local disk
/// while believing it published to the bucket.
///
/// A missing section for the declared backend is an error, matching Rust's
/// `.context("backend.oss config is required when repository_backend = oss")`.
/// Degrading to the other backend is exactly the failure above, and degrading
/// to no repository would disable volumes on a node whose config asked for
/// one.
///
/// Deviation from Rust, stated because the signature hides it: upstream
/// returns the repository *and* a `SnapshotRuntimeResolver` as a pair, built
/// over a `LocalArtifactCache`. Neither the resolver nor that cache is ported,
/// so this returns the repository alone.
RepositoryResult<std::shared_ptr<SnapshotRepository> > MakeRepositoryFromConfig(
    const cfg::AppConfig& config);

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_BACKENDS_H_
