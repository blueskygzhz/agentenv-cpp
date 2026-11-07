// SPDX-License-Identifier: MIT
// Rust: src/snapshot/store.rs — `store_volume_layers` and the parts of
// `LayerStore` it reaches (`store_disk_layers`, `store_descriptorless_layer`,
// `store_layer`, `put_prepared`, `canonicalize`, `overlaybd_layer_uuid`).
#include "agentenv/snapshot/store.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/logging.h"
#include "agentenv/image/local_layer.h"
#include "agentenv/overlaybd/compact.h"
#include "agentenv/snapshot/image_export/acr.h"
#include "agentenv/snapshot/recontainerize.h"
#include "agentenv/sandbox/extra_drive.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/dense_export.h"
#include "agentenv/storage/overlaybd/layer_metadata.h"
#include "agentenv/storage/overlaybd/local_file.h"

namespace agentenv {
namespace snapshot {
namespace {

using repository::BlobBody;
using repository::BlobKey;
using repository::LayerRole;
using repository::RepositoryError;
using repository::RepositoryResult;
using repository::SnapshotRepository;
namespace obd = storage::overlaybd;

std::string ToStr(uint64_t value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

/// Rust `canonicalize`.
RepositoryResult<std::string> Canonicalize(const std::string& source) {
    char resolved[PATH_MAX];
    if (::realpath(source.c_str(), resolved) == NULL) {
        return core::make_unexpected(RepositoryError::Backend(
            "canonicalize managed layer '" + source + "'", std::strerror(errno)));
    }
    return std::string(resolved);
}

/// Rust `overlaybd_layer_uuid` — unset when unreadable or nil.
core::Optional<std::string> OverlaybdLayerUuid(const std::string& source) {
    const core::Expected<std::string, std::string> uuid = obd::ReadOverlaybdLayerUuid(source);
    if (!uuid.ok()) return core::Optional<std::string>();
    core::Uuid parsed;
    if (!core::Uuid::Parse(uuid.value(), &parsed) || parsed.is_nil()) {
        return core::Optional<std::string>();
    }
    return core::Optional<std::string>(parsed.ToString());
}

/// Rust `LayerStore::compression`, which `LayerStore::new` reads from
/// `repository.publish_policy().compression`.
///
/// The C++ repository has no publish policy, so the mode is fixed to what
/// Rust's default policy yields — see the port-gap note in store.h. It is
/// named and passed explicitly at both call sites, rather than hidden by
/// calling `passthrough` directly, because `PrepareLayerUpload` is where the
/// mode decides whether the descriptor describes the source bytes or
/// recompressed ones; the choice belongs in sight at the call.
///
/// Note that `Raw()` here is now the *only* thing gating compression: the
/// zfile path behind it is complete and tested (`PrepareLayerUpload` →
/// `overlaybd::CompactLayers`), so returning a `ZFile(...)` mode would
/// compress for real. It stays fixed until `SnapshotRepository` grows
/// `publish_policy()`, because inventing an operator's compression setting
/// here would silently change what every publish uploads.
overlaybd::OverlaybdCompactOutput PublishCompression() {
    return overlaybd::OverlaybdCompactOutput::Raw();
}

/// Rust `LayerStore::put_prepared`.
RepositoryResult<ManagedLayer> PutPrepared(SnapshotRepository& repository,
                                           const PreparedLayerUpload& upload,
                                           const core::Optional<std::string>& uuid,
                                           LayerRole role) {
    ManagedLayer layer;
    layer.digest = upload.Digest();
    layer.size = upload.Size();
    layer.uuid = uuid;
    const RepositoryResult<core::Unit> put =
        repository.PutBlob(BlobKey::Layer(layer.digest, role), BlobBody::File(upload.Path()));
    if (!put.ok()) return core::make_unexpected(put.error());
    return layer;
}

/// Rust `LayerStore::store_layer` — a descriptor from the capture side is
/// trusted after a cheap size check; otherwise the file is hashed.
RepositoryResult<ManagedLayer> StoreLayer(SnapshotRepository& repository,
                                          const std::string& source,
                                          const core::Optional<std::string>& digest,
                                          uint64_t size, LayerRole role) {
    const RepositoryResult<std::string> canonical = Canonicalize(source);
    if (!canonical.ok()) return core::make_unexpected(canonical.error());
    if (digest.has_value()) {
        const core::Expected<uint64_t, std::string> source_size =
            core::fs::FileSize(canonical.value());
        if (!source_size.ok()) {
            return core::make_unexpected(RepositoryError::Backend(
                "read managed layer metadata '" + canonical.value() + "'", source_size.error()));
        }
        if (source_size.value() != size) {
            return core::make_unexpected(RepositoryError::Backend(
                "managed layer descriptor size mismatch for '" + canonical.value() +
                "': descriptor says " + ToStr(size) + ", file has " +
                ToStr(source_size.value())));
        }
    }
    // Recontainerizing changes the bytes, so `PrepareLayerUpload` re-hashes
    // its output instead of trusting the descriptor.
    const RepositoryResult<PreparedLayerUpload> upload =
        PrepareLayerUpload(canonical.value(), PublishCompression(), digest, size);
    if (!upload.ok()) return core::make_unexpected(upload.error());
    return PutPrepared(repository, upload.value(), OverlaybdLayerUuid(upload.value().Path()),
                       role);
}

/// Rust `LayerStore::store_descriptorless_layer`.
RepositoryResult<ManagedLayer> StoreDescriptorlessLayer(SnapshotRepository& repository,
                                                        const std::string& source,
                                                        LayerRole role) {
    const RepositoryResult<std::string> canonical = Canonicalize(source);
    if (!canonical.ok()) return core::make_unexpected(canonical.error());
    if (!obd::ShouldDenseExportLayer(canonical.value())) {
        return StoreLayer(repository, canonical.value(), core::Optional<std::string>(), 0, role);
    }

    // Rust stages this in `repository.staging_file()`, which the C++
    // repository does not have; `put_blob` copies a file body, so a temporary
    // beside the source is equivalent and is removed once stored.
    const core::Optional<std::string> directory = core::fs::Parent(canonical.value());
    const core::Expected<core::fs::TempFile, std::string> dense_temp =
        core::fs::CreateTempFileIn(directory.has_value() ? *directory : std::string("."),
                                   ".dense-layer-");
    if (!dense_temp.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("create dense layer staging file", dense_temp.error()));
    }
    const std::string dense_path = dense_temp.value().path;

    RepositoryResult<ManagedLayer> stored = core::make_unexpected(RepositoryError::Backend(""));
    do {
        const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> dest =
            obd::LocalFile::OpenRw(dense_path, false);
        if (!dest.ok()) {
            stored = core::make_unexpected(RepositoryError::Backend(
                "dense-export sparse overlaybd layer '" + canonical.value() + "'", dest.error()));
            break;
        }
        const core::Expected<core::Unit, std::string> written =
            obd::WriteDenseLayerToFile(canonical.value(), dest.value());
        if (!written.ok()) {
            stored = core::make_unexpected(RepositoryError::Backend(
                "dense-export sparse overlaybd layer '" + canonical.value() + "'",
                written.error()));
            break;
        }
        // Rust takes the descriptor `write_dense_overlaybd_layer_to_file`
        // returns; it is the digest and size of exactly these bytes.
        const core::Expected<core::FileDigest, std::string> descriptor =
            core::DescribeFile(dense_path);
        if (!descriptor.ok()) {
            stored = core::make_unexpected(RepositoryError::Backend(
                "dense-export sparse overlaybd layer '" + canonical.value() + "'",
                descriptor.error()));
            break;
        }
        // Rust follows this with `upload.retain_source(dense_temp)`, which
        // keeps the staging file alive for as long as the upload is held in
        // `StoredSnapshot::local_layers` (post-commit P2P publication). That
        // field is not ported — see store.h — so nothing outlives this
        // function and the explicit removal below is the whole lifetime.
        const RepositoryResult<PreparedLayerUpload> upload = PrepareLayerUpload(
            dense_path, PublishCompression(),
            core::Optional<std::string>(descriptor.value().sha256), descriptor.value().size);
        if (!upload.ok()) {
            stored = core::make_unexpected(upload.error());
            break;
        }
        // Dense-exported layers never carry a layer uuid, recontainerized or not.
        stored = PutPrepared(repository, upload.value(), core::Optional<std::string>(), role);
    } while (false);
    core::fs::RemoveFile(dense_path);
    return stored;
}

/// Rust `LayerStore::store_disk_layers`.
RepositoryResult<std::vector<OverlaybdLayerRef> > StoreDiskLayers(
    SnapshotRepository& repository, const std::string& image_config_path, LayerRole role,
    bool allow_descriptorless) {
    const core::Expected<obd::ImageConfig, std::string> image_config =
        obd::LoadImageConfig(image_config_path);
    if (!image_config.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "load overlaybd image config '" + image_config_path + "'", image_config.error()));
    }
    std::vector<OverlaybdLayerRef> layers;
    layers.reserve(image_config.value().lowers.size());
    for (std::size_t index = 0; index < image_config.value().lowers.size(); ++index) {
        const obd::LayerConfig& layer = image_config.value().lowers[index];
        if (!layer.file.empty()) {
            RepositoryResult<ManagedLayer> managed =
                core::make_unexpected(RepositoryError::Backend(""));
            if (!layer.digest.empty() && layer.size > 0) {
                managed = StoreLayer(repository, layer.file,
                                     core::Optional<std::string>(layer.digest), layer.size, role);
            } else if (allow_descriptorless ||
                       image::RootfsLayerIsRuntimeGeneratedDelta(layer.file)) {
                // The second test is what the snapshot path relies on:
                // publishing passes `allow_descriptorless = false`, and a
                // sandbox's writable rootfs delta is created by the runtime
                // with no descriptor at all. Without it, every snapshot of a
                // sandbox that wrote to its rootfs would be refused as
                // "missing digest/size".
                managed = StoreDescriptorlessLayer(repository, layer.file, role);
            } else {
                return core::make_unexpected(RepositoryError::Unsupported(
                    "local overlaybd lower layer " + ToStr(index) + " '" + layer.file +
                    "' missing digest/size"));
            }
            if (!managed.ok()) return core::make_unexpected(managed.error());
            layers.push_back(OverlaybdLayerRef::Managed(managed.value()));
            continue;
        }
        const std::string repo_blob_url =
            layer.EffectiveRepoBlobUrl(image_config.value().repo_blob_url);
        if (repo_blob_url.empty()) {
            return core::make_unexpected(RepositoryError::Unsupported(
                "overlaybd lower layer " + ToStr(index) + " without local file or repoBlobUrl"));
        }
        ExternalLayer external;
        if (!layer.digest.empty()) {
            external.digest = layer.digest;
        } else if (!layer.target_digest.empty()) {
            external.digest = layer.target_digest;
        } else {
            external.digest = "external:" + ToStr(index);
        }
        external.repo_blob_url = repo_blob_url;
        external.size = layer.size;
        layers.push_back(OverlaybdLayerRef::External(external));
    }
    return layers;
}

/// Rust `same_repo_blob_url` — equal after dropping trailing slashes, and an
/// empty left side is never equal.
///
/// The empty check is not redundant with the comparison: two lowers that both
/// lack a URL would otherwise compare equal and a layer of unknown origin
/// would be accepted as belonging to this repository.
bool SameRepoBlobUrl(const std::string& left, const std::string& right) {
    // Rust `!left.is_empty() && left.trim_end_matches('/') == ...`. The empty
    // test is on the *untrimmed* left, so a left of `"/"` trims to empty and
    // can still match a right that also trims to empty.
    if (left.empty()) return false;
    std::string::size_type l = left.size();
    while (l > 0 && left[l - 1] == '/') --l;
    std::string::size_type r = right.size();
    while (r > 0 && right[r - 1] == '/') --r;
    return left.substr(0, l) == right.substr(0, r);
}

/// Rust `memory_layer_from_remote_lower`.
///
/// A memory lower with no local file can only be committed when it is already
/// a layer of *this* repository: the record names it by digest alone, so a
/// digest that lives in someone else's registry would resolve to nothing on
/// restore.
RepositoryResult<ManagedLayer> MemoryLayerFromRemoteLower(
    std::size_t index, const obd::LayerConfig& layer, const std::string& repo_blob_url,
    const std::string& managed_layers_repo_blob_url) {
    if (!SameRepoBlobUrl(repo_blob_url, managed_layers_repo_blob_url)) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "memory layer " + ToStr(index) + " uses a repoBlobUrl outside this repository"));
    }
    ManagedLayer managed;
    if (!layer.digest.empty()) {
        managed.digest = layer.digest;
    } else if (!layer.target_digest.empty()) {
        managed.digest = layer.target_digest;
    } else {
        return core::make_unexpected(
            RepositoryError::Unsupported("memory layer " + ToStr(index) + " without digest"));
    }
    managed.size = layer.size;
    // Rust leaves `uuid: None`. A remote lower's uuid is a property of the
    // local file that produced it, which this node does not have.
    return managed;
}

