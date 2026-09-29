// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/launch_plan.rs
#ifndef AGENTENV_ORCHESTRATOR_LAUNCH_PLAN_H_
#define AGENTENV_ORCHESTRATOR_LAUNCH_PLAN_H_

#include <memory>
#include <string>

#include "agentenv/core/identity.h"
#include "agentenv/orchestrator/store.h"
#include "agentenv/orchestrator/types.h"
#include "agentenv/sandbox/types.h"

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
    core::SandboxId           sandbox_id;
    NewTimeout                timeout;
    /// Rust `resources: SandboxResources` — replaces the split cpu/mem fields.
    sandbox::SandboxResources resources;
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
                                const sandbox::SandboxResources& resources);

    /// Rust `sandbox_id()`.
    core::SandboxId SandboxId() const;
    /// Rust `transitional_state()` — Create yields Creating, Resume yields Resuming.
    SandboxState    TransitionalState() const;
    /// Rust `transitional_metadata()` — only a Create plan carries metadata.
    const SandboxMetadata* TransitionalMetadata() const;
    /// Rust `timeout()`.
    NewTimeout      Timeout() const;
    /// Rust `resources()`.
    sandbox::SandboxResources Resources() const;
};

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_LAUNCH_PLAN_H_
