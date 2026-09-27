// SPDX-License-Identifier: MIT
// Rust: src/sandbox/extra_drive.rs
//
// Describes a block device attached to a sandbox beyond its root filesystem.
// The validation here is a guest/host boundary: `mount_path` and `sub_path`
// arrive from the public API and end up in a mount command inside the VM, so a
// value that escapes its drive root or shadows a control-plane path has to be
// refused before it reaches the guest rather than sanitised afterwards.
//
// Only the pure value type and its validators live here. Materialising the
// runtime device (Rust `PreparedDrives`, `prepare_extra_drives`) drives the
// ublk daemon asynchronously and is not part of this file.
#ifndef AGENTENV_SANDBOX_EXTRA_DRIVE_H_
#define AGENTENV_SANDBOX_EXTRA_DRIVE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace sandbox {

/// Rust `DEFAULT_EXTRA_DRIVE_MOUNT_ROOT`.
extern const char* const kDefaultExtraDriveMountRoot;
/// Rust `ROOTFS_DRIVE_ID`.
extern const char* const kRootfsDriveId;
/// Rust `USER_ROOTFS_DRIVE_ID`.
extern const char* const kUserRootfsDriveId;
/// Rust `VOLUME_DRIVE_SLOT_PREFIX`.
extern const char* const kVolumeDriveSlotPrefix;

/// Rust `enum ExtraDrive`.
///
/// Rust has a single `Overlaybd` variant today; `Kind` keeps that shape
/// explicit so a second backing store would be a new enumerator rather than a
/// reinterpretation of these fields.
struct ExtraDrive {
    enum class Kind {
        Overlaybd,
    };

    Kind kind = Kind::Overlaybd;
    std::string drive_id;
    std::string image_config_path;
    bool read_only = false;
    std::string mount_path;
    /// Rust `virtual_size: Option<u64>` — phase-dependent. On a fresh launch
    /// this is the optional size the API asked for; on resume it is the actual
    /// recorded device size. Absent means "let the ublk daemon read it from
    /// the source image", which is why it is an optional and not a 0 sentinel.
    core::Optional<uint64_t> virtual_size;
    /// Rust `sub_path` — relative path inside the drive to bind onto
    /// `mount_path`, like a Kubernetes `subPath`.
    core::Optional<std::string> sub_path;
    /// Rust `snapshot_output_dir`.
    core::Optional<std::string> snapshot_output_dir;
    /// Rust `volume` — persistent volumes are left out of committed
    /// attached-drive manifests and rebound through reserved slots instead.
    bool volume = false;

    // ---- constructors (Rust associated fns) ------------------------------

    /// Rust `ExtraDrive::try_new_overlaybd`.
    static core::Expected<ExtraDrive, std::string> TryNewOverlaybd(
        const std::string& drive_id, const std::string& image_config_path, bool read_only);

    /// Rust `ExtraDrive::try_new_overlaybd_with_mount_path`.
    static core::Expected<ExtraDrive, std::string> TryNewOverlaybdWithMountPath(
        const std::string& drive_id, const std::string& image_config_path, bool read_only,
        const std::string& mount_path, const core::Optional<std::string>& sub_path);

    /// Rust `ExtraDrive::default_mount_path`.
    static std::string DefaultMountPath(const std::string& drive_id);

    // ---- accessors (Rust getters) ----------------------------------------

    /// Rust `ExtraDrive::runtime_dir`.
    std::string RuntimeDir(const std::string& sandbox_work_dir) const;
    /// Rust `ExtraDrive::attachment_symlink_name`.
    std::string AttachmentSymlinkName() const;
    /// Rust `ExtraDrive::is_volume`.
    bool IsVolume() const { return volume; }

    // ---- derived copies (Rust `with_*` / `try_with_*`) -------------------

    /// Rust `ExtraDrive::with_volume_snapshot_output_dir` — also sets
    /// `volume`, matching Rust: the two always move together.
    ExtraDrive WithVolumeSnapshotOutputDir(
        const core::Optional<std::string>& output_dir) const;

    /// Rust `ExtraDrive::with_image_config_path`.
    ExtraDrive WithImageConfigPath(const std::string& path) const;

    /// Rust `ExtraDrive::try_with_virtual_size` — rejects zero, which would
    /// otherwise reach the daemon as a request for an empty device.
    core::Expected<ExtraDrive, std::string> TryWithVirtualSize(uint64_t size) const;

    bool operator==(const ExtraDrive& o) const;
    bool operator!=(const ExtraDrive& o) const { return !(*this == o); }
};

/// Rust `validate_drive_id`.
core::Expected<core::Unit, std::string> ValidateDriveId(const std::string& drive_id);

/// Rust `validate_mount_path`.
core::Expected<core::Unit, std::string> ValidateMountPath(const std::string& path);

/// Rust `validate_sub_path` — returns the path so it can be used inline, as
/// Rust does with `sub_path.map(validate_sub_path).transpose()?`.
core::Expected<std::string, std::string> ValidateSubPath(const std::string& sub_path);

/// Rust `normalize_mount_path_for_drive` — substitutes the default mount path
/// for an empty value before normalising.
core::Expected<std::string, std::string> NormalizeMountPathForDrive(
    const std::string& drive_id, const std::string& mount_path);

/// Rust `normalize_mount_path` — validates, then rebuilds the path from its
/// `Normal` components so that `//a///b/.` and `/a/b` cannot both be stored
/// for one mount point.
core::Expected<std::string, std::string> NormalizeMountPath(const std::string& mount_path);

/// Rust `struct DriveMount`.
struct DriveMount {
    std::string drive_id;
    std::string attachment_path;
    bool read_only = false;
};

/// Rust `enum ExtraDrivePrepareMode`.
///
/// The distinction matters because `virtual_size` means different things in
/// each phase; see `DeviceSizes`.
struct ExtraDrivePrepareMode {
    enum class Kind {
        Fresh,
        Resume,
    };

    Kind kind = Kind::Fresh;
    /// Only meaningful for `Fresh`; Rust encodes this in the variant.
    bool allow_shrink = false;

    static ExtraDrivePrepareMode MakeFresh(bool allow_shrink);
    static ExtraDrivePrepareMode MakeResume();

    /// Rust `ExtraDrivePrepareMode::device_sizes` — returns
    /// `(target_size, base_size)`.
    void DeviceSizes(const ExtraDrive& drive, core::Optional<uint64_t>* target_size,
                     core::Optional<uint64_t>* base_size) const;

    /// Rust `ExtraDrivePrepareMode::allow_shrink` — always false on resume,
    /// where shrinking would truncate a device the snapshot already sized.
    bool AllowShrink() const;
};

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_EXTRA_DRIVE_H_
