// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/factory.rs
#include <set>
#include "agentenv/sandbox/firecracker/factory.h"

#include <cstdint>
#include <sstream>
#include <vector>

namespace agentenv {
namespace sandbox {
namespace firecracker {

core::Expected<core::Unit, std::string> ApplyLaunchToSnapshotConfig(
    SnapshotConfig* config, const SandboxLaunchConfig& launch_config,
    const core::Optional<std::string>& snapshot_custom_extension_params) {
    if (config == NULL) return core::make_unexpected(std::string("null snapshot config"));

    // Checked before anything is layered on. Firecracker cannot add a
    // virtio-block device after a restore, so every launch-time volume must
    // occupy a slot the snapshot reserved on its first boot -- a shortfall can
    // never be satisfied later, and dropping the surplus would boot a sandbox
    // whose caller finds its mount points empty.
    if (launch_config.extra_drives.size() > config->common.volume_drive_slots) {
        std::ostringstream why;
        why << "snapshot supports at most " << config->common.volume_drive_slots
            << " launch-time volume drives, requested " << launch_config.extra_drives.size();
        return core::make_unexpected(why.str());
    }

    // Appended, not replaced: the snapshot's own drives keep their slots and
    // the launch's volumes take the reserved ones after them.
    config->common.extra_drives.insert(config->common.extra_drives.end(),
                                       launch_config.extra_drives.begin(),
                                       launch_config.extra_drives.end());

    // A committed snapshot is shared, so the document it recorded names
    // whichever sandbox captured it. This launch's identity replaces it
    // wholesale rather than being patched over, which is the difference from
    // the paused-state path: there the capture is already this sandbox's.
    //
    // `WithExtra` drops reserved keys, so API-supplied extras cannot override
    // the identity or the token hash.
    config->common.mmds_metadata = core::Optional<MmdsMetadata>(
        MmdsMetadata(launch_config.sandbox_id, launch_config.snapshot_id)
            .WithAccessToken(launch_config.envd_access_token)
            .WithExtra(launch_config.extra_mmds));
    config->common.envd_access_token = launch_config.envd_access_token;
    config->common.network_policy = launch_config.network;

    // Launch-provided custom params override the snapshot's; absent inherits.
    if (launch_config.custom_extension_params.has_value()) {
        config->common.custom_extension_params_json =
            core::Optional<std::string>(launch_config.custom_extension_params->json_bytes);
    } else {
        config->common.custom_extension_params_json = snapshot_custom_extension_params;
    }

    // Rust `get_or_insert_default().extend(..)` — merged over the snapshot's
    // own, not substituted for them: the image's defaults still apply to every
    // key this launch does not name.
    if (launch_config.env_vars.has_value()) {
        if (!config->common.env_vars.has_value()) {
            config->common.env_vars =
                core::Optional<std::map<std::string, std::string> >(
                    std::map<std::string, std::string>());
        }
        for (std::map<std::string, std::string>::const_iterator it =
                 launch_config.env_vars->begin();
             it != launch_config.env_vars->end(); ++it) {
            (*config->common.env_vars)[it->first] = it->second;
        }
    }
    return core::Unit{};
}

std::vector<std::pair<std::size_t, ExtraDrive> >
InitialGuestDriveMountsForSnapshotLaunch(const SnapshotConfig& config,
                                         const SandboxLaunchConfig& launch_config) {
    std::vector<std::pair<std::size_t, ExtraDrive> > mounts;
    // Already bound before the load, so there is nothing to patch afterwards.
    if (launch_config.extra_drives_in_snapshot) return mounts;

    const std::size_t first_slot = config.common.physical_extra_drive_count;
    for (std::size_t i = 0; i < launch_config.extra_drives.size(); ++i) {
        mounts.push_back(std::make_pair(first_slot + i, launch_config.extra_drives[i]));
    }
    return mounts;
}

core::Expected<core::Unit, std::string> PrepareSnapshotConfigForResume(
    SnapshotConfig* config, const core::SandboxId& sandbox_id,
    const core::Optional<EnvdAccessToken>& envd_access_token) {
    if (config == NULL) return core::make_unexpected(std::string("null snapshot config"));

    const std::size_t drive_count = config->common.extra_drives.size();

    // A capture from before reserved volume slots existed lists every drive
    // directly and reserves none. Taken literally its volumes would be bound
    // into physical slots, so the physical count is restated as the whole
    // list -- which is what it meant when it was written.
    if (config->common.volume_drive_slots == 0) {
        config->common.physical_extra_drive_count = drive_count;
    }

    if (config->common.physical_extra_drive_count > drive_count) {
        std::ostringstream why;
        why << "snapshot records " << config->common.physical_extra_drive_count
            << " physical extra drives but only " << drive_count << " logical drives";
        return core::make_unexpected(why.str());
    }

    const std::size_t volume_count = drive_count - config->common.physical_extra_drive_count;
    if (volume_count > config->common.volume_drive_slots) {
        std::ostringstream why;
        why << "snapshot has " << volume_count
            << " launch-time volume drives but only " << config->common.volume_drive_slots
            << " reserved drive slots";
        return core::make_unexpected(why.str());
    }

    // Runtime identity and auth override whatever the capture recorded. The
    // token is a live credential: carrying the captured sandbox's forward
    // would let this one authenticate as that one.
    config->common.envd_access_token = envd_access_token;

    // Rust `get_or_insert_with(|| MmdsMetadata::new(id, "unknown"))` — a
    // capture with no document still needs one, and "unknown" is the snapshot
    // id placeholder upstream uses rather than inventing a real-looking one.
    if (!config->common.mmds_metadata.has_value()) {
        config->common.mmds_metadata =
            core::Optional<MmdsMetadata>(MmdsMetadata(sandbox_id, "unknown"));
    }
    config->common.mmds_metadata->SetSandboxId(sandbox_id);
    config->common.mmds_metadata->SetAccessToken(envd_access_token);
    return core::Unit{};
}

core::Expected<core::Unit, std::string> PartitionExtraDrives(
    SandboxConfig* config, std::size_t max_volume_count) {
    if (config == NULL) return core::make_unexpected(std::string("null config"));

    std::vector<ExtraDrive> physical;
    std::vector<ExtraDrive> volumes;
    for (std::size_t i = 0; i < config->common.extra_drives.size(); ++i) {
        const ExtraDrive& drive = config->common.extra_drives[i];
        if (drive.IsVolume()) {
            volumes.push_back(drive);
        } else {
            physical.push_back(drive);
        }
    }

    config->common.physical_extra_drive_count = physical.size();
    // Rust `min(MAX_EXTRA_DRIVES.saturating_sub(physical_count))`: the guest
    // only has letters `vdc`..`vdz`, so the reservation cannot exceed what is
    // left after the physical drives. Saturating, because a config with more
    // physical drives than letters must yield zero slots rather than wrap.
    const std::size_t remaining =
        kMaxExtraDrives > physical.size() ? kMaxExtraDrives - physical.size() : 0;
    config->common.volume_drive_slots =
        max_volume_count < remaining ? max_volume_count : remaining;

    if (volumes.size() > config->common.volume_drive_slots) {
        std::ostringstream why;
        why << "fresh sandbox has " << volumes.size() << " volumes but only "
            << config->common.volume_drive_slots << " reserved volume slots";
        return core::make_unexpected(why.str());
    }

    // Physical first, volumes after: the order here is the slot order, and
    // `BuildConfigureSequence` splits the list at `physical_extra_drive_count`.
    physical.insert(physical.end(), volumes.begin(), volumes.end());
    config->common.extra_drives = physical;
    return core::Unit{};
}

core::Expected<SandboxConfig, std::string> FreshSandboxConfigForBuild(
    const cfg::AppConfig& app_config,
    const core::Optional<std::string>& cpu_config_json,
    const FreshSandboxBuildSpec& build_spec,
    const SandboxLaunchConfig& launch_config) {
    // A fresh boot's user rootfs is the sandbox's own writable disk.
    ublk::OverlaybdDeviceConfig user_image_config;
    user_image_config.image_config_path = build_spec.image_config_path;
    user_image_config.read_only = false;
    user_image_config.runtime_upper_mode = storage::overlaybd::UpperMode::LogStructured;

    core::Expected<SandboxConfig, std::string> built =
        SandboxConfig::FromAppConfigWithUserImage(app_config, user_image_config);
    if (!built.ok()) return core::make_unexpected(built.error());
    SandboxConfig config = built.value();

    if (!config.common.rootfs_image_config.has_value()) {
        return core::make_unexpected(std::string("fresh sandbox rootfs image config is missing"));
    }
    // Rust re-derives the ublk device from the *final* rootfs config, so the
    // daemon opens the image exactly as `rootfs_image_config` describes it.
    const ublk::OverlaybdDeviceConfig& rootfs = *config.common.rootfs_image_config;
    config.common.ublk_config = ublk::UblkConfig::OverlaybdWithRuntimeUpperMode(
        rootfs.image_config_path, rootfs.read_only, rootfs.runtime_upper_mode);

    // The request's machine shape replaces the node default.
    config.vcpu_count = build_spec.resources.cpu_count;
    config.mem_size_mib = build_spec.resources.memory_mib;

    // 0 means "not requested": the daemon reads the size from the image. A
    // requested size is MiB on the API and bytes on the device. `u32 MiB` in
    // bytes always fits a u64; Rust still uses `checked_mul`, so the bound is
    // asserted rather than assumed.
    if (build_spec.resources.disk_size_mib == 0) {
        config.common.rootfs_virtual_size = core::nullopt;
    } else {
        const uint64_t mib = build_spec.resources.disk_size_mib;
        const uint64_t kMiB = 1024ull * 1024ull;
        if (mib > UINT64_MAX / kMiB) {
            return core::make_unexpected(std::string("requested rootfs size overflows bytes"));
        }
        config.common.rootfs_virtual_size = mib * kMiB;
    }

    config.common.extra_drives = build_spec.extra_drives;

    // Only API-supplied kernel arguments that pass the allowlist reach the
    // command line, appended after the node's own. Absent prefixes mean none
    // are allowed (`unwrap_or_default()` on an empty list).
    const std::vector<std::string> no_prefixes;
    const std::vector<std::string>& allowed =
        app_config.firecracker.allowed_extra_boot_args_prefixes.has_value()
            ? *app_config.firecracker.allowed_extra_boot_args_prefixes
            : no_prefixes;
    const core::Optional<std::string> extra =
        FilterExtraBootArgs(build_spec.extra_boot_args, allowed);
    if (extra.has_value()) {
        config.boot_args = config.boot_args.has_value()
                               ? core::Optional<std::string>(*config.boot_args + " " + *extra)
                               : extra;
    }

    config.common.cpu_config_json = cpu_config_json;

    // The image's command context becomes the guest defaults. Absent and
    // empty env are distinct (`then_some`), so an empty one stays absent.
    if (!build_spec.context.env_vars.empty()) {
        config.common.env_vars = build_spec.context.env_vars;
    } else {
        config.common.env_vars = core::nullopt;
    }
    config.common.default_workdir = build_spec.context.workdir;
    config.common.default_user = build_spec.context.user;

    config.ApplyLaunchConfig(launch_config);
    return config;
}

core::Expected<std::shared_ptr<PausedSandboxState>, core::AnyError>
DecodeFirecrackerPausedState(const std::string& artifact_root, const std::string& state) {
    // The trait hands the payload over as text (the C++ `PausedSandboxState`
    // encodes to a string); Rust receives an already-parsed `serde_json::Value`
    // from the record. Text that is not JSON is this port's form of the
    // deserialize error Rust would raise.
    const core::Expected<core::Json, core::AnyError> value = core::Json::Parse(state);
    if (!value.ok()) {
        return core::make_unexpected(
            core::err("deserialize Firecracker paused state: " + value.error().chain()));
    }
    const core::Expected<FirecrackerPausedState, std::string> decoded =
        FirecrackerPausedState::Decode(artifact_root, value.value());
    if (!decoded.ok()) return core::make_unexpected(core::err(decoded.error()));
    return std::shared_ptr<PausedSandboxState>(new FirecrackerPausedState(decoded.value()));
}


// Rust `snapshot_config_for_fork`.
core::Expected<SnapshotConfig, std::string> SnapshotConfigForFork(
    const SnapshotConfig& source, const SandboxForkSpec& child) {
    SnapshotConfig snapshot = source;

    // The common case: every child resumes the capture as-is.
    //
    // A shortcut, not a behavioural branch: with both lists empty the loops
    // below iterate nothing and return the same config, so no test can tell
    // this apart. Kept because it is the overwhelmingly common path -- a fork
    // whose children share the parent's volumes -- and it states that plainly.
    if (child.replace_drive_ids.empty() && child.extra_drives.empty()) {
        return snapshot;
    }

    std::set<std::string> replacement_ids;
    for (std::size_t i = 0; i < child.replace_drive_ids.size(); ++i) {
        const std::string& drive_id = child.replace_drive_ids[i].first;
        const std::string& replacement_id = child.replace_drive_ids[i].second;

        // Refused, not deduplicated: two slots resolving to one volume would
        // mount the same image twice, and for a writable volume that is two
        // mounts racing on one device.
        if (!replacement_ids.insert(replacement_id).second) {
            return core::make_unexpected(
                std::string("duplicate fork replacement drive ") + replacement_id);
        }

        // Substituted by value. The id alone carries no image config path,
        // mount path or size, and Firecracker reopens each serialized slot
        // path during the load -- so the whole drive has to be in the config
        // before the load rather than patched into a running VM.
        const ExtraDrive* replacement = 0;
        for (std::size_t k = 0; k < child.extra_drives.size(); ++k) {
            if (child.extra_drives[k].drive_id == replacement_id) {
                replacement = &child.extra_drives[k];
                break;
            }
        }
        if (replacement == 0) {
            return core::make_unexpected(
                std::string("missing fork replacement drive ") + replacement_id);
        }

        std::size_t index = snapshot.common.extra_drives.size();
        for (std::size_t k = 0; k < snapshot.common.extra_drives.size(); ++k) {
            if (snapshot.common.extra_drives[k].drive_id == drive_id) {
                index = k;
                break;
            }
        }
        if (index == snapshot.common.extra_drives.size()) {
            return core::make_unexpected(
                std::string("fork source drive ") + drive_id + " does not exist");
        }
        // Replaced in place, so the slot ordering the guest names devices by
        // is untouched.
        snapshot.common.extra_drives[index] = *replacement;
    }

    // Anything the child brings must already have a slot. Appending is refused
    // because the guest's device names come from the serialized layout: a new
    // trailing drive would be fine, but Firecracker has no slot to load it
    // into, and reordering to make room would rename every later device.
    for (std::size_t i = 0; i < child.extra_drives.size(); ++i) {
        const std::string& id = child.extra_drives[i].drive_id;
        bool present = false;
        for (std::size_t k = 0; k < snapshot.common.extra_drives.size(); ++k) {
            if (snapshot.common.extra_drives[k].drive_id == id) {
                present = true;
                break;
            }
        }
        if (!present) {
            return core::make_unexpected(
                std::string("fork cannot append drive ") + id +
                " to captured Firecracker state");
        }
    }

    return snapshot;
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
