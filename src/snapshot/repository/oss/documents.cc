// SPDX-License-Identifier: MIT
// Rust: src/repository/backends/oss/documents.rs
#include "agentenv/snapshot/repository/oss/documents.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <ctime>

namespace agentenv {
namespace snapshot {
namespace repository {
namespace oss {

const std::size_t kMaxUpdateAttempts = 32;
const int64_t kBackoffFirstMs = 20;
const int64_t kBackoffMaxMs = 500;

namespace {

/// Rust `rand::rng().random_range(lo..=hi)`.
///
/// `rand()` is adequate here and deliberately not seeded from a global: the
/// value only has to decorrelate colliding writers, not resist prediction.
int64_t DefaultJitter(int64_t lo, int64_t hi) {
    if (hi <= lo) return lo;
    const int64_t span = hi - lo + 1;
    return lo + static_cast<int64_t>(static_cast<uint64_t>(::rand()) % static_cast<uint64_t>(span));
}

void DefaultSleep(int64_t ms) {
    if (ms <= 0) return;
    // Restarted on EINTR, as `core/file_lock.cc` does: a signal must shorten
    // the pause by the time already slept, not cancel it.
    timespec request;
    request.tv_sec = static_cast<time_t>(ms / 1000);
    request.tv_nsec = static_cast<long>((ms % 1000) * 1000000);
    while (::nanosleep(&request, &request) != 0 && errno == EINTR) {
        // Restart with the remaining time.
    }
}

/// Rust's `read` helper — a missing object is `None`, not an error.
///
/// `Changed` collapses to "nothing consistent was read" here, which only the
/// update loop distinguishes; a plain read has no version to reconcile and so
/// treats it as the absence of a usable answer and reports the bytes it did
/// get. The store never returns `Changed` from an unconditional read.
RepositoryResult<core::Optional<std::string> > ReadDocument(const ObjectStore& store,
                                                            const std::string& path) {
    const RepositoryResult<ObjectRead> read = store.GetWithEtag(path);
    if (!read.ok()) return core::make_unexpected(read.error());
    if (read.value().kind == ObjectRead::Kind::Found) {
        return core::Optional<std::string>(read.value().bytes);
    }
    return core::Optional<std::string>();
}

/// The guard both mutating entry points share.
///
/// An append-only store is Alibaba's native OSS, which ignores a write
/// precondition. Running the CAS against it would report success while
/// overwriting a concurrent update, so the operation is refused until the
/// lease path is ported.
RepositoryResult<core::Unit> RefuseAppendOnlyStore(const ObjectStore& store,
                                                   const std::string& path) {
    if (!store.CanAppend()) return core::Unit();
    return core::make_unexpected(RepositoryError::Unsupported(
        "writing '" + path +
        "' on an append-only object store requires the document lease, which is not "
        "available in this build"));
}

}  // namespace

void Backoff::Wait() {
    const int64_t lo = next_ms_ / 2;
    const int64_t hi = next_ms_;
    const int64_t delay = jitter_ ? jitter_(lo, hi) : DefaultJitter(lo, hi);
    // Grow before sleeping: the schedule must advance even if the sleeper is
    // a no-op, or an injected one would spin on the first interval forever.
    next_ms_ = std::min(next_ms_ * 2, kBackoffMaxMs);
    if (sleeper_) {
        sleeper_(delay);
    } else {
        DefaultSleep(delay);
    }
}

RepositoryResult<core::Optional<std::string> > GetDocument(const ObjectStore& store,
                                                           const DocumentKey& key) {
    return ReadDocument(store, key.Path());
}

RepositoryResult<core::Unit> PutDocument(ObjectStore* store, const DocumentKey& key,
                                         const std::string& bytes) {
    const std::string path = key.Path();
    const RepositoryResult<core::Unit> allowed = RefuseAppendOnlyStore(*store, path);
    if (!allowed.ok()) return allowed;
    const RepositoryResult<core::Unit> written = store->Put(path, bytes);
    if (!written.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("write '" + path + "'", written.error().message));
    }
    return core::Unit();
}

RepositoryResult<core::Optional<std::string> > UpdateDocument(
    ObjectStore* store, const DocumentKey& key, const DocumentUpdate& update,
    const DocumentUpdatePolicy& policy) {
    const std::string path = key.Path();
    const RepositoryResult<core::Unit> allowed = RefuseAppendOnlyStore(*store, path);
    if (!allowed.ok()) return core::make_unexpected(allowed.error());

    // Rust `update_if_unchanged`.
    Backoff backoff(policy.jitter, policy.sleeper);
    for (std::size_t attempt = 0; attempt < policy.max_attempts; ++attempt) {
        const RepositoryResult<ObjectRead> read = store->GetWithEtag(path);
        if (!read.ok()) {
            return core::make_unexpected(
                RepositoryError::Backend("read '" + path + "'", read.error().message));
        }

        core::Optional<std::string> current;
        core::Optional<std::string> etag;
        switch (read.value().kind) {
            case ObjectRead::Kind::Found:
                if (!read.value().etag.has_value()) {
                    // Rust returns InvalidRequest here. Without a version
                    // token there is nothing to compare against, and writing
                    // anyway would silently drop a concurrent update.
                    return core::make_unexpected(RepositoryError::InvalidRequest(
                        "updating '" + path + "' requires object storage ETags"));
                }
                current = core::Optional<std::string>(read.value().bytes);
                etag = read.value().etag;
                break;
            case ObjectRead::Kind::Changed:
                // The object moved under the read. Nothing consistent was
                // obtained, so there is nothing to hand the decision
                // function; read again rather than deciding from a mix.
                backoff.Wait();
                continue;
            case ObjectRead::Kind::NotFound:
                // Both stay absent: the write below becomes create-only.
                break;
        }

        const RepositoryResult<DocumentChange> change = update(current);
        if (!change.ok()) return core::make_unexpected(change.error());

        bool written = false;
        switch (change.value().kind) {
            case DocumentChange::Kind::Keep:
                return current;
            case DocumentChange::Kind::Put: {
                const RepositoryResult<bool> put =
                    store->PutConditionally(path, change.value().bytes, etag);
                if (!put.ok()) {
                    return core::make_unexpected(
                        RepositoryError::Backend("update '" + path + "'", put.error().message));
                }
                written = put.value();
                break;
            }
            case DocumentChange::Kind::Delete: {
                if (!current.has_value()) return core::Optional<std::string>();
                // Rust: opendal cannot delete conditionally, so a write that
                // lands between the read and this delete is lost. The
                // catalogs delete conditionally only to release a name or to
                // undo their own write, so losing that race leaves a name
                // unbound rather than corrupting a record.
                const RepositoryResult<core::Unit> deleted = store->Delete(path);
                if (!deleted.ok()) {
                    return core::make_unexpected(RepositoryError::Backend(
                        "update '" + path + "'", deleted.error().message));
                }
                written = true;
                break;
            }
        }

        if (written) return current;
        backoff.Wait();
    }

    return core::make_unexpected(
        RepositoryError::InvalidRequest("'" + path + "' changed too often during update"));
}

RepositoryResult<core::Optional<std::string> > UpdateDocument(ObjectStore* store,
                                                              const DocumentKey& key,
                                                              const DocumentUpdate& update) {
    return UpdateDocument(store, key, update, DocumentUpdatePolicy());
}

RepositoryResult<core::Unit> DeleteDocument(ObjectStore* store, const DocumentKey& key) {
    const std::string path = key.Path();
    const RepositoryResult<core::Unit> allowed = RefuseAppendOnlyStore(*store, path);
    if (!allowed.ok()) return allowed;
    const RepositoryResult<core::Unit> deleted = store->Delete(path);
    if (!deleted.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("delete '" + path + "'", deleted.error().message));
    }
    return core::Unit();
}

RepositoryResult<std::string> DocumentIdFromKey(DocumentCollection collection,
                                                const std::string& key) {
    const std::string prefix = DocumentCollectionPrefix(collection);
    const RepositoryError invalid =
        RepositoryError::InvalidRequest("invalid document key '" + key + "'");
    if (key.size() <= prefix.size() || key.compare(0, prefix.size(), prefix) != 0) {
        return core::make_unexpected(invalid);
    }
    const std::string name = key.substr(prefix.size());
    // Rust `.filter(|name| !name.contains('/'))` — a key nested deeper under
    // the prefix is not a document, and taking its leading component as an id
    // would invent one from a path.
    if (name.find('/') != std::string::npos) return core::make_unexpected(invalid);
    const core::Optional<std::string> id = DocumentCollectionIdFromFileName(collection, name);
    if (!id.has_value()) return core::make_unexpected(invalid);
    return *id;
}

RepositoryResult<std::vector<std::string> > ListDocuments(
    const ObjectStore& store, DocumentCollection collection,
    const core::Optional<std::string>& after, std::size_t limit) {
    const std::string prefix = DocumentCollectionPrefix(collection);

    core::Optional<std::string> start_after;
    if (after.has_value()) {
        // Rust resolves `after` through the collection's own key, so the
        // cursor is compared in object-key space rather than as a bare id.
        const core::Optional<DocumentKey> key = DocumentCollectionKey(collection, *after);
        if (!key.has_value()) {
            return core::make_unexpected(
                RepositoryError::InvalidRequest("invalid document id '" + *after + "'"));
        }
        start_after = core::Optional<std::string>(key->Path());
    }

    const RepositoryResult<std::vector<std::string> > keys =
        store.ListKeysPage(prefix, start_after, limit);
    if (!keys.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("list '" + prefix + "'", keys.error().message));
    }

