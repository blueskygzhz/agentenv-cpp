// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/interfaces.rs — the SnapshotRepository trait
// and SnapshotListFilter.
//
// The runtime resolver is *not* part of this boundary; see the note at the
// bottom of this file and `agentenv/snapshot/resolver.h`.
//
// Type unification note. This header used to carry its own cut-down
// `SnapshotRecord`, `TemplateBuildStatus` and `SnapshotSourceKind` — declared
// as "the subset relevant to the repository boundary". Rust has exactly one
// definition of each, in `snapshot/types/snapshot.rs`, and the repository
// trait uses that one.
//
// The duplicates were not merely redundant, they were incompatible, and that
// is what kept the posix-fs backend from ever being wired up: its catalog
// store speaks the real types (`core::SnapshotId` ids, `Ready` rather than
// `Committed`), so no adapter could satisfy both this trait and that store.
// `record.h` even documented the hazard, warning that the two spellings "must
// not be confused when a record is written".
//
// So the boundary types now come from where they are defined: records from
// `record.h`, and the two filter/page types that genuinely belong to this
// interface are declared here and shared with the catalog store.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_INTERFACES_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_INTERFACES_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cstddef>

// `cfg::SnapshotPublishCompressionConfig`, carried by `PublishPolicy`.
#include "agentenv/cfg.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/layers.h"
// The one definition of `SnapshotRecord`, `SnapshotSourceKind`,
// `TemplateBuildStatus` and `TemplateBuildErrorReason`.
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/blobs.h"
#include "agentenv/snapshot/repository/documents.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/types.h"

namespace agentenv {
namespace snapshot {
namespace repository {

// The record types are the ones from `snapshot/`, re-exported into this
// namespace so existing `repository::SnapshotRecord` spellings keep resolving
// — to the real type now, not to a parallel one.
using snapshot::SnapshotRecord;
using snapshot::SnapshotSourceKind;
using snapshot::TemplateBuildStatus;
using snapshot::TemplateBuildErrorReason;

/// Rust struct `SnapshotListFilter` — AND-combined optional filters.
///
/// Every field is an independent conjunct: an absent one matches everything,
/// so a default-constructed filter matches all records, including pending
/// template builds.
struct SnapshotListFilter {
    /// Match aliases that start with this prefix.
    core::Optional<std::string> alias_prefix;
    /// Restrict results to this exact set of snapshot ids.
    ///
    /// Typed as ids rather than strings, as Rust has it: the catalog indexes
    /// by `SnapshotId`, and a string set would make every lookup re-parse —
    /// or worse, silently miss on a malformed entry.
    core::Optional<std::vector<core::SnapshotId> > snapshot_ids;
    /// A single id *or* exact alias, whichever matches.
    core::Optional<std::string> snapshot_id_or_alias;
    /// Only matches records whose source is `Sandbox`.
    core::Optional<std::string> source_sandbox_id;
    core::Optional<std::vector<SnapshotSourceKind> > sources;
    /// Template records only; sandbox records never match this field.
    core::Optional<std::vector<TemplateBuildStatus> > template_statuses;

    /// Rust `matches_all()`.
    static SnapshotListFilter MatchesAll() { return SnapshotListFilter(); }
    /// Rust `templates()`.
    static SnapshotListFilter Templates();
    /// Rust `by_ids()`.
    static SnapshotListFilter ByIds(const std::vector<core::SnapshotId>& ids);
    /// Rust: does this record satisfy the filter?
    bool Matches(const SnapshotRecord& r) const;
};

/// Rust `PublishPolicy` — how snapshots and volumes are published into one
/// repository.
///
/// Both fields are ported. `image_storage`'s single consumer is the branch in
/// `StoreSnapshot` that selects an `AcrDiskImageExporter` for the
/// `SourceRegistry` case (`store.cc`, Rust `store_disk_image`).
struct PublishPolicy {
    /// Recontainerizes raw local layers as zfile before they are stored.
    cfg::SnapshotPublishCompressionConfig compression;

    /// Whether disk images go to their source registry or to managed layers.
    ///
    /// Only the OSS backend ever reports `SourceRegistry`, here and upstream
    /// alike: it is the one backend that overrides the policy, and the one
    /// `snapshot.image_publish.enabled` is wired to
    /// (`snapshot/backends/mod.rs:90-94`). Every other backend inherits the
    /// default below, so a posix-fs node stores managed layers no matter what
    /// `image_publish` says — which is upstream's behaviour, not a port gap.
    cfg::SnapshotImageStoragePolicy image_storage;

