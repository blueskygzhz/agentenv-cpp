// SPDX-License-Identifier: MIT
// Rust: src/api/impls/volumes.rs
#include "agentenv/api/volumes.h"

#include <set>
#include <sstream>

#include "agentenv/core/logging.h"

namespace agentenv {
namespace api {

int VolumeErrorStatus(const volume::VolumeError& error) {
    switch (error.kind) {
        case volume::VolumeErrorKind::InvalidName:
        case volume::VolumeErrorKind::MultipleSources:
        case volume::VolumeErrorKind::SourceNotFound:
        case volume::VolumeErrorKind::InvalidSize:
        case volume::VolumeErrorKind::InvalidNextToken:
        case volume::VolumeErrorKind::InvalidPageLimit:
        case volume::VolumeErrorKind::SizeLimitExceeded:
        case volume::VolumeErrorKind::SizeMismatch:
        case volume::VolumeErrorKind::TooManyMountedVolumes:
            return 400;
        case volume::VolumeErrorKind::NotFound:
            return 404;
        // The volume exists but cannot serve this request right now, which is
        // a conflict rather than a malformed request.
        case volume::VolumeErrorKind::NameConflict:
        case volume::VolumeErrorKind::Reserved:
        case volume::VolumeErrorKind::Uploading:
        case volume::VolumeErrorKind::Failed:
            return 409;
        case volume::VolumeErrorKind::Storage:
            return 500;
    }
    return 500;
}

VolumeApiError VolumeErrorResponse(const volume::VolumeError& error) {
    return VolumeApiError::Make(VolumeErrorStatus(error), error.ToString());
}

core::Json VolumeRecordToJson(const volume::VolumeRecord& record) {
    core::JsonObject object;
    object["volumeID"] = core::Json(record.id);
    object["name"]     = core::Json(record.name);
    // Rust spells the API mode `"ro"` / `"exclusive"`, which is not the serde
    // spelling of `VolumeMode`, so the mapping is written out here.
    object["mode"] = core::Json(std::string(
        record.mode == volume::VolumeMode::ReadOnly ? "ro" : "exclusive"));
    object["sizeMB"] = core::Json(static_cast<int64_t>(record.size_mb));
    object["status"] = core::Json(std::string(volume::VolumeStatusToString(record.status)));
    return core::Json(object);
}

core::Expected<volume::VolumeMode, VolumeApiError>
ParseVolumeModeParam(const core::Optional<std::string>& mode) {
    if (!mode.has_value() || *mode == "exclusive") return volume::VolumeMode::Exclusive;
    if (*mode == "ro") return volume::VolumeMode::ReadOnly;
    // Rust rejects rather than defaulting: silently downgrading an unknown
    // mode could hand out a writable volume to a caller that asked for
    // read-only.
    return core::make_unexpected(
        VolumeApiError::Make(400, std::string("unsupported volume mode: ") + *mode));
}

bool MountPathsOverlap(const std::string& a, const std::string& b) {
    // Delegates to the sandbox layer, which owns mount paths: volume mounts and
    // Firecracker extra drives must apply the same rule, and two copies could
    // drift apart.
    return sandbox::MountPathsOverlap(a, b);
}

namespace {

/// Rust `resolve_volume_mounts_inner`. `reserved` accumulates every volume id
/// this call has leased, so the caller can release them on failure.
core::Expected<ResolvedVolumeMounts, VolumeApiError> ResolveVolumeMountsInner(
    volume::VolumeManager* manager, const std::map<std::string, std::string>& mounts,
    const std::string& owner, std::vector<std::string>* reserved) {
    ResolvedVolumeMounts resolved;
    const volume::VolumeLimits limits = manager->limits();

    if (mounts.size() > limits.max_mounts) {
        return core::make_unexpected(VolumeErrorResponse(
            volume::VolumeError::TooManyMountedVolumes(limits.max_mounts)));
    }

    std::set<std::string> volume_ids;

    // Rust sorts the mounts by path before the loop so the reservation order
    // is deterministic. `std::map` is already ordered by key, so plain
    // iteration gives the same sequence.
    for (std::map<std::string, std::string>::const_iterator it = mounts.begin();
         it != mounts.end(); ++it) {
        core::Expected<std::string, std::string> normalized =
            sandbox::NormalizeMountPath(it->first);
        if (!normalized.ok()) {
            return core::make_unexpected(VolumeApiError::Make(400, normalized.error()));
        }
        const std::string mount_path = normalized.value();

        for (std::map<std::string, std::string>::const_iterator existing =
                 resolved.normalized_mounts.begin();
             existing != resolved.normalized_mounts.end(); ++existing) {
            if (MountPathsOverlap(existing->first, mount_path)) {
                return core::make_unexpected(VolumeApiError::Make(
                    400, std::string("volume mount path overlaps another mount: ") +
                             mount_path));
            }
        }

        volume::VolumeResult<volume::VolumeRecord> looked_up = manager->Get(it->second);
        if (!looked_up.ok()) {
            return core::make_unexpected(VolumeErrorResponse(looked_up.error()));
        }
        volume::VolumeRecord record = looked_up.value();

        if (record.size_mb > limits.max_size_mb) {
            return core::make_unexpected(VolumeErrorResponse(
                volume::VolumeError::SizeLimitExceeded(limits.max_size_mb)));
        }
        if (!volume_ids.insert(record.id).second) {
            // Two mount paths resolving to the same volume would give the
            // guest two views of one device.
            return core::make_unexpected(VolumeApiError::Make(
                400, std::string("volume ") + record.id + " is mounted more than once"));
        }

        core::Expected<core::Unit, volume::VolumeError> leased =
            manager->Reserve(record.id, owner);
        if (!leased.ok()) {
            return core::make_unexpected(VolumeErrorResponse(leased.error()));
        }
        // Record the lease before anything else can fail, so the rollback in
        // the caller covers it.
        reserved->push_back(record.id);

        volume::VolumeResult<volume::VolumeRecord> materialized =
            manager->MaterializeBacking(record.id);
        if (!materialized.ok()) {
            return core::make_unexpected(VolumeErrorResponse(materialized.error()));
        }
        record = materialized.value();

        if (!record.backing_image_config.has_value()) {
            return core::make_unexpected(VolumeApiError::Make(
                500, std::string("volume ") + record.id + " has no backing image"));
        }

        core::Expected<sandbox::ExtraDrive, std::string> drive =
            sandbox::ExtraDrive::TryNewOverlaybdWithMountPath(
                record.id, *record.backing_image_config,
                record.mode == volume::VolumeMode::ReadOnly, mount_path,
                core::Optional<std::string>());
        if (!drive.ok()) {
            return core::make_unexpected(VolumeApiError::Make(400, drive.error()));
        }

        // Rust uses `checked_mul`: a size large enough to overflow would
        // otherwise wrap into a small device.
        if (record.size_mb > UINT64_MAX / volume::kBytesPerMb) {
            return core::make_unexpected(
                VolumeApiError::Make(400, "volume size is too large"));
        }
        core::Expected<sandbox::ExtraDrive, std::string> sized =
            drive.value().TryWithVirtualSize(record.size_mb * volume::kBytesPerMb);
        if (!sized.ok()) {
            return core::make_unexpected(VolumeApiError::Make(400, sized.error()));
        }

        // Only an exclusive volume captures its changes back out: a read-only
        // mount has nothing to write, so it gets no output directory.
        core::Optional<std::string> snapshot_output_dir;
        if (record.mode == volume::VolumeMode::Exclusive) {
            snapshot_output_dir =
                core::Optional<std::string>(manager->DataDir(record.id));
        }

        resolved.drives.push_back(
            sized.value().WithVolumeSnapshotOutputDir(snapshot_output_dir));
        resolved.normalized_mounts[mount_path] = record.id;
    }

    return resolved;
}

}  // namespace

core::Expected<ResolvedVolumeMounts, VolumeApiError>
ResolveVolumeMounts(volume::VolumeManager* manager,
                    const std::map<std::string, std::string>& mounts,
                    const std::string& owner) {
    if (manager == NULL) {
        if (mounts.empty()) return ResolvedVolumeMounts();
        return core::make_unexpected(
            VolumeApiError::Make(500, "no volume manager configured"));
    }

    std::vector<std::string> reserved;
    core::Expected<ResolvedVolumeMounts, VolumeApiError> result =
        ResolveVolumeMountsInner(manager, mounts, owner, &reserved);

    if (!result.ok()) {
        // A half-reserved request would leave volumes leased to a sandbox
        // that never launched, and nothing else would release them.
        core::Expected<core::Unit, volume::VolumeError> released =
            manager->ReplaceOwnerFor(owner, core::Optional<std::string>(), reserved);
        if (!released.ok()) {
            AGENTENV_WARN("failed to clean up volume reservations after mount resolution "
                          "failed: owner=" + owner + ": " + released.error().ToString());
        }
    }
    return result;
}

}  // namespace api
}  // namespace agentenv
