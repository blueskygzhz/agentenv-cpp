// SPDX-License-Identifier: MIT
// Rust: src/repository/layout.rs — `BlobKey`, `SnapshotFile`.
#include "agentenv/snapshot/repository/blobs.h"

namespace agentenv {
namespace snapshot {
namespace repository {

const char* SnapshotFileName(SnapshotFile file) {
    switch (file) {
        case SnapshotFile::VmState:
            return "vm_state.bin";
        case SnapshotFile::FirecrackerManifest:
            return "firecracker-manifest.json";
        case SnapshotFile::StartupManifest:
            return "memory-startup.pack";
    }
    return "";
}

const char* const BlobKey::kLayersDir = "managed-layers";
const char* const BlobKey::kPosixStagingDir = "staging";

BlobKey BlobKey::Layer(const std::string& digest, LayerRole role) {
    BlobKey key;
    key.kind_ = Kind::Layer;
    key.digest_ = digest;
    key.role_ = role;
    return key;
}

BlobKey BlobKey::Snapshot(const core::SnapshotId& id, SnapshotFile file) {
    BlobKey key;
    key.kind_ = Kind::Snapshot;
    key.snapshot_id_ = id;
    key.file_ = file;
    return key;
}

std::string BlobKey::Path() const {
    if (kind_ == Kind::Layer) return std::string(kLayersDir) + "/" + digest_;
    return SnapshotDir(snapshot_id_) + "/" + SnapshotFileName(file_);
}

std::string BlobKey::PosixPath() const {
    if (kind_ == Kind::Layer) {
        // Rust `digest.replace([':', '/'], "_")`.
        std::string sanitized = digest_;
        for (std::size_t i = 0; i < sanitized.size(); ++i) {
            if (sanitized[i] == ':' || sanitized[i] == '/') sanitized[i] = '_';
        }
        return std::string(kLayersDir) + "/" + sanitized + ".overlaybd.commit";
    }
    return PosixSnapshotDir(snapshot_id_) + "/" + SnapshotFileName(file_);
}

std::string BlobKey::SnapshotDir(const core::SnapshotId& id) {
    return "artifacts/" + id.ToString();
}

std::string BlobKey::PosixSnapshotDir(const core::SnapshotId& id) {
    return "snapshots/" + id.ToString();
}

bool BlobKey::operator==(const BlobKey& o) const {
    if (kind_ != o.kind_) return false;
    if (kind_ == Kind::Layer) return digest_ == o.digest_ && role_ == o.role_;
    return snapshot_id_ == o.snapshot_id_ && file_ == o.file_;
}

BlobBody BlobBody::File(const std::string& path) {
    BlobBody body;
    body.kind = Kind::File;
    body.path = path;
    return body;
}

BlobBody BlobBody::Bytes(std::vector<uint8_t> bytes) {
    BlobBody body;
    body.kind = Kind::Bytes;
    body.bytes.swap(bytes);
    return body;
}

BlobLocation BlobLocation::Local(const std::string& path) {
    BlobLocation location;
    location.kind = Kind::Local;
    location.value = path;
    return location;
}

BlobLocation BlobLocation::Remote(const std::string& url) {
    BlobLocation location;
    location.kind = Kind::Remote;
    location.value = url;
    return location;
}

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
