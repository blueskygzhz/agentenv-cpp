// SPDX-License-Identifier: MIT
// Rust: src/repository/layout.rs (`BlobKey`, `SnapshotFile`) and the blob
// types of src/repository/interfaces.rs (`LayerRole`, `BlobBody`,
// `BlobLocation`).
//
// Part of the newer Rust repository, added alongside `documents.h`. Like
// `DocumentKey`, `BlobKey` is the only place that names blob paths; changing
// one strands the blobs already stored.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_BLOBS_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_BLOBS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/identity.h"

namespace agentenv {
namespace snapshot {
namespace repository {

/// Rust `LayerRole` — labels metrics and logs only; layers are stored by
/// digest alone.
enum class LayerRole { Rootfs, AttachedDrive, Memory };

/// Rust `SnapshotFile` — the files a snapshot stores besides its layers.
enum class SnapshotFile { VmState, FirecrackerManifest, StartupManifest };

/// Rust `SnapshotFile::name`.
const char* SnapshotFileName(SnapshotFile file);

/// Rust `BlobKey`.
class BlobKey {
 public:
    enum class Kind { Layer, Snapshot };

    /// Rust `BlobKey::LAYERS_DIR`.
    static const char* const kLayersDir;
    /// Rust `BlobKey::POSIX_STAGING_DIR`.
    static const char* const kPosixStagingDir;

    /// Rust `BlobKey::Layer { digest, role }`.
    static BlobKey Layer(const std::string& digest, LayerRole role);
    /// Rust `BlobKey::Snapshot(id, file)`.
    static BlobKey Snapshot(const core::SnapshotId& id, SnapshotFile file);

    Kind kind() const { return kind_; }
    /// Set for `Layer`.
    const std::string& digest() const { return digest_; }
    LayerRole role() const { return role_; }
    /// Set for `Snapshot`.
    const core::SnapshotId& snapshot_id() const { return snapshot_id_; }
    SnapshotFile file() const { return file_; }

    /// Rust `path` — the object key in an object-store repository.
    std::string Path() const;
    /// Rust `posix_path` — the path in a POSIX repository, which predates the
    /// shared layout.
    std::string PosixPath() const;

    /// Rust `snapshot_dir`.
    static std::string SnapshotDir(const core::SnapshotId& id);
    /// Rust `posix_snapshot_dir`.
    static std::string PosixSnapshotDir(const core::SnapshotId& id);

    bool operator==(const BlobKey& o) const;
    bool operator!=(const BlobKey& o) const { return !(*this == o); }

 private:
    BlobKey() : kind_(Kind::Layer), role_(LayerRole::Rootfs), file_(SnapshotFile::VmState) {}

    Kind kind_;
    std::string digest_;
    LayerRole role_;
    core::SnapshotId snapshot_id_;
    SnapshotFile file_;
};

/// Rust `BlobBody<'a>` — `File(&Path)` or `Bytes(Vec<u8>)`.
struct BlobBody {
    enum class Kind { File, Bytes };
    Kind kind;
    std::string path;
    std::vector<uint8_t> bytes;

    static BlobBody File(const std::string& path);
    static BlobBody Bytes(std::vector<uint8_t> bytes);

 private:
    BlobBody() : kind(Kind::File) {}
};

/// Rust `BlobLocation` — `Local(PathBuf)` on this node, or `Remote(url)`.
struct BlobLocation {
    enum class Kind { Local, Remote };
    Kind kind;
    /// The path (`Local`) or URL (`Remote`).
    std::string value;

    static BlobLocation Local(const std::string& path);
    static BlobLocation Remote(const std::string& url);

    bool operator==(const BlobLocation& o) const { return kind == o.kind && value == o.value; }

 private:
    BlobLocation() : kind(Kind::Local) {}
};

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_BLOBS_H_
