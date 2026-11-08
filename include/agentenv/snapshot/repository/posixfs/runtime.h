// SPDX-License-Identifier: MIT
// Rust: src/snapshot/resolver.rs :: RepositoryRuntimeResolver.
//
// Provenance correction. This header used to cite
// `src/snapshot/repository/backends/posixfs/runtime.rs`, **which does not
// exist upstream** -- `repository/backends/posixfs/` holds only
// `artifacts.rs`, `backend.rs`, `catalog.rs` and `mod.rs`. The real
// counterpart is the *generic* `RepositoryRuntimeResolver`, which resolves
// against any `Repository`; upstream's `PosixFsBackend::from_parts`
// constructs that one rather than a posix-specific resolver.
//
// So the posix-only shape of this class is a port deviation, not upstream's
// design. It is kept for now because nothing here yet needs to resolve
// against the OSS backend (whose resolver upstream builds from the same
// generic type plus a P2P transport), and generalising it with one
// implementation and no second caller would be speculative. Recorded rather
// than left to look deliberate.
//
// It does now implement `snapshot::SnapshotRuntimeResolver`. The previous note
// here said it could not until `interfaces.h` stopped carrying a cut-down
// `SnapshotRecord` -- that unification has since happened, so the note was
// stale; what actually remained was the trait's wrong return type, now fixed
// by moving it to `snapshot/resolver.h` where upstream keeps it.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_RUNTIME_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_RUNTIME_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/sandbox/manifest.h"
#include "agentenv/snapshot/artifact_cache.h"
#include "agentenv/snapshot/drive.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/errors.h"
// `RunnableSnapshot` and the `SnapshotRuntimeResolver` seam this implements.
#include "agentenv/snapshot/resolver.h"
#include "agentenv/snapshot/runtime_support.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {

// `RunnableSnapshot` now lives in `snapshot/resolver.h`, beside the trait
// that returns it -- it is not posix-specific, and declaring it here left the
// seam unable to name its own return type.

/// Rust `RepositoryRuntimeResolver`, specialised to a POSIX repository root
/// (see the provenance note at the top of this file).
class PosixFsRuntimeResolver : public SnapshotRuntimeResolver {
 public:
    PosixFsRuntimeResolver(const std::string& repository_root,
                           const std::string& runtime_cache_root,
                           const image::cache::OverlaybdLayerStore* store,
                           std::shared_ptr<LocalArtifactCache> cache);

    /// Rust `resolve(snapshot) -> RunnableSnapshot`.
    RepositoryResult<RunnableSnapshot> Resolve(const SnapshotRecord& snapshot) override;

    /// Rust `resolve_local_managed_layer`. Public because the materializer
    /// reaches it through a C-style callback (`ResolveManagedFn`).
    RepositoryResult<storage::overlaybd::LayerConfig>
        ResolveLocalManagedLayer(size_t index, const ManagedLayer& layer,
                                 const std::string& artifact_prefix) const;

    /// Exposed for the same callback bridge.
    RuntimeImageMaterializer& image_materializer() { return image_materializer_; }

 private:
    /// Rust `snapshot_vm_state_path`.
    RepositoryResult<std::string> SnapshotVmStatePath(const core::SnapshotId& id) const;
    /// Rust `load_committed_firecracker_manifest`.
    RepositoryResult<sandbox::SandboxSnapshotManifest>
        LoadCommittedFirecrackerManifest(const core::SnapshotId& id) const;

    /// Rust `resolve_attached_drives`.
    RepositoryResult<std::vector<ResolvedAttachedDrive> >
        ResolveAttachedDrives(const core::SnapshotId& id,
                              const CommittedSnapshot& snapshot,
                              CacheArtifactLease* lease);

    /// Describes one `materialize_and_pin` call.
    struct MaterializeSpec {
        std::string label;
        std::string cache_key;
        std::string artifact_prefix;
        bool        allow_empty_layers = false;
    };

    /// Rust `materialize_and_pin` — materialises the config, then pins it in
    /// the node-local cache so a concurrent resolve reuses the same file.
    RepositoryResult<std::string>
        MaterializeAndPin(const std::vector<OverlaybdLayerRef>& layers,
                          const std::string& destination,
                          const MaterializeSpec& spec,
                          CacheArtifactLease* lease);

    /// Rust `materialize_mem_image_config`.
    RepositoryResult<std::string>
        MaterializeMemImageConfig(const core::SnapshotId& id,
                                  const CommittedSnapshot& snapshot,
                                  CacheArtifactLease* lease);

    std::string                          repository_root_;
    RuntimeImageMaterializer             image_materializer_;
    std::shared_ptr<LocalArtifactCache>  cache_;
};

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_RUNTIME_H_
