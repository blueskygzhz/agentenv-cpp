// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/runtime.rs
#include "agentenv/snapshot/repository/posixfs/runtime.h"

#include "agentenv/core/fs.h"
#include "agentenv/sandbox/extra_drive.h"
#include "agentenv/snapshot/repository/posixfs/layout.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {

PosixFsRuntimeResolver::PosixFsRuntimeResolver(
    const std::string& repository_root,
    const std::string& runtime_cache_root,
    const OverlaybdLayerStore* store,
    std::shared_ptr<LocalArtifactCache> cache)
    : repository_root_(repository_root),
      image_materializer_(runtime_cache_root, store),
      cache_(cache) {}

// ---- managed layer resolution ----------------------------------------------

RepositoryResult<storage::overlaybd::LayerConfig>
PosixFsRuntimeResolver::ResolveLocalManagedLayer(size_t index, const ManagedLayer& layer,
                                                 const std::string& artifact_prefix) const {
    const std::string path =
        PosixFsSnapshotArtifactLayout::ManagedLayerPath(repository_root_, layer.digest);
    if (!core::fs::Exists(path)) {
        std::ostringstream os;
        os << artifact_prefix << "_" << index << " at " << path;
        return core::make_unexpected(RepositoryError::ArtifactNotFound(os.str()));
    }
    storage::overlaybd::LayerConfig config;
    config.file   = path;
    config.digest = layer.digest;
    config.size   = layer.size;
    if (layer.uuid.has_value()) config.uuid = *layer.uuid;
    return config;
}

namespace {

/// Bridges `RuntimeImageMaterializer::ResolveManagedFn` (a plain function
/// pointer plus a void* context) onto the resolver member function.
struct ResolveContext {
    const PosixFsRuntimeResolver* resolver;
    std::string                   artifact_prefix;
};

RuntimeImageMaterializer::ResolveManagedResult ResolveManagedCallback(
    void* ctx, size_t index, const ManagedLayer& layer) {
    ResolveContext* rc = static_cast<ResolveContext*>(ctx);
    return rc->resolver->ResolveLocalManagedLayer(index, layer, rc->artifact_prefix);
}

}  // namespace

// ---- materialisation --------------------------------------------------------

RepositoryResult<std::string>
PosixFsRuntimeResolver::MaterializeAndPin(const std::vector<OverlaybdLayerRef>& layers,
                                          const std::string& destination,
                                          const MaterializeSpec& spec,
                                          CacheArtifactLease* lease) {
    if (!spec.allow_empty_layers && layers.empty()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest(spec.label + " has no layers"));
    }

    // Rust captures the layer set in an async closure; C++11 uses a small
    // functor so the fetch can be retried by the cache without re-entering
    // this function.
    struct Fetch {
        PosixFsRuntimeResolver*               resolver;
        const std::vector<OverlaybdLayerRef>* layers;
        std::string                           label;
        std::string                           artifact_prefix;

        core::Expected<uint64_t, std::string> operator()(const std::string& dest) const {
            ResolveContext rc;
            rc.resolver        = resolver;
            rc.artifact_prefix = artifact_prefix;

            RepositoryResult<std::string> written =
                resolver->image_materializer().MaterializeImageConfig(
                    *layers, dest, label, std::string(), &ResolveManagedCallback, &rc);
            if (!written.ok()) {
                return core::make_unexpected(written.error().ToString());
            }
            core::Expected<uint64_t, std::string> size = core::fs::FileSize(dest);
            if (!size.ok()) return core::make_unexpected(size.error());
            return size.value();
        }
    };

    Fetch fetch;
    fetch.resolver        = this;
    fetch.layers          = &layers;
    fetch.label           = spec.label;
    fetch.artifact_prefix = spec.artifact_prefix;

    core::Expected<CacheHandle*, std::string> handle =
        cache_->EnsureCachedAt(spec.cache_key, destination, fetch);
    if (!handle.ok()) {
        return core::make_unexpected(MaterializeImageConfigError(
            spec.label, RepositoryError::Backend(spec.label, handle.error())));
    }
    const std::string path = handle.value()->path();
    lease->AddHandle(handle.value());
    return path;
}

RepositoryResult<std::string>
PosixFsRuntimeResolver::MaterializeMemImageConfig(const core::SnapshotId& id,
                                                  const CommittedSnapshot& snapshot,
                                                  CacheArtifactLease* lease) {
    std::vector<OverlaybdLayerRef> layers;
    for (std::size_t i = 0; i < snapshot.memory_layers.size(); ++i) {
        layers.push_back(OverlaybdLayerRef::Managed(snapshot.memory_layers[i]));
    }

    MaterializeSpec spec;
    spec.label              = std::string("memory for snapshot '") + id.ToString() + "'";
    spec.cache_key          = RuntimeImageCacheKey(id, "memory/image.json");
    spec.artifact_prefix    = "memory_layer";
    // Rust allows an empty memory layer set: a snapshot may carry no memory.
    spec.allow_empty_layers = true;

    return MaterializeAndPin(layers, image_materializer_.MemoryImageConfigPath(id), spec,
                             lease);
}

// ---- attached drives --------------------------------------------------------

