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
/// Named and passed explicitly at both call sites, rather than hidden by
/// calling `passthrough` directly, because `PrepareLayerUpload` is where the
/// mode decides whether the descriptor describes the source bytes or
/// recompressed ones; the choice belongs in sight at the call.
///
/// Read per call rather than cached in a struct, which is the one shape
/// difference from Rust: `LayerStore` reads the policy once in `new` and
/// keeps it, while this port has free functions that each take the
/// repository. The values agree because a repository's policy is fixed at
/// construction (see `OssSnapshotRepository`), so there is no window in which
/// two calls could disagree.
overlaybd::OverlaybdCompactOutput PublishCompression(const SnapshotRepository& repository) {
    return overlaybd::OverlaybdCompactOutput::FromPublishCompressionConfig(
        repository.GetPublishPolicy().compression);
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
        PrepareLayerUpload(canonical.value(), PublishCompression(repository), digest, size);
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
            dense_path, PublishCompression(repository),
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

/// Rust `fallback_would_mix_sources`.
///
/// True when at least one lower of this image is remote and does *not* live in
/// this repository's managed-layer location.
///
/// This guards the fallback, and the question it asks is narrower than it
/// looks. Falling back means storing the image's local lowers here while
/// leaving its remote lowers named where they are. That is fine when the
/// remote ones already point at this repository — the result is one coherent
/// image. It is not fine when they point somewhere else: the stored image
/// would then be half in this repository and half in a registry that nothing
/// in this node's lifecycle manages, so deleting the snapshot would leave the
/// other half dangling and a restore would depend on a source no one here can
/// keep alive.
///
/// An empty `managed_layers_repo_blob_url` (every backend but OSS) makes
/// `SameRepoBlobUrl` false for any remote lower, so *any* remote lower
/// refuses the fallback. That is upstream's behaviour and the conservative
/// direction: a backend that cannot say where its managed layers live cannot
/// claim a remote lower is one of them.
RepositoryResult<bool> FallbackWouldMixSources(const std::string& image_config_path,
                                               const std::string& managed_layers_repo_blob_url) {
    const core::Expected<obd::ImageConfig, std::string> image_config =
        obd::LoadImageConfig(image_config_path);
    if (!image_config.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "load overlaybd image config '" + image_config_path + "'", image_config.error()));
    }
    for (std::size_t i = 0; i < image_config.value().lowers.size(); ++i) {
        const obd::LayerConfig& layer = image_config.value().lowers[i];
        if (!layer.file.empty()) continue;
        if (!SameRepoBlobUrl(layer.EffectiveRepoBlobUrl(image_config.value().repo_blob_url),
                             managed_layers_repo_blob_url)) {
            return true;
        }
    }
    return false;
}

/// Rust `LayerStore::store_disk_image` — publishes a disk image to its source
/// registry when the policy asks for it, otherwise (or when the registry
/// cannot take it) stores its local layers here.
///
/// The fallback is only taken for `Unsupported`, never for any other error.
/// That distinction is the whole design: `Unsupported` means "this image's
/// shape is not something the registry path models", which is a statement
/// about the image and is answered by storing it locally instead. Every other
/// error — a refused auth, a failed upload, a broken connection — is a
/// statement about *this attempt*, and silently storing locally would convert
/// an operator's `image_publish = true` into a quiet no-op precisely when the
/// registry is misconfigured.
RepositoryResult<std::vector<OverlaybdLayerRef> > StoreDiskImage(
    SnapshotRepository& repository, const DiskImageExport* exporter,
    const core::SnapshotId& id, const image_export::DiskImageSubject& subject,
    const std::string& image_config_path,
    const core::Optional<SnapshotOciConfigInput>& config,
    std::vector<PersistedDiskImagePublication>* publications) {
    const LayerRole role =
        subject.is_rootfs() ? LayerRole::Rootfs : LayerRole::AttachedDrive;
    if (exporter == NULL) {
        return StoreDiskLayers(repository, image_config_path, role, false);
    }

    const RepositoryResult<image_export::DiskImageExportOutcome> outcome =
        (*exporter)(id, subject, image_config_path, config);
    if (outcome.ok()) {
        // Rust `self.disk_publications.extend(outcome.publication)` — an
        // `Option` extends by zero or one element.
        if (outcome.value().publication.has_value()) {
            publications->push_back(*outcome.value().publication);
        }
        return outcome.value().layers;
    }
    if (outcome.error().kind != repository::RepositoryErrorKind::Unsupported) {
        return core::make_unexpected(outcome.error());
    }

    const std::string& feature = outcome.error().message;
    const core::Optional<std::string> managed_url = repository.ManagedLayersRepoBlobUrl();
    // Rust `.as_deref().unwrap_or("")`.
    const RepositoryResult<bool> would_mix = FallbackWouldMixSources(
        image_config_path, managed_url.has_value() ? *managed_url : std::string());
    if (!would_mix.ok()) return core::make_unexpected(would_mix.error());
    if (would_mix.value()) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "source-registry export is unsupported for this remote-backed disk image: " +
            feature));
    }
    AGENTENV_INFO("falling back to managed disk image layers for snapshot "
                  << id.ToString() << " " << subject.LogLabel() << ": " << feature);
    return StoreDiskLayers(repository, image_config_path, role, false);
}

