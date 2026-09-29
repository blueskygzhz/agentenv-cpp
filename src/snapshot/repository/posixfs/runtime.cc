// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/runtime.rs
#include "agentenv/snapshot/repository/posixfs/runtime.h"

#include "agentenv/core/fs.h"
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

RepositoryResult<RunnableSnapshot>
PosixFsRuntimeResolver::Resolve(const SnapshotRecord& snapshot) {
    if (!snapshot.committed.has_value()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("snapshot '") + snapshot.id.ToString() + "' is not ready"));
    }
    const CommittedSnapshot& committed = *snapshot.committed;

    auto vm_state_path_result = SnapshotVmStatePath(snapshot.id);
    if (!vm_state_path_result.ok()) return core::make_unexpected(vm_state_path_result.error());

    auto committed_manifest_result = LoadCommittedFirecrackerManifest(snapshot.id);
    if (!committed_manifest_result.ok())
        return core::make_unexpected(committed_manifest_result.error());

    std::vector<CacheHandle*> handles;

    auto mem_image_config_result =
        MaterializeMemImageConfig(snapshot.id, committed, &handles);
    if (!mem_image_config_result.ok()) {
        for (size_t i = 0; i < handles.size(); ++i) delete handles[i];
        return core::make_unexpected(mem_image_config_result.error());
    }

    const std::string rootfs_label = std::string("snapshot '") + snapshot.id.ToString() + "'";
    const std::string rootfs_cache_key =
        RuntimeImageCacheKey(snapshot.id, "rootfs/image.json");
    MaterializeSpec rootfs_spec;
    rootfs_spec.label = rootfs_label;
    rootfs_spec.cache_key = rootfs_cache_key;
    rootfs_spec.artifact_prefix = "rootfs_layer";
    rootfs_spec.allow_empty_layers = false;

    auto rootfs_image_config_result =
        MaterializeAndPin(committed.rootfs_layers,
                          image_materializer_.RootfsImageConfigPath(snapshot.id),
                          rootfs_spec, &handles);
    if (!rootfs_image_config_result.ok()) {
        for (size_t i = 0; i < handles.size(); ++i) delete handles[i];
        return core::make_unexpected(rootfs_image_config_result.error());
    }

    auto attached_drives_result = ResolveAttachedDrives(snapshot.id, committed, &handles);
    if (!attached_drives_result.ok()) {
        for (size_t i = 0; i < handles.size(); ++i) delete handles[i];
        return core::make_unexpected(attached_drives_result.error());
    }

    auto runtime_manifest_result =
        HydrateRuntimeManifest(committed_manifest_result.value(),
                               vm_state_path_result.value(),
                               mem_image_config_result.value(),
                               rootfs_image_config_result.value(),
                               attached_drives_result.value());
    if (!runtime_manifest_result.ok()) {
        for (size_t i = 0; i < handles.size(); ++i) delete handles[i];
        return core::make_unexpected(runtime_manifest_result.error());
    }

    CacheArtifactLease* lease = new CacheArtifactLease();
    for (size_t i = 0; i < handles.size(); ++i) lease->AddHandle(handles[i]);

    RunnableSnapshot runnable;
    runnable.record = snapshot;
    runnable.manifest = runtime_manifest_result.value();
    runnable.lease.reset(lease);
    return runnable;
}

RepositoryResult<std::string>
PosixFsRuntimeResolver::SnapshotVmStatePath(const core::SnapshotId& id) {
    PosixFsSnapshotArtifactLayout layout(repository_root_, id);
    const std::string vm_state_path = layout.Path(kSnapshotArtifactLayout.vm_state);
    if (!core::fs::Exists(vm_state_path)) {
        return core::make_unexpected(RepositoryError::ArtifactNotFound(
            std::string("vm state at ") + vm_state_path));
    }
    return vm_state_path;
}

RepositoryResult<sandbox::SandboxSnapshotManifest>
PosixFsRuntimeResolver::LoadCommittedFirecrackerManifest(const core::SnapshotId& id) {
    PosixFsSnapshotArtifactLayout layout(repository_root_, id);
    const std::string manifest_path =
        layout.Path(kSnapshotArtifactLayout.firecracker_manifest);
    return LoadFirecrackerManifestFromPath(manifest_path);
}

