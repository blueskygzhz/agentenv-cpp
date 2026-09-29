// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/runtime.rs
//
// Resolves committed snapshot artifacts into node-local runnable paths on a
// POSIX filesystem.
//
// Porting note. `repository::SnapshotRuntimeResolver` in `interfaces.h` is
// still the scaffold subset (its `SnapshotRecord` carries `id` as a string and
// `committed` as a bool), so this resolver deliberately does *not* implement
// that interface yet: it works on the real `snapshot::SnapshotRecord` and
// returns a `RunnableSnapshot`, exactly as Rust does. Once `interfaces.h` is
// migrated onto the real record, the only change needed here is adding the
// `: public SnapshotRuntimeResolver` base and an `override`.
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
#include "agentenv/snapshot/runtime_support.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {

/// Rust `struct RunnableSnapshot` — the record, the hydrated node-local
/// manifest, and the cache lease that keeps the materialised files pinned for
/// as long as the caller holds it.
struct RunnableSnapshot {
    SnapshotRecord                       record;
    sandbox::SandboxSnapshotManifest     manifest;
    std::shared_ptr<CacheArtifactLease>  lease;
};

/// Rust `struct PosixFsRuntimeResolver`.
class PosixFsRuntimeResolver {
 public:
    PosixFsRuntimeResolver(const std::string& repository_root,
                           const std::string& runtime_cache_root,
                           const OverlaybdLayerStore* store,
                           std::shared_ptr<LocalArtifactCache> cache);

    /// Rust `resolve(snapshot) -> RunnableSnapshot`.
    RepositoryResult<RunnableSnapshot> Resolve(const SnapshotRecord& snapshot);

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
