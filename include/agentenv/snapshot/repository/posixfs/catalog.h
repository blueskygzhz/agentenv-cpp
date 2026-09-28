// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/catalog.rs
//
// The durable catalog behind the POSIX repository. Two invariants shape
// everything here:
//
//   * writers serialise through advisory `flock` files, and every file is
//     replaced by rename, so a reader takes no lock and still never sees a
//     partial record;
//   * the commit marker is written *before* the record, so a snapshot
//     directory that survived a crash mid-publish is recognisable as
//     uncommitted and can be cleaned up.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_CATALOG_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_CATALOG_H_

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/file_lock.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/repository/posixfs/layout.h"
#include "agentenv/volume/record.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {

/// Rust `FILE_LOCK_TIMEOUT`.
const int64_t kFileLockTimeoutMs = 10000;

/// Rust `struct SnapshotListFilter` (src/snapshot/repository/interfaces.rs).
///
/// Every field is an independent conjunct: an absent one matches everything.
struct SnapshotListFilter {
    core::Optional<std::string> alias_prefix;
    core::Optional<std::vector<core::SnapshotId> > snapshot_ids;
    /// A single id *or* exact alias, whichever matches.
    core::Optional<std::string> snapshot_id_or_alias;
    /// Only matches records whose source is `Sandbox`.
    core::Optional<std::string> source_sandbox_id;
    core::Optional<std::vector<SnapshotSourceKind> > sources;
    core::Optional<std::vector<TemplateBuildStatus> > template_statuses;
};

/// Rust `struct VolumeRecordPage`.
struct VolumeRecordPage {
    std::vector<volume::VolumeRecord> records;
    /// Absent on the last page.
    core::Optional<std::string> next_volume_id;
};

/// Rust `struct BuildCacheState` (src/snapshot/repository/build_cache.rs).
struct BuildCacheState {
    core::Optional<std::string> current;
    /// A `BTreeSet` upstream, so iteration is ordered and the encoded form is
    /// stable.
    std::set<std::string> retired;

    /// Rust `BuildCacheState::decode` — validates every id, and rejects a
    /// state whose current seed is also retired.
    static RepositoryResult<BuildCacheState> Decode(const std::string& bytes);

    core::Json ToJson() const;

    /// Rust `BuildCacheState::replace` — returns the seed that was replaced.
    /// Publishing a retired seed is refused: it would resurrect a cache the
    /// operator already discarded.
    RepositoryResult<core::Optional<std::string> > Replace(const std::string& id);
};

/// Rust `struct PublishSession`.
struct PublishSession {
    core::SnapshotId snapshot_id;
};

/// Rust `struct PosixFsCatalogStore`.
class PosixFsCatalogStore {
 public:
    explicit PosixFsCatalogStore(const std::string& root) : root_(root) {}

    // ---- publish lifecycle ----------------------------------------------

    /// Rust `begin_publish`.
    RepositoryResult<PublishSession> BeginPublish(const core::SnapshotId& snapshot_id);

    /// Rust `commit_publish`.
    ///
    /// Order matters: alias lock, alias binding, commit marker, then the
    /// record. A failure anywhere unbinds the alias this call created and
    /// removes the uncommitted snapshot directory.
    RepositoryResult<SnapshotRecord> CommitPublish(const PublishSession& session,
                                                   const SnapshotPublishMetadata& metadata,
                                                   const CommittedSnapshot& committed);

    /// Rust `abort_publish`.
    RepositoryResult<core::Unit> AbortPublish(const PublishSession& session);

    // ---- snapshot records ------------------------------------------------

    /// Rust `create` — pre-creates a waiting template record.
    RepositoryResult<SnapshotRecord> Create(const SnapshotRecord& record);

    /// Rust `get` — accepts an id or an alias, and drops a dangling alias on
    /// the way.
    RepositoryResult<core::Optional<SnapshotRecord> > Get(const std::string& id_or_alias);

    /// Rust `list` — newest first, ties broken by id so the order is total.
    RepositoryResult<std::vector<SnapshotRecord> > List(const SnapshotListFilter& filter);

    /// Rust `delete_record` — idempotent.
    RepositoryResult<core::Unit> DeleteRecord(const core::SnapshotId& id);

    /// Rust `resolve_alias`.
    RepositoryResult<core::Optional<core::SnapshotId> > ResolveAlias(const std::string& alias);

    /// Rust `try_start` — Waiting -> Building.
    RepositoryResult<SnapshotRecord> TryStart(const core::SnapshotId& id);

    /// Rust `mark_error`.
    RepositoryResult<core::Unit> MarkError(const core::SnapshotId& id,
                                           const TemplateBuildErrorReason& reason);

    // ---- volumes ---------------------------------------------------------

    /// Rust `get_volume` — accepts an id or a name.
    RepositoryResult<core::Optional<volume::VolumeRecord> > GetVolume(
        const std::string& reference);

