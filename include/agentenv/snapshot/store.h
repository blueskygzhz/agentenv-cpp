// SPDX-License-Identifier: MIT
// Rust: src/snapshot/store.rs — `store_snapshot`, `delete_snapshot` and
// `store_volume_layers`, plus the `LayerStore` parts they reach.
//
// Rust keeps one `LayerStore` for snapshots and volumes. Both entry points are
// here, written against the repository's blob interface like Rust's, so they
// work on any backend rather than only on POSIX.
#ifndef AGENTENV_SNAPSHOT_STORE_H_
#define AGENTENV_SNAPSHOT_STORE_H_

#include <functional>
#include <string>
#include <vector>

#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/overlaybd/compact.h"
#include "agentenv/sandbox/manifest.h"
#include "agentenv/snapshot/acr_manifest.h"
#include "agentenv/snapshot/drive.h"
#include "agentenv/snapshot/image_export/acr_publisher.h"
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
/// The upload format comes from the repository's
/// `GetPublishPolicy().compression`, as it does upstream. A backend that does
/// not override the policy stores layers raw -- which is every backend except
/// OSS, here and in Rust alike, because that is the one the operator's
/// `[snapshot.publish_compression]` switch is wired to.
repository::RepositoryResult<std::vector<OverlaybdLayerRef> > StoreVolumeLayers(
    repository::SnapshotRepository& repository, const std::string& image_config_path);

/// Rust `struct StoredSnapshot` — what was stored for one snapshot before its
/// record is committed.
struct StoredSnapshot {
    std::vector<OverlaybdLayerRef> rootfs_layers;
    std::vector<ManagedLayer> memory_layers;
    std::vector<CommittedAttachedDrive> attached_drives;

    /// The source-registry publications made for this snapshot, in the order
    /// they were created — rootfs first, then one per attached drive.
    ///
    /// Empty unless the repository's `GetPublishPolicy().image_storage` is
    /// `SourceRegistry`, which only the OSS backend ever reports. The order is
    /// load-bearing: `DeleteSnapshot` unwinds it in reverse.
    std::vector<PersistedDiskImagePublication> disk_publications;

    /// Rust `local_layers` — the exact files stored as layers, kept alive for
    /// the post-commit P2P announcement.
    ///
    /// Each entry owns the temporary it was prepared into (a recontainerized
    /// zfile, or a dense export of a sparse layer), so holding them past the
    /// commit is what keeps those bytes readable. A layer uploaded unchanged
    /// contributes its original path and owns no temporary.
    ///
    /// Populated on both routes: `PutPrepared` appends every layer it stores,
    /// and the source-registry route appends whatever its exporter uploaded.
    /// Survives a mid-way failure for the same reason `disk_publications`
    /// does — `StoreSnapshot` keeps the partial result to clean up with.
    std::vector<PreparedLayerUpload> local_layers;
};

/// Rolls one source-registry publication back. Rust calls
/// `AcrDiskImageExporter::rollback_publication` directly; here it is a functor
/// so `DeleteSnapshot` can be driven without a registry, and so this header
/// does not have to pull in the HTTP transport.
typedef std::function<repository::RepositoryResult<core::Unit>(
    const PersistedDiskImagePublication&)>
    PublicationRollback;

/// The production rollback: `image_export::AcrDiskImageExporter` over
/// Docker-config credentials and real HTTPS.
PublicationRollback AcrPublicationRollback();

/// Publishes one disk image to its source registry — Rust
/// `AcrDiskImageExporter::export`.
///
/// The `Optional<SnapshotOciConfigInput>` argument is upstream's `config:
/// Option<..>`: present for the rootfs (which publishes the effective runtime
/// config) and absent for an attached drive (whose build-time config the
/// runtime resolver regenerates).
typedef std::function<repository::RepositoryResult<image_export::DiskImageExportOutcome>(
    const core::SnapshotId& snapshot_id, const image_export::DiskImageSubject& subject,
    const std::string& image_config_path,
    const core::Optional<SnapshotOciConfigInput>& config)>
    DiskImageExport;

