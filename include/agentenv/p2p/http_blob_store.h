// SPDX-License-Identifier: MIT
// The byte store behind the HTTP P2P transport.
//
// Rust equivalent: `iroh_blobs::store::fs::FsStore`, as used by
// `src/p2p/iroh/transport.rs`. That store is content-addressed, keeps
// retention tags, and runs a gated GC. This is the same shape over plain
// files, minus iroh's BLAKE3/bao verified-streaming layout, which exists to
// let a *remote* reader verify a partial transfer incrementally. A reader here
// verifies the whole artifact once it lands (see `HttpP2pTransport::Fetch*`),
// so the on-disk form can stay a plain file.
//
// Layout:
//
//   <root>/blobs/<digest-with-: and / replaced>   Copy-mode bytes
//   <root>/refs/<hex(key)>.json                   publish records
//
// Why publish records are persisted separately from the catalog. The catalog
// (`PublishedArtifactCatalog`) answers "what descriptor do I serve for this
// key"; it does not know where the bytes are. For `Reference` publications the
// bytes stay wherever the caller put them, so the mapping from locator to a
// servable path has to survive a restart too, or a restart would leave the
// catalog promising keys this store can no longer resolve.
#ifndef AGENTENV_P2P_HTTP_BLOB_STORE_H_
#define AGENTENV_P2P_HTTP_BLOB_STORE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/p2p/error.h"
#include "agentenv/p2p/types.h"

namespace agentenv {
namespace p2p {

class HttpBlobStore;

/// One publication's byte location.
///
/// Rust models this with `ImportMode::{Copy, TryReference}` inside the store.
/// Here it has to be explicit because a `Reference` entry names a file this
/// store does not own and therefore cannot trust to stay put.
struct BlobEntry {
    /// `sha256:<hex>` — also the locator peers dial with.
    std::string digest;
    uint64_t    size = 0;
    /// Absolute path the bytes are served from. Inside `<root>/blobs` for
    /// `Copy`, the caller's own path for `Reference`.
    std::string path;
    /// False for `Copy`. True means `path` is foreign and must be re-checked
    /// on every serve.
    bool        referenced = false;
};

/// A read lease on a blob.
///
/// Rust's `TempTag`, which `export_local_blob_range` deliberately keeps alive
/// inside the lazy stream "until EOF or caller cancellation". Same purpose:
/// while one of these exists the entry will not be removed from under a
/// transfer that is still running, so a reader cannot observe a locator that
/// resolved a moment ago and now names nothing.
///
/// Held by value, released on destruction — the port's standing substitute for
/// a Rust `Drop`.
class BlobLease {
 public:
    ~BlobLease();

    const BlobEntry& entry() const { return entry_; }
    bool valid() const { return !!owner_; }

 private:
    friend class HttpBlobStore;
    BlobLease(std::shared_ptr<HttpBlobStore> owner, BlobEntry entry)
        : owner_(std::move(owner)), entry_(std::move(entry)) {}

    BlobLease(const BlobLease&);
    BlobLease& operator=(const BlobLease&);

    std::shared_ptr<HttpBlobStore> owner_;
    BlobEntry                      entry_;
};

class HttpBlobStore : public std::enable_shared_from_this<HttpBlobStore> {
 public:
    /// Opens (creating if needed) the store under `root_dir` and replays the
    /// persisted publish records.
    ///
    /// A record that cannot be parsed fails the open rather than being
    /// skipped, for the same reason `PublishedArtifactCatalog::Load` refuses a
    /// corrupt entry: serving a partial set of publications silently retracts
    /// promises this node already made to peers.
    static core::Expected<std::shared_ptr<HttpBlobStore>, P2pError>
        Open(const std::string& root_dir);

    /// Imports `source` under `mode` and records it against `key`.
    ///
    /// Returns the entry, whose `digest` is what the descriptor advertises as
    /// its `backend_locator`.
    ///
    /// `Copy` hardlinks first and falls back to a copy. A content-addressed
    /// file is immutable by construction, so sharing the inode is safe and
    /// costs no bytes — the same trade `volume::CreateChildBacking` makes.
    core::Expected<BlobEntry, P2pError> Import(const P2pArtifactKey& key,
                                               const P2pPublishSource& source,
                                               P2pPublishMode mode);

    /// Records bytes this node fetched from a peer, so it can serve them on.
    ///
    /// Rust `try_advertise_fetched_blob`. Always `Copy`-like: the fetched file
    /// belongs to the caller, so the store takes its own reference rather than
    /// depending on a destination it does not control.
    core::Expected<BlobEntry, P2pError> ImportFetched(const P2pArtifactKey& key,
                                                      const std::string& path);

    /// Takes a read lease by locator. `nullptr` when the locator is unknown.
    std::shared_ptr<BlobLease> Acquire(const std::string& locator);

    /// Drops the publication for `key`; true when one existed.
    ///
    /// The bytes are only unlinked when no other key still names the same
    /// digest. Two keys publishing identical content share one file (that is
    /// what content addressing buys), so removing it on the first unpublish
    /// would break the second.
    ///
    /// Removal waits for outstanding leases: a transfer in flight keeps the
    /// file until it finishes. The record is dropped immediately either way,
    /// so the key stops resolving at once even if the bytes linger briefly.
    core::Expected<bool, P2pError> Remove(const P2pArtifactKey& key);

    /// Locator for `key`, if published here.
    core::Optional<std::string> LocatorFor(const P2pArtifactKey& key) const;

    std::size_t size() const;

 private:
    friend class BlobLease;

    explicit HttpBlobStore(std::string root_dir) : root_dir_(std::move(root_dir)) {}

    /// Returns a lease (`BlobLease` is not publicly constructible).
    std::shared_ptr<BlobLease> MakeLease(const BlobEntry& entry);
    /// Called by `~BlobLease`.
    void ReleaseLease(const std::string& digest);

    std::string BlobsDir() const;
    std::string RefsDir() const;
    std::string BlobPathFor(const std::string& digest) const;
    std::string RecordPathFor(const P2pArtifactKey& key) const;

    core::Expected<core::Unit, P2pError> WriteRecord(const P2pArtifactKey& key,
                                                     const BlobEntry& entry);
    core::Expected<core::Unit, P2pError> LoadRecords();

    /// True when some key other than `except` still names `digest`.
    bool DigestStillReferencedLocked(const std::string& digest,
                                     const P2pArtifactKey& except) const;

    std::string root_dir_;

    mutable std::mutex                        mutex_;
    std::map<P2pArtifactKey, BlobEntry>       entries_;
    /// digest -> outstanding lease count.
    std::map<std::string, int>                leases_;
    /// Digests whose last key is gone but whose file is still leased.
    std::map<std::string, std::string>        pending_unlink_;
};

/// Filesystem-safe form of a digest: `:` and `/` become `-`.
///
/// Exposed because the route handler builds the same name when it serves a
/// locator, and the two must not drift.
std::string BlobFileNameForDigest(const std::string& digest);

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_HTTP_BLOB_STORE_H_