RepositoryResult<std::vector<ResolvedAttachedDrive>>
PosixFsRuntimeResolver::ResolveAttachedDrives(const core::SnapshotId& id,
                                               const CommittedSnapshot& snapshot,
                                               std::vector<CacheHandle*>* handles) {
    if (snapshot.attached_drives.empty()) {
        return std::vector<ResolvedAttachedDrive>();
    }

    std::vector<ResolvedAttachedDrive> drives;
    for (size_t i = 0; i < snapshot.attached_drives.size(); ++i) {
        const CommittedAttachedDrive& drive = snapshot.attached_drives[i];
        if (drive.kind != CommittedAttachedDrive::Kind::Overlaybd) continue;

        if (drive.virtual_size == 0) {
            return core::make_unexpected(RepositoryError::InvalidRequest(
                std::string("attached drive '") + drive.drive_id +
                "' virtual_size must be non-zero"));
        }

        const std::string label =
            std::string("attached drive '") + drive.drive_id +
            "' for snapshot '" + id.ToString() + "'";
        const std::string cache_key =
            RuntimeImageCacheKey(id, std::string("drives/") + drive.drive_id + "/image.json");
        MaterializeSpec spec;
        spec.label = label;
        spec.cache_key = cache_key;
        spec.artifact_prefix = "rootfs_layer";
        spec.allow_empty_layers = false;

        auto image_config_result =
            MaterializeAndPin(drive.layers,
                              image_materializer_.DriveImageConfigPath(id, drive.drive_id),
                              spec, handles);
        if (!image_config_result.ok()) return core::make_unexpected(image_config_result.error());

        ResolvedAttachedDrive resolved;
        resolved.kind = ResolvedAttachedDrive::Kind::Overlaybd;
        resolved.drive_id = drive.drive_id;
        resolved.image_config_path = image_config_result.value();
        resolved.read_only = drive.read_only;
        resolved.virtual_size = drive.virtual_size;
        resolved.mount_path = sandbox::NormalizeMountPathForDrive(drive.drive_id,
                                                                   drive.mount_path).value_or(
            sandbox::ExtraDrive::DefaultMountPath(drive.drive_id));
        resolved.sub_path = drive.sub_path;
        drives.push_back(resolved);
    }
    return drives;
}

RepositoryResult<std::string>
PosixFsRuntimeResolver::ResolveLocalManagedLayer(size_t index,
                                                  const ManagedLayer& layer,
                                                  const std::string& artifact_prefix) {
    PosixFsSnapshotArtifactLayout layout(repository_root_, core::SnapshotId());
    const std::string path = layout.ManagedLayerPath(layer.digest);
    if (!core::fs::Exists(path)) {
        return core::make_unexpected(RepositoryError::ArtifactNotFound(
            artifact_prefix + "_" + std::to_string(index) + " at " + path));
    }
    storage::overlaybd::LayerConfig config;
    config.file = path;
    config.digest = layer.digest;
    config.size = layer.size;
    if (layer.uuid.has_value()) config.uuid = *layer.uuid;
    return config;
}

struct ResolveContext {
    PosixFsRuntimeResolver* resolver;
    std::string artifact_prefix;
};

static RepositoryResult<storage::overlaybd::LayerConfig>
ResolveManagedCallback(void* ctx, size_t index, const ManagedLayer& layer) {
    ResolveContext* rc = static_cast<ResolveContext*>(ctx);
    return rc->resolver->ResolveLocalManagedLayer(index, layer, rc->artifact_prefix);
}

RepositoryResult<std::string>
PosixFsRuntimeResolver::MaterializeAndPin(const std::vector<OverlaybdLayerRef>& layers,
                                           const std::string& destination,
                                           const MaterializeSpec& spec,
                                           std::vector<CacheHandle*>* handles) {
    if (!spec.allow_empty_layers && layers.empty()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            spec.label + " has no layers"));
    }

    ResolveContext resolve_ctx;
    resolve_ctx.resolver = this;
    resolve_ctx.artifact_prefix = spec.artifact_prefix;

    struct FetchClosure {
        PosixFsRuntimeResolver* resolver;
        const std::vector<OverlaybdLayerRef>* layers;
        std::string label;
        std::string artifact_prefix;
        std::string dest_path;

        core::Expected<uint64_t, std::string> operator()(const std::string& dest) const {
            ResolveContext rc;
            rc.resolver = resolver;
            rc.artifact_prefix = artifact_prefix;

            auto result = resolver->image_materializer_.MaterializeImageConfig(
                *layers, dest, label, std::string(),
                &ResolveManagedCallback, &rc);
            if (!result.ok()) {
                return core::make_unexpected(result.error().ToString());
            }
            auto size = core::fs::FileSize(dest);
            if (!size.ok()) return core::make_unexpected(size.error());
            return size.value();
        }
    };

    FetchClosure fetch;
    fetch.resolver = this;
    fetch.layers = &layers;
    fetch.label = spec.label;
    fetch.artifact_prefix = spec.artifact_prefix;
    fetch.dest_path = destination;

    auto handle_result = cache_->EnsureCachedAt(spec.cache_key, destination, fetch);
    if (!handle_result.ok()) {
        return core::make_unexpected(
            MaterializeImageConfigError(spec.label,
                                        RepositoryError::Backend(spec.label, handle_result.error())));
    }
    handles->push_back(handle_result.value());
    return handle_result.value()->path();
}

RepositoryResult<std::string>
PosixFsRuntimeResolver::MaterializeMemImageConfig(const core::SnapshotId& id,
                                                   const CommittedSnapshot& snapshot,
                                                   std::vector<CacheHandle*>* handles) {
    const std::string destination = image_materializer_.MemoryImageConfigPath(id);
    const std::string label = std::string("memory for snapshot '") + id.ToString() + "'";
    const std::string cache_key = RuntimeImageCacheKey(id, "memory/image.json");

    std::vector<OverlaybdLayerRef> layers;
    for (size_t i = 0; i < snapshot.memory_layers.size(); ++i) {
        layers.push_back(OverlaybdLayerRef::FromManaged(snapshot.memory_layers[i]));
    }

    MaterializeSpec spec;
    spec.label = label;
    spec.cache_key = cache_key;
    spec.artifact_prefix = "memory_layer";
    spec.allow_empty_layers = true;

    return MaterializeAndPin(layers, destination, spec, handles);
}

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
