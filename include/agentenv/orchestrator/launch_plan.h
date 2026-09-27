// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/launch_plan.rs
#ifndef AGENTENV_ORCHESTRATOR_LAUNCH_PLAN_H_
#define AGENTENV_ORCHESTRATOR_LAUNCH_PLAN_H_

#include <memory>
#include <string>

#include "agentenv/core/identity.h"
#include "agentenv/orchestrator/store.h"
#include "agentenv/orchestrator/types.h"

namespace agentenv {
namespace orchestrator {

/// Rust enum tag for CreateLaunchSource.
enum class CreateLaunchSourceKind {
    Snapshot,
    Fresh,
};

/// Rust struct `CreateLaunchSource`.
struct CreateLaunchSource {
    CreateLaunchSourceKind kind = CreateLaunchSourceKind::Fresh;
    std::string snapshot_id;   // when kind == Snapshot
    std::string image_ref;     // when kind == Fresh
};

/// Rust struct `CreateLaunchPlan`.
struct CreateLaunchPlan {
    core::SandboxId    sandbox_id;
    CreateLaunchSource source;
    SandboxMetadata    metadata;
    NewTimeout         timeout;
};

/// Rust struct `ResumeLaunchPlan`.
struct ResumeLaunchPlan {
    core::SandboxId sandbox_id;
    NewTimeout      timeout;
    uint32_t        cpu_count = 1;
    uint32_t        memory_mib = 128;
};

/// Rust enum `LaunchPlan`.
enum class LaunchPlanKind { Create, Resume };
struct LaunchPlan {
    LaunchPlanKind kind = LaunchPlanKind::Create;
    std::shared_ptr<CreateLaunchPlan> create;
    std::shared_ptr<ResumeLaunchPlan> resume;

    static LaunchPlan ForCreateFromSnapshot(core::SandboxId id,
                                            std::string snapshot_id,
                                            SandboxMetadata meta,
                                            NewTimeout timeout);

    static LaunchPlan ForCreateFresh(core::SandboxId id,
                                     std::string image_ref,
                                     SandboxMetadata meta,
                                     NewTimeout timeout);

    static LaunchPlan ForResume(core::SandboxId id,
                                NewTimeout timeout,
                                uint32_t cpu_count,
                                uint32_t memory_mib);

    core::SandboxId    SandboxId() const;
    LifecyclePhase     TransitionalState() const;
    NewTimeout         Timeout() const;
};

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_LAUNCH_PLAN_H_
