// SPDX-License-Identifier: MIT
// Rust: src/snapshot/store.rs — `store_snapshot`, `delete_snapshot` and
// `store_volume_layers`, plus the `LayerStore` parts they reach.
//
// Rust keeps one `LayerStore` for snapshots and volumes. Both entry points are
// here, written against the repository's blob interface like Rust's, so they
// work on any backend rather than only on POSIX.
#ifndef AGENTENV_SNAPSHOT_STORE_H_
#define AGENTENV_SNAPSHOT_STORE_H_

#include <string>
#include <vector>

#include "agentenv/core/identity.h"
#include "agentenv/sandbox/manifest.h"
#include "agentenv/snapshot/drive.h"
#include "agentenv/snapshot/layers.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/interfaces.h"

namespace agentenv {
namespace snapshot {

/// Rust `store_volume_layers` — stores a volume's node-local backing layers
/// and returns their references:
/// `LayerStore::new(repository).store_disk_layers(path, LayerRole::Rootfs, true)`.
///
/// Every local lower becomes a managed layer keyed by content digest (a
/// capture-side descriptor is trusted after a size check, otherwise the file
/// is hashed; a sparse layer is dense-exported first). Lowers that only carry
/// a `repoBlobUrl` stay external references.
///
/// Port gap: Rust picks the upload format from the repository's
/// `publish_policy().compression`. The C++ repository has no publish policy,
/// so layers are always stored as Rust's `OverlaybdCompactOutput::Raw` stores
/// them — passed through unchanged.
repository::RepositoryResult<std::vector<OverlaybdLayerRef> > StoreVolumeLayers(
    repository::SnapshotRepository& repository, const std::string& image_config_path);

/// Rust `struct StoredSnapshot` — what was stored for one snapshot before its
/// record is committed.
struct StoredSnapshot {
    std::vector<OverlaybdLayerRef> rootfs_layers;
    std::vector<ManagedLayer> memory_layers;
    std::vector<CommittedAttachedDrive> attached_drives;
    /// Rust also carries `disk_publications` (source-registry exports) and
    /// `local_layers` (the exact files kept alive for post-commit P2P
    /// announcement). Both are absent here, and neither is a dropped field:
    ///
    ///   * `disk_publications` can only be produced by `AcrDiskImageExporter`,
    ///     which the policy that selects it (`publish_policy().image_storage ==
    ///     SourceRegistry`) does not exist to select in this port — so the
    ///     vector is unconditionally empty rather than unported.
    ///   * `local_layers` exists to feed P2P publication, whose transport is
    ///     not ported either.
    ///
    /// Adding empty fields for them would imply this port *can* produce them.
};

/// Rust `store_snapshot` — stores one captured snapshot's files and layers.
///
/// On failure it removes what it stored, *except* shared content-addressed
/// layers: those are keyed by digest and another snapshot may already
/// reference the identical bytes, so deleting them here would corrupt an
/// unrelated snapshot. The cleanup failure is logged rather than returned,
/// because the caller must see the original error.
///
/// Order matters and is upstream's: the rootfs and memory layers first, then
/// the VM state blob, then the serialised manifest, then the attached drives.
/// The manifest is what a restore reads to find everything else, so it is
/// written after the artifacts it names — a manifest pointing at a layer that
/// is not there yet is exactly what a crash between the two would leave.
repository::RepositoryResult<StoredSnapshot> StoreSnapshot(
    repository::SnapshotRepository& repository, const SnapshotPublishMetadata& metadata,
    const sandbox::SandboxSnapshotManifest& manifest);

/// Rust `delete_snapshot` — removes one snapshot's files.
///
/// Shared layers stay: other snapshots may reference them. Rust also rolls
/// back source-registry publications here, in reverse order, before deleting
/// blobs; that half needs `AcrDiskImageExporter` and is unreachable in this
/// port for the reason given on `StoredSnapshot`, so this is the blob delete
/// alone.
repository::RepositoryResult<core::Unit> DeleteSnapshot(
    repository::SnapshotRepository& repository, const core::SnapshotId& id);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_STORE_H_
