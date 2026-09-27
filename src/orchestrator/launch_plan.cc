// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/launch_plan.rs
#include "agentenv/orchestrator/launch_plan.h"

namespace agentenv {
namespace orchestrator {

LaunchPlan LaunchPlan::ForCreateFromSnapshot(core::SandboxId id,
                                             std::string snapshot_id,
                                             SandboxMetadata meta,
                                             NewTimeout timeout) {
    LaunchPlan p;
    p.kind = LaunchPlanKind::Create;
    p.create.reset(new CreateLaunchPlan);
    p.create->sandbox_id = id;
    p.create->source.kind = CreateLaunchSourceKind::Snapshot;
    p.create->source.snapshot_id = std::move(snapshot_id);
    p.create->metadata = std::move(meta);
    p.create->timeout = timeout;
    return p;
}

LaunchPlan LaunchPlan::ForCreateFresh(core::SandboxId id,
                                      std::string image_ref,
                                      SandboxMetadata meta,
                                      NewTimeout timeout) {
    LaunchPlan p;
    p.kind = LaunchPlanKind::Create;
    p.create.reset(new CreateLaunchPlan);
    p.create->sandbox_id = id;
    p.create->source.kind = CreateLaunchSourceKind::Fresh;
    p.create->source.image_ref = std::move(image_ref);
    p.create->metadata = std::move(meta);
    p.create->timeout = timeout;
    return p;
}

LaunchPlan LaunchPlan::ForResume(core::SandboxId id,
                                 NewTimeout timeout,
                                 uint32_t cpu_count,
                                 uint32_t memory_mib) {
    LaunchPlan p;
    p.kind = LaunchPlanKind::Resume;
    p.resume.reset(new ResumeLaunchPlan);
    p.resume->sandbox_id = id;
    p.resume->timeout = timeout;
    p.resume->cpu_count = cpu_count;
    p.resume->memory_mib = memory_mib;
    return p;
}

core::SandboxId LaunchPlan::SandboxId() const {
    if (kind == LaunchPlanKind::Create) return create->sandbox_id;
    return resume->sandbox_id;
}

LifecyclePhase LaunchPlan::TransitionalState() const {
    return kind == LaunchPlanKind::Create ? LifecyclePhase::Booting
                                          : LifecyclePhase::Booting;
}

NewTimeout LaunchPlan::Timeout() const {
    if (kind == LaunchPlanKind::Create) return create->timeout;
    return resume->timeout;
}

}  // namespace orchestrator
}  // namespace agentenv