/// Rust `LayerStore::store_memory_layers`.
RepositoryResult<std::vector<ManagedLayer> > StoreMemoryLayers(
    SnapshotRepository& repository, const std::string& image_config_path) {
    const core::Expected<obd::ImageConfig, std::string> image_config =
        obd::LoadImageConfig(image_config_path);
    if (!image_config.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "load mem image config '" + image_config_path + "'", image_config.error()));
    }
    const core::Optional<std::string> managed_url = repository.ManagedLayersRepoBlobUrl();

    std::vector<ManagedLayer> layers;
    layers.reserve(image_config.value().lowers.size());
    for (std::size_t index = 0; index < image_config.value().lowers.size(); ++index) {
        const obd::LayerConfig& layer = image_config.value().lowers[index];
        if (layer.file.empty()) {
            const std::string repo_blob_url =
                layer.EffectiveRepoBlobUrl(image_config.value().repo_blob_url);
            // Both conditions are upstream's single match arm: a backend with
            // no managed URL cannot vouch for *any* remote lower, and a lower
            // with no URL of its own names nothing to vouch for.
            if (!managed_url.has_value() || repo_blob_url.empty()) {
                return core::make_unexpected(RepositoryError::Unsupported(
                    "memory layer " + ToStr(index) + " without local file path"));
            }
            const RepositoryResult<ManagedLayer> remote =
                MemoryLayerFromRemoteLower(index, layer, repo_blob_url, *managed_url);
            if (!remote.ok()) return core::make_unexpected(remote.error());
            layers.push_back(remote.value());
            continue;
        }
        // Unlike `store_disk_layers`, a descriptorless *local* memory lower is
        // hashed rather than refused: `store_layer` with no descriptor is the
        // normal path for a freshly captured memory delta.
        const bool has_descriptor = !layer.digest.empty() && layer.size > 0;
        const RepositoryResult<ManagedLayer> managed = StoreLayer(
            repository, layer.file,
            has_descriptor ? core::Optional<std::string>(layer.digest)
                           : core::Optional<std::string>(),
            has_descriptor ? layer.size : 0, LayerRole::Memory);
        if (!managed.ok()) return core::make_unexpected(managed.error());
        layers.push_back(managed.value());
    }
    return layers;
}

