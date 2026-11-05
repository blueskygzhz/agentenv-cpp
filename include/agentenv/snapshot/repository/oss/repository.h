// SPDX-License-Identifier: MIT
// Rust: src/repository/backends/oss/repository.rs — `OssRepository`.
//
// The committed-state layer of the object-storage repository: the document
// half from `documents.h` and the blob half from `client.h`, assembled behind
// the one interface the rest of the tree uses.
//
// What upstream's `Repository` trait does *not* contain is the record half.
// `create` / `get` / `list` / `delete` / `resolve_alias` / `try_start_build` /
// `mark_build_error` come from the blanket `impl<R: Repository> SnapshotCatalog
// for R` in `src/snapshot/catalog.rs`, which stores each record as an ordinary
// *document* and is therefore identical on every backend. This port merged
// that layer into `SnapshotRepository`, and its POSIX backend answers the
// record methods from its own on-disk layout instead, so there is no shared
// document-backed catalog to reuse here yet. Those methods report
// `Unsupported` rather than growing a second record layout: records written
// under a key space the Rust server does not read would be invisible to it,
// and cross-node sharing is the entire reason this backend exists.
//
// The document half is complete, which is what makes the volume catalog
// (`volume/catalog.h`, built on documents exactly as upstream) usable over
// object storage today.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_OSS_REPOSITORY_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_OSS_REPOSITORY_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cstddef>

#include "agentenv/core/optional.h"
#include "agentenv/snapshot/repository/interfaces.h"
#include "agentenv/snapshot/repository/oss/client.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace oss {

/// Uploads a local file as one object.
///
/// Its own interface because the multipart implementation this tree already
/// has — `storage::overlaybd::OssBackend::UploadPath` — signs and sends
/// through the global libcurl transport and cannot be driven by the client's
/// injected one. Keeping it behind a seam lets the repository's *routing* (a
/// byte body goes to a plain `PUT`, a file body goes multipart) be tested
/// without a server, while the protocol itself stays the single already
/// reviewed implementation rather than a second copy.
///
/// Multipart is not an optimisation here: a sealed layer can exceed the 5 GiB
/// single-`PUT` ceiling, and a single `PUT` would hold the whole object in
/// memory.
class ObjectUploader {
 public:
    virtual ~ObjectUploader() {}

    /// `object_url` is the `s3://<bucket>/<key>` form `OssClient::ObjectUrl`
    /// returns, so a located blob and an uploaded one are named identically.
    virtual RepositoryResult<core::Unit> UploadFile(const std::string& object_url,
                                                    const std::string& path) = 0;
};

/// The production uploader: multipart through the overlaybd OSS backend.
RepositoryResult<std::shared_ptr<ObjectUploader> > MakeOssBackendUploader(
    const OssClientConfig& config);

/// Rust `OssRepository`.
class OssSnapshotRepository : public SnapshotRepository {
 public:
    /// `uploader` may be null, in which case a file-bodied `PutBlob` reports
    /// `Unsupported` instead of silently reading a layer into memory.
    OssSnapshotRepository(std::shared_ptr<OssClient> client,
                          std::shared_ptr<ObjectUploader> uploader)
        : client_(client), uploader_(uploader) {}

    // ---- snapshot records: see the file comment -------------------------

    RepositoryResult<SnapshotRecord> Create(const SnapshotRecord& record);
    RepositoryResult<core::Optional<SnapshotRecord> > Get(const std::string& id_or_alias);
    RepositoryResult<std::vector<SnapshotRecord> > List(const SnapshotListFilter& filter);
    RepositoryResult<core::Unit> Delete(const std::string& id_or_alias);
    RepositoryResult<core::Optional<core::SnapshotId> > ResolveAlias(const std::string& alias);
    RepositoryResult<SnapshotRecord> TryStartBuild(const core::SnapshotId& id);
    RepositoryResult<core::Unit> MarkBuildError(const core::SnapshotId& id,
                                                const TemplateBuildErrorReason& reason);

    // ---- documents ------------------------------------------------------

    RepositoryResult<core::Optional<std::string> > GetDocument(const DocumentKey& key);
    RepositoryResult<core::Unit> PutDocument(const DocumentKey& key, const std::string& bytes);
    RepositoryResult<core::Optional<std::string> > UpdateDocument(const DocumentKey& key,
                                                                  const DocumentUpdate& update);
    RepositoryResult<core::Unit> DeleteDocument(const DocumentKey& key);
    RepositoryResult<std::vector<std::string> > ListDocuments(
        DocumentCollection collection, const core::Optional<std::string>& after,
        std::size_t limit);
    RepositoryResult<std::vector<std::pair<std::string, std::string> > > ReadDocuments(
        DocumentCollection collection);

    // ---- blobs ----------------------------------------------------------

    core::Optional<std::string> ManagedLayersRepoBlobUrl() const;
    RepositoryResult<core::Unit> PutBlob(const BlobKey& key, const BlobBody& body);
    RepositoryResult<BlobLocation> LocateBlob(const BlobKey& key) const;
    RepositoryResult<bool> HasBlob(const BlobKey& key);
    RepositoryResult<std::string> ReadBlob(const BlobKey& key);
    RepositoryResult<uint64_t> FetchBlob(const BlobKey& key, const std::string& destination);
    RepositoryResult<core::Unit> DeleteSnapshotBlobs(const core::SnapshotId& id);

 private:
    std::shared_ptr<OssClient> client_;
    std::shared_ptr<ObjectUploader> uploader_;
};

}  // namespace oss
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_OSS_REPOSITORY_H_
