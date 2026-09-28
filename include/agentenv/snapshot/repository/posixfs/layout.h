// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/layout.rs
//
// Every path the POSIX-backed repository touches is derived here, so the
// on-disk shape is defined in one place rather than rebuilt at each call
// site. Lock files live beside the thing they guard (`<name>.lock`), which is
// why the records and aliases directories must skip non-`.json` entries when
// listing.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_LAYOUT_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_LAYOUT_H_

#include <string>

#include "agentenv/core/identity.h"
#include "agentenv/snapshot/types.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {

/// Rust `POSIXFS_SNAPSHOT_COMMIT_MARKER`.
extern const char* const kPosixFsSnapshotCommitMarker;
/// Rust `LOCK_SUFFIX`.
extern const char* const kLockSuffix;

/// Rust `managed_layer_file_name` — `:` and `/` are replaced so a digest can
/// be used as a single filename.
std::string ManagedLayerFileName(const std::string& digest);

/// Rust `struct PosixFsSnapshotArtifactLayout`.
class PosixFsSnapshotArtifactLayout {
 public:
    PosixFsSnapshotArtifactLayout(const std::string& root, const core::SnapshotId& snapshot_id)
        : root_(root), snapshot_id_(snapshot_id) {}

    // ---- catalog ---------------------------------------------------------

    static std::string CatalogDir(const std::string& root);
    static std::string AliasesDir(const std::string& root);
    static std::string RecordsDir(const std::string& root);

    static std::string AliasPath(const std::string& root, const SnapshotAlias& alias);
    static std::string AliasLockPath(const std::string& root, const SnapshotAlias& alias);
    static std::string RecordPath(const std::string& root, const core::SnapshotId& id);
    static std::string RecordLockPath(const std::string& root, const core::SnapshotId& id);

    // ---- volumes ---------------------------------------------------------

    static std::string VolumesDir(const std::string& root);
    static std::string VolumeAliasesDir(const std::string& root);
    static std::string VolumeRecordsDir(const std::string& root);
    static std::string VolumeRecordPath(const std::string& root, const std::string& volume_id);
    static std::string VolumeAliasPath(const std::string& root, const std::string& alias);
    static std::string VolumeAliasLockPath(const std::string& root, const std::string& alias);
    static std::string VolumeRecordLockPath(const std::string& root,
                                            const std::string& volume_id);

    // ---- artifacts -------------------------------------------------------

    static std::string SnapshotsDir(const std::string& root);
    static std::string ManagedLayersDir(const std::string& root);
    static std::string ManagedLayerPath(const std::string& root, const std::string& digest);

    /// Rust `snapshot_dir`.
    std::string SnapshotDir() const;
    /// Rust `path` — a file inside this snapshot's directory.
    std::string Path(const std::string& relative_path) const;

 private:
    std::string root_;
    core::SnapshotId snapshot_id_;
};

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_LAYOUT_H_