/// Rust `validate_image_configs` — every image config must parse *before*
/// anything is uploaded.
///
/// This is why it exists as a separate pass: an attached drive's config that
/// fails to parse would otherwise be discovered after the rootfs and memory
/// layers were already stored, leaving a half-published snapshot to roll back.
RepositoryResult<core::Unit> ValidateImageConfigs(
    const sandbox::SandboxSnapshotManifest& manifest) {
    struct Entry {
        const std::string* path;
        const char* what;
        std::string suffix;
    };
    std::vector<Entry> entries;
    Entry rootfs = {&manifest.rootfs.image_config_path, "rootfs", std::string()};
    Entry memory = {&manifest.memory.image_config_path, "memory", std::string()};
    entries.push_back(rootfs);
    entries.push_back(memory);
    for (std::size_t i = 0; i < manifest.attached_drives.size(); ++i) {
        Entry drive = {&manifest.attached_drives[i].image_config_path, "drive",
                       " for drive '" + manifest.attached_drives[i].drive_id + "'"};
        entries.push_back(drive);
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const core::Expected<obd::ImageConfig, std::string> parsed =
            obd::LoadImageConfig(*entries[i].path);
        if (!parsed.ok()) {
            return core::make_unexpected(RepositoryError::Backend(
                std::string("validate ") + entries[i].what + " image config '" +
                    *entries[i].path + "'" + entries[i].suffix,
                parsed.error()));
        }
    }
    return core::Unit();
}

