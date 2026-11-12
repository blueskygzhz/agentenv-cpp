// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/backend.rs
#include "agentenv/snapshot/repository/posixfs/backend.h"

#include "agentenv/core/fs.h"
#include "agentenv/snapshot/repository/record_catalog.h"

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
                               const image::cache::OverlaybdLayerStore* store,
                               std::shared_ptr<LocalArtifactCache> cache) {
    const std::string cache_root =
        config.cache_root.has_value() ? *config.cache_root : SharedRuntimeCacheRoot();
    const std::string runtime_cache_root = config.runtime_cache_root.has_value()
        ? *config.runtime_cache_root
        : cache_root + "/runtime";

    catalog_store_.reset(new PosixFsCatalogStore(config.root));
    artifact_store_.reset(new PosixFsArtifactStore(config.root));
    // Built here rather than on first use, because the resolver now takes the
    // repository itself instead of a root path -- `RepositoryRuntimeResolver`
    // asks `LocateBlob` where each artifact is. Upstream's `from_parts`
    // constructs them in this order for the same reason.
    repository_.reset(new PosixFsSnapshotRepository(catalog_store_, artifact_store_));
    runtime_resolver_.reset(new RepositoryRuntimeResolver(
        repository_, cache, runtime_cache_root, store,
        std::shared_ptr<p2p::P2pTransport>()));
}


std::shared_ptr<SnapshotRepository> PosixFsBackend::repository() {
    // Shared, not rebuilt per call: the catalog serialises publishes through
    // on-disk file locks, and handing out independent repositories would be
    // correct but would hide that they contend on the same locks.
    if (!repository_) {
        repository_.reset(new PosixFsSnapshotRepository(catalog_store_, artifact_store_));
    }
    return repository_;
}

// ---------------------------------------------------------------------------
// Rust `impl SnapshotRepository for PosixFsSnapshotRepository`
//
// The record methods forward to the **shared** catalog (`record_catalog.h`),
// which is where they live upstream: `backends/posixfs/catalog.rs` has only
// document methods, and records come from the backend-agnostic
// `impl<R: Repository> SnapshotCatalog for R`. This backend used to carry its
// own second implementation of them.
//
// Nothing about the on-disk layout changes. The shared layer writes a record
// through `PutDocument`/`UpdateDocument`, which resolve
// `DocumentKey::SnapshotRecord(id).PosixPath()` to `catalog/records/<id>.json`
// and lock `catalog/records/<id>.lock` -- byte for byte the paths the removed
// code used, including `catalog/aliases/<name>` for an alias binding and its
// `"<uuid>"` JSON body.
//
// What *does* change is behaviour, and every difference was a divergence from
// upstream: a record marked `deleting` is now hidden from `Get`, `List`,
// `ResolveAlias` and both build transitions (it used to be handed out and
// could be started as a build while its files were being removed); a failed
// alias claim in `Create` now rolls the record back; a record is encoded
// pretty, as `encode_record` does; and `Delete` now performs upstream's full
// sequence instead of just dropping the record.
//
// The forwarding cannot recurse: `record_catalog` calls only the document and
// blob methods below.
// ---------------------------------------------------------------------------

RepositoryResult<SnapshotRecord>
PosixFsSnapshotRepository::Create(const SnapshotRecord& record) {
    return record_catalog::Create(*this, record);
}

RepositoryResult<core::Optional<SnapshotRecord> >
PosixFsSnapshotRepository::Get(const std::string& id_or_alias) {
    return record_catalog::Get(*this, id_or_alias);
}

RepositoryResult<std::vector<SnapshotRecord> >
PosixFsSnapshotRepository::List(const SnapshotListFilter& filter) {
    return record_catalog::List(*this, filter);
}

RepositoryResult<core::Unit>
PosixFsSnapshotRepository::Delete(const std::string& id_or_alias) {
    return record_catalog::Delete(*this, id_or_alias);
}

RepositoryResult<core::Optional<core::SnapshotId> >
PosixFsSnapshotRepository::ResolveAlias(const std::string& alias) {
    return record_catalog::ResolveAlias(*this, alias);
}

