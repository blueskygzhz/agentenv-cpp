// SPDX-License-Identifier: MIT
// Rust: src/api/impls/attached_drives.rs
#include "agentenv/api/attached_drives.h"

#include <set>
#include <sstream>

#include "agentenv/image/reference.h"

namespace agentenv {
namespace api {

const uint64_t kAttachedDriveMib = 1024 * 1024;

AttachedDriveError AttachedDriveError::BadRequest(const std::string& message) {
    AttachedDriveError error;
    error.status  = 400;
    error.message = message;
    return error;
}

AttachedDriveError AttachedDriveError::Internal(const std::string& message) {
    AttachedDriveError error;
    error.status  = 500;
    error.message = message;
    return error;
}

namespace {

std::string Trim(const std::string& value) {
    const std::size_t begin = value.find_first_not_of(" \t\n\r");
    if (begin == std::string::npos) return std::string();
    const std::size_t end = value.find_last_not_of(" \t\n\r");
    return value.substr(begin, end - begin + 1);
}

/// Rust's `PendingAttachedDrive` — everything validated, image not yet resolved.
struct PendingAttachedDrive {
    std::string                 drive_id;
    bool                        read_only = true;
    std::string                 mount_path;
    core::Optional<std::string> sub_path;
    core::Optional<uint64_t>    virtual_size;
    std::string                 image;
};

}  // namespace

core::Expected<core::Optional<uint64_t>, AttachedDriveError>
VirtualSizeFromDiskSizeMb(const core::Optional<uint32_t>& disk_size_mb) {
    if (!disk_size_mb.has_value()) {
        // Unset means "use the source image's size"; the ublk daemon resolves
        // it because prepare_extra_drives(Fresh) passes no known size.
        return core::Optional<uint64_t>();
    }

    const uint32_t mb = *disk_size_mb;
    if (mb < 1024 || (mb % 1024) != 0) {
        return core::make_unexpected(AttachedDriveError::BadRequest(
            "attached drive diskSizeMB must be at least 1024 and divisible by 1024"));
    }

    // Rust uses `checked_mul`: the multiply is what can overflow, not the
    // input, so the guard belongs here rather than on `mb`.
    const uint64_t bytes = static_cast<uint64_t>(mb);
    if (bytes > UINT64_MAX / kAttachedDriveMib) {
        return core::make_unexpected(
            AttachedDriveError::BadRequest("attached drive diskSizeMB overflows bytes"));
    }
    return core::Optional<uint64_t>(bytes * kAttachedDriveMib);
}

core::Expected<std::vector<ResolvedAttachedDriveSpec>, AttachedDriveError>
ResolveAttachedDrives(const std::vector<AttachedDriveRequest>& drives,
                      image::ImageResolver* image_resolver) {
    std::vector<PendingAttachedDrive> pending;
    std::set<std::string>             drive_ids;
    std::set<std::string>             mount_paths;

    for (std::size_t i = 0; i < drives.size(); ++i) {
        const AttachedDriveRequest& drive = drives[i];

        const std::string drive_id = Trim(drive.drive_id);
        core::Expected<core::Unit, std::string> id_valid =
            sandbox::ValidateDriveId(drive_id);
        if (!id_valid.ok()) {
            return core::make_unexpected(AttachedDriveError::BadRequest(id_valid.error()));
        }
        if (!drive_ids.insert(drive_id).second) {
            return core::make_unexpected(AttachedDriveError::BadRequest(
                std::string("duplicate attached drive driveID: ") + drive_id));
        }

        // An omitted or blank mount path falls back to the per-drive default.
        // Rust treats whitespace-only as absent, not as a path.
        std::string mount_path;
        if (drive.mount_path.has_value()) mount_path = Trim(*drive.mount_path);
        if (mount_path.empty()) {
            mount_path = sandbox::ExtraDrive::DefaultMountPath(drive_id);
        }
        core::Expected<core::Unit, std::string> mount_valid =
            sandbox::ValidateMountPath(mount_path);
        if (!mount_valid.ok()) {
            return core::make_unexpected(AttachedDriveError::BadRequest(mount_valid.error()));
        }

        core::Optional<std::string> sub_path;
        if (drive.sub_path.has_value()) {
            core::Expected<std::string, std::string> normalized =
                sandbox::ValidateSubPath(*drive.sub_path);
            if (!normalized.ok()) {
                return core::make_unexpected(
                    AttachedDriveError::BadRequest(normalized.error()));
            }
            sub_path = core::Optional<std::string>(normalized.value());
        }

        // Uniqueness is checked on the *resolved* path: two drives that both
        // omit `mountPath` would otherwise only collide at launch.
        if (!mount_paths.insert(mount_path).second) {
            return core::make_unexpected(AttachedDriveError::BadRequest(
                std::string("duplicate attached drive mountPath: ") + mount_path));
        }

        core::Expected<core::Optional<uint64_t>, AttachedDriveError> virtual_size =
            VirtualSizeFromDiskSizeMb(drive.disk_size_mb);
        if (!virtual_size.ok()) return core::make_unexpected(virtual_size.error());

        const std::string image = Trim(drive.source_image);
        if (image.empty()) {
            std::ostringstream os;
            os << "attached drive '" << drive_id << "' source requires exactly one image";
            return core::make_unexpected(AttachedDriveError::BadRequest(os.str()));
        }

        PendingAttachedDrive entry;
        entry.drive_id = drive_id;
        // Rust `unwrap_or(true)`: a drive is read-only unless asked otherwise.
        entry.read_only    = drive.read_only.has_value() ? *drive.read_only : true;
        entry.mount_path   = mount_path;
        entry.sub_path     = sub_path;
        entry.virtual_size = virtual_size.value();
        entry.image        = image;
        pending.push_back(entry);
    }

    // Image resolution is a second pass: Rust validates every declaration
    // before starting any registry work, so a malformed request fails fast.
    if (!pending.empty() && image_resolver == NULL) {
        return core::make_unexpected(
            AttachedDriveError::Internal("no image resolver configured"));
    }

    std::vector<ResolvedAttachedDriveSpec> resolved;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const PendingAttachedDrive& drive = pending[i];

        core::Expected<image::ImageReference, std::string> reference =
            image::ParseReference(drive.image);
        if (!reference.ok()) {
            std::ostringstream os;
            os << "resolve attached drive '" << drive.drive_id << "' image '" << drive.image
               << "': " << reference.error();
            return core::make_unexpected(AttachedDriveError::BadRequest(os.str()));
        }

        core::Expected<image::ResolvedBlockImage, std::string> image_result =
            image_resolver->Resolve(reference.value());
        if (!image_result.ok()) {
            // Rust picks 400 vs 500 from `err.is_user_error()`; the C++
            // resolver reports a flat string, so a resolution failure is
            // surfaced as a server error rather than blaming the request.
            std::ostringstream os;
            os << "resolve attached drive '" << drive.drive_id << "' image '" << drive.image
               << "': " << image_result.error();
            return core::make_unexpected(AttachedDriveError::Internal(os.str()));
        }

        core::Expected<sandbox::ExtraDrive, std::string> extra_drive =
            sandbox::ExtraDrive::TryNewOverlaybdWithMountPath(
                drive.drive_id, image_result.value().overlaybd_config_path, drive.read_only,
                drive.mount_path, drive.sub_path);
        if (!extra_drive.ok()) {
            return core::make_unexpected(
                AttachedDriveError::BadRequest(extra_drive.error()));
        }

        sandbox::ExtraDrive built = extra_drive.value();
        if (drive.virtual_size.has_value()) {
            core::Expected<sandbox::ExtraDrive, std::string> sized =
                built.TryWithVirtualSize(*drive.virtual_size);
            if (!sized.ok()) {
                return core::make_unexpected(AttachedDriveError::BadRequest(sized.error()));
            }
            built = sized.value();
        }

        ResolvedAttachedDriveSpec spec;
        spec.drive = built;
        resolved.push_back(spec);
    }

    return resolved;
}

}  // namespace api
}  // namespace agentenv
