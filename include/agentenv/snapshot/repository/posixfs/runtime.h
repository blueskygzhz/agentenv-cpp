// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/runtime.rs
//
// Resolves committed snapshot artifacts into node-local runnable paths on a
// POSIX filesystem. This is the resolver that stamps runtime-local image
// configs and vm_state paths onto a committed manifest.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_RUNTIME_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_RUNTIME_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/snapshot/artifact_cache.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/repository/interfaces.h"
#include "agentenv/snapshot/repository/posixfs/layout.h"
#include "agentenv/snapshot/runtime_support.h"
#include "agentenv/snapshot/types.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {

/// Rust `struct PosixFsRuntimeResolver`.
class PosixFsRuntimeResolver : public SnapshotRuntimeResolver {
 public:
    PosixFsRuntimeResolver(const std::string& repository_root,
                           const std::string& runtime_cache_root,
                           const OverlaybdLayerStore* store,
                           std::shared_ptr<LocalArtifactCache> cache);

    /// Rust `resolve(snapshot) -> RunnableSnapshot`.
    RepositoryResult<RunnableSnapshot> Resolve(const SnapshotRecord& snapshot) override;

 private:
    RepositoryResult<std::string> SnapshotVmStatePath(const core::SnapshotId& id);
    RepositoryResult<sandbox::SandboxSnapshotManifest>
        LoadCommittedFirecrackerManifest(const core::SnapshotId& id);

    RepositoryResult<std::vector<ResolvedAttachedDrive>>
        ResolveAttachedDrives(const core::SnapshotId& id,
                              const CommittedSnapshot& snapshot,
                              std::vector<CacheHandle*>* handles);

    RepositoryResult<std::string> ResolveLocalManagedLayer(size_t index,
                                                           const ManagedLayer& layer,
                                                           const std::string& artifact_prefix);

    struct MaterializeSpec {
        std::string label;
        std::string cache_key;
        std::string artifact_prefix;
        bool allow_empty_layers;
    };

    RepositoryResult<std::string> MaterializeAndPin(
        const std::vector<OverlaybdLayerRef>& layers,
        const std::string& destination,
        const MaterializeSpec& spec,
        std::vector<CacheHandle*>* handles);

    RepositoryResult<std::string> MaterializeMemImageConfig(
        const core::SnapshotId& id,
        const CommittedSnapshot& snapshot,
        std::vector<CacheHandle*>* handles);

    std::string                       repository_root_;
    RuntimeImageMaterializer          image_materializer_;
    std::shared_ptr<LocalArtifactCache> cache_;
};

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_RUNTIME_H_
