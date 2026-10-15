// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/factory.rs
#include "agentenv/sandbox/firecracker/factory.h"

#include <cstdint>
#include <sstream>
#include <vector>

namespace agentenv {
namespace sandbox {
namespace firecracker {

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

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
