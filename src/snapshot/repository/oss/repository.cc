// SPDX-License-Identifier: MIT
// Rust: src/repository/backends/oss/repository.rs
#include "agentenv/snapshot/repository/oss/repository.h"

#include "agentenv/snapshot/repository/oss/documents.h"
#include "agentenv/snapshot/repository/record_catalog.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/oss.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace oss {

namespace sov = storage::overlaybd;

namespace {

/// Rust `OSS_SEALED_UPLOAD_PART_SIZE` / `OSS_SEALED_UPLOAD_CONCURRENCY`
/// (storage/overlaybd/src/image/image_service.rs), which upload the same kind
/// of payload: one sealed layer, read from a local file.
const size_t kUploadPartSize = 64u * 1024u * 1024u;
const size_t kUploadConcurrency = 4;

/// The one record operation that still cannot be served.
RepositoryError NoSnapshotDeletion(const std::string& lookup) {
    return RepositoryError::Unsupported(
        "deleting snapshot '" + lookup +
        "'; removing its artifacts needs Rust snapshot::store::delete_snapshot "
        "(Repository::delete_snapshot_blobs plus the ACR publication rollback), "
        "which is not ported");
}

/// Multipart upload through the overlaybd OSS backend.
class OssBackendUploader : public ObjectUploader {
 public:
    explicit OssBackendUploader(std::shared_ptr<sov::OssBackend> backend) : backend_(backend) {}

    RepositoryResult<core::Unit> UploadFile(const std::string& object_url,
                                            const std::string& path) {
        const core::Expected<core::Unit, std::string> uploaded =
            backend_->UploadPath(object_url, path, kUploadPartSize, kUploadConcurrency, 0);
        if (!uploaded.ok()) {
            return core::make_unexpected(
                RepositoryError::Backend("upload '" + object_url + "'", uploaded.error()));
        }
        return core::Unit();
    }

 private:
    std::shared_ptr<sov::OssBackend> backend_;
};

}  // namespace

RepositoryResult<std::shared_ptr<ObjectUploader> > MakeOssBackendUploader(
    const OssClientConfig& config) {
    sov::OssConfig oss;
    oss.enable = true;
    oss.access_key_id = config.credentials.access_key_id;
    oss.secret_access_key = config.credentials.secret_access_key;
    oss.security_token = config.credentials.security_token;
    oss.default_region = config.region;
    oss.default_endpoint = config.endpoint;
    // Passed explicitly when configured rather than left to the backend's own
    // detection: the two detectors agree today, and a disagreement would send
    // the upload to a different URL than `ObjectUrl` reports for the same key.
    if (config.addressing_style.has_value()) {
        oss.default_addressing_style =
            *config.addressing_style == sov::kAddressingVirtual ? "virtual" : "path";
    }
    oss.timeout_secs = static_cast<uint64_t>(config.timeout_secs);
    oss.retry_count = config.retry_count;

    const core::Expected<std::shared_ptr<sov::OssBackend>, std::string> backend =
        sov::OssBackend::New(oss);
    if (!backend.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("construct the oss upload backend", backend.error()));
    }
    return std::shared_ptr<ObjectUploader>(new OssBackendUploader(backend.value()));
}

// ---- snapshot records -----------------------------------------------------
//
// Forwarded to the shared catalog, which keeps them in the same document key
// space every backend uses -- the only reason a record written here is
// readable by a node on another backend. The forwarding cannot recurse:
// `record_catalog` calls only the *document* methods below.

RepositoryResult<SnapshotRecord> OssSnapshotRepository::Create(const SnapshotRecord& record) {
    return record_catalog::Create(*this, record);
}

RepositoryResult<core::Optional<SnapshotRecord> > OssSnapshotRepository::Get(
    const std::string& id_or_alias) {
    return record_catalog::Get(*this, id_or_alias);
}

RepositoryResult<std::vector<SnapshotRecord> > OssSnapshotRepository::List(
    const SnapshotListFilter& filter) {
    return record_catalog::List(*this, filter);
}

RepositoryResult<core::Unit> OssSnapshotRepository::Delete(const std::string& id_or_alias) {
    // The only record method still refused, and refused *before* mutating
    // anything: upstream marks the record, frees the alias and then removes
    // the artifacts. Doing the first two and skipping the third would leave
    // the bucket holding files nothing references, while reporting that the
    // snapshot was deleted.
    return core::make_unexpected(NoSnapshotDeletion(id_or_alias));
}

RepositoryResult<core::Optional<core::SnapshotId> > OssSnapshotRepository::ResolveAlias(
    const std::string& alias) {
    return record_catalog::ResolveAlias(*this, alias);
}

RepositoryResult<SnapshotRecord> OssSnapshotRepository::TryStartBuild(const core::SnapshotId& id) {
    return record_catalog::TryStartBuild(*this, id);
}

RepositoryResult<core::Unit> OssSnapshotRepository::MarkBuildError(
    const core::SnapshotId& id, const TemplateBuildErrorReason& reason) {
    return record_catalog::MarkBuildError(*this, id, reason);
}

