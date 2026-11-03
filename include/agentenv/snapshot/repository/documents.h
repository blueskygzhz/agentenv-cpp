// SPDX-License-Identifier: MIT
// Rust: src/repository/layout.rs (`DocumentKey`) and the document half of
// src/repository/interfaces.rs (`DocumentChange`, `DocumentUpdate`).
//
// The C++ repository was ported from the older `snapshot::repository` layout,
// which had no generic document store. These are the shared-document pieces
// of the newer Rust repository, added to the same namespace so the existing
// `SnapshotRepository` can carry them as Rust's single `Repository` trait does.
//
// `DocumentKey` is the only place that names document paths; changing one
// strands the data already in existing repositories.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_DOCUMENTS_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_DOCUMENTS_H_

#include <functional>
#include <string>

#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/image_tag.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/types.h"

namespace agentenv {
namespace snapshot {
namespace repository {

/// Rust enum `DocumentKey`. A tagged class: only the payload selected by
/// `kind()` is meaningful, and the factories are the only way to build one,
/// so an `Image` key always carries a validated digest (Rust's private
/// `ImageDigest`).
class DocumentKey {
 public:
    enum class Kind {
        BuildLog,
        BuildCacheState,
        VolumeRecord,
        VolumeAlias,
        SnapshotRecord,
        SnapshotAlias,
        Image,
        ImageTag,
        ImageBuild,
    };

    static DocumentKey BuildLog(const core::SnapshotId& id);
    static DocumentKey BuildCacheState();
    static DocumentKey VolumeRecord(const std::string& id);
    static DocumentKey VolumeAlias(const std::string& name);
    static DocumentKey SnapshotRecord(const core::SnapshotId& id);
    static DocumentKey SnapshotAlias(const snapshot::SnapshotAlias& alias);
    /// Rust `DocumentKey::image` — `InvalidRequest` unless `digest` is
    /// `sha256:` followed by 64 lowercase hex digits, which is what keeps a
    /// hostile digest from naming a path outside the repository.
    static RepositoryResult<DocumentKey> Image(const std::string& digest);
    static DocumentKey ImageTag(const snapshot::ImageTag& tag);
    static DocumentKey ImageBuild(const core::SnapshotId& id);

    Kind kind() const { return kind_; }

    /// Rust `path` — the path relative to the repository root on every backend.
    std::string Path() const;
    /// Rust `posix_path` — differs only for aliases, whose POSIX layout
    /// predates the shared one.
    std::string PosixPath() const;
    /// Rust `posix_lock_path` — the lock file that serializes POSIX document
    /// updates across nodes. Some are kept at historical paths so nodes on
    /// older versions still exclude each other.
    std::string PosixLockPath() const;

 private:
    DocumentKey(Kind kind, const std::string& value) : kind_(kind), value_(value) {}

    Kind kind_;
    /// The id, name, alias or digest, already rendered as Rust's `Display`.
    std::string value_;
    /// Set for `Kind::ImageTag` only.
    core::Optional<snapshot::ImageTag> tag_;
};

/// Rust enum `DocumentCollection` (layout.rs) — documents that are listed
/// together, in id order.
enum class DocumentCollection {
    VolumeRecords,
    SnapshotRecords,
};

/// Rust `DocumentCollection::prefix` — the directory or object prefix that
/// holds every document in the collection.
const char* DocumentCollectionPrefix(DocumentCollection collection);

/// Rust `DocumentCollection::key` — the key of the document with `id`; unset
/// when `id` is not valid for the collection.
core::Optional<DocumentKey> DocumentCollectionKey(DocumentCollection collection,
                                                  const std::string& id);

/// Rust `DocumentCollection::id_from_file_name` — the id from a file name
/// under the collection's prefix; unset for anything that is not a document
/// (lock files share the directory).
core::Optional<std::string> DocumentCollectionIdFromFileName(DocumentCollection collection,
                                                             const std::string& file_name);

/// Rust enum `DocumentChange` — what an update does to a document.
struct DocumentChange {
    enum class Kind { Keep, Put, Delete };

    Kind kind;
    /// Set for `Put`.
    std::string bytes;

    static DocumentChange Keep() { return DocumentChange(Kind::Keep, std::string()); }
    static DocumentChange Put(const std::string& bytes) { return DocumentChange(Kind::Put, bytes); }
    static DocumentChange Delete() { return DocumentChange(Kind::Delete, std::string()); }

 private:
    DocumentChange(Kind k, const std::string& b) : kind(k), bytes(b) {}
};

/// Rust `DocumentUpdate` — decides how to change a document from its current
/// bytes, if any. A backend may call it again when a concurrent writer wins,
/// so it must be a pure function of its input.
typedef std::function<RepositoryResult<DocumentChange>(const core::Optional<std::string>&)>
    DocumentUpdate;

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_DOCUMENTS_H_
