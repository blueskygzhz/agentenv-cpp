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
// Not ported here: `publish` / `publish_with_local_layers` / `commit` /
// `attach_startup_manifest`, which need `publish.rs` and `store.rs`.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_RECORD_CATALOG_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_RECORD_CATALOG_H_

#include <string>
#include <vector>

#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/interfaces.h"
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

// `SnapshotCatalog::delete` is deliberately absent. Upstream's sequence is
// mark-deleting -> release alias -> **remove the artifacts** -> delete build
// log -> delete record, and that middle step is `store::delete_snapshot`,
// which needs `Repository::delete_snapshot_blobs` (not on this interface) and
// a best-effort rollback of any ACR disk publication (`AcrDiskImageExporter`,
// not ported). Deleting the record without it would drop the only reference
// to artifacts that stay in the bucket forever, and reporting success for
// that is worse than refusing: the caller believes the storage was reclaimed.
// A backend's `Delete` therefore reports `unsupported` naming the gap.

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

}  // namespace record_catalog
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_RECORD_CATALOG_H_
