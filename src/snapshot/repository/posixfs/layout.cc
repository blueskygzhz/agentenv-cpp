// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/layout.rs
#include "agentenv/snapshot/repository/posixfs/layout.h"

#include "agentenv/core/fs.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {

const char* const kPosixFsSnapshotCommitMarker = "commit";
const char* const kLockSuffix = ".lock";

std::string ManagedLayerFileName(const std::string& digest) {
    // Rust `digest.replace([':', '/'], "_")`: a digest such as
    // `sha256:abc...` cannot be a filename as-is, and a `/` would make it a
    // directory component pointing outside the managed-layers directory.
    std::string sanitized = digest;
    for (std::size_t i = 0; i < sanitized.size(); ++i) {
        if (sanitized[i] == ':' || sanitized[i] == '/') sanitized[i] = '_';
    }
    return sanitized + ".overlaybd.commit";
}

std::string PosixFsSnapshotArtifactLayout::CatalogDir(const std::string& root) {
    return core::fs::Join(root, "catalog");
}

std::string PosixFsSnapshotArtifactLayout::AliasesDir(const std::string& root) {
    return core::fs::Join(CatalogDir(root), "aliases");
}

std::string PosixFsSnapshotArtifactLayout::RecordsDir(const std::string& root) {
    return core::fs::Join(CatalogDir(root), "records");
}

std::string PosixFsSnapshotArtifactLayout::AliasPath(const std::string& root,
                                                     const SnapshotAlias& alias) {
    return core::fs::Join(AliasesDir(root), alias.ToString());
}

std::string PosixFsSnapshotArtifactLayout::AliasLockPath(const std::string& root,
                                                         const SnapshotAlias& alias) {
    // Beside the alias file, not inside it: the lock must be openable before
    // the alias exists.
    return core::fs::Join(AliasesDir(root), alias.ToString() + kLockSuffix);
}

std::string PosixFsSnapshotArtifactLayout::RecordPath(const std::string& root,
                                                      const core::SnapshotId& id) {
    return core::fs::Join(RecordsDir(root), id.ToString() + ".json");
}

std::string PosixFsSnapshotArtifactLayout::RecordLockPath(const std::string& root,
                                                          const core::SnapshotId& id) {
    return core::fs::Join(RecordsDir(root), id.ToString() + kLockSuffix);
}

std::string PosixFsSnapshotArtifactLayout::VolumesDir(const std::string& root) {
    return core::fs::Join(root, "volumes");
}

std::string PosixFsSnapshotArtifactLayout::VolumeAliasesDir(const std::string& root) {
    return core::fs::Join(VolumesDir(root), "aliases");
}

std::string PosixFsSnapshotArtifactLayout::VolumeRecordsDir(const std::string& root) {
    return core::fs::Join(VolumesDir(root), "records");
}

std::string PosixFsSnapshotArtifactLayout::VolumeRecordPath(const std::string& root,
                                                            const std::string& volume_id) {
    return core::fs::Join(VolumeRecordsDir(root), volume_id + ".json");
}

std::string PosixFsSnapshotArtifactLayout::VolumeAliasPath(const std::string& root,
                                                           const std::string& alias) {
    return core::fs::Join(VolumeAliasesDir(root), alias);
}

std::string PosixFsSnapshotArtifactLayout::VolumeAliasLockPath(const std::string& root,
                                                               const std::string& alias) {
    return core::fs::Join(VolumeAliasesDir(root), alias + kLockSuffix);
}

std::string PosixFsSnapshotArtifactLayout::VolumeRecordLockPath(const std::string& root,
                                                                const std::string& volume_id) {
    return core::fs::Join(VolumeRecordsDir(root), volume_id + kLockSuffix);
}

std::string PosixFsSnapshotArtifactLayout::SnapshotsDir(const std::string& root) {
    return core::fs::Join(root, "snapshots");
}

std::string PosixFsSnapshotArtifactLayout::ManagedLayersDir(const std::string& root) {
    return core::fs::Join(root, "managed-layers");
}

std::string PosixFsSnapshotArtifactLayout::ManagedLayerPath(const std::string& root,
                                                            const std::string& digest) {
    return core::fs::Join(ManagedLayersDir(root), ManagedLayerFileName(digest));
}

std::string PosixFsSnapshotArtifactLayout::SnapshotDir() const {
    return core::fs::Join(SnapshotsDir(root_), snapshot_id_.ToString());
}

std::string PosixFsSnapshotArtifactLayout::Path(const std::string& relative_path) const {
    return core::fs::Join(SnapshotDir(), relative_path);
}

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
