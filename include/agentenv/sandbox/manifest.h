// SPDX-License-Identifier: MIT
// Rust: src/sandbox/manifest.rs
//
// Describes the on-disk layout of a captured sandbox. The shape is the same
// whichever VMM took the capture — one VM-state file, an overlaybd image for
// memory and one for the root filesystem, one descriptor per attached drive —
// and `backend` names the VMM so a factory can refuse a snapshot it cannot
// restore.
//
// Deliberately decoupled from the in-memory snapshot types: the snapshot layer
// retrieves artifacts from this manifest when publishing, and rebuilds it with
// hydrated paths when resolving. Every path here is absolute.
#ifndef AGENTENV_SANDBOX_MANIFEST_H_
#define AGENTENV_SANDBOX_MANIFEST_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/extra_drive.h"
#include "agentenv/snapshot/startup_pack.h"

namespace agentenv {
namespace sandbox {

/// Rust `MANIFEST_FORMAT_VERSION`.
const uint32_t kManifestFormatVersion = 1;

/// Rust `FIRECRACKER_BACKEND` — also the name a record takes when it predates
/// the `backend` field.
extern const char* const kFirecrackerBackend;

/// Rust `KNOWN_BACKENDS`.
bool IsKnownBackend(const std::string& backend);

/// Rust `struct SnapshotVmStateArtifacts`.
///
/// `path` is `#[serde(skip)]` in Rust: it is a local location hydrated at
/// resolve time, not something a published record should pin.
struct SnapshotVmStateArtifacts {
    std::string path;

    bool operator==(const SnapshotVmStateArtifacts& o) const { return path == o.path; }
    bool operator!=(const SnapshotVmStateArtifacts& o) const { return !(*this == o); }
};

/// Rust `struct SnapshotMemoryArtifacts`.
struct SnapshotMemoryArtifacts {
    /// `#[serde(skip)]` — hydrated locally, as above.
    std::string image_config_path;
    uint64_t virtual_size = 0;

    bool operator==(const SnapshotMemoryArtifacts& o) const {
        return image_config_path == o.image_config_path && virtual_size == o.virtual_size;
    }
    bool operator!=(const SnapshotMemoryArtifacts& o) const { return !(*this == o); }
};

/// Rust `struct SnapshotRootfsArtifacts`.
struct SnapshotRootfsArtifacts {
    /// `#[serde(skip)]`.
    std::string image_config_path;
    uint64_t virtual_size = 0;

    bool operator==(const SnapshotRootfsArtifacts& o) const {
        return image_config_path == o.image_config_path && virtual_size == o.virtual_size;
    }
    bool operator!=(const SnapshotRootfsArtifacts& o) const { return !(*this == o); }
};

/// Rust `struct SnapshotAttachedDriveArtifacts`.
struct SnapshotAttachedDriveArtifacts {
    std::string drive_id;
    bool read_only = false;
    /// `#[serde(default)]`.
    std::string mount_path;
    /// `#[serde(default, skip_serializing_if = "Option::is_none")]`.
    core::Optional<std::string> sub_path;
    /// Required on the wire: a drive whose size is unknown cannot be restored,
    /// so there is no default.
    uint64_t virtual_size = 0;
    /// `#[serde(skip)]`.
    std::string image_config_path;

    core::Json ToJson() const;
    static core::Expected<SnapshotAttachedDriveArtifacts, std::string> FromJson(
        const core::Json& json);

    bool operator==(const SnapshotAttachedDriveArtifacts& o) const;
    bool operator!=(const SnapshotAttachedDriveArtifacts& o) const { return !(*this == o); }
};

/// Rust `struct SandboxSnapshotManifest`.
struct SandboxSnapshotManifest {
    uint32_t version = kManifestFormatVersion;
    /// Rust `#[serde(default = "default_backend")]` — a record written before
    /// the field existed holds a Firecracker capture, so that is what a
    /// missing value means.
    std::string backend;
    SnapshotVmStateArtifacts vm_state;
    SnapshotMemoryArtifacts memory;
    SnapshotRootfsArtifacts rootfs;
    std::vector<SnapshotAttachedDriveArtifacts> attached_drives;
    /// Reserved virtio-block slots kept free for launch-time volumes.
    std::size_t volume_drive_slots = 0;
    /// How many attached drives the VMM snapshot itself holds ids for.
    /// Launch-time volumes take the reserved slots that follow these.
    std::size_t physical_extra_drive_count = 0;
    /// Runtime-only reference resolved from the committed record. Absent for
    /// older snapshots, v1 packs, POSIX backends, and disabled consumption.
    core::Optional<snapshot::ResolvedStartupPack> memory_startup_pack;

    /// Rust `SandboxSnapshotManifest::new`.
    ///
    /// `backend` must be one of the `*_BACKEND` constants; an unknown name is
    /// refused here, before it can reach a published record that no restore
    /// could then act on.
    static core::Expected<SandboxSnapshotManifest, std::string> New(
        const std::string& backend, const std::string& vm_state_path,
        const std::string& mem_image_config_path, uint64_t mem_virtual_size,
        const std::string& rootfs_image_config_path, uint64_t rootfs_virtual_size,
        const std::vector<ExtraDrive>& attached_drives);

    /// Rust `SandboxSnapshotManifest::extra_drives`.
    std::vector<ExtraDrive> ExtraDrives() const;

    /// Rust `SandboxSnapshotManifest::with_extra_drives` — returns a copy, so
    /// a rejected drive leaves the original manifest untouched.
    core::Expected<SandboxSnapshotManifest, std::string> WithExtraDrives(
        const std::vector<ExtraDrive>& extra_drives) const;

    core::Json ToJson() const;
    static core::Expected<SandboxSnapshotManifest, std::string> FromJson(
        const core::Json& json);

    bool operator==(const SandboxSnapshotManifest& o) const;
    bool operator!=(const SandboxSnapshotManifest& o) const { return !(*this == o); }
};

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_MANIFEST_H_
