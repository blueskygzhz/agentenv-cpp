// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/launch_plan.rs
#ifndef AGENTENV_ORCHESTRATOR_LAUNCH_PLAN_H_
#define AGENTENV_ORCHESTRATOR_LAUNCH_PLAN_H_

#include <memory>
#include <string>

#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/orchestrator/store.h"
#include "agentenv/orchestrator/types.h"
#include "agentenv/sandbox/access.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/types.h"

namespace agentenv {
namespace orchestrator {

/// Rust enum tag for CreateLaunchSource.
enum class CreateLaunchSourceKind {
    Snapshot,
    Fresh,
};

/// Rust enum `CreateLaunchSource`.
struct CreateLaunchSource {
    CreateLaunchSourceKind kind = CreateLaunchSourceKind::Fresh;

    /// Live when kind == Snapshot.
    ///
    /// Rust carries `snapshot: Box<RunnableSnapshot>`. The runnable-snapshot
    /// runtime is not ported yet, so the plan carries the snapshot id that
    /// `SandboxBackendFactory::BuildFromSnapshot` resolves.
    std::string snapshot_id;

    /// Live when kind == Fresh. Rust `build_spec: Box<FreshSandboxBuildSpec>`.
    std::shared_ptr<sandbox::FreshSandboxBuildSpec> build_spec;
};

/// Rust struct `CreateLaunchPlan`.
struct CreateLaunchPlan {
    core::SandboxId             sandbox_id;
    CreateLaunchSource          source;
    sandbox::SandboxLaunchConfig launch_config;
    SandboxMetadata             metadata;
    NewTimeout                  timeout;
};

/// Rust struct `ResumeLaunchPlan`.
struct ResumeLaunchPlan {
    core::SandboxId           sandbox_id;
    /// Rust `paused_state: Arc<dyn PausedSandboxState>`.
    std::shared_ptr<sandbox::PausedSandboxState> paused_state;
    NewTimeout                timeout;
    /// Rust `resources: SandboxResources` — replaces the split cpu/mem fields.
    sandbox::SandboxResources resources;
    core::Optional<sandbox::EnvdAccessToken> envd_access_token;
};

/// Rust enum `LaunchPlan`.
enum class LaunchPlanKind { Create, Resume };
struct LaunchPlan {
    LaunchPlanKind kind = LaunchPlanKind::Create;
    std::shared_ptr<CreateLaunchPlan> create;
    std::shared_ptr<ResumeLaunchPlan> resume;

    /// Rust `LaunchPlan::for_create_from_snapshot`.
    static LaunchPlan ForCreateFromSnapshot(core::SandboxId id,
                                            std::string snapshot_id,
                                            sandbox::SandboxLaunchConfig launch_config,
                                            SandboxMetadata meta,
                                            NewTimeout timeout);

    /// Rust `LaunchPlan::for_create_fresh`.
    static LaunchPlan ForCreateFresh(core::SandboxId id,
                                     sandbox::FreshSandboxBuildSpec build_spec,
                                     sandbox::SandboxLaunchConfig launch_config,
                                     SandboxMetadata meta,
                                     NewTimeout timeout);

    /// Rust `LaunchPlan::for_resume`.
    static LaunchPlan ForResume(
        core::SandboxId id,
        std::shared_ptr<sandbox::PausedSandboxState> paused_state,
        NewTimeout timeout,
        const sandbox::SandboxResources& resources,
        const core::Optional<sandbox::EnvdAccessToken>& envd_access_token);

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
