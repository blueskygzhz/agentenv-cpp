// SPDX-License-Identifier: MIT
// Rust: src/template/builder.rs — turns a `TemplateBuildSpec` into a
// validated `TemplateBuildContext` for the runner, then publishes the result.
//
// This header covers the preparation-and-validation half. The execute/publish
// half drives `TemplateBuildRunner` (a live build VM) and `SnapshotManager`,
// and lands with the runner port.
//
// The snapshot-based path is where the validation earns its keep. Building on
// top of a committed snapshot *resumes* that snapshot, so several things the
// caller may legitimately ask for are impossible rather than merely unusual:
// the CPU/memory shape is baked into the capture, and a capture taken under a
// different virtualization mode cannot be resumed at all. Each of those is
// rejected up front with a precise reason instead of failing deep inside the
// build.
#ifndef AGENTENV_TEMPLATE_BUILDER_H_
#define AGENTENV_TEMPLATE_BUILDER_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/types.h"
#include "agentenv/sandbox/ublk.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/template/build_spec.h"
#include "agentenv/template/errors.h"

namespace agentenv {
namespace tpl {

/// Rust enum `TemplateBuildBase` (declared in runner.rs).
struct TemplateBuildBase {
    enum class Kind {
        /// `Rootfs { launch_rootfs_path, ublk_config, image_configs }`.
        Rootfs,
        /// `Snapshot { base_snapshot }`.
        Snapshot,
    };

    Kind kind = Kind::Rootfs;

    // --- Rootfs payload -----------------------------------------------------
    std::string                           launch_rootfs_path;
    core::Optional<sandbox::ublk::UblkConfig> ublk_config;
    ImageConfigs                          image_configs;

    // --- Snapshot payload ---------------------------------------------------
    /// Rust boxes a `RunnableSnapshot` (record + manifest + a runtime artifact
    /// lease that pins the layers for the build's duration). Only the record
    /// is needed to validate and prepare the context; the manifest and lease
    /// belong to the runner and arrive with it.
    snapshot::SnapshotRecord base_snapshot;

    static TemplateBuildBase Rootfs(const std::string& launch_rootfs_path,
                                    const core::Optional<sandbox::ublk::UblkConfig>& ublk_config,
                                    const ImageConfigs& image_configs);
    static TemplateBuildBase Snapshot(const snapshot::SnapshotRecord& base_snapshot);
};

/// Rust struct `TemplateBuildContext` (declared in runner.rs).
struct TemplateBuildContext {
    core::SnapshotId                        build_snapshot_id;
    core::Optional<snapshot::SnapshotAlias> alias;
    snapshot::CommandContext                initial_context;
    core::Optional<snapshot::StartupCommand> startup;
    bool                                    override_startup = false;
    sandbox::SandboxResources               resources;
    /// Rust holds a `TempDir` whose Drop removes the tree; it is kept alive
    /// through publish because the artifacts live inside it. Here it is just
    /// the path — `TemplateBuilder` creates it and the caller removes it.
    std::string                             workspace;
    std::vector<TemplateBuildStep>          steps;
    TemplateBuildBase                       base;
    core::Optional<std::string>             cpu_config_json;
    core::VirtualizationMode virtualization_mode = core::VirtualizationMode::Kvm;

    /// Rust `TemplateBuildContext::local_dir`.
    const std::string& LocalDir() const { return workspace; }
};

/// Rust struct `TemplateBuilder`.
class TemplateBuilder {
 public:
    /// Rust `TemplateBuilder::new`.
    TemplateBuilder() {}

    /// Rust `TemplateBuilder::with_cpu_config` — the cluster CPU intersection
    /// applied to newly built templates, so a template stays runnable on every
    /// node in the cluster rather than only the one that built it.
    static TemplateBuilder WithCpuConfig(const core::Optional<std::string>& cpu_config_json);

    /// Rust `TemplateBuilder::current_cpu_config`. Rust reads through an
    /// `Arc<RwLock<Option<String>>>` that the cluster watcher updates; this
    /// holds the value directly, so a caller re-reads it per build.
    const core::Optional<std::string>& CurrentCpuConfig() const { return cpu_config_json_; }

    /// Rust `TemplateBuilder::prepare_fresh_context`.
    TemplateBuildResult<TemplateBuildContext> PrepareFreshContext(
        const TemplateBuildSpec& spec, const core::SnapshotId& snapshot_id) const;

    /// Rust `TemplateBuilder::prepare_snapshot_base_context`.
    TemplateBuildResult<TemplateBuildContext> PrepareSnapshotBaseContext(
        const TemplateBuildSpec& spec, const core::SnapshotId& snapshot_id,
        const snapshot::SnapshotRecord& base_snapshot) const;

    /// Rust `TemplateBuilder::default_sandbox_resources` — the machine
    /// defaults with `disk_size_mib` left at 0, because the disk size is only
    /// known after the build measures the produced rootfs.
    static sandbox::SandboxResources DefaultSandboxResources();

    /// Rust `TemplateBuilder::startup_from_spec` — present only when the spec
    /// actually overrides startup, so a spec that says nothing does not
    /// install an empty startup command over the base image's.
    static core::Optional<snapshot::StartupCommand> StartupFromSpec(
        const TemplateBuildSpec& spec);

    /// Rust `TemplateBuilder::prepare_base_rootfs`.
    static TemplateBuildResult<TemplateBuildBase> PrepareBaseRootfs(
        const TemplateBuildRootfsBase& rootfs_base);

    /// Rust `TemplateBuilder::load_overlaybd_build_ublk_config`.
    static sandbox::ublk::UblkConfig LoadOverlaybdBuildUblkConfig(
        const std::string& image_config_path);

 private:
    core::Optional<std::string> cpu_config_json_;
};

}  // namespace tpl
}  // namespace agentenv
#endif  // AGENTENV_TEMPLATE_BUILDER_H_