RepositoryResult<SnapshotRecord>
PosixFsSnapshotRepository::TryStartBuild(const core::SnapshotId& id) {
    return record_catalog::TryStartBuild(*this, id);
}

RepositoryResult<core::Unit>
PosixFsSnapshotRepository::MarkBuildError(const core::SnapshotId& id,
                                          const TemplateBuildErrorReason& reason) {
    return record_catalog::MarkBuildError(*this, id, reason);
}

RepositoryResult<core::Optional<std::string> > PosixFsSnapshotRepository::GetDocument(
    const DocumentKey& key) {
    return catalog_store_->GetDocument(key);
}

RepositoryResult<core::Unit> PosixFsSnapshotRepository::PutDocument(const DocumentKey& key,
                                                                    const std::string& bytes) {
    return catalog_store_->PutDocument(key, bytes);
}

RepositoryResult<core::Optional<std::string> > PosixFsSnapshotRepository::UpdateDocument(
    const DocumentKey& key, const DocumentUpdate& update) {
    return catalog_store_->UpdateDocument(key, update);
}

RepositoryResult<core::Unit> PosixFsSnapshotRepository::DeleteDocument(const DocumentKey& key) {
    return catalog_store_->DeleteDocument(key);
}

RepositoryResult<std::vector<std::string> > PosixFsSnapshotRepository::ListDocuments(
    DocumentCollection collection, const core::Optional<std::string>& after,
    std::size_t limit) {
    return catalog_store_->ListDocuments(collection, after, limit);
}

RepositoryResult<std::vector<std::pair<std::string, std::string> > >
PosixFsSnapshotRepository::ReadDocuments(DocumentCollection collection) {
    return catalog_store_->ReadDocuments(collection);
}

RepositoryResult<core::Unit> PosixFsSnapshotRepository::PutBlob(const BlobKey& key,
                                                                const BlobBody& body) {
    if (!artifact_store_) return SnapshotRepository::PutBlob(key, body);
    return artifact_store_->PutBlob(key, body);
}

RepositoryResult<BlobLocation> PosixFsSnapshotRepository::LocateBlob(const BlobKey& key) const {
    if (!artifact_store_) return SnapshotRepository::LocateBlob(key);
    return BlobLocation::Local(artifact_store_->BlobPath(key));
}

RepositoryResult<bool> PosixFsSnapshotRepository::HasBlob(const BlobKey& key) {
    if (!artifact_store_) return SnapshotRepository::HasBlob(key);
    const std::string path = artifact_store_->BlobPath(key);
    const core::Expected<bool, std::string> exists = core::fs::TryExists(path);
    if (!exists.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("check '" + path + "'", exists.error()));
    }
    return exists.value();
}

RepositoryResult<std::string> PosixFsSnapshotRepository::ReadBlob(const BlobKey& key) {
    if (!artifact_store_) return SnapshotRepository::ReadBlob(key);
    const std::string path = artifact_store_->BlobPath(key);
    const core::Expected<bool, std::string> exists = core::fs::TryExists(path);
    if (exists.ok() && !exists.value()) {
        return core::make_unexpected(RepositoryError::ArtifactNotFound("'" + path + "'"));
    }
    const core::Expected<std::string, std::string> bytes = core::fs::ReadToString(path);
    if (!bytes.ok()) {
        return core::make_unexpected(RepositoryError::Backend("read '" + path + "'", bytes.error()));
    }
    return bytes.value();
}

RepositoryResult<uint64_t> PosixFsSnapshotRepository::FetchBlob(const BlobKey& key,
                                                                const std::string& destination) {
    if (!artifact_store_) return SnapshotRepository::FetchBlob(key, destination);
    return artifact_store_->FetchBlob(key, destination);
}

RepositoryResult<core::Unit> PosixFsSnapshotRepository::DeleteSnapshotBlobs(
    const core::SnapshotId& id) {
    if (!artifact_store_) return SnapshotRepository::DeleteSnapshotBlobs(id);
    return artifact_store_->DeleteSnapshotFiles(id);
}

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
