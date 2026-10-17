// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/interfaces.rs — SnapshotRepository /
// SnapshotRuntimeResolver traits + SnapshotListFilter.
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
#include <vector>

#include <cstddef>

#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/layers.h"
// The one definition of `SnapshotRecord`, `SnapshotSourceKind`,
// `TemplateBuildStatus` and `TemplateBuildErrorReason`.
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/volume/record.h"

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

/// Rust struct `VolumeRecordPage` — one page of the durable volume catalog.
struct VolumeRecordPage {
    std::vector<volume::VolumeRecord> records;
    /// The last returned volume id. Supplying it to the next request resumes
    /// after that record in the repository's stable shard-and-id order.
    /// Absent when this is the last page.
    core::Optional<std::string> next_volume_id;
};

/// Rust trait `SnapshotRepository`.
///
/// The volume half of the trait carries `unsupported("...")` default bodies
/// upstream, so a backend that only implements snapshots keeps compiling and
/// reports the same error at runtime. The C++ port mirrors that exactly: these
/// are virtual functions *with* a default implementation, not pure virtuals —
/// adding them therefore does not break existing backends.
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

    // --- volume catalog (Rust: default bodies returning `unsupported`) ------

    /// Rust `get_volume` — looked up by id *or* name.
    virtual RepositoryResult<core::Optional<volume::VolumeRecord> >
        GetVolume(const std::string& reference);
    /// Rust `list_volumes_page`.
    virtual RepositoryResult<VolumeRecordPage>
        ListVolumesPage(const core::Optional<std::string>& after_volume_id, std::size_t limit);
    /// Rust `create_volume` — inserts a new durable record.
    virtual RepositoryResult<core::Unit> CreateVolume(const volume::VolumeRecord& record);
    /// Rust `put_volume` — updates one existing durable record.
    virtual RepositoryResult<core::Unit> PutVolume(const volume::VolumeRecord& record);
    /// Rust `delete_volume`.
    virtual RepositoryResult<core::Unit> DeleteVolume(const std::string& volume_id);
    /// Rust `reserve_volume` — atomic where supported. A returned owner string
    /// reports an existing conflicting reservation.
    virtual RepositoryResult<core::Optional<std::string> >
        ReserveVolume(const std::string& volume_id, const std::string& owner);
    /// Rust `reserve_read_only_volume`.
    virtual RepositoryResult<core::Unit>
        ReserveReadOnlyVolume(const std::string& volume_id, const std::string& owner);
    /// Rust `replace_volume_owner_for` — `new_owner` unset releases the lease.
    virtual RepositoryResult<core::Unit>
        ReplaceVolumeOwnerFor(const std::string& volume_id, const std::string& owner,
                              const core::Optional<std::string>& new_owner);
    /// Rust `publish_volume_backing` — uploads the local image config's layers
    /// and returns the repository-owned descriptors.
    virtual RepositoryResult<std::vector<OverlaybdLayerRef> >
        PublishVolumeBacking(const std::string& volume_id, const std::string& image_config_path);
    /// Rust `materialize_volume_backing` — writes a node-local image config for
    /// the given repository-owned layers and returns its path.
    virtual RepositoryResult<std::string>
        MaterializeVolumeBacking(const std::string& volume_id,
                                 const std::vector<OverlaybdLayerRef>& layers,
                                 const std::string& destination);
};

/// Rust trait `SnapshotRuntimeResolver`.
class SnapshotRuntimeResolver {
 public:
    virtual ~SnapshotRuntimeResolver() {}
    /// Rust `resolve(snapshot) -> RunnableSnapshot`. Returns a runnable path bundle.
    virtual RepositoryResult<std::string> Resolve(const SnapshotRecord& record) = 0;
};

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_INTERFACES_H_
