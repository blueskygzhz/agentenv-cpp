// SPDX-License-Identifier: MIT
// Rust: src/snapshot/catalog.rs — `impl<R: Repository + ?Sized> SnapshotCatalog for R`.
//
// The record layer every backend shares. Upstream's `Repository` trait has no
// record methods at all: a snapshot record is an ordinary *document*, and
// `create` / `get` / `list` / `delete` / `resolve_alias` / `try_start_build` /
// `mark_build_error` come from one blanket impl over the document methods. A
// backend that can store a document therefore gets records for free, and —
// more to the point — every backend stores them in the *same* key space, which
// is the only reason two nodes on different backends can read each other's
// catalog.
//
// This is a free-function set rather than a base class because C++11 has no
// blanket impl: a backend forwards its record methods here, the way
// `oss::OssSnapshotRepository` does. The functions take `SnapshotRepository&`
// and call only its *document* methods, so forwarding cannot recurse.
//
// Not ported here: `publish_with_local_layers`, the `SnapshotCatalog` method
// that wires publication to P2P layer announcement — it needs the P2P
// publication half. `publish` itself lives in `snapshot/publish.h`, and
// `commit` / `attach_startup_manifest` are below.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_RECORD_CATALOG_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_RECORD_CATALOG_H_

#include <string>
#include <vector>

#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/interfaces.h"
#include "agentenv/snapshot/startup_pack.h"
#include "agentenv/snapshot/types.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace record_catalog {

/// Rust `MAX_ALIAS_CLAIM_ATTEMPTS`.
const int kMaxAliasClaimAttempts = 5;

/// Rust `SnapshotCatalog::create` — pre-creates a waiting template record.
///
/// The alias is claimed *after* the record is written, and a failed claim
/// rolls the record back: a record whose alias belongs to someone else would
/// otherwise be resolvable by id while invisible by name.
RepositoryResult<SnapshotRecord> Create(SnapshotRepository& repository,
                                        const SnapshotRecord& record);

/// Rust `SnapshotCatalog::get` — by id or alias, hiding one being deleted.
RepositoryResult<core::Optional<SnapshotRecord> > Get(SnapshotRepository& repository,
                                                      const std::string& id_or_alias);

/// Rust `SnapshotCatalog::list` — matching records, newest first.
RepositoryResult<std::vector<SnapshotRecord> > List(SnapshotRepository& repository,
                                                    const SnapshotListFilter& filter);

/// Rust `SnapshotCatalog::delete` — one snapshot by id or alias.
///
/// Order is the whole design, and it is upstream's: mark the record
/// `deleting`, release the alias, remove the artifacts, delete the build log,
/// delete the record. Every step is retryable and each one is safe to be
/// interrupted after:
///   - marking first makes the snapshot invisible before its files go, so
///     nothing starts resuming from a snapshot that is being dismantled;
///   - the alias is freed early, so a delete that stops partway does not keep
///     the name reserved;
///   - the files go *before* the record, because the record is the only thing
///     that names them — dropping it first would leak them permanently.
/// A missing snapshot is success, so a retry finishes rather than failing.
///
/// Refused for a record with ACR disk publications: rolling those back is
/// `AcrDiskImageExporter::rollback_publication`, which is not ported, and
/// skipping it would leave an image published in a registry that this call
/// claims to have deleted. The refusal happens inside the compare-and-swap,
/// so nothing is mutated.
RepositoryResult<core::Unit> Delete(SnapshotRepository& repository,
                                    const std::string& id_or_alias);

/// Rust `SnapshotCatalog::resolve_alias`.
RepositoryResult<core::Optional<core::SnapshotId> > ResolveAlias(SnapshotRepository& repository,
                                                                 const std::string& alias);

/// Rust `SnapshotCatalog::try_start_build` — waiting to building, atomically.
RepositoryResult<SnapshotRecord> TryStartBuild(SnapshotRepository& repository,
                                               const core::SnapshotId& id);

/// Rust `SnapshotCatalog::mark_build_error`.
RepositoryResult<core::Unit> MarkBuildError(SnapshotRepository& repository,
                                            const core::SnapshotId& id,
                                            const TemplateBuildErrorReason& reason);

/// Rust `ensure_alias_available` — fails when a *live* snapshot other than
/// `id` owns `alias`. Exposed because `publish` will need it.
RepositoryResult<core::Unit> EnsureAliasAvailable(SnapshotRepository& repository,
                                                  const SnapshotAlias& alias,
                                                  const core::SnapshotId& id);

/// Rust `claim_alias` — points `alias` at `id` unless a live snapshot owns it.
RepositoryResult<core::Unit> ClaimAlias(SnapshotRepository& repository,
                                        const SnapshotAlias& alias,
                                        const core::SnapshotId& id);

/// Rust `release_alias` — removes `alias` if it still points at `id`.
RepositoryResult<core::Unit> ReleaseAlias(SnapshotRepository& repository,
                                          const SnapshotAlias& alias,
                                          const core::SnapshotId& id);

/// Rust `catalog::commit` — commits a stored snapshot's record, then claims
/// its alias.
///
/// The record write and the alias claim are two separate documents, so this
/// cannot be one atomic step; the order and the rollback are what make the
/// pair safe:
///
///   * **The record is written first and the alias claimed second.** A record
///     holding an alias it does not own is findable by id, invisible by name,
///     and reconciled by nothing — so the claim is what confirms the commit.
///   * **A lost claim rolls the record back**, to exactly the document the
///     commit replaced (a `Put` of the previous bytes, or a `Delete` when
///     there was none) — and only *if the record has not changed since*,
///     compared byte for byte against what this commit wrote. Rolling back
///     unconditionally would discard a concurrent writer's record; not
///     rolling back would leave a committed snapshot nobody can reach by the
///     name it claims.
///   * **A rollback failure is logged, not returned.** The caller must see
///     the alias conflict, which is the actionable cause; reporting the
///     rollback instead would name a symptom.
///   * **An already-committed record is refused**, because republishing would
///     overwrite the files the existing record points at — and `publish`
///     then deletes them on failure, which would take the live snapshot's
///     artifacts with it.
///   * **A record marked `deleting` is refused**, since its artifacts may
///     already be gone.
///   * **A missing record is created**, because a sandbox snapshot has no
///     pre-created `Waiting` record the way a template build does.
RepositoryResult<SnapshotRecord> Commit(SnapshotRepository& repository,
                                        const SnapshotPublishMetadata& metadata,
                                        const CommittedSnapshot& committed);

/// Rust `catalog::attach_startup_manifest` — records a stored startup
/// manifest on a committed snapshot.
///
/// Returns **false** when the snapshot is gone or being deleted, which is not
/// an error: the manifest is built after publication returns, so a delete can
/// legitimately win the race. The caller uses the false to remove the
/// manifest file it just stored — without it, that blob is referenced by no
/// record and is never collected.
///
/// Attaching twice is a no-op rather than an overwrite (the first manifest is
/// already the one the record names), and a snapshot that exists but is *not*
/// committed is an error rather than a false: that is a caller bug, not a
/// race, and reporting it as a race would silently drop the manifest.
RepositoryResult<bool> AttachStartupManifest(SnapshotRepository& repository,
                                             const core::SnapshotId& id,
                                             const MemoryStartupPackInfo& info);

}  // namespace record_catalog
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_RECORD_CATALOG_H_
