// SPDX-License-Identifier: MIT
// Rust: src/template/builder.rs
#include "agentenv/template/builder.h"

#include <sstream>

#include "agentenv/cfg.h"
#include "agentenv/core/fs.h"

namespace agentenv {
namespace tpl {

// ---------------------------------------------------------------------------
// TemplateBuildBase
// ---------------------------------------------------------------------------

TemplateBuildBase TemplateBuildBase::Rootfs(
    const std::string& launch_rootfs_path,
    const core::Optional<sandbox::ublk::UblkConfig>& ublk_config,
    const ImageConfigs& image_configs) {
    TemplateBuildBase base;
    base.kind               = Kind::Rootfs;
    base.launch_rootfs_path = launch_rootfs_path;
    base.ublk_config        = ublk_config;
    base.image_configs      = image_configs;
    return base;
}

TemplateBuildBase TemplateBuildBase::Snapshot(const snapshot::SnapshotRecord& base_snapshot) {
    TemplateBuildBase base;
    base.kind          = Kind::Snapshot;
    base.base_snapshot = base_snapshot;
    return base;
}

// ---------------------------------------------------------------------------
// TemplateBuilder
// ---------------------------------------------------------------------------

TemplateBuilder TemplateBuilder::WithCpuConfig(
    const core::Optional<std::string>& cpu_config_json) {
    TemplateBuilder builder;
    builder.cpu_config_json_ = cpu_config_json;
    return builder;
}

sandbox::SandboxResources TemplateBuilder::DefaultSandboxResources() {
    const cfg::AppConfig* config = cfg::ConfigManager::GlobalConfig();
    sandbox::SandboxResources resources;
    // Rust reads the global config unconditionally; a missing global falls
    // back to the same literals `MachineConfig` declares.
    resources.cpu_count  = config != NULL ? config->machine.vcpu_count : 2;
    resources.memory_mib = config != NULL ? config->machine.mem_size_mib : 1024;
    // Only known after the build measures the produced rootfs.
    resources.disk_size_mib = 0;
    return resources;
}

core::Optional<snapshot::StartupCommand> TemplateBuilder::StartupFromSpec(
    const TemplateBuildSpec& spec) {
    // A spec that overrides nothing must not install an empty startup command
    // over whatever the base image already declares.
    if (!spec.OverridesStartup()) return core::Optional<snapshot::StartupCommand>();

    snapshot::StartupCommand startup;
    startup.start_cmd = spec.StartCmdRef().has_value() ? *spec.StartCmdRef() : std::string();
    startup.ready_cmd = spec.ReadyCmdRef().has_value() ? *spec.ReadyCmdRef() : std::string();
    startup.context   = snapshot::CommandContext();
    startup.shell     = spec.StartupShell();
    return core::Optional<snapshot::StartupCommand>(startup);
}

sandbox::ublk::UblkConfig TemplateBuilder::LoadOverlaybdBuildUblkConfig(
    const std::string& image_config_path) {
    const cfg::AppConfig* config = cfg::ConfigManager::GlobalConfig();
    if (config == NULL) {
        return sandbox::ublk::UblkConfig::OverlaybdWithRuntimeUpperMode(
            image_config_path, false, storage::overlaybd::UpperMode::HybridLogStructured);
    }
    return sandbox::ublk::UblkConfig::OverlaybdWithRuntimeUpperMode(
        image_config_path, config->ublk.overlaybd.read_only,
        config->ublk.overlaybd.runtime_upper_mode);
}

TemplateBuildResult<TemplateBuildBase> TemplateBuilder::PrepareBaseRootfs(
    const TemplateBuildRootfsBase& rootfs_base) {
    if (rootfs_base.is_ext4()) {
        // Publishing is overlaybd-only: an ext4 base would produce artifacts
        // the repository cannot layer or share.
        return core::make_unexpected(TemplateBuildError::InvalidInput(
            "overlaybd-only publish mode does not support ext4 rootfs bases"));
    }

    // Checked here rather than at launch: a missing config is the caller's
    // input error, and reporting it now keeps it out of the build VM.
    if (!core::fs::Exists(rootfs_base.image_config_path)) {
        return core::make_unexpected(TemplateBuildError::InvalidInput(
            "overlaybd image config not found at " + rootfs_base.image_config_path));
    }

    return TemplateBuildBase::Rootfs(
        rootfs_base.image_config_path,
        core::Optional<sandbox::ublk::UblkConfig>(
            LoadOverlaybdBuildUblkConfig(rootfs_base.image_config_path)),
        rootfs_base.image_configs);
}

TemplateBuildResult<TemplateBuildContext> TemplateBuilder::PrepareFreshContext(
    const TemplateBuildSpec& spec, const core::SnapshotId& snapshot_id) const {
    const TemplateBuildResult<core::Optional<snapshot::SnapshotAlias> > alias =
        spec.ParsedAlias();
    if (!alias.ok()) return core::make_unexpected(alias.error());

    if (!spec.RootfsBase().has_value()) {
        return core::make_unexpected(
            TemplateBuildError::InvalidInput("rootfs base is required for build"));
    }

    const TemplateBuildResult<TemplateBuildBase> base = PrepareBaseRootfs(*spec.RootfsBase());
    if (!base.ok()) return core::make_unexpected(base.error());

    const core::Expected<std::string, std::string> workspace =
        core::fs::CreateTempDir("snapshot-" + snapshot_id.ToString() + "-");
    if (!workspace.ok()) {
        return core::make_unexpected(TemplateBuildError::WithSource(
            "create temporary snapshot dir", workspace.error()));
    }

    TemplateBuildContext context;
    context.build_snapshot_id = snapshot_id;
    context.alias             = alias.value();
    context.initial_context =
        spec.BaseContextRef().has_value() ? *spec.BaseContextRef() : snapshot::CommandContext();
    context.startup          = StartupFromSpec(spec);
    context.override_startup = spec.OverridesStartup();
    context.resources =
        spec.ResourcesRef().has_value() ? *spec.ResourcesRef() : DefaultSandboxResources();
    context.workspace = workspace.value();
    context.steps     = spec.Steps();
    context.base      = base.value();
    context.cpu_config_json = cpu_config_json_;

    const cfg::AppConfig* config = cfg::ConfigManager::GlobalConfig();
    context.virtualization_mode =
        config != NULL ? config->virtualization_mode : core::VirtualizationMode::Kvm;
    return context;
}

TemplateBuildResult<TemplateBuildContext> TemplateBuilder::PrepareSnapshotBaseContext(
    const TemplateBuildSpec& spec, const core::SnapshotId& snapshot_id,
    const snapshot::SnapshotRecord& base_snapshot) const {
    // Building "onto" itself would publish over the record the build resumes
    // from, so the two ids must differ.
    if (snapshot_id == base_snapshot.id) {
        return core::make_unexpected(TemplateBuildError::InvalidInput(
            "snapshot build target id must differ from the base snapshot id"));
    }
    // The base *is* the rootfs here; a spec-supplied base would be silently
    // ignored, so it is rejected instead.
    if (spec.RootfsBase().has_value()) {
        return core::make_unexpected(TemplateBuildError::InvalidInput(
            "snapshot-based build must not override the rootfs base"));
    }
    if (!base_snapshot.committed.has_value()) {
        return core::make_unexpected(TemplateBuildError::InvalidInput(
            "base snapshot has no committed artifacts to resume from"));
    }

    const cfg::AppConfig* config = cfg::ConfigManager::GlobalConfig();
    const core::VirtualizationMode node_mode =
        config != NULL ? config->virtualization_mode : core::VirtualizationMode::Kvm;
    const core::VirtualizationMode base_mode = base_snapshot.committed->virtualization_mode;
    if (base_mode != node_mode) {
        // A capture is only resumable under the ABI it was taken with.
        std::ostringstream message;
        message << "base snapshot '" << base_snapshot.id.ToString()
                << "' uses virtualization mode '" << core::VirtualizationModeToString(base_mode)
                << "', but this node runs in mode '"
                << core::VirtualizationModeToString(node_mode) << "'";
        return core::make_unexpected(TemplateBuildError::InvalidInput(message.str()));
    }

    // The CPU/memory shape is baked into the capture, so it cannot be changed
    // by a build that resumes it.
    const sandbox::SandboxResources resources = base_snapshot.resources;
    if (spec.ResourcesRef().has_value()) {
        const sandbox::SandboxResources& requested = *spec.ResourcesRef();
        if (requested.cpu_count != resources.cpu_count ||
            requested.memory_mib != resources.memory_mib) {
            return core::make_unexpected(TemplateBuildError::InvalidInput(
                "snapshot-based build cannot change CPU or memory because it resumes from a "
                "committed snapshot"));
        }
    }

    const TemplateBuildResult<core::Optional<snapshot::SnapshotAlias> > alias =
        spec.ParsedAlias();
    if (!alias.ok()) return core::make_unexpected(alias.error());

    const bool override_startup = spec.OverridesStartup();
    core::Optional<snapshot::StartupCommand> startup;
    if (override_startup) {
        startup = StartupFromSpec(spec);
        // An override that names no shell inherits the base's rather than
        // silently dropping to the default: the base image may only be usable
        // under its own shell.
        if (startup.has_value() && !startup->shell.has_value() &&
            base_snapshot.committed->startup.has_value()) {
            startup->shell = base_snapshot.committed->startup->shell;
        }
    } else {
        startup = base_snapshot.committed->startup;
    }

    const core::Expected<std::string, std::string> workspace =
        core::fs::CreateTempDir("snapshot-" + snapshot_id.ToString() + "-");
    if (!workspace.ok()) {
        return core::make_unexpected(TemplateBuildError::WithSource(
            "create temporary snapshot dir", workspace.error()));
    }

    TemplateBuildContext context;
    context.build_snapshot_id = snapshot_id;
    context.alias             = alias.value();
    context.initial_context   = base_snapshot.committed->context;
    context.startup           = startup;
    context.override_startup  = override_startup;
    context.resources         = resources;
    context.workspace         = workspace.value();
    context.steps             = spec.Steps();
    context.base              = TemplateBuildBase::Snapshot(base_snapshot);
    context.cpu_config_json   = cpu_config_json_;
    // The base's mode, not the node's: they were just proven equal, and the
    // capture's own value is the authoritative one.
    context.virtualization_mode = base_mode;
    return context;
}

}  // namespace tpl
}  // namespace agentenv
