// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/launch_plan.rs
#include "agentenv/orchestrator/launch_plan.h"

#include <utility>

namespace agentenv {
namespace orchestrator {

LaunchPlan LaunchPlan::ForCreateFromSnapshot(
    core::SandboxId id,
    std::string snapshot_id,
    sandbox::SandboxLaunchConfig launch_config,
    SandboxMetadata meta,
    NewTimeout timeout) {
    LaunchPlan p;
    p.kind = LaunchPlanKind::Create;
    p.create.reset(new CreateLaunchPlan);
    p.create->sandbox_id = id;
    p.create->source.kind = CreateLaunchSourceKind::Snapshot;
    p.create->source.snapshot_id = std::move(snapshot_id);
    p.create->launch_config = std::move(launch_config);
    p.create->metadata = std::move(meta);
    p.create->timeout = timeout;
    return p;
}

LaunchPlan LaunchPlan::ForCreateFresh(
    core::SandboxId id,
    sandbox::FreshSandboxBuildSpec build_spec,
    sandbox::SandboxLaunchConfig launch_config,
    SandboxMetadata meta,
    NewTimeout timeout) {
    LaunchPlan p;
    p.kind = LaunchPlanKind::Create;
    p.create.reset(new CreateLaunchPlan);
    p.create->sandbox_id = id;
    p.create->source.kind = CreateLaunchSourceKind::Fresh;
    // Rust boxes the spec inside the enum variant.
    p.create->source.build_spec.reset(
        new sandbox::FreshSandboxBuildSpec(std::move(build_spec)));
    p.create->launch_config = std::move(launch_config);
    p.create->metadata = std::move(meta);
    p.create->timeout = timeout;
    return p;
}

LaunchPlan LaunchPlan::ForResume(
    core::SandboxId id,
    std::shared_ptr<sandbox::PausedSandboxState> paused_state,
    NewTimeout timeout,
    const sandbox::SandboxResources& resources,
    const core::Optional<sandbox::EnvdAccessToken>& envd_access_token) {
    LaunchPlan p;
    p.kind = LaunchPlanKind::Resume;
    p.resume.reset(new ResumeLaunchPlan);
    p.resume->sandbox_id        = id;
    p.resume->paused_state      = std::move(paused_state);
    p.resume->timeout           = timeout;
    p.resume->resources         = resources;
    p.resume->envd_access_token = envd_access_token;
    return p;
}

core::SandboxId LaunchPlan::SandboxId() const {
    if (kind == LaunchPlanKind::Create) return create->sandbox_id;
    return resume->sandbox_id;
}

// Rust `transitional_state`:
//   Create => SandboxState::Creating
//   Resume => SandboxState::Resuming
SandboxState LaunchPlan::TransitionalState() const {
    return kind == LaunchPlanKind::Create ? SandboxState::Creating
                                          : SandboxState::Resuming;
}

// Rust `transitional_metadata` — only Create carries metadata.
const SandboxMetadata* LaunchPlan::TransitionalMetadata() const {
    if (kind == LaunchPlanKind::Create) return &create->metadata;
    return nullptr;
}

NewTimeout LaunchPlan::Timeout() const {
    if (kind == LaunchPlanKind::Create) return create->timeout;
    return resume->timeout;
}

// Rust `resources`:
//   Create => plan.metadata.resources
//   Resume => plan.resources
sandbox::SandboxResources LaunchPlan::Resources() const {
    if (kind == LaunchPlanKind::Create) return create->metadata.resources;
    return resume->resources;
}

}  // namespace orchestrator
}  // namespace agentenv
