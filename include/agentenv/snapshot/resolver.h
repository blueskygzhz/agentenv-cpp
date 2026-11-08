// SPDX-License-Identifier: MIT
// Rust: src/snapshot/resolver.rs — the `SnapshotRuntimeResolver` trait, and
//       `RunnableSnapshot` from src/snapshot/types/snapshot.rs.
//
// Why this is its own header. The trait used to live in
// `snapshot/repository/interfaces.h`, which is not where upstream keeps it:
// `repository/interfaces.rs` carries the *repository* trait, and the runtime
// resolver has a file of its own. The misplacement was not harmless -- that
// copy declared `Resolve` as returning `RepositoryResult<std::string>` where
// upstream returns a `RunnableSnapshot`, so the one real resolver
// (`PosixFsRuntimeResolver`) could not implement it and deliberately did not
// try. A seam nothing can satisfy is decorative, and this is the seam a
// backend selector has to return.
//
// **Deviation on `RunnableSnapshot`'s home**: upstream declares it in
// `types/snapshot.rs`, beside `SnapshotRecord` -- so the C++ counterpart
// would be `snapshot/record.h`. It is here instead, deliberately.
// `record.h` is compiled into `agentenv-snapshot-record`, a cut kept small on
// purpose because `agentenv-image` links it; `RunnableSnapshot` carries a
// `SandboxSnapshotManifest` and a cache lease, which would drag
// `sandbox/manifest.h` and the artifact cache into every translation unit
// that only wanted a record. Upstream pays nothing for that grouping because
// Rust modules are not translation units. This header is included only by
// the resolver and its implementations, which need all three types anyway.
#ifndef AGENTENV_SNAPSHOT_RESOLVER_H_
#define AGENTENV_SNAPSHOT_RESOLVER_H_

#include <memory>

#include "agentenv/sandbox/manifest.h"
#include "agentenv/snapshot/artifact_cache.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/errors.h"

namespace agentenv {
namespace snapshot {

/// Rust `struct RunnableSnapshot` — the record, the hydrated node-local
/// manifest, and the cache lease that keeps the materialised files pinned for
/// as long as the caller holds it.
///
/// The lease is the load-bearing field: upstream names it `_lease` and never
/// reads it, because its only job is to stay alive. Dropping it early lets
/// the runtime cache reclaim the very `image.json` and layer files the
/// manifest points at, while the launch that is about to use them is still
/// in flight.
struct RunnableSnapshot {
    SnapshotRecord                       record;
    sandbox::SandboxSnapshotManifest     manifest;
    std::shared_ptr<CacheArtifactLease>  lease;
};

/// Rust `trait SnapshotRuntimeResolver`.
///
/// Turns one committed record into paths this node can launch from. The
/// contract upstream states, and the reason the lease above exists:
///
///   - the record's committed payload must be present;
///   - the returned paths are directly usable by the launch code on *this*
///     node;
///   - node-local derived files (a runnable `image.json`) may be
///     materialised, written atomically so a concurrent resolve never
///     observes a half-written config;
///   - committed repository truth is never mutated to produce them.
class SnapshotRuntimeResolver {
 public:
    virtual ~SnapshotRuntimeResolver() {}

    /// Rust `resolve(snapshot) -> RunnableSnapshot`.
    ///
    /// Takes the record by const reference where upstream takes an
    /// `Arc<SnapshotRecord>`: the resolver does not outlive the call, so there
    /// is no ownership to share.
    virtual repository::RepositoryResult<RunnableSnapshot> Resolve(
        const SnapshotRecord& snapshot) = 0;
};

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_RESOLVER_H_