    /// Rust `impl Default for PublishPolicy` — raw layers.
    ///
    /// **`enabled` is forced off here, and that is not a copy of the config
    /// default.** `cfg::SnapshotPublishCompressionConfig` defaults `enabled`
    /// to `true`, because that is the TOML default an operator gets when the
    /// `[snapshot.publish_compression]` table is absent. Rust's
    /// `PublishPolicy::default()` deliberately overrides it to `false`
    /// (`interfaces.rs:20-31`), so a backend that does not implement the
    /// policy stores layers raw rather than inheriting an operator default it
    /// never consulted. Defaulting the member instead would turn every such
    /// backend -- posix-fs and in-memory, i.e. every production repository in
    /// this tree today -- from passthrough into lz4 compression.
    ///
    /// `image_storage` is set explicitly for the same reason the enum has no
    /// defaulted member: `ObjectStorage` is the inert direction (store the
    /// layers here), and a backend that has not been told to publish to a
    /// source registry must not start doing it.
    PublishPolicy() : image_storage(cfg::SnapshotImageStoragePolicy::ObjectStorage) {
        compression.enabled = false;
    }
};

/// Rust trait `SnapshotRepository`, which has since become `Repository`.
///
/// The document and blob halves carry `unsupported("...")` default bodies
/// upstream, so a backend that only implements snapshots keeps compiling and
/// reports the same error at runtime. The C++ port mirrors that exactly: these
/// are virtual functions *with* a default implementation, not pure virtuals.
///
/// There is no volume half. Rust's repository "stores; it does not decide":
/// the volume catalog (`src/volume/catalog.rs`, ported as
/// `agentenv/volume/catalog.h`) is built on the documents below and is the
/// same on every backend, and volume layers go through `put_blob` like any
/// other layer (`agentenv/snapshot/store.h`).
class SnapshotRepository {
 public:
    virtual ~SnapshotRepository() {}
    virtual RepositoryResult<SnapshotRecord> Create(const SnapshotRecord& record) = 0;
    /// Accepts an id or an alias, as Rust does.
    virtual RepositoryResult<core::Optional<SnapshotRecord> >
        Get(const std::string& id_or_alias) = 0;
    virtual RepositoryResult<std::vector<SnapshotRecord> >
        List(const SnapshotListFilter& filter) = 0;
    virtual RepositoryResult<core::Unit> Delete(const std::string& id_or_alias) = 0;
    virtual RepositoryResult<core::Optional<core::SnapshotId> >
        ResolveAlias(const std::string& alias) = 0;

    /// Rust `try_start_build(&SnapshotId)` — Waiting -> Building.
    ///
    /// Keyed by id, not by a string: a build is started against a record that
    /// is known to exist, so there is no alias to resolve here.
    virtual RepositoryResult<SnapshotRecord>
        TryStartBuild(const core::SnapshotId& id) = 0;

    /// Rust `mark_build_error(&SnapshotId, TemplateBuildErrorReason)`.
    ///
    /// The reason is structured because the record stores it that way — it
    /// carries an optional failing step alongside the message, and the API
    /// surfaces that as its own field.
    virtual RepositoryResult<core::Unit>
        MarkBuildError(const core::SnapshotId& id,
                       const TemplateBuildErrorReason& reason) = 0;

    // --- shared documents (Rust src/repository/interfaces.rs; default bodies
    // returning `unsupported("repository documents")`) ----------------------

    /// Rust `get_document` — `None` when absent.
    virtual RepositoryResult<core::Optional<std::string> > GetDocument(const DocumentKey& key);
    /// Rust `put_document` — atomically replaces one document.
    virtual RepositoryResult<core::Unit> PutDocument(const DocumentKey& key,
                                                     const std::string& bytes);
    /// Rust `update_document` — applies `update` atomically against
    /// concurrent writers on every node, returning the document it was
    /// applied to.
    virtual RepositoryResult<core::Optional<std::string> > UpdateDocument(
        const DocumentKey& key, const DocumentUpdate& update);
    /// Rust `delete_document` — a missing document is success.
    virtual RepositoryResult<core::Unit> DeleteDocument(const DocumentKey& key);
    /// Rust `list_documents` — up to `limit` document ids strictly after
    /// `after`, in the backend's stable order.
    virtual RepositoryResult<std::vector<std::string> > ListDocuments(
        DocumentCollection collection, const core::Optional<std::string>& after,
        std::size_t limit);

    /// Rust `read_documents` — every document in `collection`, in id order,
    /// as `(id, bytes)` pairs.
    ///
    /// Not expressible as a loop over `ListDocuments` by a caller: a document
    /// deleted between the listing and its read must be *skipped*, and only
    /// the backend can tell that apart from a read failure. The shared catalog
    /// lists records this way, so a `List` on a backend without it reports
    /// `unsupported` rather than an empty catalog.
    virtual RepositoryResult<std::vector<std::pair<std::string, std::string> > > ReadDocuments(
        DocumentCollection collection);

    // --- blobs (Rust src/repository/interfaces.rs; default bodies returning
    // `unsupported("blob storage")`, `None` for the URL) -----------------------
    //
    // Rust's `staging_file` is not ported; nothing in this tree calls it.