// ---- documents ------------------------------------------------------------

RepositoryResult<core::Optional<std::string> > OssSnapshotRepository::GetDocument(
    const DocumentKey& key) {
    return oss::GetDocument(*client_, key);
}

RepositoryResult<core::Unit> OssSnapshotRepository::PutDocument(const DocumentKey& key,
                                                                const std::string& bytes) {
    return oss::PutDocument(client_.get(), key, bytes);
}

RepositoryResult<core::Optional<std::string> > OssSnapshotRepository::UpdateDocument(
    const DocumentKey& key, const DocumentUpdate& update) {
    return oss::UpdateDocument(client_.get(), key, update);
}

RepositoryResult<core::Unit> OssSnapshotRepository::DeleteDocument(const DocumentKey& key) {
    return oss::DeleteDocument(client_.get(), key);
}

RepositoryResult<std::vector<std::string> > OssSnapshotRepository::ListDocuments(
    DocumentCollection collection, const core::Optional<std::string>& after,
    std::size_t limit) {
    return oss::ListDocuments(*client_, collection, after, limit);
}

RepositoryResult<std::vector<std::pair<std::string, std::string> > >
OssSnapshotRepository::ReadDocuments(DocumentCollection collection) {
    return oss::ReadAllDocuments(*client_, collection);
}

// ---- blobs ----------------------------------------------------------------

core::Optional<std::string> OssSnapshotRepository::ManagedLayersRepoBlobUrl() const {
    // Always set, unlike the POSIX backend's: a reader on another node cannot
    // reach these layers by path, so the URL is the only way to address them.
    return core::Optional<std::string>(client_->ObjectUrl(BlobKey::kLayersDir));
}

RepositoryResult<core::Unit> OssSnapshotRepository::PutBlob(const BlobKey& key,
                                                            const BlobBody& body) {
    const std::string object_key = key.Path();

    if (key.kind() == BlobKey::Kind::Layer) {
        // A layer is named by its digest, so two writers of one layer write
        // identical bytes and this `exists -> put` race is benign. The check
        // is what keeps a re-publish from re-sending gigabytes that are
        // already stored.
        const RepositoryResult<bool> exists = client_->Exists(object_key);
        // Propagated rather than re-wrapped: the client's message already
        // names the operation, the key and the status, and a second layer of
        // prose would bury them.
        if (!exists.ok()) return core::make_unexpected(exists.error());
        if (exists.value()) return core::Unit();
    }

    if (body.kind == BlobBody::Kind::Bytes) {
        // Unconditional, as upstream: Alibaba OSS rejects
        // `x-oss-forbid-overwrite` on the multipart uploads the file path
        // uses, so neither path can rely on a create-only precondition.
        return client_->Put(object_key,
                            std::string(body.bytes.begin(), body.bytes.end()));
    }

    if (!uploader_) {
        // A layer is routinely gigabytes. Reading it into memory to reuse the
        // byte path would work on a test fixture and exhaust the node on a
        // real one, so the gap is reported instead.
        return core::make_unexpected(
            RepositoryError::Unsupported("multipart upload for '" + object_key +
                                         "'; this repository was built without an uploader"));
    }
    return uploader_->UploadFile(client_->ObjectUrl(object_key), body.path);
}

RepositoryResult<BlobLocation> OssSnapshotRepository::LocateBlob(const BlobKey& key) const {
    // Never `Local`: the bytes are not reachable by path from any node, which
    // is what makes `FetchBlob` the only way to obtain them.
    return BlobLocation::Remote(client_->ObjectUrl(key.Path()));
}

RepositoryResult<bool> OssSnapshotRepository::HasBlob(const BlobKey& key) {
    return client_->Exists(key.Path());
}

RepositoryResult<std::string> OssSnapshotRepository::ReadBlob(const BlobKey& key) {
    const std::string object_key = key.Path();
    const RepositoryResult<ObjectRead> read = client_->GetWithEtag(object_key);
    if (!read.ok()) return core::make_unexpected(read.error());
    switch (read.value().kind) {
        case ObjectRead::Kind::Found:
            return read.value().bytes;
        case ObjectRead::Kind::NotFound:
            // Distinct from a transport failure: the caller is deciding
            // whether the artifact was ever published.
            return core::make_unexpected(RepositoryError::ArtifactNotFound("'" + object_key + "'"));
        case ObjectRead::Kind::Changed:
            break;
    }
    // Unreachable through `OssClient`, whose single `GET` cannot observe a
    // change, but the interface admits a store that can. Reported rather than
    // retried or returned empty: the bytes were never obtained.
    return core::make_unexpected(
        RepositoryError::Backend("oss read '" + object_key + "' raced a concurrent write"));
}

RepositoryResult<uint64_t> OssSnapshotRepository::FetchBlob(const BlobKey& key,
                                                            const std::string& destination) {
    return client_->GetToFile(key.Path(), destination);
}

}  // namespace oss
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