/// Rust `LayerStore::store_snapshot`.
RepositoryResult<StoredSnapshot> StoreSnapshotInner(
    SnapshotRepository& repository, const SnapshotPublishMetadata& metadata,
    const sandbox::SandboxSnapshotManifest& manifest) {
    const RepositoryResult<core::Unit> valid = ValidateImageConfigs(manifest);
    if (!valid.ok()) return core::make_unexpected(valid.error());

    StoredSnapshot stored;

    // Rust selects an `AcrDiskImageExporter` here when
    // `publish_policy().image_storage == SourceRegistry`, and falls back to
    // local layers when the registry refuses. No publish policy exists in this
    // port, so the exporter is never selected and `store_disk_image` is always
    // its fallback arm — which is this call. `fallback_would_mix_sources` is
    // likewise unreachable: it only guards that fallback.
    const RepositoryResult<std::vector<OverlaybdLayerRef> > rootfs_layers = StoreDiskLayers(
        repository, manifest.rootfs.image_config_path, LayerRole::Rootfs, false);
    if (!rootfs_layers.ok()) return core::make_unexpected(rootfs_layers.error());
    stored.rootfs_layers = rootfs_layers.value();

    const RepositoryResult<std::vector<ManagedLayer> > memory_layers =
        StoreMemoryLayers(repository, manifest.memory.image_config_path);
    if (!memory_layers.ok()) return core::make_unexpected(memory_layers.error());
    stored.memory_layers = memory_layers.value();

    const RepositoryResult<core::Unit> vm_state = repository.PutBlob(
        BlobKey::Snapshot(metadata.id, repository::SnapshotFile::VmState),
        BlobBody::File(manifest.vm_state.path));
    if (!vm_state.ok()) return core::make_unexpected(vm_state.error());

    // `to_vec_pretty`, so a manifest written here is byte-identical to one the
    // Rust server writes.
    const std::string manifest_bytes = manifest.ToJson().ToStringPretty();
    const RepositoryResult<core::Unit> written = repository.PutBlob(
        BlobKey::Snapshot(metadata.id, repository::SnapshotFile::FirecrackerManifest),
        BlobBody::Bytes(std::vector<uint8_t>(manifest_bytes.begin(), manifest_bytes.end())));
    if (!written.ok()) return core::make_unexpected(written.error());

    stored.attached_drives.reserve(manifest.attached_drives.size());
    for (std::size_t i = 0; i < manifest.attached_drives.size(); ++i) {
        const sandbox::SnapshotAttachedDriveArtifacts& drive = manifest.attached_drives[i];
        // Only logical layers are committed; the build-time image config is
        // regenerated by the runtime resolver.
        const RepositoryResult<std::vector<OverlaybdLayerRef> > layers = StoreDiskLayers(
            repository, drive.image_config_path, LayerRole::AttachedDrive, false);
        if (!layers.ok()) return core::make_unexpected(layers.error());

        CommittedAttachedDrive committed;
        committed.kind = CommittedAttachedDrive::Kind::Overlaybd;
        committed.drive_id = drive.drive_id;
        committed.layers = layers.value();
        committed.read_only = drive.read_only;
        committed.virtual_size = drive.virtual_size;
        // Rust `.unwrap_or_else(|_| ExtraDrive::default_mount_path(..))`: a
        // mount path that cannot be normalised falls back to the default
        // rather than failing the publish. The capture already happened, and
        // the path is reconstructible at restore.
        const core::Expected<std::string, std::string> normalized =
            sandbox::NormalizeMountPathForDrive(drive.drive_id, drive.mount_path);
        committed.mount_path = normalized.ok()
                                   ? normalized.value()
                                   : sandbox::ExtraDrive::DefaultMountPath(drive.drive_id);
        committed.sub_path = drive.sub_path;
        stored.attached_drives.push_back(committed);
    }
    return stored;
}

}  // namespace

