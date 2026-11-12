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
#include <string>
#include <vector>

#include "agentenv/sandbox/manifest.h"
#include "agentenv/snapshot/artifact_cache.h"
#include "agentenv/snapshot/drive.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/repository/interfaces.h"
#include "agentenv/snapshot/runtime_support.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace p2p {
class P2pTransport;
}
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

/// Rust `RepositoryRuntimeResolver` — **one resolver serves every
/// repository**, which is upstream's opening sentence for this file and the
/// property the port had lost.
///
/// What it replaces. There used to be a `PosixFsRuntimeResolver`, built from a
/// *repository root path*, that found every artifact by composing that root
/// with the posix-fs layout. Nothing upstream has that shape: its
/// `PosixFsBackend` constructs this generic type like every other backend. The
/// consequences of the deviation were not cosmetic:
///
///   * an OSS node got **no resolver at all**, so `POST /sandboxes` answered
///     501 for every snapshot. It could publish snapshots it could never
///     launch — and the node said so only in a startup warning.
///   * `SnapshotRepository::FetchBlob` lost its one production caller.
///     `FetchBlob` exists precisely so a node that cannot mount the
///     repository can still read it, and the only code that needed it was the
///     resolver, which had been specialised to the case where mounting always
///     works.
///
/// How it decides, in upstream's words: files a backend can read in place are
/// used where they are, everything else is fetched P2P-first into the
/// node-local cache. `LocateBlob` draws that line — a `Local` answer is read
/// at its path, a `Remote` one is fetched. **A missing in-place file is an
/// error, never a reason to fetch from peers**: if the repository says it
/// holds the bytes at a path, a node that cannot see them there is
/// misconfigured, and silently sourcing them from a peer would hide that for
/// as long as some peer still had a copy.
class RepositoryRuntimeResolver : public SnapshotRuntimeResolver {
 public:
    /// `p2p_transport` may be null, which simply disables the peer-first leg
    /// and leaves the repository fetch — the transport is an accelerator, not
    /// a source of truth.
    ///
    /// `store` is a **raw** pointer because `RuntimeImageMaterializer` takes
    /// one; whatever owns it must outlive this resolver. `SnapshotBackend`
    /// holds the owning `shared_ptr` for exactly that reason.
    RepositoryRuntimeResolver(
        std::shared_ptr<repository::SnapshotRepository> repository,
        std::shared_ptr<LocalArtifactCache> cache,
        const std::string& runtime_root,
        const image::cache::OverlaybdLayerStore* store,
        std::shared_ptr<p2p::P2pTransport> p2p_transport);

    repository::RepositoryResult<RunnableSnapshot> Resolve(
        const SnapshotRecord& snapshot) override;

    /// Public only because `RuntimeImageMaterializer` reaches it through a
    /// C-style callback (`ResolveManagedFn`), which cannot name a private
    /// member.
    repository::RepositoryResult<storage::overlaybd::LayerConfig>
        ResolveManagedLayer(const ManagedLayer& layer, repository::LayerRole role) const;

 private:
    /// Rust `vm_state_path` — the one path that calls `FetchBlob`.
    repository::RepositoryResult<std::string>
        VmStatePath(const core::SnapshotId& id, CacheArtifactLease* lease);
    /// Rust `load_firecracker_manifest`.
    repository::RepositoryResult<sandbox::SandboxSnapshotManifest>
        LoadFirecrackerManifest(const core::SnapshotId& id);
    /// Rust `resolve_attached_drives`.
    repository::RepositoryResult<std::vector<ResolvedAttachedDrive> >
        ResolveAttachedDrives(const core::SnapshotId& id, const CommittedSnapshot& snapshot,
                              CacheArtifactLease* lease);

    /// Describes one `materialize_and_pin` call.
    struct MaterializeSpec {
        std::string              label;
        std::string              cache_key;
        repository::LayerRole    role;
        bool                     allow_empty_layers;
        MaterializeSpec() : role(repository::LayerRole::Rootfs), allow_empty_layers(false) {}
    };

    /// Rust `materialize_and_pin`.
    repository::RepositoryResult<std::string>
        MaterializeAndPin(const std::vector<OverlaybdLayerRef>& layers,
                          const std::string& destination, const MaterializeSpec& spec,
                          CacheArtifactLease* lease);
    /// Rust `materialize_image_config` — validates every managed layer is
    /// stored, then writes the config.
    repository::RepositoryResult<std::string>
        MaterializeImageConfig(const std::vector<OverlaybdLayerRef>& layers,
                               const std::string& destination, const MaterializeSpec& spec);

    std::shared_ptr<repository::SnapshotRepository> repository_;
    std::shared_ptr<LocalArtifactCache>             cache_;
    RuntimeImageMaterializer                        image_materializer_;
    std::shared_ptr<p2p::P2pTransport>              p2p_transport_;
};

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_RESOLVER_H_