    std::vector<std::string> ids;
    ids.reserve(keys.value().size());
    for (std::size_t i = 0; i < keys.value().size(); ++i) {
        const RepositoryResult<std::string> id = DocumentIdFromKey(collection, keys.value()[i]);
        if (!id.ok()) return core::make_unexpected(id.error());
        ids.push_back(id.value());
    }
    return ids;
}

RepositoryResult<std::vector<std::pair<std::string, std::string> > > ReadAllDocuments(
    const ObjectStore& store, DocumentCollection collection) {
    const std::string prefix = DocumentCollectionPrefix(collection);

    const RepositoryResult<std::vector<std::string> > keys = store.ListKeysRecursive(prefix);
    if (!keys.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("list '" + prefix + "'", keys.error().message));
    }

    std::vector<std::pair<std::string, std::string> > documents;
    documents.reserve(keys.value().size());
    for (std::size_t i = 0; i < keys.value().size(); ++i) {
        // The id is resolved before the read, as upstream does: a key that is
        // not a document is a malformed store, and reading it first would
        // spend a round trip to then reject it anyway.
        const RepositoryResult<std::string> id = DocumentIdFromKey(collection, keys.value()[i]);
        if (!id.ok()) return core::make_unexpected(id.error());

        const RepositoryResult<core::Optional<std::string> > bytes =
            ReadDocument(store, keys.value()[i]);
        if (!bytes.ok()) return core::make_unexpected(bytes.error());
        // Rust's `.map(...)` then `flatten()`: absent means the document was
        // deleted since the listing, which is a normal outcome on a store
        // other nodes write to.
        if (!bytes.value().has_value()) continue;

        documents.push_back(std::make_pair(id.value(), *bytes.value()));
    }
    return documents;
}

}  // namespace oss
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
