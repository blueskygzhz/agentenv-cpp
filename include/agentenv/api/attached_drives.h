// SPDX-License-Identifier: MIT
// Rust: src/api/impls/attached_drives.rs — turns the API's attached-drive
// declarations into launch-ready `ExtraDrive` values.
//
// The validation order matters and is preserved: drive id, then mount path,
// then sub path, then size, then image. Uniqueness is enforced on both the
// drive id and the *resolved* mount path — a request that omits `mountPath`
// for two drives would otherwise collide on the default path only at launch,
// where the error is far less actionable.
//
// Image resolution is deliberately a second pass. Rust resolves every image
// concurrently after all declarations have been validated, so a request with a
// malformed drive fails before any registry work is started.
#ifndef AGENTENV_API_ATTACHED_DRIVES_H_
#define AGENTENV_API_ATTACHED_DRIVES_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/image/layers.h"
#include "agentenv/sandbox/extra_drive.h"

namespace agentenv {
namespace api {

/// Rust `MIB`.
extern const uint64_t kAttachedDriveMib;

/// Rust `models::AttachedDrive` (the generated request model).
struct AttachedDriveRequest {
    std::string                 drive_id;
    /// Rust `Option<bool>`, defaulting to read-only.
    core::Optional<bool>        read_only;
    core::Optional<std::string> mount_path;
    core::Optional<std::string> sub_path;
    core::Optional<uint32_t>    disk_size_mb;
    /// Rust `drive.source.image`.
    std::string                 source_image;
};

/// Rust `struct ResolvedAttachedDrive` (this module's, not the snapshot one).
struct ResolvedAttachedDriveSpec {
    sandbox::ExtraDrive drive;
    /// Raw image config JSON from the source image, empty when absent.
    std::string         raw_config;
};

/// An API-shaped failure: the HTTP status plus the message.
struct AttachedDriveError {
    int         status = 400;
    std::string message;

    static AttachedDriveError BadRequest(const std::string& message);
    static AttachedDriveError Internal(const std::string& message);
};

/// Rust `virtual_size_from_disk_size_mb`.
///
/// An unset size means "inherit the source image's size", which the ublk
/// daemon resolves at prepare time. A set size must be a whole number of GiB:
/// the guest filesystem is grown to it, and a non-GiB-aligned target is
/// rejected up front rather than producing a surprising rounded size.
core::Expected<core::Optional<uint64_t>, AttachedDriveError>
    VirtualSizeFromDiskSizeMb(const core::Optional<uint32_t>& disk_size_mb);

/// Rust `resolve_attached_drives`.
core::Expected<std::vector<ResolvedAttachedDriveSpec>, AttachedDriveError>
    ResolveAttachedDrives(const std::vector<AttachedDriveRequest>& drives,
                          image::ImageResolver* image_resolver);

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_ATTACHED_DRIVES_H_
