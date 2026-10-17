// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/backend.rs
//
// Bundles the durable POSIX repository state (catalog + artifact stores) with
// the node-local runtime resolver.
//
// Porting note. Rust's `PosixFsBackend` wraps both stores in a
// `PosixFsSnapshotRepository` that implements the `SnapshotRepository` trait,
// and so does this port now. That adapter was blocked for a long time because
// `interfaces.h` carried a cut-down `SnapshotRecord` of its own, which no
// object could satisfy alongside the catalog store's real one; with the two
// unified, the adapter is a plain forward.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_BACKEND_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_BACKEND_H_

#include <memory>
#include <string>

#include "agentenv/core/optional.h"
#include "agentenv/snapshot/artifact_cache.h"
// `SnapshotRepository` — `PosixFsSnapshotRepository` below implements it.
#include "agentenv/snapshot/repository/interfaces.h"
#include "agentenv/snapshot/repository/posixfs/artifacts.h"
#include "agentenv/snapshot/repository/posixfs/catalog.h"
#include "agentenv/snapshot/repository/posixfs/runtime.h"
#include "agentenv/snapshot/runtime_support.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {

/// Rust `struct PosixFsBackendConfig`.
struct PosixFsBackendConfig {
    /// Durable repository root; may live on a shared filesystem.
    std::string root;
    /// Node-local cache root for downloaded/materialised artifacts.
    core::Optional<std::string> cache_root;
    /// Node-local root for runtime-materialised files. Defaults to
    /// `<cache_root>/runtime`, matching Rust.
    core::Optional<std::string> runtime_cache_root;
};

/// Rust `shared_runtime_cache_root` — the node-local default.
std::string SharedRuntimeCacheRoot();

/// Rust `PosixFsSnapshotRepository` (backend.rs).
///
/// The `SnapshotRepository` face of the posix-fs backend. `PosixFsBackend`
/// itself is only a bag of parts — it does not implement the trait, and in
/// Rust it does not either; `backend.repository()` hands back this separate
/// type, which forwards each trait method to the catalog store.
///
/// Having it be a distinct object is what lets the catalog be shared: the
/// runtime resolver and the artifact store hold the same `catalog_store_`
/// without going through the repository interface.
class PosixFsSnapshotRepository : public SnapshotRepository {
 public:
    explicit PosixFsSnapshotRepository(std::shared_ptr<PosixFsCatalogStore> catalog_store)
        : catalog_store_(std::move(catalog_store)) {}

    // ---- snapshot records -------------------------------------------------

    RepositoryResult<SnapshotRecord> Create(const SnapshotRecord& record) override;
    RepositoryResult<core::Optional<SnapshotRecord> >
        Get(const std::string& id_or_alias) override;
    RepositoryResult<std::vector<SnapshotRecord> >
        List(const SnapshotListFilter& filter) override;
    RepositoryResult<core::Unit> Delete(const std::string& id_or_alias) override;
    RepositoryResult<core::Optional<core::SnapshotId> >
        ResolveAlias(const std::string& alias) override;
    RepositoryResult<SnapshotRecord> TryStartBuild(const core::SnapshotId& id) override;
    RepositoryResult<core::Unit>
        MarkBuildError(const core::SnapshotId& id,
                       const TemplateBuildErrorReason& reason) override;

    // ---- volume catalog ---------------------------------------------------
    //
    // The base class defaults these to "unsupported". Forwarded here because
    // the catalog store does implement them durably, and leaving the defaults
    // would make a configured posix-fs node report no volume support at all.

    RepositoryResult<core::Optional<volume::VolumeRecord> >
        GetVolume(const std::string& reference) override;
    RepositoryResult<VolumeRecordPage>
        ListVolumesPage(const core::Optional<std::string>& after_volume_id,
                        std::size_t limit) override;
    RepositoryResult<core::Unit> CreateVolume(const volume::VolumeRecord& record) override;
    RepositoryResult<core::Unit> PutVolume(const volume::VolumeRecord& record) override;
    RepositoryResult<core::Unit> DeleteVolume(const std::string& volume_id) override;

 private:
    std::shared_ptr<PosixFsCatalogStore> catalog_store_;
};

/// Rust `struct PosixFsBackend`.
class PosixFsBackend {
 public:
    /// Rust `PosixFsBackend::from_parts`.
    PosixFsBackend(const PosixFsBackendConfig& config,
                   const OverlaybdLayerStore* store,
                   std::shared_ptr<LocalArtifactCache> cache);

    const std::shared_ptr<PosixFsCatalogStore>&  catalog_store() const { return catalog_store_; }
    const std::shared_ptr<PosixFsArtifactStore>& artifact_store() const { return artifact_store_; }

    /// Rust `repository()` — the `SnapshotRepository` face over this
    /// backend's catalog. Built on first use and shared thereafter, so every
    /// caller talks to the same catalog and its file locks.
    std::shared_ptr<SnapshotRepository> repository();

    /// Rust `runtime_resolver`.
    const std::shared_ptr<PosixFsRuntimeResolver>& runtime_resolver() const {
        return runtime_resolver_;
    }

 private:
    std::shared_ptr<PosixFsCatalogStore>   catalog_store_;
    std::shared_ptr<PosixFsArtifactStore>  artifact_store_;
    std::shared_ptr<PosixFsRuntimeResolver> runtime_resolver_;
    /// Built lazily by `repository()`.
    std::shared_ptr<SnapshotRepository>    repository_;
};

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_BACKEND_H_
