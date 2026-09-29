// SPDX-License-Identifier: MIT
// Rust: src/snapshot/runtime_support.rs — image config materialisation helpers,
//       hydrate_runtime_manifest, load_firecracker_manifest_from_path.
//
// This header is the boundary between the posixfs runtime resolver and the
// overlaybd layer store. Everything that is not a network call or an async
// operation is implemented here; the async runtime_support fns in Rust become
// synchronous in C++11 with exactly the same observable behaviour.
#ifndef AGENTENV_SNAPSHOT_RUNTIME_SUPPORT_H_
#define AGENTENV_SNAPSHOT_RUNTIME_SUPPORT_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/sandbox/manifest.h"
#include "agentenv/snapshot/drive.h"
#include "agentenv/snapshot/layers.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace snapshot {

// ---- OverlaybdLayerStore ---------------------------------------------------

/// Rust `enum OverlaybdLayerLocation`.
struct OverlaybdLayerLocation {
    enum class Kind { LocalFile, CacheDir };

    Kind        kind = Kind::LocalFile;
    std::string path;

    static OverlaybdLayerLocation LocalFile(const std::string& p) {
        OverlaybdLayerLocation loc;
        loc.kind = Kind::LocalFile;
        loc.path = p;
        return loc;
    }
    static OverlaybdLayerLocation CacheDir(const std::string& p) {
        OverlaybdLayerLocation loc;
        loc.kind = Kind::CacheDir;
        loc.path = p;
        return loc;
    }
};

/// Rust `trait OverlaybdLayerStore`.
///
/// Decides where a layer lives on this node. The `has_remote` flag signals
/// that the layer also has a registry URL, so overlaybd can recover it
/// without a local file — allowing the local entry to use `dir=` (reclaimable
/// cache) rather than `file=` (required local copy).
class OverlaybdLayerStore {
 public:
    virtual ~OverlaybdLayerStore() {}
    virtual OverlaybdLayerLocation LayerLocation(const std::string& digest, uint64_t size,
                                                 bool has_remote) const = 0;
    virtual std::vector<std::string> PublishableRoots() const = 0;
};

// ---- RuntimeImageMaterializer ----------------------------------------------

/// Rust `struct RuntimeImageMaterializer`.
///
/// Derives node-local overlaybd image config files from committed snapshot
/// layer references. The produced configs are placed under `runtime_root`
/// and are node-local ephemeral state — never committed back to the
/// repository.
class RuntimeImageMaterializer {
 public:
    RuntimeImageMaterializer(const std::string& runtime_root,
                             const OverlaybdLayerStore* store);

    /// Rust `snapshot_dir`.
    std::string SnapshotDir(const core::SnapshotId& id) const;
    /// Rust `rootfs_image_config_path`.
    std::string RootfsImageConfigPath(const core::SnapshotId& id) const;
    /// Rust `memory_image_config_path`.
    std::string MemoryImageConfigPath(const core::SnapshotId& id) const;
    /// Rust `drive_image_config_path`.
    std::string DriveImageConfigPath(const core::SnapshotId& id,
                                     const std::string& drive_id) const;

    /// Rust `materialize_image_config`.
    ///
    /// Builds an overlaybd `image.json` at `destination` for the given layer
    /// references. `resolve_managed(index, layer)` must return a
    /// `LayerConfig` with `file` and/or `digest`/`size` populated.
    ///
    /// repo_blob_url propagation rules (verbatim from Rust):
    ///   - if every external layer has the same URL (ignoring trailing slashes),
    ///     place it on the top-level `repoBlobUrl` and clear per-layer URLs
    ///     ("single-backend legacy shape");
    ///   - if the layers come from different registries, set per-layer
    ///     `repoBlobUrl` and leave the top-level empty ("mixed backends").
    ///   - managed layers that have a `managed_repo_blob_url` participate in
    ///     the same deduplication.
    ///
    /// Returns the path of the written config on success.
    typedef repository::RepositoryResult<storage::overlaybd::LayerConfig>
        ResolveManagedResult;
    typedef ResolveManagedResult (*ResolveManagedFn)(void* ctx, size_t index,
                                                     const ManagedLayer& layer);

    repository::RepositoryResult<std::string>
    MaterializeImageConfig(const std::vector<OverlaybdLayerRef>& layers,
                           const std::string& destination,
                           const std::string& label,
                           const std::string& managed_repo_blob_url,
                           ResolveManagedFn resolve_managed,
                           void* resolve_ctx);

 private:
    std::string           runtime_root_;
    const OverlaybdLayerStore* store_;
};

// ---- pure helpers (all synchronous) ----------------------------------------

/// Rust `runtime_image_cache_key`.
std::string RuntimeImageCacheKey(const core::SnapshotId& id, const std::string& relative);

/// Rust `materialize_image_config_error` — lifts an `ArtifactNotFound` cause
/// to the top level so the caller can distinguish "missing file" from an
/// unrelated backend error.
repository::RepositoryError
    MaterializeImageConfigError(const std::string& label,
                                const repository::RepositoryError& error);

/// Rust `parse_firecracker_manifest`.
repository::RepositoryResult<sandbox::SandboxSnapshotManifest>
    ParseFirecrackerManifest(const std::string& bytes, const std::string& manifest_ref);

/// Rust `load_firecracker_manifest_from_path`.
repository::RepositoryResult<sandbox::SandboxSnapshotManifest>
    LoadFirecrackerManifestFromPath(const std::string& path);

/// Rust `hydrate_runtime_manifest` — stamps node-local paths onto a committed
/// manifest and replaces the attached-drive entries with their resolved forms.
repository::RepositoryResult<sandbox::SandboxSnapshotManifest>
    HydrateRuntimeManifest(sandbox::SandboxSnapshotManifest manifest,
                           const std::string& vm_state_path,
                           const std::string& memory_image_config_path,
                           const std::string& rootfs_image_config_path,
                           const std::vector<ResolvedAttachedDrive>& attached_drives);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_RUNTIME_SUPPORT_H_
