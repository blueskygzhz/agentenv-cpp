// SPDX-License-Identifier: MIT
// Rust: src/snapshot/publish.rs
#include "agentenv/snapshot/publish.h"

#include <set>
#include <string>

#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"
#include "agentenv/snapshot/repository/documents.h"
#include "agentenv/snapshot/repository/record_catalog.h"
#include "agentenv/snapshot/store.h"

namespace agentenv {
namespace snapshot {
namespace {

using repository::DocumentKey;
using repository::RepositoryError;
using repository::RepositoryResult;
using repository::SnapshotRepository;

/// Rust `validate_attached_drives`.
RepositoryResult<core::Unit> ValidateAttachedDrives(
    const sandbox::SandboxSnapshotManifest& manifest) {
    std::set<std::string> drive_ids;
    for (std::size_t i = 0; i < manifest.attached_drives.size(); ++i) {
        const sandbox::SnapshotAttachedDriveArtifacts& drive = manifest.attached_drives[i];
        // Duplicate ids would publish two drives the restore cannot tell
        // apart: the record keys them by id, so the second would shadow the
        // first and one captured disk would silently vanish.
        if (!drive_ids.insert(drive.drive_id).second) {
            return core::make_unexpected(RepositoryError::InvalidRequest(
                "duplicate attached drive id in publish request: " + drive.drive_id));
        }
        // A zero virtual size cannot be mounted on restore, and the failure
        // would surface there rather than here.
        if (drive.virtual_size == 0) {
            return core::make_unexpected(RepositoryError::InvalidRequest(
                "attached drive '" + drive.drive_id + "' virtual_size must be non-zero"));
        }
    }
    return core::Unit();
}

/// Rust `ensure_not_committed`.
///
/// Reads the record as *untyped* JSON and asks only whether `committed` is
/// present and non-null. That is deliberate: this gate must work on a record
/// this build cannot fully parse — a record written by a newer server, or one
/// whose unknown fields would fail the typed decoder. Decoding it as a
/// `SnapshotRecord` would turn "I cannot read this" into a publish that
/// proceeds to overwrite a live snapshot's files.
RepositoryResult<core::Unit> EnsureNotCommitted(SnapshotRepository& repository,
                                                const core::SnapshotId& id) {
    const RepositoryResult<core::Optional<std::string> > bytes =
        repository.GetDocument(DocumentKey::SnapshotRecord(id));
    if (!bytes.ok()) return core::make_unexpected(bytes.error());
    if (!bytes.value().has_value()) return core::Unit();

    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(*bytes.value());
    if (!json.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "parse snapshot record '" + id.ToString() + "'", json.error().chain()));
    }
    bool committed = false;
    if (json.value().kind() == core::Json::Kind::Object) {
        const core::JsonObject& fields = json.value().as_object();
        const core::JsonObject::const_iterator found = fields.find("committed");
        committed = found != fields.end() && found->second.kind() != core::Json::Kind::Null;
    }
    if (committed) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            "snapshot '" + id.ToString() + "' is already committed"));
    }
    return core::Unit();
}

}  // namespace

RepositoryResult<SnapshotPublication> Publish(
    SnapshotRepository& repository, const SnapshotPublishMetadata& metadata,
    const sandbox::SandboxSnapshotManifest& manifest) {
    return Publish(repository, metadata, manifest,
                   AcrSourceRegistryExporter(
                       overlaybd::OverlaybdCompactOutput::FromPublishCompressionConfig(
                           repository.GetPublishPolicy().compression)));
}

RepositoryResult<SnapshotPublication> Publish(
    SnapshotRepository& repository, const SnapshotPublishMetadata& metadata,
    const sandbox::SandboxSnapshotManifest& manifest,
    const SourceRegistryExporter& exporter) {
    const RepositoryResult<core::Unit> drives = ValidateAttachedDrives(manifest);
    if (!drives.ok()) return core::make_unexpected(drives.error());

    // Fail before uploading anything; the commit checks both again.
    const RepositoryResult<core::Unit> uncommitted = EnsureNotCommitted(repository, metadata.id);
    if (!uncommitted.ok()) return core::make_unexpected(uncommitted.error());

    if (metadata.alias.has_value()) {
        const RepositoryResult<core::Unit> available = repository::record_catalog::
            EnsureAliasAvailable(repository, *metadata.alias, metadata.id);
        if (!available.ok()) return core::make_unexpected(available.error());
    }

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(repository, metadata, manifest, exporter);
    if (!stored.ok()) return core::make_unexpected(stored.error());

    CommittedSnapshot committed;
    committed.context = metadata.context;
    committed.startup = metadata.startup;
    committed.runtime_versions = metadata.runtime_versions;
    committed.virtualization_mode = metadata.virtualization_mode;
    committed.image_configs = metadata.image_configs;
    committed.custom_extension_params = metadata.custom_extension_params;
    committed.rootfs_layers = stored.value().rootfs_layers;
    committed.attached_drives = stored.value().attached_drives;
    committed.volume_snapshots = metadata.volume_snapshots;
    committed.memory_layers = stored.value().memory_layers;
    // The source-registry publications, in creation order. Empty unless the
    // repository's policy is `SourceRegistry`.
    //
    // These have to reach the record: it is the only thing that names them, so
    // a publication committed without one could never be rolled back — see
    // `record_catalog`'s delete path, which reads exactly this field.
    committed.disk_publications = stored.value().disk_publications;
    // The startup manifest is attached later by its continuation, which is why
    // it is absent rather than built here: the pack is produced after the
    // record is committed, so a publish that failed would otherwise have to
    // undo it.

    const RepositoryResult<SnapshotRecord> record =
        repository::record_catalog::Commit(repository, metadata, committed);
    if (record.ok()) {
        AGENTENV_DEBUG("published snapshot " << record.value().id.ToString());
        SnapshotPublication publication;
        publication.record = record.value();
        return publication;
    }

    // Rust `delete_snapshot(repository, &metadata.id, &disk_publications)` —
    // the publications go too. The commit is what would have named them, so
    // once it fails nothing else ever can: leaving them would strand the
    // images in the registry permanently.
    const RepositoryResult<core::Unit> cleanup = DeleteSnapshot(
        repository, metadata.id, committed.disk_publications, exporter.rollback);
    if (!cleanup.ok()) {
        // Logged, not returned: the caller must see the commit error, which is
        // the actionable cause.
        AGENTENV_WARN("failed to roll back snapshot files " << metadata.id.ToString() << ": "
                                                           << cleanup.error().ToString());
    }
    return core::make_unexpected(record.error());
}

}  // namespace snapshot
}  // namespace agentenv
