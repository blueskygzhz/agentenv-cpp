// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/artifacts.rs
//
// Imports manager-owned local build artifacts into committed repository
// storage on a shared POSIX filesystem.
//
// Two invariants drive the whole file:
//   1. Managed layers are content-addressed and immutable. Re-importing the
//      same digest must be a no-op, and a digest that already exists with a
//      different size is a hard error rather than a silent overwrite.
//   2. A copy is only trusted after it has been re-read from disk. Hard links
//      are preferred (no copy at all), but when we do copy we verify the
//      persisted bytes, not the bytes we believe we wrote.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_ARTIFACTS_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_ARTIFACTS_H_

#include <string>
#include <vector>

#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/manifest.h"
#include "agentenv/snapshot/drive.h"
#include "agentenv/snapshot/layers.h"
#include "agentenv/snapshot/repository/errors.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {

/// Rust `struct CollectedBuiltArtifacts`.
struct CollectedBuiltArtifacts {
    std::vector<OverlaybdLayerRef>      rootfs_layers;
    std::vector<ManagedLayer>           memory_layers;
    std::vector<CommittedAttachedDrive> attached_drives;
};

/// Rust `struct PosixFsArtifactStore`.
class PosixFsArtifactStore {
 public:
    explicit PosixFsArtifactStore(std::string root) : root_(std::move(root)) {}

    /// Rust `managed_layer_path`.
    std::string ManagedLayerPath(const std::string& digest) const;

    /// Rust `publish_volume_backing` — a volume's lowers are allowed to be
    /// descriptorless because the volume manager owns them locally.
    RepositoryResult<std::vector<OverlaybdLayerRef> >
        PublishVolumeBacking(const std::string& image_config_path);

    /// Rust `import_built_artifacts`.
    ///
    /// 1. copy committed fixed-layout runtime files (`vm_state.bin`)
    /// 2. persist the firecracker manifest
    /// 3. derive committed memory layers from the build-time mem image config
    /// 4. derive attached-drive layers, then rootfs layers
    ///
    /// Committed metadata intentionally does NOT persist the build-time
    /// `image.json` files: runtime resolvers regenerate them from the
    /// committed manifest plus the backend layout, so a node-local path can
    /// never leak into a portable record.
    RepositoryResult<CollectedBuiltArtifacts>
        ImportBuiltArtifacts(const core::SnapshotId& snapshot_id,
                             const sandbox::SandboxSnapshotManifest& manifest);

    // ---- pieces exposed for tests (Rust keeps them private but its in-file
    // ---- `mod tests` can still reach them) ---------------------------------

    /// Rust `derive_rootfs_layers`.
    ///
    /// Per lower, in Rust's order:
    ///   - local file + digest + size>0 -> managed import trusting the descriptor
    ///   - local file, and (allow_descriptorless || runtime-generated delta)
    ///     -> managed import by content hash
    ///   - local file otherwise -> Unsupported
    ///   - no file but an effective repoBlobUrl -> External
    ///   - neither -> Unsupported
    RepositoryResult<std::vector<OverlaybdLayerRef> >
        DeriveRootfsLayers(const std::string& image_config_path, bool allow_descriptorless);

    /// Rust `derive_memory_layers` — every memory lower must be local.
    RepositoryResult<std::vector<ManagedLayer> >
        DeriveMemoryLayers(const std::string& mem_image_config_path);

    /// Rust `store_managed_layer` — idempotent by digest; a size mismatch
    /// against an existing file is an error.
    RepositoryResult<ManagedLayer>
        StoreManagedLayer(const std::string& source, const std::string& digest, uint64_t size,
                          const core::Optional<std::string>& uuid);

    /// Rust `import_managed_layer_with_descriptor` — validates only the cheap
    /// size invariant, trusting internally generated content digests.
    RepositoryResult<ManagedLayer>
        ImportManagedLayerWithDescriptor(const std::string& source, const std::string& digest,
                                         uint64_t size);

    /// Rust `import_managed_layer_by_hash`.
    RepositoryResult<ManagedLayer> ImportManagedLayerByHash(const std::string& source);

    /// Rust `copy_local_artifact` — hard-links when possible, verifies size.
    RepositoryResult<core::Unit>
        CopyLocalArtifact(const std::string& destination, const std::string& source);

 private:
    std::string root_;
};

// ---- free helpers (Rust module-level fns) --------------------------------

/// Rust `same_file` — dev+ino equality; a missing destination is `false`,
/// not an error.
RepositoryResult<bool> SameFile(const std::string& source, const std::string& destination);

/// Rust `chmod_read_only` — clears the write bits (`mode & !0o222`).
RepositoryResult<core::Unit> ChmodReadOnly(const std::string& path);

/// Rust `copy_file_with_sha256` — copies while hashing, fsyncs, then re-reads
/// the destination and compares digests. The second pass is deliberate: it
/// validates the persisted bytes rather than the write loop's view of them.
RepositoryResult<core::Unit>
    CopyFileWithSha256(const std::string& source, const std::string& destination);

/// Rust `hard_link_or_copy_file_with_sha256`.
RepositoryResult<core::Unit>
    HardLinkOrCopyFileWithSha256(const std::string& source, const std::string& destination);

/// Rust `hard_link_or_copy_managed_layer` — an existing destination is
/// success (the layer is content-addressed, so it is already correct).
RepositoryResult<core::Unit>
    HardLinkOrCopyManagedLayer(const std::string& source, const std::string& destination,
                               const std::string& destination_parent);

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_ARTIFACTS_H_
