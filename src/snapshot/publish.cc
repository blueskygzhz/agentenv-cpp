// SPDX-License-Identifier: MIT
// Rust: src/snapshot/publish.rs
#include "agentenv/snapshot/publish.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "agentenv/core/executor.h"
#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"
#include "agentenv/snapshot/p2p.h"
#include "agentenv/snapshot/repository/documents.h"
#include "agentenv/snapshot/repository/record_catalog.h"
#include "agentenv/snapshot/store.h"
#include "agentenv/snapshot/types.h"

namespace agentenv {
namespace snapshot {
namespace {

using repository::DocumentKey;
using repository::RepositoryError;
using repository::RepositoryResult;
using repository::SnapshotRepository;

/// Rust `const SNAPSHOT_P2P_PUBLISH_CONCURRENCY: usize = 8`.
const std::size_t kSnapshotP2pPublishConcurrency = 8;

/// Rust `managed_layer_uuids` — the uuids of the managed layers in a committed
/// subject's layer list. External layers have none.
std::set<std::string> ManagedLayerUuids(const std::vector<OverlaybdLayerRef>& layers) {
    std::set<std::string> uuids;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (!layers[i].is_managed()) continue;
        if (!layers[i].managed.uuid.has_value()) continue;
        uuids.insert(*layers[i].managed.uuid);
    }
    return uuids;
}

/// Rust `managed_layer_uuids_from_managed`.
std::set<std::string> ManagedLayerUuidsFromManaged(const std::vector<ManagedLayer>& layers) {
    std::set<std::string> uuids;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (!layers[i].uuid.has_value()) continue;
        uuids.insert(*layers[i].uuid);
    }
    return uuids;
}

/// Rust `committed_layer_digests` — every layer digest the committed record
/// references for one subject, managed and external alike.
std::set<std::string> CommittedLayerDigests(const std::vector<OverlaybdLayerRef>& layers) {
    std::set<std::string> digests;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        digests.insert(layers[i].digest());
    }
    return digests;
}

/// Rust `committed_memory_layer_digests`.
std::set<std::string> CommittedMemoryLayerDigests(const std::vector<ManagedLayer>& layers) {
    std::set<std::string> digests;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        digests.insert(layers[i].digest);
    }
    return digests;
}

void Extend(std::vector<SnapshotP2pArtifact>* into,
            const std::vector<SnapshotP2pArtifact>& from) {
    into->insert(into->end(), from.begin(), from.end());
}

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
        // Rust `local_layers: layer_store.local_layers`. Moved out of the
        // stored result so the uploads — and the temporaries they own — stay
        // alive for the caller's post-commit P2P announcement.
        publication.local_layers = stored.value().local_layers;
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