    /// Rust `publish_policy` — how layers are published into this repository.
    ///
    /// Named `GetPublishPolicy` rather than `PublishPolicy` only because a
    /// member function may not share its name with the type it returns.
    ///
    /// A virtual with a default body, exactly as upstream: Rust declares it
    /// on the trait returning `PublishPolicy::default()`, and only the OSS
    /// backend overrides it. The default is the reason an operator's
    /// `[snapshot.publish_compression]` switch has no effect on a posix-fs
    /// repository -- upstream behaves identically, because the switch is read
    /// where the OSS backend is constructed, not where layers are stored.
    virtual PublishPolicy GetPublishPolicy() const;

    /// Rust `managed_layers_repo_blob_url` — set when readers address managed
    /// layers by URL.
    virtual core::Optional<std::string> ManagedLayersRepoBlobUrl() const;
    /// Rust `put_blob` — a layer already stored is kept, since its digest
    /// names its contents; a snapshot file is replaced.
    virtual RepositoryResult<core::Unit> PutBlob(const BlobKey& key, const BlobBody& body);
    /// Rust `locate_blob` — in place on a shared filesystem, or by URL.
    virtual RepositoryResult<BlobLocation> LocateBlob(const BlobKey& key) const;
    /// Rust `has_blob`.
    virtual RepositoryResult<bool> HasBlob(const BlobKey& key);
    /// Rust `read_blob` — `ArtifactNotFound` when absent.
    virtual RepositoryResult<std::string> ReadBlob(const BlobKey& key);

    /// Rust `fetch_blob(key, destination) -> u64` — copies the blob out of the
    /// repository into a caller-owned `destination`, returning the byte count.
    ///
    /// This is the operation that makes a repository usable from a node that
    /// does *not* share its filesystem: `LocateBlob` can only answer `Local`
    /// when the bytes are already reachable by path, so a reader that cannot
    /// mount the repository has no other way to obtain a layer.
    ///
    /// Distinct from `ReadBlob`, which returns the bytes in memory — a volume
    /// layer is routinely gigabytes, so the destination is a file and the
    /// return value is a size rather than the contents.
    ///
    /// `ArtifactNotFound` when the blob is absent, so a caller can tell "not
    /// published yet" from a transport failure.
    virtual RepositoryResult<uint64_t> FetchBlob(const BlobKey& key,
                                                 const std::string& destination);

    /// Rust `delete_snapshot_blobs` — removes every file of one snapshot.
    ///
    /// Shared managed layers **stay**: they are content-addressed and may
    /// belong to other snapshots, so removing them here would break a
    /// snapshot nobody deleted. Only the per-snapshot directory goes.
    ///
    /// A missing file is success, because this runs before the record is
    /// deleted and must be retryable: a delete that failed partway is
    /// finished by running it again.
    virtual RepositoryResult<core::Unit> DeleteSnapshotBlobs(const core::SnapshotId& id);
};

/// Rust `managed_layer_config` (layer_config.rs) — one managed layer as an
/// overlaybd `LayerConfig`, resolved against whatever the repository can
/// offer.
///
/// `LocateBlob` decides the shape: a `Local` answer fills `file`, so overlaybd
/// reads the layer in place; a `Remote` one leaves `file` empty and the layer
/// is addressed by digest against the image's `repoBlobUrl`. That is the whole
/// reason one resolver can serve a shared filesystem and a bucket alike.
///
/// Exported because the snapshot runtime resolver needs exactly this and used
/// to hard-code the posix-fs layout instead (see `RepositoryRuntimeResolver`).
/// It had been sitting in an anonymous namespace with `MaterializeVolumeBacking`
/// as its only caller, which is why the volume path was already generic while
/// the snapshot path was not.
RepositoryResult<storage::overlaybd::LayerConfig> ManagedLayerConfig(
    const SnapshotRepository& repository, const ManagedLayer& layer, LayerRole role);

/// Rust `repository::materialize_volume_backing` (layer_config.rs) — writes a
/// runtime image config for `layers` at `destination`, atomically, and
/// returns its path. Managed layers are read in place when the repository
/// allows it, otherwise by digest from its `repoBlobUrl`.
RepositoryResult<std::string> MaterializeVolumeBacking(
    const SnapshotRepository& repository, const std::vector<OverlaybdLayerRef>& layers,
    const std::string& destination);

// `SnapshotRuntimeResolver` is **not** here, and upstream does not put it
// here either: `repository/interfaces.rs` carries the repository trait, while
// the runtime resolver has its own `snapshot/resolver.rs`. The copy that used
// to sit below declared `Resolve` as returning `RepositoryResult<std::string>`
// against upstream's `RunnableSnapshot`, which is why the one real resolver
// could not implement it. See `agentenv/snapshot/resolver.h`.

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_INTERFACES_H_