PublicationRollback AcrPublicationRollback() {
    // One exporter per call, shared across the publications of a single
    // delete: that is what makes its per-registry client cache worth having,
    // since a snapshot's publications are usually all in one registry.
    const std::shared_ptr<image_export::AcrDiskImageExporter> exporter(
        new image_export::AcrDiskImageExporter(
            image_export::AcrDiskImageExporter::DockerConfigClientBuilder()));
    return [exporter](const PersistedDiskImagePublication& publication)
               -> RepositoryResult<core::Unit> {
        return exporter->RollbackPublication(publication);
    };
}

RepositoryResult<core::Unit> DeleteSnapshot(
    SnapshotRepository& repository, const core::SnapshotId& id,
    const std::vector<PersistedDiskImagePublication>& publications,
    const PublicationRollback& rollback) {
    // Reverse order, as upstream: publications are appended as they are
    // created, so unwinding from the end undoes the most recent first.
    for (std::vector<PersistedDiskImagePublication>::size_type i = publications.size(); i > 0;
         --i) {
        const PersistedDiskImagePublication& publication = publications[i - 1];
        const RepositoryResult<core::Unit> rolled_back = rollback(publication);
        if (!rolled_back.ok()) {
            // Warned and stepped over, never returned: a registry that is
            // down must not pin the record forever, and the record is what
            // makes the delete retryable at all.
            AGENTENV_WARN("failed to delete source-registry publication; continuing snapshot "
                          "removal: snapshot "
                          << id.ToString() << " image_ref " << publication.image_ref
                          << " manifest_digest " << publication.manifest_digest << ": "
                          << rolled_back.error().ToString());
        }
    }
    return repository.DeleteSnapshotBlobs(id);
}