// Rust `SnapshotManager::publish_p2p_artifacts`.
void PublishSnapshotP2pArtifacts(const std::shared_ptr<p2p::P2pTransport>& transport,
                                 const SnapshotPublication& publication,
                                 const sandbox::SandboxSnapshotManifest& manifest) {
    // Rust: `let Some(transport) = self.p2p_transport.as_ref() else { return };`
    if (!transport) return;
    // Rust: `let Some(committed) = publication.record.committed.as_ref() else
    // { return };`. An uncommitted record names no layers, and announcing a
    // snapshot that is not yet usable would advertise bytes a peer cannot
    // resolve to anything.
    if (!publication.record.committed.has_value()) return;

    const CommittedSnapshot& committed = *publication.record.committed;
    const core::SnapshotId&  id        = publication.record.id;

    std::vector<SnapshotP2pArtifact> artifacts;

    // 1. The vm state file, by its per-snapshot key. Read from the capture
    //    path: the repository copy may be remote, and on an OSS backend it is
    //    always remote, which is exactly the case P2P exists to accelerate.
    artifacts.push_back(SnapshotP2pArtifact::Fixed(id, kSnapshotArtifactLayout.vm_state,
                                                   manifest.vm_state.path));

    // 2. The manifest, as bytes. Compact, matching upstream's
    //    `serde_json::to_vec` -- deliberately not the pretty copy
    //    `StoreSnapshot` writes to the repository. A consumer parses this, so
    //    the formatting is not part of the contract; the key is.
    const std::string manifest_json = manifest.ToJson().ToString();
    artifacts.push_back(SnapshotP2pArtifact::Bytes(
        id, kSnapshotArtifactLayout.firecracker_manifest,
        std::vector<uint8_t>(manifest_json.begin(), manifest_json.end())));

    // 3. The prepared uploads, content-addressed. These are the authoritative
    //    source for anything publish-time compression or dense export
    //    rewrote: the record names the rewritten digest, and those bytes exist
    //    only in the temporary each upload still owns.
    for (std::size_t i = 0; i < publication.local_layers.size(); ++i) {
        const PreparedLayerUpload& upload = publication.local_layers[i];
        artifacts.push_back(SnapshotP2pArtifact::ContentAddressedOverlaybdLayer(
            upload.Path(), upload.Digest(), upload.Size()));
    }

    // 4. The local overlaybd layers each runtime image config references,
    //    gated by what the committed record actually names. The gate is what
    //    keeps a raw local layer from being announced under a digest no
    //    resolver will ever ask for after compression rewrote it.
    Extend(&artifacts, SnapshotP2pArtifact::LocalOverlaybdLayers(
                           manifest.rootfs.image_config_path,
                           CommittedLayerDigests(committed.rootfs_layers),
                           ManagedLayerUuids(committed.rootfs_layers)));
    Extend(&artifacts, SnapshotP2pArtifact::LocalOverlaybdLayers(
                           manifest.memory.image_config_path,
                           CommittedMemoryLayerDigests(committed.memory_layers),
                           ManagedLayerUuidsFromManaged(committed.memory_layers)));
    for (std::size_t i = 0; i < manifest.attached_drives.size(); ++i) {
        const sandbox::SnapshotAttachedDriveArtifacts& drive = manifest.attached_drives[i];
        // Matched by drive id rather than by position: the manifest filters
        // volume drives out of `attached_drives`, so the two lists need not
        // line up, and a positional pairing would announce one drive's layers
        // under another drive's gate.
        const CommittedAttachedDrive* committed_drive = NULL;
        for (std::size_t j = 0; j < committed.attached_drives.size(); ++j) {
            if (committed.attached_drives[j].drive_id == drive.drive_id) {
                committed_drive = &committed.attached_drives[j];
                break;
            }
        }
        if (committed_drive == NULL) continue;
        Extend(&artifacts, SnapshotP2pArtifact::LocalOverlaybdLayers(
                               drive.image_config_path,
                               CommittedLayerDigests(committed_drive->layers),
                               ManagedLayerUuids(committed_drive->layers)));
    }

    // Rust dedups on `artifact.key`. An unchanged local layer reaches this
    // list twice -- once as a prepared upload, once from the image config --
    // and publishing the same key twice is wasted work on the second.
    std::vector<SnapshotP2pArtifact> unique;
    std::set<std::string>            seen;
    unique.reserve(artifacts.size());
    for (std::size_t i = 0; i < artifacts.size(); ++i) {
        if (!seen.insert(artifacts[i].key).second) continue;
        unique.push_back(artifacts[i]);
    }

    AGENTENV_DEBUG("announcing " << unique.size() << " p2p artifacts for snapshot "
                                 << id.ToString());

    // Rust `stream::iter(..).for_each_concurrent(SNAPSHOT_P2P_PUBLISH_CONCURRENCY, ..)`.
    // Bounded on purpose: a snapshot can carry dozens of layers, and
    // announcing all of them at once would have each publish contend for the
    // same disk the node is also resuming VMs from.
    for (std::size_t base = 0; base < unique.size(); base += kSnapshotP2pPublishConcurrency) {
        const std::size_t end =
            std::min(base + kSnapshotP2pPublishConcurrency, unique.size());
        std::vector<std::future<void> > pending;
        pending.reserve(end - base);
        for (std::size_t i = base; i < end; ++i) {
            const SnapshotP2pArtifact& artifact = unique[i];
            pending.push_back(core::GlobalExecutor().Submit([&artifact, &transport]() {
                const core::Expected<core::Unit, std::string> published =
                    artifact.Publish(transport);
                if (!published.ok()) {
                    // Rust `warn!`, and the stream continues. P2P is an
                    // optional acceleration path: a snapshot nobody can fetch
                    // over P2P still resolves through the repository, so one
                    // failed announcement must not abort the rest.
                    AGENTENV_WARN("failed to publish p2p artifact '" << artifact.key
                                                                     << "': "
                                                                     << published.error());
                }
            }));
        }
        // Joined before the next batch: the lambdas capture `artifact` and
        // `transport` by reference, and the bound is only a bound if the
        // batch completes.
        for (std::size_t i = 0; i < pending.size(); ++i) pending[i].get();
    }
}

}  // namespace snapshot
}  // namespace agentenv