    /// Rust `list_volumes_page`.
    RepositoryResult<VolumeRecordPage> ListVolumesPage(
        const core::Optional<std::string>& after_volume_id, std::size_t limit);

    /// Rust `create_volume`.
    RepositoryResult<core::Unit> CreateVolume(const volume::VolumeRecord& record);

    /// Rust `put_volume`.
    RepositoryResult<core::Unit> PutVolume(const volume::VolumeRecord& record);

    /// Rust `delete_volume` — refuses while the volume is still mounted.
    RepositoryResult<core::Unit> DeleteVolume(const std::string& volume_id);

    /// Rust `reserve_volume` — returns the current owner when the lease is
    /// already held by someone else, rather than failing.
    RepositoryResult<core::Optional<std::string> > ReserveVolume(const std::string& volume_id,
                                                                 const std::string& owner);

    /// Rust `reserve_read_only_volume`.
    RepositoryResult<core::Unit> ReserveReadOnlyVolume(const std::string& volume_id,
                                                       const std::string& owner);

    /// Rust `replace_volume_owner_for` — an unset `to` releases the lease.
    RepositoryResult<core::Unit> ReplaceVolumeOwnerFor(const std::string& volume_id,
                                                       const std::string& from,
                                                       const core::Optional<std::string>& to);

    // ---- build cache ------------------------------------------------------

    /// Rust `get_build_cache_state`.
    RepositoryResult<BuildCacheState> GetBuildCacheState() const;

    /// Rust `replace_build_cache_head`.
    RepositoryResult<core::Optional<std::string> > ReplaceBuildCacheHead(
        const std::string& volume_id);

    /// Rust `forget_retired_build_cache`.
    RepositoryResult<core::Unit> ForgetRetiredBuildCache(const std::string& volume_id);

    /// Rust `matches_record_filter`. Exposed for tests.
    static bool MatchesRecordFilter(const SnapshotRecord& record,
                                    const SnapshotListFilter& filter);

    const std::string& root() const { return root_; }

 private:
    PosixFsSnapshotArtifactLayout Layout(const core::SnapshotId& snapshot_id) const;
    std::string CommitMarkerPath(const core::SnapshotId& snapshot_id) const;
    std::string RecordPath(const core::SnapshotId& snapshot_id) const;

    RepositoryResult<core::Unit> EnsureLayout() const;

    RepositoryResult<core::Json> ReadJson(const std::string& path) const;
    RepositoryResult<core::Unit> WriteJson(const std::string& path,
                                           const core::Json& value) const;
    RepositoryResult<core::Unit> WriteCommitMarker(const core::SnapshotId& id) const;
    RepositoryResult<core::Unit> RemoveFileIfExists(const std::string& path) const;
    RepositoryResult<core::Unit> RemoveDirIfExists(const std::string& path) const;

    RepositoryResult<core::FileLockGuard> AcquireCatalogLock(const std::string& lock_path,
                                                             const std::string& label) const;
    RepositoryResult<core::FileLockGuard> AcquireAliasLock(const SnapshotAlias& alias) const;
    RepositoryResult<core::FileLockGuard> AcquireRecordLock(const core::SnapshotId& id) const;
    RepositoryResult<core::FileLockGuard> AcquireVolumeAliasLock(
        const std::string& alias) const;
    RepositoryResult<core::FileLockGuard> AcquireVolumeRecordLock(
        const std::string& volume_id) const;

    RepositoryResult<core::Optional<SnapshotRecord> > LoadRecordByIdUnlocked(
        const core::SnapshotId& id) const;
    RepositoryResult<core::Optional<core::SnapshotId> > LoadAliasTarget(
        const SnapshotAlias& alias) const;
    RepositoryResult<core::Unit> WriteRecordUnlocked(const SnapshotRecord& record) const;
    RepositoryResult<core::Unit> EnsureAliasAvailable(const SnapshotAlias& alias,
                                                      const core::SnapshotId& new_id) const;
    RepositoryResult<SnapshotRecord> CommittedRecordUnlocked(
        const SnapshotPublishMetadata& metadata, const CommittedSnapshot& committed,
        int64_t now_unix_ms) const;

    bool IsCommitted(const core::SnapshotId& id) const;
    RepositoryResult<core::Unit> CleanupUncommittedSnapshotDir(
        const core::SnapshotId& id) const;

    RepositoryResult<core::Unit> EnsureVolumeComponent(const std::string& value,
                                                       const std::string& kind) const;
    RepositoryResult<core::Unit> EnsureVolumeId(const std::string& volume_id) const;
    RepositoryResult<std::vector<std::string> > VolumeIdsUnlocked() const;
    RepositoryResult<core::Optional<volume::VolumeRecord> > LoadVolumeByIdUnlocked(
        const std::string& volume_id) const;

    std::string root_;
};

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_CATALOG_H_
