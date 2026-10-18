// SPDX-License-Identifier: MIT
// Rust: src/api/impls/volumes.rs
#include "agentenv/api/volumes.h"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <sstream>

#include "agentenv/core/identity.h"
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


// ---------------------------------------------------------------------------
// fork volume preparation
// ---------------------------------------------------------------------------

void CleanupForkVolumeChildren(volume::VolumeManager* manager,
                               const ForkVolumeChildren& children) {
    if (manager == 0) return;
    for (ForkVolumeChildren::const_iterator it = children.begin(); it != children.end();
         ++it) {
        const std::string& owner = it->first;
        const std::vector<std::string>& volume_ids = it->second;
        if (volume_ids.empty()) continue;

        // Released before deleting, not after: `Remove` refuses while any
        // lease is held, so the other order would leave every clone behind
        // with no sandbox left to free it.
        manager->ReplaceOwnerFor(owner, core::Optional<std::string>(), volume_ids);
        for (std::size_t i = 0; i < volume_ids.size(); ++i) {
            manager->Remove(volume_ids[i]);
        }
    }
    // Both calls' results are dropped on purpose. This runs while the caller
    // is already failing, and its error is the one worth reporting -- a
    // cleanup failure reported instead would hide the cause.
}

core::Expected<PreparedVolumeForkSpecs, VolumeApiError> PrepareVolumeForkSpecs(
    volume::VolumeManager* manager, const orchestrator::SandboxMetadata& source,
    uint32_t count) {
    if (manager == 0) {
        return core::make_unexpected(
            ApiError::Make(500, "volume fork requested without a volume manager"));
    }

    const std::string source_owner = source.id.ToString();

    // Walked in sorted order. The source's mounts live in an unordered map, so
    // iterating it directly would make `replace_drive_ids` come out in an
    // arbitrary order. Nothing downstream depends on that order -- each
    // replacement names a distinct slot -- but an arbitrary one is not worth
    // having when a stable one costs a sort.
    std::vector<std::string> mount_paths;
    for (std::unordered_map<std::string, std::string>::const_iterator it =
             source.volume_mounts.begin();
         it != source.volume_mounts.end(); ++it) {
        mount_paths.push_back(it->first);
    }
    std::sort(mount_paths.begin(), mount_paths.end());

    // Resolved once, before any child is created. Each child needs the same
    // facts (mode and size), and re-reading per child would let a concurrent
    // change make two children disagree about the same volume.
    std::map<std::string, volume::VolumeRecord> source_volumes;
    for (std::size_t i = 0; i < mount_paths.size(); ++i) {
        const std::string& reference = source.volume_mounts.find(mount_paths[i])->second;
        if (source_volumes.find(reference) != source_volumes.end()) continue;
        const volume::VolumeResult<volume::VolumeRecord> record = manager->Get(reference);
        if (!record.ok()) {
            return core::make_unexpected(VolumeErrorResponse(record.error()));
        }
        source_volumes[reference] = record.value();
    }

    PreparedVolumeForkSpecs prepared;
    for (uint32_t c = 0; c < count; ++c) {
        const core::SandboxId child_id = core::SandboxId::Fresh();
        const std::string owner = child_id.ToString();
        // Present even when it stays empty, so a child that clones nothing is
        // still accounted for by the cleanup walk.
        prepared.children[owner];

        std::map<std::string, std::string> volume_mounts;
        std::map<std::string, std::string> child_mounts;
        std::vector<std::pair<std::string, std::string> > replace_drive_ids;

        for (std::size_t i = 0; i < mount_paths.size(); ++i) {
            const std::string& mount_path = mount_paths[i];
            const volume::VolumeRecord& volume =
                source_volumes[source.volume_mounts.find(mount_path)->second];

            if (volume.mode == volume::VolumeMode::Exclusive) {
                // Cloned. An exclusive volume has exactly one writer, so the
                // parent cannot keep writing it while a child does too.
                const std::string child_name =
                    std::string("volume-fork-") + core::Uuid::GenV7().ToSimpleString();
                const volume::VolumeResult<volume::VolumeRecord> child =
                    manager->CreateChildForOwner(volume.id, child_name,
                                                 volume::VolumeMode::Exclusive,
                                                 volume.size_mb, source_owner, owner);
                if (!child.ok()) {
                    // Everything created so far, for this child and every
                    // earlier one, is undone before returning.
                    CleanupForkVolumeChildren(manager, prepared.children);
                    return core::make_unexpected(VolumeErrorResponse(child.error()));
                }
                prepared.children[owner].push_back(child.value().id);
                child_mounts[mount_path] = child.value().id;
                // The child resumes from the parent's capture, whose config
                // still names the parent's volume in this slot. Without the
                // substitution the child would mount the parent's image.
                replace_drive_ids.push_back(std::make_pair(volume.id, child.value().id));
                volume_mounts[mount_path] = child.value().id;
            } else {
                // Shared. A read-only volume has no writer, so there is
                // nothing for parent and child to race on -- and cloning it
                // would copy data that cannot change.
                volume_mounts[mount_path] = volume.id;
                child_mounts[mount_path] = volume.id;
            }
        }

        // Reserves each mount for the child and builds its drives. A failure
        // here releases its own reservations, but the clones above are this
        // function's to undo.
        const core::Expected<ResolvedVolumeMounts, VolumeApiError> resolved =
            ResolveVolumeMounts(manager, child_mounts, owner);
        if (!resolved.ok()) {
            CleanupForkVolumeChildren(manager, prepared.children);
            return core::make_unexpected(resolved.error());
        }

        orchestrator::SandboxForkChildSpec spec;
        spec.sandbox_id = child_id;
        for (std::map<std::string, std::string>::const_iterator it = volume_mounts.begin();
             it != volume_mounts.end(); ++it) {
            spec.volume_mounts[it->first] = it->second;
        }
        spec.extra_drives = resolved.value().drives;
        spec.replace_drive_ids = replace_drive_ids;
        prepared.specs.push_back(spec);
    }

    return prepared;
}

}  // namespace api
}  // namespace agentenv
