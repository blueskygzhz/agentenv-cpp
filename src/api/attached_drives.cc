// SPDX-License-Identifier: MIT
// Rust: src/api/impls/attached_drives.rs
#include "agentenv/api/attached_drives.h"

#include <set>
#include <sstream>

#include "agentenv/image/layers.h"

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

        // The raw reference goes straight to the resolver. Rust calls
        // `resolver.resolve(&drive.image)` with no parse step, and the parse
        // this port used to perform decomposed the string with rules Rust does
        // not have -- notably keeping both tag and digest, which can resolve to
        // a different image than the digest pins.
        core::Expected<image::ResolvedBlockImage, image::ImageError> image_result =
            image_resolver->Resolve(drive.image);
        if (!image_result.ok()) {
            std::ostringstream os;
            os << "resolve attached drive '" << drive.drive_id << "' image '" << drive.image
               << "': " << image_result.error().ToString();
            // Rust picks 400 vs 500 from `err.is_user_error()`, and now so does
            // this: a mistyped tag is the caller's problem, a registry outage
            // is ours. Reporting both as 500 told clients to retry a request
            // that can never succeed.
            if (image_result.error().IsUserError()) {
                return core::make_unexpected(AttachedDriveError::BadRequest(os.str()));
            }
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
        // Rust `raw_config` — carried out of the resolver unchanged, including
        // its absence. The caller turns it into this drive's `ImageConfigs`
        // entry; dropping it here made an attached drive's `Env`/`Entrypoint`
        // invisible to the sandbox and to any snapshot taken from it.
        spec.raw_config = image_result.value().raw_config;
        resolved.push_back(spec);
    }

    return resolved;
}

core::Expected<core::Optional<std::vector<AttachedDriveRequest> >, std::string>
ParseAttachedDrives(const core::Json& body) {
    const core::Json& drives = body["attachedDrives"];
    if (drives.kind() == core::Json::Kind::Null) {
        return core::Optional<std::vector<AttachedDriveRequest> >();
    }
    if (drives.kind() != core::Json::Kind::Array) {
        return core::make_unexpected(
            std::string("invalid field: attachedDrives must be an array"));
    }
    std::vector<AttachedDriveRequest> parsed;
    const core::JsonArray& entries = drives.as_array();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const core::Json& entry = entries[i];
        if (entry.kind() != core::Json::Kind::Object) {
            return core::make_unexpected(
                std::string("invalid field: attachedDrives entries must be objects"));
        }
        AttachedDriveRequest request;
        // `driveID` and `source` are the schema's two required members, so an
        // entry missing either does not deserialize. A drive with no id would
        // otherwise reach `ResolveAttachedDrives` as the empty string, which
        // it rejects with a message about the id rather than about the body.
        const core::Json& drive_id = entry["driveID"];
        if (drive_id.kind() != core::Json::Kind::String) {
            return core::make_unexpected(
                std::string("invalid field: attachedDrives entry missing driveID"));
        }
        request.drive_id = drive_id.as_string();

        // `source` is `AttachedDriveSource`, an object whose `image` names the
        // OCI reference to lay the drive down from.
        const core::Json& source = entry["source"];
        if (source.kind() != core::Json::Kind::Object) {
            return core::make_unexpected(
                std::string("invalid field: attachedDrives entry missing source"));
        }
        const core::Json& source_image = source["image"];
        if (source_image.kind() != core::Json::Kind::String ||
            source_image.as_string().empty()) {
            return core::make_unexpected(
                std::string("invalid field: attachedDrives entry source.image must be "
                            "a non-empty string"));
        }
        request.source_image = source_image.as_string();

        // `readOnly` defaults to true per the schema. Left unset here rather
        // than defaulted, because `ResolveAttachedDrives` applies that default
        // and applying it twice is how the two copies come to disagree.
        const core::Json& read_only = entry["readOnly"];
        if (read_only.kind() == core::Json::Kind::Bool) {
            request.read_only = core::Optional<bool>(read_only.as_bool());
        } else if (read_only.kind() != core::Json::Kind::Null) {
            return core::make_unexpected(
                std::string("invalid field: attachedDrives entry readOnly must be a boolean"));
        }

        const char* const kPaths[2] = {"mountPath", "subPath"};
        for (int p = 0; p < 2; ++p) {
            const core::Json& value = entry[kPaths[p]];
            if (value.kind() == core::Json::Kind::Null) continue;
            if (value.kind() != core::Json::Kind::String) {
                return core::make_unexpected(
                    std::string("invalid field: attachedDrives entry ") + kPaths[p] +
                    " must be a string");
            }
            // Not validated here: `NormalizeMountPathForDrive` and the
            // sub-path rules live in `ResolveAttachedDrives`, so a body that
            // parses can still be refused with the precise reason.
            if (p == 0) {
                request.mount_path = core::Optional<std::string>(value.as_string());
            } else {
                request.sub_path = core::Optional<std::string>(value.as_string());
            }
        }

        const core::Json& disk_size = entry["diskSizeMB"];
        if (disk_size.kind() == core::Json::Kind::Int) {
            if (disk_size.as_int() < 0) {
                return core::make_unexpected(std::string(
                    "invalid field: attachedDrives entry diskSizeMB must not be negative"));
            }
            request.disk_size_mb =
                core::Optional<uint32_t>(static_cast<uint32_t>(disk_size.as_int()));
        } else if (disk_size.kind() != core::Json::Kind::Null) {
            return core::make_unexpected(std::string(
                "invalid field: attachedDrives entry diskSizeMB must be an integer"));
        }

        parsed.push_back(request);
    }
    return core::Optional<std::vector<AttachedDriveRequest> >(parsed);
}

void WriteAttachedDrives(
    core::Json* body,
    const core::Optional<std::vector<AttachedDriveRequest> >& drives) {
    if (!drives.has_value()) return;
    core::JsonArray out;
    for (std::size_t i = 0; i < drives->size(); ++i) {
        const AttachedDriveRequest& drive = (*drives)[i];
        core::Json entry;
        entry["driveID"] = core::Json(drive.drive_id);
        core::Json source;
        source["image"] = core::Json(drive.source_image);
        entry["source"] = std::move(source);
        if (drive.read_only.has_value()) {
            entry["readOnly"] = core::Json(*drive.read_only);
        }
        if (drive.mount_path.has_value()) {
            entry["mountPath"] = core::Json(*drive.mount_path);
        }
        if (drive.sub_path.has_value()) {
            entry["subPath"] = core::Json(*drive.sub_path);
        }
        if (drive.disk_size_mb.has_value()) {
            entry["diskSizeMB"] = core::Json(static_cast<int64_t>(*drive.disk_size_mb));
        }
        out.emplace_back(std::move(entry));
    }
    (*body)["attachedDrives"] = core::Json(std::move(out));
}

}  // namespace api
}  // namespace agentenv
