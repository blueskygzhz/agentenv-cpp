// SPDX-License-Identifier: MIT
// Rust: src/snapshot/runtime_support.rs
#include "agentenv/snapshot/runtime_support.h"

#include <cerrno>
#include <cstring>
#include <sstream>

#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"

namespace agentenv {
namespace snapshot {

// ---- RuntimeImageMaterializer ----------------------------------------------

RuntimeImageMaterializer::RuntimeImageMaterializer(const std::string& runtime_root,
                                                   const OverlaybdLayerStore* store)
    : runtime_root_(runtime_root), store_(store) {}

std::string RuntimeImageMaterializer::SnapshotDir(const core::SnapshotId& id) const {
    return runtime_root_ + "/" + id.ToString();
}

std::string RuntimeImageMaterializer::RootfsImageConfigPath(const core::SnapshotId& id) const {
    return SnapshotDir(id) + "/" +
           kSnapshotArtifactLayout.rootfs_dir + "/" +
           kSnapshotArtifactLayout.overlaybd_image_config_file;
}

std::string RuntimeImageMaterializer::MemoryImageConfigPath(const core::SnapshotId& id) const {
    return SnapshotDir(id) + "/memory/" + kSnapshotArtifactLayout.overlaybd_image_config_file;
}

std::string RuntimeImageMaterializer::DriveImageConfigPath(const core::SnapshotId& id,
                                                            const std::string& drive_id) const {
    return SnapshotDir(id) + "/" +
           kSnapshotArtifactLayout.drives_dir + "/" + drive_id + "/" +
           kSnapshotArtifactLayout.overlaybd_image_config_file;
}

// ---- attach_local_layer_location -------------------------------------------
// Rust: sets lower.file or lower.dir as decided by the OverlaybdLayerStore.
// A lower that already has a placement, or whose digest is empty, is left
// untouched.
static void AttachLocalLayerLocation(storage::overlaybd::LayerConfig* lower,
                                     const OverlaybdLayerStore* store, bool has_remote) {
    if (!lower->file.empty() || !lower->dir.empty() || lower->digest.empty()) return;
    OverlaybdLayerLocation loc = store->LayerLocation(lower->digest, lower->size, has_remote);
    if (loc.kind == OverlaybdLayerLocation::Kind::LocalFile) {
        lower->file = loc.path;
    } else {
        lower->dir = loc.path;
    }
}

// ---- TrimTrailingSlash helper -----------------------------------------------
static std::string TrimTrailingSlash(const std::string& s) {
    size_t n = s.size();
    while (n > 0 && s[n - 1] == '/') --n;
    return s.substr(0, n);
}

// ---- write_image_config (Rust async fn, synchronous here) -------------------
static repository::RepositoryResult<std::string>
WriteImageConfig(const std::string& destination, const std::string& label,
                 const storage::overlaybd::ImageConfig& cfg) {
    core::Optional<std::string> parent = core::fs::Parent(destination);
    if (parent.has_value()) {
        core::Expected<core::Unit, std::string> mk = core::fs::CreateDirAll(*parent);
        if (!mk.ok()) {
            return core::make_unexpected(repository::RepositoryError::Backend(
                std::string("create runtime image config dir '") + *parent + "'", mk.error()));
        }
    }

    std::string tmp_path = destination + ".tmp";
    std::string serialized = storage::overlaybd::SerializeImageConfig(cfg).ToString();
    core::Expected<core::Unit, std::string> w = core::fs::Write(tmp_path, serialized);
    if (!w.ok()) {
        return core::make_unexpected(repository::RepositoryError::Backend(
            std::string("write temp runtime image config '") + tmp_path + "'", w.error()));
    }

    core::Expected<core::Unit, std::string> mv =
        core::fs::Rename(tmp_path, destination);
    if (!mv.ok()) {
        core::fs::RemoveFile(tmp_path);
        return core::make_unexpected(repository::RepositoryError::Backend(
            std::string("move runtime image config '") + tmp_path + "' to '" + destination + "'",
            mv.error()));
    }
    return destination;
}

repository::RepositoryResult<std::string>
RuntimeImageMaterializer::MaterializeImageConfig(
    const std::vector<OverlaybdLayerRef>& layers,
    const std::string& destination,
    const std::string& label,
    const std::string& managed_repo_blob_url,
    ResolveManagedFn resolve_managed,
    void* resolve_ctx) {

    struct MaterializedLower {
        storage::overlaybd::LayerConfig config;
        std::string remote_repo_blob_url;  // empty means no remote
    };

    std::vector<MaterializedLower> materialized;
    for (size_t i = 0; i < layers.size(); ++i) {
        const OverlaybdLayerRef& layer = layers[i];
        if (layer.is_managed()) {
            std::string rurl = managed_repo_blob_url;
            repository::RepositoryResult<storage::overlaybd::LayerConfig> res =
                resolve_managed(resolve_ctx, i, layer.managed);
            if (!res.ok()) return core::make_unexpected(res.error());
            storage::overlaybd::LayerConfig lower = res.value();
            AttachLocalLayerLocation(&lower, store_, !rurl.empty());
            MaterializedLower ml;
            ml.config = lower;
            ml.remote_repo_blob_url = rurl;
            materialized.push_back(ml);
        } else {
            storage::overlaybd::LayerConfig lower;
            lower.digest = layer.external.digest;
            lower.size   = layer.external.size;
            std::string rurl;
            if (!layer.external.repo_blob_url.empty()) rurl = layer.external.repo_blob_url;
            AttachLocalLayerLocation(&lower, store_, !rurl.empty());
            MaterializedLower ml;
            ml.config = lower;
            ml.remote_repo_blob_url = rurl;
            materialized.push_back(ml);
        }
    }

    // Determine if all remotes use the same base URL (single-backend legacy shape).
    std::string first_url;
    bool has_any_remote = false;
    bool mixed = false;
    for (size_t i = 0; i < materialized.size(); ++i) {
        const std::string& url = materialized[i].remote_repo_blob_url;
        if (!url.empty()) {
            if (!has_any_remote) {
                first_url = url;
                has_any_remote = true;
            } else if (TrimTrailingSlash(first_url) != TrimTrailingSlash(url)) {
                mixed = true;
                break;
            }
        }
    }

    storage::overlaybd::ImageConfig image_cfg;
    if (!mixed && has_any_remote) {
        image_cfg.repo_blob_url = TrimTrailingSlash(first_url);
    }

    for (size_t i = 0; i < materialized.size(); ++i) {
        storage::overlaybd::LayerConfig lower = materialized[i].config;
        if (mixed && !materialized[i].remote_repo_blob_url.empty()) {
            lower.repo_blob_url = materialized[i].remote_repo_blob_url;
        }
        image_cfg.lowers.push_back(lower);
    }

    return WriteImageConfig(destination, label, image_cfg);
}

// ---- free helpers ----------------------------------------------------------

std::string RuntimeImageCacheKey(const core::SnapshotId& id, const std::string& relative) {
    return std::string("runtime/") + id.ToString() + "/" + relative;
}

repository::RepositoryError
MaterializeImageConfigError(const std::string& label,
                             const repository::RepositoryError& error) {
    // Rust: lift an ArtifactNotFound cause from the chain to the top level.
    if (error.kind == repository::RepositoryErrorKind::ArtifactNotFound) {
        return error;
    }
    return repository::RepositoryError::Backend(
        std::string("materialize ") + label + " image config",
        error.ToString());
}

repository::RepositoryResult<sandbox::SandboxSnapshotManifest>
ParseFirecrackerManifest(const std::string& bytes, const std::string& manifest_ref) {
    core::Expected<core::Json, std::string> json_result = core::Json::Parse(bytes);
    if (!json_result.ok()) {
        return core::make_unexpected(repository::RepositoryError::Backend(
            std::string("parse firecracker manifest '") + manifest_ref + "'",
            json_result.error()));
    }
    core::Expected<sandbox::SandboxSnapshotManifest, std::string> manifest =
        sandbox::SandboxSnapshotManifest::FromJson(json_result.value());
    if (!manifest.ok()) {
        return core::make_unexpected(repository::RepositoryError::Backend(
            std::string("parse firecracker manifest '") + manifest_ref + "'",
            manifest.error()));
    }
    return manifest.value();
}

repository::RepositoryResult<sandbox::SandboxSnapshotManifest>
LoadFirecrackerManifestFromPath(const std::string& path) {
    core::Expected<std::string, std::string> contents = core::fs::ReadToString(path);
    if (!contents.ok()) {
        // Rust: NotFound maps to ArtifactNotFound, anything else to Backend.
        if (contents.error().find("No such file") != std::string::npos ||
            contents.error().find("ENOENT") != std::string::npos) {
            return core::make_unexpected(repository::RepositoryError::ArtifactNotFound(
                std::string("firecracker manifest at ") + path));
        }
        return core::make_unexpected(repository::RepositoryError::Backend(
            std::string("read firecracker manifest '") + path + "'", contents.error()));
    }
    return ParseFirecrackerManifest(contents.value(), path);
}

repository::RepositoryResult<sandbox::SandboxSnapshotManifest>
HydrateRuntimeManifest(sandbox::SandboxSnapshotManifest manifest,
                        const std::string& vm_state_path,
                        const std::string& memory_image_config_path,
                        const std::string& rootfs_image_config_path,
                        const std::vector<ResolvedAttachedDrive>& attached_drives) {
    // Rust: build ExtraDrive list then call manifest.with_extra_drives(&drives).
    std::vector<sandbox::ExtraDrive> extra_drives;
    for (size_t i = 0; i < attached_drives.size(); ++i) {
        extra_drives.push_back(attached_drives[i].ToExtraDrive());
    }
    core::Expected<sandbox::SandboxSnapshotManifest, std::string> updated =
        manifest.WithExtraDrives(extra_drives);
    if (!updated.ok()) {
        return core::make_unexpected(repository::RepositoryError::InvalidRequest(
            std::string("hydrate attached drives in firecracker manifest: ") + updated.error()));
    }
    manifest = updated.value();
    manifest.vm_state.path          = vm_state_path;
    manifest.memory.image_config_path = memory_image_config_path;
    manifest.rootfs.image_config_path = rootfs_image_config_path;
    return manifest;
}

}  // namespace snapshot
}  // namespace agentenv