RepositoryResult<std::vector<ResolvedAttachedDrive> >
PosixFsRuntimeResolver::ResolveAttachedDrives(const core::SnapshotId& id,
                                               const CommittedSnapshot& snapshot,
                                               CacheArtifactLease* lease) {
    std::vector<ResolvedAttachedDrive> drives;
    for (std::size_t i = 0; i < snapshot.attached_drives.size(); ++i) {
        const CommittedAttachedDrive& drive = snapshot.attached_drives[i];

        if (drive.virtual_size == 0) {
            return core::make_unexpected(RepositoryError::InvalidRequest(
                std::string("attached drive '") + drive.drive_id +
                "' virtual_size must be non-zero"));
        }

        MaterializeSpec spec;
        spec.label = std::string("attached drive '") + drive.drive_id +
                     "' for snapshot '" + id.ToString() + "'";
        spec.cache_key =
            RuntimeImageCacheKey(id, std::string("drives/") + drive.drive_id + "/image.json");
        spec.artifact_prefix    = "rootfs_layer";
        spec.allow_empty_layers = false;

        RepositoryResult<std::string> image_config = MaterializeAndPin(
            drive.layers, image_materializer_.DriveImageConfigPath(id, drive.drive_id), spec,
            lease);
        if (!image_config.ok()) return core::make_unexpected(image_config.error());

        ResolvedAttachedDrive resolved;
        resolved.kind              = ResolvedAttachedDrive::Kind::Overlaybd;
        resolved.drive_id          = drive.drive_id;
        resolved.image_config_path = image_config.value();
        resolved.read_only         = drive.read_only;
        resolved.virtual_size      = drive.virtual_size;

        // Rust normalises the requested mount path and falls back to the
        // per-drive default when the record carries none.
        core::Expected<std::string, std::string> mount_path =
            sandbox::NormalizeMountPathForDrive(drive.drive_id, drive.mount_path);
        resolved.mount_path = mount_path.ok()
            ? mount_path.value()
            : sandbox::ExtraDrive::DefaultMountPath(drive.drive_id);
        resolved.sub_path = drive.sub_path;

        drives.push_back(resolved);
    }
    return drives;
}

// ---- entry point ------------------------------------------------------------

RepositoryResult<std::string>
PosixFsRuntimeResolver::SnapshotVmStatePath(const core::SnapshotId& id) const {
    PosixFsSnapshotArtifactLayout layout(repository_root_, id);
    const std::string vm_state_path = layout.Path(kSnapshotArtifactLayout.vm_state);
    if (!core::fs::Exists(vm_state_path)) {
        return core::make_unexpected(
            RepositoryError::ArtifactNotFound(std::string("vm state at ") + vm_state_path));
    }
    return vm_state_path;
}

RepositoryResult<sandbox::SandboxSnapshotManifest>
PosixFsRuntimeResolver::LoadCommittedFirecrackerManifest(const core::SnapshotId& id) const {
    PosixFsSnapshotArtifactLayout layout(repository_root_, id);
    return LoadFirecrackerManifestFromPath(
        layout.Path(kSnapshotArtifactLayout.firecracker_manifest));
}

RepositoryResult<RunnableSnapshot>
PosixFsRuntimeResolver::Resolve(const SnapshotRecord& snapshot) {
    if (!snapshot.committed.has_value()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("snapshot '") + snapshot.id.ToString() + "' is not ready"));
    }
    const CommittedSnapshot& committed = *snapshot.committed;

    RepositoryResult<std::string> vm_state_path = SnapshotVmStatePath(snapshot.id);
    if (!vm_state_path.ok()) return core::make_unexpected(vm_state_path.error());

    RepositoryResult<sandbox::SandboxSnapshotManifest> committed_manifest =
        LoadCommittedFirecrackerManifest(snapshot.id);
    if (!committed_manifest.ok()) return core::make_unexpected(committed_manifest.error());

    // The lease owns every handle acquired below, so an early return releases
    // the pins that were already taken.
    std::shared_ptr<CacheArtifactLease> lease(new CacheArtifactLease());

    RepositoryResult<std::string> mem_image_config =
        MaterializeMemImageConfig(snapshot.id, committed, lease.get());
    if (!mem_image_config.ok()) return core::make_unexpected(mem_image_config.error());

    MaterializeSpec rootfs_spec;
    rootfs_spec.label              = std::string("snapshot '") + snapshot.id.ToString() + "'";
    rootfs_spec.cache_key          = RuntimeImageCacheKey(snapshot.id, "rootfs/image.json");
    rootfs_spec.artifact_prefix    = "rootfs_layer";
    rootfs_spec.allow_empty_layers = false;

    RepositoryResult<std::string> rootfs_image_config =
        MaterializeAndPin(committed.rootfs_layers,
                          image_materializer_.RootfsImageConfigPath(snapshot.id), rootfs_spec,
                          lease.get());
    if (!rootfs_image_config.ok()) return core::make_unexpected(rootfs_image_config.error());

    RepositoryResult<std::vector<ResolvedAttachedDrive> > attached_drives =
        ResolveAttachedDrives(snapshot.id, committed, lease.get());
    if (!attached_drives.ok()) return core::make_unexpected(attached_drives.error());

    RepositoryResult<sandbox::SandboxSnapshotManifest> runtime_manifest =
        HydrateRuntimeManifest(committed_manifest.value(), vm_state_path.value(),
                               mem_image_config.value(), rootfs_image_config.value(),
                               attached_drives.value());
    if (!runtime_manifest.ok()) return core::make_unexpected(runtime_manifest.error());

    RunnableSnapshot runnable;
    runnable.record   = snapshot;
    runnable.manifest = runtime_manifest.value();
    runnable.lease    = lease;
    return runnable;
}

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