/// Rust `LayerStore::store_snapshot`.
///
/// Writes into `*out` as it goes instead of returning a `StoredSnapshot`, so
/// the caller can roll back the publications made before a mid-way failure.
RepositoryResult<core::Unit> StoreSnapshotInner(
    SnapshotRepository& repository, const SnapshotPublishMetadata& metadata,
    const sandbox::SandboxSnapshotManifest& manifest, const DiskImageExport& exporter,
    StoredSnapshot* out) {
    const RepositoryResult<core::Unit> valid = ValidateImageConfigs(manifest);
    if (!valid.ok()) return core::make_unexpected(valid.error());

    StoredSnapshot& stored = *out;

    // Rust: `matches!(self.image_storage, SourceRegistry).then(|| ..)`. The
    // policy decides, not the presence of an exporter: a caller may always
    // hand one in, and a repository that stores its images as managed layers
    // still never reaches it.
    //
    // An unset functor is treated as no exporter too, so a caller that builds
    // a `SourceRegistryExporter` by hand and leaves `publish` empty gets
    // managed layers rather than a bad call through an empty `std::function`.
    const bool to_source_registry = repository.GetPublishPolicy().image_storage ==
                                    cfg::SnapshotImageStoragePolicy::SourceRegistry;
    const DiskImageExport* selected = (to_source_registry && exporter) ? &exporter : NULL;

    // Rootfs is exported with the effective runtime config.
    const SnapshotOciConfigInput rootfs_config_value = {metadata.context,
                                                        metadata.image_configs.RootfsConfig()};
    const RepositoryResult<std::vector<OverlaybdLayerRef> > rootfs_layers = StoreDiskImage(
        repository, selected, metadata.id, image_export::DiskImageSubject::Rootfs(),
        manifest.rootfs.image_config_path,
        core::Optional<SnapshotOciConfigInput>(rootfs_config_value),
        &stored.disk_publications);
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
        // regenerated by the runtime resolver — hence the absent config, as
        // upstream passes `None` here.
        const RepositoryResult<std::vector<OverlaybdLayerRef> > layers = StoreDiskImage(
            repository, selected, metadata.id,
            image_export::DiskImageSubject::AttachedDrive(drive.drive_id),
            drive.image_config_path, core::Optional<SnapshotOciConfigInput>(),
            &stored.disk_publications);
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
    return core::Unit();
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

SourceRegistryExporter AcrSourceRegistryExporter(
    const overlaybd::OverlaybdCompactOutput& publish_compression) {
    // One exporter for both halves, as upstream has: a publish and the
    // rollback that may have to undo it share the per-registry client cache,
    // and a snapshot's publications are usually all in one registry.
    const std::shared_ptr<image_export::AcrDiskImageExporter> exporter(
        new image_export::AcrDiskImageExporter(
            image_export::AcrDiskImageExporter::DockerConfigClientBuilder(),
            publish_compression));
    SourceRegistryExporter out;
    out.publish = [exporter](const core::SnapshotId& id,
                             const image_export::DiskImageSubject& subject,
                             const std::string& image_config_path,
                             const core::Optional<SnapshotOciConfigInput>& config) {
        return exporter->Export(id, subject, image_config_path, config);
    };
    out.rollback = [exporter](const PersistedDiskImagePublication& publication) {
        return exporter->RollbackPublication(publication);
    };
    return out;
}

RepositoryResult<StoredSnapshot> StoreSnapshot(
    SnapshotRepository& repository, const SnapshotPublishMetadata& metadata,
    const sandbox::SandboxSnapshotManifest& manifest,
    const SourceRegistryExporter& exporter) {
    // Rust keeps its `LayerStore` alive past the `match`, so the publications
    // it already made are still reachable in the error arm. This port has to
    // say that explicitly: `partial` is filled in as the store progresses and
    // is what the cleanup below rolls back.
    StoredSnapshot partial;
    const RepositoryResult<core::Unit> stored =
        StoreSnapshotInner(repository, metadata, manifest, exporter.publish, &partial);
    if (stored.ok()) return partial;

    // Remove what was stored. Shared content-addressed layers stay: they are
    // keyed by digest, so another snapshot may already reference the identical
    // bytes and deleting them here would corrupt an unrelated snapshot.
    const RepositoryResult<core::Unit> cleanup =
        DeleteSnapshot(repository, metadata.id, partial.disk_publications, exporter.rollback);
    if (!cleanup.ok()) {
        // Logged, not returned: the caller must see the store failure, which
        // is the actionable cause.
        AGENTENV_WARN("failed to roll back snapshot files after store failure "
                      << metadata.id.ToString() << ": " << cleanup.error().ToString());
    }
    return core::make_unexpected(stored.error());
}

RepositoryResult<StoredSnapshot> StoreSnapshot(
    SnapshotRepository& repository, const SnapshotPublishMetadata& metadata,
    const sandbox::SandboxSnapshotManifest& manifest) {
    return StoreSnapshot(repository, metadata, manifest,
                         AcrSourceRegistryExporter(PublishCompression(repository)));
}

RepositoryResult<std::vector<OverlaybdLayerRef> > StoreVolumeLayers(
    SnapshotRepository& repository, const std::string& image_config_path) {
    return StoreDiskLayers(repository, image_config_path, LayerRole::Rootfs, true);
}

}  // namespace snapshot
}  // namespace agentenv
