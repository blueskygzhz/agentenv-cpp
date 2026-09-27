// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/interfaces.rs — SnapshotRepository /
// SnapshotRuntimeResolver traits + SnapshotListFilter.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_INTERFACES_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_INTERFACES_H_

#include <memory>
#include <string>
#include <vector>

#include <cstddef>

#include "agentenv/core/optional.h"
#include "agentenv/snapshot/layers.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/volume/record.h"

namespace agentenv {
namespace snapshot {
namespace repository {

/// Rust enum `SnapshotSourceKind`.
enum class SnapshotSourceKind {
    Template,
    Sandbox,
};

/// Rust enum `TemplateBuildStatus`.
enum class TemplateBuildStatus {
    Waiting,
    Building,
    Committed,
    Error,
};

/// Rust struct `SnapshotRecord` (subset relevant to the repository boundary).
struct SnapshotRecord {
    std::string        id;
    std::string        alias;
    SnapshotSourceKind source = SnapshotSourceKind::Template;
    bool               committed = false;
    TemplateBuildStatus build_status = TemplateBuildStatus::Waiting;
    std::string        source_sandbox_id;   // when source == Sandbox
};

/// Rust struct `SnapshotListFilter` — AND-combined optional filters.
struct SnapshotListFilter {
    core::Optional<std::string>              alias_prefix;
    core::Optional<std::vector<std::string> > snapshot_ids;
    core::Optional<std::string>              snapshot_id_or_alias;
    core::Optional<std::string>              source_sandbox_id;
    core::Optional<std::vector<SnapshotSourceKind> > sources;
    core::Optional<std::vector<TemplateBuildStatus> > template_statuses;

    /// Rust `matches_all()`.
    static SnapshotListFilter MatchesAll() { return SnapshotListFilter(); }
    /// Rust `templates()`.
    static SnapshotListFilter Templates();
    /// Rust: does this record satisfy the filter?
    bool Matches(const SnapshotRecord& r) const;
};

/// Rust struct `VolumeRecordPage` — one page of the durable volume catalog.
struct VolumeRecordPage {
    std::vector<volume::VolumeRecord> records;
    /// Rust `Option<String>`: absent when this is the last page.
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
    virtual RepositoryResult<core::Optional<SnapshotRecord> >
        Get(const std::string& id_or_alias) = 0;
    virtual RepositoryResult<std::vector<SnapshotRecord> >
        List(const SnapshotListFilter& filter) = 0;
    virtual RepositoryResult<core::Unit> Delete(const std::string& id_or_alias) = 0;
    virtual RepositoryResult<core::Optional<std::string> >
        ResolveAlias(const std::string& alias) = 0;
    virtual RepositoryResult<SnapshotRecord> TryStartBuild(const std::string& id) = 0;
    virtual RepositoryResult<core::Unit>
        MarkBuildError(const std::string& id, const std::string& reason) = 0;

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