RepositoryResult<core::Unit> DeleteSnapshot(SnapshotRepository& repository,
                                            const core::SnapshotId& id) {
    return DeleteSnapshot(repository, id, std::vector<PersistedDiskImagePublication>(),
                          PublicationRollback());
}

RepositoryResult<StoredSnapshot> StoreSnapshot(
    SnapshotRepository& repository, const SnapshotPublishMetadata& metadata,
    const sandbox::SandboxSnapshotManifest& manifest) {
    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshotInner(repository, metadata, manifest);
    if (stored.ok()) return stored;

    // Remove what was stored. Shared content-addressed layers stay: they are
    // keyed by digest, so another snapshot may already reference the identical
    // bytes and deleting them here would corrupt an unrelated snapshot.
    const RepositoryResult<core::Unit> cleanup = DeleteSnapshot(repository, metadata.id);
    if (!cleanup.ok()) {
        // Logged, not returned: the caller must see the store failure, which
        // is the actionable cause.
        AGENTENV_WARN("failed to roll back snapshot files after store failure "
                      << metadata.id.ToString() << ": " << cleanup.error().ToString());
    }
    return stored;
}

RepositoryResult<std::vector<OverlaybdLayerRef> > StoreVolumeLayers(
    SnapshotRepository& repository, const std::string& image_config_path) {
    return StoreDiskLayers(repository, image_config_path, LayerRole::Rootfs, true);
}

}  // namespace snapshot
}  // namespace agentenv
