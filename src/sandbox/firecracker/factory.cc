// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/factory.rs
#include "agentenv/sandbox/firecracker/factory.h"

#include <cstdint>
#include <vector>

namespace agentenv {
namespace sandbox {
namespace firecracker {

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

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
