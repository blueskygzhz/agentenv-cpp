// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/backend.rs
#include "agentenv/snapshot/repository/posixfs/backend.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {

std::string SharedRuntimeCacheRoot() {
    // Rust anchors the default under the process temp directory so it stays
    // node-local and survives across sandbox launches.
    return "/tmp/agentenv-snapshot-cache";
}

PosixFsBackend::PosixFsBackend(const PosixFsBackendConfig& config,
                               const OverlaybdLayerStore* store,
                               std::shared_ptr<LocalArtifactCache> cache) {
    const std::string cache_root =
        config.cache_root.has_value() ? *config.cache_root : SharedRuntimeCacheRoot();
    const std::string runtime_cache_root = config.runtime_cache_root.has_value()
        ? *config.runtime_cache_root
        : cache_root + "/runtime";

    catalog_store_.reset(new PosixFsCatalogStore(config.root));
    artifact_store_.reset(new PosixFsArtifactStore(config.root));
    runtime_resolver_.reset(
        new PosixFsRuntimeResolver(config.root, runtime_cache_root, store, cache));
}


std::shared_ptr<SnapshotRepository> PosixFsBackend::repository() {
    // Shared, not rebuilt per call: the catalog serialises publishes through
    // on-disk file locks, and handing out independent repositories would be
    // correct but would hide that they contend on the same locks.
    if (!repository_) {
        repository_.reset(new PosixFsSnapshotRepository(catalog_store_));
    }
    return repository_;
}

// ---------------------------------------------------------------------------
// Rust `impl SnapshotRepository for PosixFsSnapshotRepository`
//
// Each method is the `*_sync` body upstream: a forward to the catalog store.
// Now that the boundary types are the real ones, the forwards are direct --
// there is nothing left to convert.
// ---------------------------------------------------------------------------

RepositoryResult<SnapshotRecord>
PosixFsSnapshotRepository::Create(const SnapshotRecord& record) {
    return catalog_store_->Create(record);
}

RepositoryResult<core::Optional<SnapshotRecord> >
PosixFsSnapshotRepository::Get(const std::string& id_or_alias) {
    return catalog_store_->Get(id_or_alias);
}

RepositoryResult<std::vector<SnapshotRecord> >
PosixFsSnapshotRepository::List(const SnapshotListFilter& filter) {
    return catalog_store_->List(filter);
}

RepositoryResult<core::Unit>
PosixFsSnapshotRepository::Delete(const std::string& id_or_alias) {
    // Rust `delete_sync`: resolve first, because the store deletes by id while
    // the trait accepts an alias too. A missing record is success -- delete is
    // idempotent, so a retry after a partial failure must not start failing.
    const RepositoryResult<core::Optional<SnapshotRecord> > found =
        catalog_store_->Get(id_or_alias);
    if (!found.ok()) return core::make_unexpected(found.error());
    if (!found.value().has_value()) return core::Unit{};
    return catalog_store_->DeleteRecord(found.value()->id);
}

RepositoryResult<core::Optional<core::SnapshotId> >
PosixFsSnapshotRepository::ResolveAlias(const std::string& alias) {
    return catalog_store_->ResolveAlias(alias);
}

RepositoryResult<SnapshotRecord>
PosixFsSnapshotRepository::TryStartBuild(const core::SnapshotId& id) {
    return catalog_store_->TryStart(id);
}

RepositoryResult<core::Unit>
PosixFsSnapshotRepository::MarkBuildError(const core::SnapshotId& id,
                                          const TemplateBuildErrorReason& reason) {
    return catalog_store_->MarkError(id, reason);
}

RepositoryResult<core::Optional<volume::VolumeRecord> >
PosixFsSnapshotRepository::GetVolume(const std::string& reference) {
    return catalog_store_->GetVolume(reference);
}

RepositoryResult<VolumeRecordPage>
PosixFsSnapshotRepository::ListVolumesPage(
    const core::Optional<std::string>& after_volume_id, std::size_t limit) {
    return catalog_store_->ListVolumesPage(after_volume_id, limit);
}

RepositoryResult<core::Unit>
PosixFsSnapshotRepository::CreateVolume(const volume::VolumeRecord& record) {
    return catalog_store_->CreateVolume(record);
}

RepositoryResult<core::Unit>
PosixFsSnapshotRepository::PutVolume(const volume::VolumeRecord& record) {
    return catalog_store_->PutVolume(record);
}

RepositoryResult<core::Unit>
PosixFsSnapshotRepository::DeleteVolume(const std::string& volume_id) {
    return catalog_store_->DeleteVolume(volume_id);
}

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