/// The two halves of `AcrDiskImageExporter` that a publish needs, injected
/// together.
///
/// They are bundled rather than passed as two arguments because upstream has
/// **one** object here: `LayerStore` publishes with
/// `AcrDiskImageExporter::export`, and the cleanup on failure rolls the
/// already-made publications back through `rollback_publication` on an
/// exporter of the same kind. Splitting them into independent parameters would
/// let a caller supply a publisher with no matching rollback, and the failure
/// that produces is silent: a publish that dies partway would leave its
/// earlier publications in the registry with nothing naming them.
struct SourceRegistryExporter {
    DiskImageExport publish;
    PublicationRollback rollback;
};

/// The production exporter: `image_export::AcrDiskImageExporter` over
/// Docker-config credentials and real HTTPS.
///
/// `publish_compression` is **required**, matching Rust's
/// `AcrDiskImageExporter::new(self.compression)` — and unlike the exporter
/// class's own constructor default. Here the value is always available (it is
/// the repository's own `GetPublishPolicy().compression`), so defaulting it
/// could only ever mean "silently upload raw bytes where the operator asked
/// for zfile", which is the publish format changing behind their back.
///
/// The rollback half deliberately does **not** carry the compression: a
/// rollback only deletes a manifest by digest, and upstream likewise builds
/// its rollback exporter with `OverlaybdCompactOutput::Raw`.
SourceRegistryExporter AcrSourceRegistryExporter(
    const overlaybd::OverlaybdCompactOutput& publish_compression);

/// Rust `store_snapshot` — stores one captured snapshot's files and layers.
///
/// On failure it removes what it stored, *except* shared content-addressed
/// layers: those are keyed by digest and another snapshot may already
/// reference the identical bytes, so deleting them here would corrupt an
/// unrelated snapshot. The cleanup failure is logged rather than returned,
/// because the caller must see the original error.
///
/// **The cleanup also rolls back any publication already made**, which is why
/// a partial `StoredSnapshot` has to survive the error (Rust keeps `store`
/// alive past the `match` for exactly this). A snapshot that published its
/// rootfs and then failed on its second drive would otherwise leave the
/// rootfs image in the registry, referenced by no record.
///
/// Order matters and is upstream's: the rootfs and memory layers first, then
/// the VM state blob, then the serialised manifest, then the attached drives.
/// The manifest is what a restore reads to find everything else, so it is
/// written after the artifacts it names — a manifest pointing at a layer that
/// is not there yet is exactly what a crash between the two would leave.
///
/// This overload supplies the production ACR exporter, built over the
/// repository's own `GetPublishPolicy().compression`.
repository::RepositoryResult<StoredSnapshot> StoreSnapshot(
    repository::SnapshotRepository& repository, const SnapshotPublishMetadata& metadata,
    const sandbox::SandboxSnapshotManifest& manifest);

/// `StoreSnapshot` with the source-registry exporter injected.
///
/// Rust `LayerStore::store_snapshot` builds the exporter itself, guarded by
/// the policy. The guard stays here — `exporter.publish` is consulted **only**
/// when `GetPublishPolicy().image_storage == SourceRegistry` — so passing one
/// does not force a publish to the registry. A node whose policy says
/// `ObjectStorage` never calls it, exactly as upstream never constructs one.
repository::RepositoryResult<StoredSnapshot> StoreSnapshot(
    repository::SnapshotRepository& repository, const SnapshotPublishMetadata& metadata,
    const sandbox::SandboxSnapshotManifest& manifest,
    const SourceRegistryExporter& exporter);

/// Rust `delete_snapshot` — rolls the publications back, then removes the
/// snapshot's files.
///
/// Shared layers stay: other snapshots may reference them.
///
/// Publications go in **reverse** order, the opposite of how they were
/// created, and each failure is logged and stepped over rather than returned.
/// Both halves of that matter: a rollback error must not stop the record from
/// being removed (the record is what makes the snapshot retryable, and a
/// registry that is down would otherwise pin it forever), yet the attempt has
/// to happen first, because once the record is gone nothing names the
/// publications and they would stay in the registry with no way to find them.
repository::RepositoryResult<core::Unit> DeleteSnapshot(
    repository::SnapshotRepository& repository, const core::SnapshotId& id,
    const std::vector<PersistedDiskImagePublication>& publications,
    const PublicationRollback& rollback);

/// `DeleteSnapshot` with nothing to roll back.
///
/// Used by the publish path, which fails before any publication exists.
repository::RepositoryResult<core::Unit> DeleteSnapshot(
    repository::SnapshotRepository& repository, const core::SnapshotId& id);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_STORE_H_
