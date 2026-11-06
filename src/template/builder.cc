// SPDX-License-Identifier: MIT
// Rust: src/template/builder.rs
#include "agentenv/template/builder.h"

#include <sstream>

#include "agentenv/cfg.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/snapshot/publish.h"

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

// ---------------------------------------------------------------------------
// execute + publish
// ---------------------------------------------------------------------------

snapshot::TemplateBuildErrorReason TemplateBuilder::BuildFailureReason(
    const core::AnyError& error) {
    if (const TemplateBuildFailure* failure = error.Find<TemplateBuildFailure>()) {
        return failure->reason;
    }
    return snapshot::TemplateBuildErrorReason::New(
        "template build failed while running the build sandbox");
}

namespace {

/// Rust `virtual_size.div_ceil(1 << 20).try_into().unwrap_or(u32::MAX)`.
///
/// Rounds **up**: a rootfs of 1 MiB + 1 byte needs two mebibytes of disk, and
/// rounding down would publish a template whose declared disk cannot hold the
/// image it was built from. Saturates rather than wrapping, because a wrapped
/// value would be a *small* disk size for an enormous image — the one failure
/// mode worse than refusing.
uint32_t DiskSizeMibCeil(uint64_t virtual_size) {
    const uint64_t kMib = 1024 * 1024;
    // Divide first, then correct. The obvious `(size + kMib - 1) / kMib`
    // overflows for a size within a mebibyte of `UINT64_MAX` and lands on a
    // *tiny* disk size -- which is the exact failure the saturation below
    // exists to prevent, reintroduced one line above it. Rust's `div_ceil`
    // does it this way for the same reason.
    uint64_t mib = virtual_size / kMib;
    if (virtual_size % kMib != 0) ++mib;
    const uint64_t kMaxU32 = 0xFFFFFFFFULL;
    return static_cast<uint32_t>(mib > kMaxU32 ? kMaxU32 : mib);
}

}  // namespace

TemplatePipelineResult<snapshot::SnapshotRecord> TemplateBuilder::ExecuteAndPublish(
    snapshot::repository::SnapshotRepository& repository, const TemplateBuildContext& context,
    const std::string& operation, const TemplateBuildExecuteFn& execute) const {
    AGENTENV_INFO("executing template build: build_id=" + context.build_snapshot_id.ToString());

    const core::Expected<TemplateBuildExecution, core::AnyError> executed = execute(context);
    if (!executed.ok()) {
        // The reason comes from the payload when there is one, so a step
        // failure keeps its step number; `operation` is layered on as context
        // the way Rust's `error.context(operation)` does.
        return core::make_unexpected(TemplatePipelineError::FromBuild(
            TemplateBuildError::WithReasonSource(BuildFailureReason(executed.error()),
                                                 executed.error().context(operation).chain())));
    }
    const TemplateBuildExecution& execution = executed.value();

    // The disk size is the one resource the *caller* could not supply: it is
    // only known once the build has measured the rootfs it produced, which is
    // why `DefaultSandboxResources` leaves it at 0.
    sandbox::SandboxResources resources = context.resources;
    resources.disk_size_mib = DiskSizeMibCeil(execution.manifest.rootfs.virtual_size);

    AGENTENV_INFO("publishing template snapshot: build_id=" +
                  context.build_snapshot_id.ToString());

    snapshot::SnapshotPublishMetadata metadata;
    metadata.id      = context.build_snapshot_id;
    metadata.alias   = context.alias;
    metadata.source  = snapshot::SnapshotPublishSource::Template();
    // The context the steps *left*, not the one they started from: WORKDIR,
    // ENV and USER set by a step have to be what a sandbox resumes with.
    metadata.context = execution.build_context;
    metadata.startup = execution.startup;
    metadata.resources            = resources;
    metadata.runtime_versions     = execution.runtime_versions;
    metadata.virtualization_mode  = context.virtualization_mode;
    metadata.image_configs        = execution.image_configs;
    // Template builds publish no volume snapshots: a build VM gets no volumes
    // attached, so there is nothing to seal.
    metadata.volume_snapshots.clear();
    // Deliberately not propagated from the base snapshot, as upstream
    // comments: the extension's custom config is a per-sandbox user setting,
    // not part of the built image's content. Inheriting it would bake one
    // caller's runtime parameters into a template everyone else then gets.
    metadata.custom_extension_params = core::Optional<core::Json>();

    // Rust spawns the startup-manifest recording here, before publishing, so
    // the throwaway recording VM overlaps the backend's layer uploads. Not
    // ported: the recording needs `record_startup_pack` and the detached
    // continuation that attaches the manifest afterwards (see
    // `snapshot/publish.h`). Its absence costs the published snapshot its
    // memory startup pack, nothing else -- `publish` leaves that field absent
    // on return upstream too.
    const snapshot::repository::RepositoryResult<snapshot::SnapshotPublication> published =
        snapshot::Publish(repository, metadata, execution.manifest);
    if (!published.ok()) {
        return core::make_unexpected(TemplatePipelineError::FromRepository(published.error()));
    }

    AGENTENV_INFO("template snapshot published: build_id=" +
                  context.build_snapshot_id.ToString());
    return published.value().record;
}

TemplatePipelineResult<snapshot::SnapshotRecord> TemplateBuilder::BuildAndPublishWithId(
    snapshot::repository::SnapshotRepository& repository, const core::SnapshotId& snapshot_id,
    const TemplateBuildSpec& spec, const TemplateBuildExecuteFn& execute) const {
    const TemplateBuildResult<TemplateBuildContext> context =
        PrepareFreshContext(spec, snapshot_id);
    if (!context.ok()) {
        return core::make_unexpected(TemplatePipelineError::FromBuild(context.error()));
    }
    return ExecuteAndPublish(repository, context.value(),
                             "build snapshot artifacts from requested base", execute);
}

TemplatePipelineResult<snapshot::SnapshotRecord> TemplateBuilder::BuildAndPublish(
    snapshot::repository::SnapshotRepository& repository, const TemplateBuildSpec& spec,
    const TemplateBuildExecuteFn& execute) const {
    return BuildAndPublishWithId(repository, core::SnapshotId::Fresh(), spec, execute);
}

TemplatePipelineResult<snapshot::SnapshotRecord> TemplateBuilder::BuildFromSnapshotAndPublish(
    snapshot::repository::SnapshotRepository& repository, const TemplateBuildSpec& spec,
    const core::SnapshotId& snapshot_id, const snapshot::SnapshotRecord& base_snapshot,
    const TemplateBuildExecuteFn& execute) const {
    const TemplateBuildResult<TemplateBuildContext> context =
        PrepareSnapshotBaseContext(spec, snapshot_id, base_snapshot);
    if (!context.ok()) {
        return core::make_unexpected(TemplatePipelineError::FromBuild(context.error()));
    }
    return ExecuteAndPublish(repository, context.value(),
                             "build snapshot artifacts from committed snapshot", execute);
}

}  // namespace tpl
}  // namespace agentenv
