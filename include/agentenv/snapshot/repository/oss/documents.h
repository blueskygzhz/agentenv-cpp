// SPDX-License-Identifier: MIT
// Rust: src/repository/backends/oss/documents.rs
//
// Shared documents on object storage. Each document is one object at
// `DocumentKey::Path()`, which nodes on other versions read and write too, so
// the paths are a compatibility contract rather than an implementation
// detail.
//
// Writers of one document must exclude each other or an update is lost. There
// are two mechanisms upstream, chosen by the store:
//
//   * S3-compatible stores (MinIO among them) replace a document only if its
//     ETag is unchanged — a compare-and-swap. That is what this file
//     implements.
//   * Alibaba's native OSS cannot write conditionally, so there every write
//     holds the document's lease. The lease is not ported yet, and the entry
//     points below refuse such a store instead of falling back to the CAS:
//     a precondition the store ignores is not a weaker guarantee, it is no
//     guarantee, and the lost update would be invisible.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_OSS_DOCUMENTS_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_OSS_DOCUMENTS_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "agentenv/core/optional.h"
#include "agentenv/snapshot/repository/documents.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/repository/oss/object_store.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace oss {

/// Rust `MAX_UPDATE_ATTEMPTS`.
extern const std::size_t kMaxUpdateAttempts;
/// Rust `Backoff::FIRST` and `Backoff::MAX`, in milliseconds.
extern const int64_t kBackoffFirstMs;
extern const int64_t kBackoffMaxMs;

/// Rust `Backoff` — jittered, growing pauses between attempts that lost to
/// another writer.
///
/// The jitter is not decoration: without it a set of nodes that collide once
/// retry in lockstep and collide again on every subsequent attempt. Each
/// delay is drawn from `[next/2, next]` and `next` then doubles up to a cap.
class Backoff {
 public:
    /// Draws a value in `[lo, hi]`. Injectable so a test can pin the delay
    /// rather than sample it.
    typedef std::function<int64_t(int64_t lo, int64_t hi)> Jitter;
    /// Waits for `ms`. Injectable so a test can exhaust every attempt without
    /// actually sleeping — otherwise pinning the 32-attempt limit would cost
    /// several seconds of wall clock per run.
    typedef std::function<void(int64_t ms)> Sleeper;

    Backoff() : next_ms_(kBackoffFirstMs) {}
    Backoff(const Jitter& jitter, const Sleeper& sleeper)
        : next_ms_(kBackoffFirstMs), jitter_(jitter), sleeper_(sleeper) {}

    /// Rust `wait` — sleeps one jittered interval and grows the next one.
    void Wait();

    /// The delay this call would sleep for, and the growth it would apply.
    /// Exposed so the schedule can be asserted directly.
    int64_t NextDelayMs() const { return next_ms_; }

 private:
    int64_t next_ms_;
    Jitter  jitter_;
    Sleeper sleeper_;
};

/// How the update loop retries. Defaults match Rust; a test overrides them to
/// stay deterministic and instant.
struct DocumentUpdatePolicy {
    std::size_t max_attempts;
    Backoff::Jitter jitter;
    Backoff::Sleeper sleeper;

    DocumentUpdatePolicy() : max_attempts(kMaxUpdateAttempts) {}
};

/// Rust `get` — `None` when the document does not exist.
RepositoryResult<core::Optional<std::string> > GetDocument(const ObjectStore& store,
                                                           const DocumentKey& key);

/// Rust `put` — unconditionally replaces one document.
///
/// Refuses an append-only store, whose upstream path takes a lease first.
RepositoryResult<core::Unit> PutDocument(ObjectStore* store, const DocumentKey& key,
                                         const std::string& bytes);

/// Rust `update` — applies `update` atomically against concurrent writers,
/// returning the document the decision was made from (its value *before* the
/// write, as the POSIX backend also returns).
///
/// Refuses an append-only store, for the reason in the file comment.
RepositoryResult<core::Optional<std::string> > UpdateDocument(
    ObjectStore* store, const DocumentKey& key, const DocumentUpdate& update,
    const DocumentUpdatePolicy& policy);

/// `UpdateDocument` with Rust's retry schedule.
RepositoryResult<core::Optional<std::string> > UpdateDocument(ObjectStore* store,
                                                              const DocumentKey& key,
                                                              const DocumentUpdate& update);

/// Rust `delete` — a missing document is success.
RepositoryResult<core::Unit> DeleteDocument(ObjectStore* store, const DocumentKey& key);

/// Rust `list` — up to `limit` document ids after `after`, in object key order.
RepositoryResult<std::vector<std::string> > ListDocuments(
    const ObjectStore& store, DocumentCollection collection,
    const core::Optional<std::string>& after, std::size_t limit);

/// Rust `document_id` — the id of the document stored at `key`.
///
/// Exposed because it is the one piece of the listing that can reject input:
/// a key under the collection's prefix but nested deeper (it still contains a
/// `/`) is not a document, and accepting it would invent an id from a path
/// component.
RepositoryResult<std::string> DocumentIdFromKey(DocumentCollection collection,
                                                const std::string& key);

}  // namespace oss
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_OSS_DOCUMENTS_H_
