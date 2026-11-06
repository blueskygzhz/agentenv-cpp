// SPDX-License-Identifier: MIT
// Rust: src/template/builder.rs — turns a `TemplateBuildSpec` into a
// validated `TemplateBuildContext` for the runner, then publishes the result.
//
// This header covers preparation-and-validation *and* publication. What is
// still missing is the middle: the runner's `build_template` /
// `run_template_build`, which boot the build VM. That is why execution arrives
// here as a `TemplateBuildExecuteFn` — upstream passes it as a parameter too
// (`execute_in_background`), so this is the real seam rather than a stand-in,
// and the publish half's decisions can be driven without a VMM.
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

#include <functional>
#include <string>
#include <vector>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/types.h"
#include "agentenv/sandbox/ublk.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/interfaces.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/template/build_spec.h"
#include "agentenv/template/errors.h"
// `TemplateBuildExecution` — declared there because upstream declares it in
// runner.rs, and the publish half consumes it.
#include "agentenv/template/runner.h"

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

/// How `ExecuteAndPublish` runs the build.
///
/// This is Rust's own seam, not an invention: `execute_in_background(context,
/// execute)` takes the executing closure as a parameter and the only
/// production caller passes `TemplateBuildRunner::new().execute`. Keeping it a
/// parameter is what lets the publish half be driven and asserted without a
/// VMM — and it is why this half could be ported before the sandbox-driving
/// half, which still needs `SandboxBackend::capture_to_dir`.
///
/// The error is an `AnyError` because the build's failures are of two kinds
/// and must stay distinguishable: a step failure carries a
/// `TemplateBuildFailure` payload (and therefore a step number), while a
/// sandbox lifecycle failure carries only a message. `BuildFailureReason`
/// depends on being able to tell them apart.
typedef std::function<core::Expected<TemplateBuildExecution, core::AnyError>(
    const TemplateBuildContext&)>
    TemplateBuildExecuteFn;

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

    /// Rust `TemplateBuilder::build_and_publish` — a generated id.
    TemplatePipelineResult<snapshot::SnapshotRecord> BuildAndPublish(
        snapshot::repository::SnapshotRepository& repository, const TemplateBuildSpec& spec,
        const TemplateBuildExecuteFn& execute) const;

    /// Rust `TemplateBuilder::build_and_publish_with_id`.
    TemplatePipelineResult<snapshot::SnapshotRecord> BuildAndPublishWithId(
        snapshot::repository::SnapshotRepository& repository, const core::SnapshotId& snapshot_id,
        const TemplateBuildSpec& spec, const TemplateBuildExecuteFn& execute) const;

    /// Rust `TemplateBuilder::build_from_snapshot_and_publish`.
    TemplatePipelineResult<snapshot::SnapshotRecord> BuildFromSnapshotAndPublish(
        snapshot::repository::SnapshotRepository& repository, const TemplateBuildSpec& spec,
        const core::SnapshotId& snapshot_id, const snapshot::SnapshotRecord& base_snapshot,
        const TemplateBuildExecuteFn& execute) const;

    /// Rust `TemplateBuilder::execute_and_publish`.
    ///
    /// `operation` is the context string attached to a build failure, so the
    /// rendered error says which of the two entry points was taken.
    ///
    /// The context is taken by value and kept alive across the publish for the
    /// reason upstream gives it a `workspace_lease`: the manifest the publish
    /// reads names artifacts that live *inside* the context's workspace, so
    /// releasing it first would have the backend uploading from a deleted
    /// directory.
    TemplatePipelineResult<snapshot::SnapshotRecord> ExecuteAndPublish(
        snapshot::repository::SnapshotRepository& repository, const TemplateBuildContext& context,
        const std::string& operation, const TemplateBuildExecuteFn& execute) const;

    /// Rust `TemplateBuilder::build_failure_reason`.
    ///
    /// Prefers a `TemplateBuildFailure` found anywhere in the chain over a
    /// generic reason: that is the only carrier of the *failing step*, and the
    /// e2b SDKs read that field as an integer index. A sandbox-lifecycle error
    /// (start, capture, stop) has no such payload and gets the generic text.
    static snapshot::TemplateBuildErrorReason BuildFailureReason(const core::AnyError& error);

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
