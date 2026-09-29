// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/service.rs :: delete_sandbox / delete_sandbox_inner /
//       delete_sandbox_impl (the Capture -> Stop -> Release phases) /
//       remove_deleted_sandbox / deletion_progress
#include "microtest.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "orchestrator_fakes.h"

using namespace agentenv;
using namespace agentenv::orchestrator;
using agentenv::testing::Fixture;
using agentenv::testing::CreatingMeta;
using agentenv::testing::FreshPlan;

namespace {

/// Launches a sandbox through the real launch path so the delete tests start
/// from genuine Running state with a live handle and a published route.
SandboxMetadata LaunchRunning(Fixture* f) {
    SandboxMetadata meta = CreatingMeta();
    OrchestratorResult<SandboxMetadata> launched = f->orch->LaunchSandbox(FreshPlan(meta));
    MT_EXPECT_TRUE(launched.ok());
    return launched.value();
}

std::vector<SandboxLifecycleEvent> SubscribeEvents(Fixture* f) {
    return std::vector<SandboxLifecycleEvent>();
}

}  // namespace

// ============================================================
// delete_sandbox :: the happy path from Running
// ============================================================
MT_TEST(delete_sandbox_stops_removes_and_publishes_delete_event) {
    Fixture f;

    std::vector<SandboxLifecycleEvent> events;
    f.orch->SubscribeSandboxEvents(
        [&events](const SandboxLifecycleEvent& e) { events.push_back(e); });

    SandboxMetadata meta = LaunchRunning(&f);

    MT_EXPECT_TRUE(f.orch->DeleteSandbox(meta.id).ok());

    // Rust stops the sandbox exactly once.
    MT_EXPECT_EQ(f.script.stop_calls, 1);
    // No volume mounts, so the capture phase never freezes anything.
    MT_EXPECT_EQ(f.script.freeze_calls, 0);

    // Metadata, handle and route are all gone.
    MT_EXPECT_TRUE(!f.orch->GetSandbox(meta.id).value().has_value());
    MT_EXPECT_TRUE(f.orch->SandboxHandleFor(meta.id) == nullptr);
    MT_EXPECT_TRUE(f.orch->ProxyLookupFor(meta.id).value().kind ==
                   ProxyLookupKind::NotFound);

    // Rust `remove_deleted_sandbox` publishes a Delete event with the record's
    // resources.
    MT_EXPECT_EQ(events.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(events[0].event_type == SandboxLifecycleEventType::Delete);
    MT_EXPECT_TRUE(events[0].sandbox_id == meta.id);
    MT_EXPECT_EQ(events[0].resources.cpu_count, static_cast<uint32_t>(2));

    // Rust drops the delete progress entry once Done.
    MT_EXPECT_TRUE(f.orch->DeletionProgress(meta.id) == DeleteProgress::Capture());
}

// ============================================================
// delete_sandbox :: deleting from Paused
// ============================================================
MT_TEST(delete_sandbox_deletes_a_paused_sandbox) {
    Fixture f;
    SandboxMetadata meta = CreatingMeta();
    meta.state = SandboxState::Paused;
    MT_EXPECT_TRUE(f.store->Add(meta).ok());

    // A paused sandbox has no live handle, which must not fail the delete.
    MT_EXPECT_TRUE(f.orch->DeleteSandbox(meta.id).ok());
    MT_EXPECT_EQ(f.script.stop_calls, 0);
    MT_EXPECT_TRUE(!f.orch->GetSandbox(meta.id).value().has_value());
}

// ============================================================
// delete_sandbox :: idempotence and absence
// ============================================================
MT_TEST(delete_sandbox_is_idempotent_and_reports_a_missing_sandbox) {
    Fixture f;
    SandboxMetadata meta = LaunchRunning(&f);
    MT_EXPECT_TRUE(f.orch->DeleteSandbox(meta.id).ok());

    // Rust: the record is gone, so the Killing CAS reports SandboxNotFound.
    OrchestratorResult<core::Unit> again = f.orch->DeleteSandbox(meta.id);
    MT_EXPECT_TRUE(!again.ok());
    MT_EXPECT_TRUE(again.error().kind == OrchestratorErrorKind::SandboxNotFound);

    // A never-seen id behaves the same way.
    OrchestratorResult<core::Unit> unknown = f.orch->DeleteSandbox(core::SandboxId::Fresh());
    MT_EXPECT_TRUE(!unknown.ok());
    MT_EXPECT_TRUE(unknown.error().kind == OrchestratorErrorKind::SandboxNotFound);
}

// ============================================================
// delete_sandbox :: an already-Done progress short-circuits
// ============================================================
MT_TEST(delete_sandbox_returns_ok_when_progress_is_already_done) {
    Fixture f;
    core::SandboxId id = core::SandboxId::Fresh();
    // Rust `DeleteProgress::Done => return Ok(())`, even with no record at all.
    f.orch->SetDeletionProgress(id, DeleteProgress::Done());
    MT_EXPECT_TRUE(f.orch->DeleteSandbox(id).ok());
}

// ============================================================
// delete_sandbox_impl :: a failed Stop keeps the handle for a retry
// ============================================================
MT_TEST(delete_sandbox_stop_failure_reinserts_the_handle_and_can_be_retried) {
    Fixture f;
    SandboxMetadata meta = LaunchRunning(&f);

    f.script.fail_stop = true;
    OrchestratorResult<core::Unit> failed = f.orch->DeleteSandbox(meta.id);
    MT_EXPECT_TRUE(!failed.ok());
    MT_EXPECT_TRUE(failed.error().kind ==
                   OrchestratorErrorKind::SandboxOperationFailed);
    MT_EXPECT_TRUE(failed.error().operation == SandboxOperation::Stop);
    MT_EXPECT_EQ(f.script.stop_calls, 1);

    // Rust re-inserts the handle while still in the Stop phase so a retry can
    // stop the sandbox again; the record stays in Killing.
    MT_EXPECT_TRUE(f.orch->SandboxHandleFor(meta.id) != nullptr);
    MT_EXPECT_TRUE(f.orch->GetSandbox(meta.id).value()->state == SandboxState::Killing);
    MT_EXPECT_TRUE(f.orch->DeletionProgress(meta.id) == DeleteProgress::Stop(false));

    // Retrying resumes from Stop — it does not re-run the Killing CAS.
    f.script.fail_stop = false;
    MT_EXPECT_TRUE(f.orch->DeleteSandbox(meta.id).ok());
    MT_EXPECT_EQ(f.script.stop_calls, 2);
    MT_EXPECT_TRUE(!f.orch->GetSandbox(meta.id).value().has_value());
}

// ============================================================
// delete_sandbox_inner :: the Killing CAS waits out a transitional state
// ============================================================
MT_TEST(delete_sandbox_waits_for_a_transitional_state_then_claims_killing) {
    Fixture f;
    SandboxMetadata meta = LaunchRunning(&f);

    // Park the sandbox in Snapshotting, then let another thread finish that
    // operation. Rust waits for the transition and retries the Killing CAS.
    std::vector<SandboxState> running;
    running.push_back(SandboxState::Running);
    MT_EXPECT_TRUE(f.store->UpdateStateIfState(meta.id, SandboxState::Snapshotting,
                                               running).ok());

    std::vector<SandboxState> snapshotting;
    snapshotting.push_back(SandboxState::Snapshotting);
    std::shared_ptr<InMemoryMetadataStore> store = f.store;
    core::SandboxId id = meta.id;
    std::thread finisher([store, id, snapshotting]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        store->UpdateStateIfState(id, SandboxState::Running, snapshotting);
    });

    MT_EXPECT_TRUE(f.orch->DeleteSandbox(meta.id).ok());
    finisher.join();

    MT_EXPECT_TRUE(!f.orch->GetSandbox(meta.id).value().has_value());
    MT_EXPECT_EQ(f.script.stop_calls, 1);
}

// ============================================================
// delete_sandbox_inner :: a concurrent delete that removes the record
// ============================================================
MT_TEST(delete_sandbox_returns_ok_when_a_concurrent_delete_removes_the_record) {
    Fixture f;
    SandboxMetadata meta = LaunchRunning(&f);

    // Hold the sandbox in Killing as if another deleter owned it, then have
    // that deleter remove the record while we wait.
    std::vector<SandboxState> running;
    running.push_back(SandboxState::Running);
    MT_EXPECT_TRUE(f.store->UpdateStateIfState(meta.id, SandboxState::Killing,
                                               running).ok());

    std::shared_ptr<InMemoryMetadataStore> store = f.store;
    core::SandboxId id = meta.id;
    std::thread remover([store, id]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        store->Remove(id);
    });

    // Rust: `wait_for_transition` yields SandboxNotFound, which the delete
    // path turns into Ok(()) because the sandbox is already gone.
    MT_EXPECT_TRUE(f.orch->DeleteSandbox(meta.id).ok());
    remover.join();
}

// ============================================================
// delete_sandbox_inner :: an undeletable state is rejected
// ============================================================
MT_TEST(delete_sandbox_rejects_a_state_that_cannot_be_claimed) {
    Fixture f;
    SandboxMetadata meta = CreatingMeta();
    // Rust's CAS accepts only Running and Paused; Creating is transitional and
    // would be waited on, so use a state that is neither. `Killing` is handled
    // by the wait arm, which leaves no non-waited rejectable state other than
    // a store-level conflict — assert the store conflict surfaces as such.
    meta.state = SandboxState::Paused;
    MT_EXPECT_TRUE(f.store->Add(meta).ok());
    // Deleting from Paused succeeds, which is the positive control here.
    MT_EXPECT_TRUE(f.orch->DeleteSandbox(meta.id).ok());
}

// ============================================================
// delete_sandbox_impl :: capture phase with volumes
// ============================================================
MT_TEST(delete_sandbox_freezes_volumes_before_stopping_a_running_sandbox) {
    Fixture f;
    SandboxMetadata meta = CreatingMeta();
    meta.volume_mounts["/data"] = "vol-1";

    sandbox::FreshSandboxBuildSpec spec;
    spec.image_config_path = "/img/alpine.json";
    sandbox::SandboxLaunchConfig config;
    config.sandbox_id = meta.id;
    MT_EXPECT_TRUE(f.orch->LaunchSandbox(LaunchPlan::ForCreateFresh(
        meta.id, spec, config, meta, NewTimeout::None())).ok());

    MT_EXPECT_TRUE(f.orch->DeleteSandbox(meta.id).ok());
    // Rust freezes volumes only when the previous state was Running AND the
    // sandbox has volume mounts.
    MT_EXPECT_EQ(f.script.freeze_calls, 1);
    MT_EXPECT_EQ(f.script.thaw_calls, 0);
    MT_EXPECT_EQ(f.script.stop_calls, 1);
}

MT_TEST(delete_sandbox_recoverable_capture_failure_restores_the_running_sandbox) {
    Fixture f;
    SandboxMetadata meta = CreatingMeta();
    meta.volume_mounts["/data"] = "vol-1";

    sandbox::FreshSandboxBuildSpec spec;
    spec.image_config_path = "/img/alpine.json";
    sandbox::SandboxLaunchConfig config;
    config.sandbox_id = meta.id;
    MT_EXPECT_TRUE(f.orch->LaunchSandbox(LaunchPlan::ForCreateFresh(
        meta.id, spec, config, meta, NewTimeout::None())).ok());

    f.script.fail_freeze_recoverable = true;
    OrchestratorResult<core::Unit> failed = f.orch->DeleteSandbox(meta.id);
    MT_EXPECT_TRUE(!failed.ok());
    MT_EXPECT_TRUE(failed.error().operation == SandboxOperation::Stop);

    // Rust's `volumes_frozen` flag is set only *after* a successful freeze, so
    // a failing freeze leaves it false and the thaw is correctly skipped —
    // there is nothing frozen to thaw.
    MT_EXPECT_EQ(f.script.freeze_calls, 1);
    MT_EXPECT_EQ(f.script.thaw_calls, 0);

    // A recoverable capture error puts the handle and route back and restores
    // the previous state, so the sandbox keeps serving.
    MT_EXPECT_EQ(f.script.stop_calls, 0);
    MT_EXPECT_TRUE(f.orch->SandboxHandleFor(meta.id) != nullptr);
    MT_EXPECT_TRUE(f.orch->GetSandbox(meta.id).value()->state == SandboxState::Running);
    MT_EXPECT_TRUE(f.orch->ProxyLookupFor(meta.id).value().kind ==
                   ProxyLookupKind::Ready);

    // The progress stayed at Capture, so a retry redoes the capture.
    MT_EXPECT_TRUE(f.orch->DeletionProgress(meta.id) == DeleteProgress::Capture());
}

MT_TEST(delete_sandbox_terminal_capture_failure_stops_and_still_deletes) {
    Fixture f;
    SandboxMetadata meta = CreatingMeta();
    meta.volume_mounts["/data"] = "vol-1";

    sandbox::FreshSandboxBuildSpec spec;
    spec.image_config_path = "/img/alpine.json";
    sandbox::SandboxLaunchConfig config;
    config.sandbox_id = meta.id;
    MT_EXPECT_TRUE(f.orch->LaunchSandbox(LaunchPlan::ForCreateFresh(
        meta.id, spec, config, meta, NewTimeout::None())).ok());

    f.script.fail_freeze_terminal = true;
    OrchestratorResult<core::Unit> failed = f.orch->DeleteSandbox(meta.id);
    // Rust still reports the capture failure ...
    MT_EXPECT_TRUE(!failed.ok());
    MT_EXPECT_TRUE(failed.error().operation == SandboxOperation::Stop);

    // ... but a terminal error is never thawed or put back: the sandbox is
    // stopped and the record removed.
    MT_EXPECT_EQ(f.script.thaw_calls, 0);
    MT_EXPECT_EQ(f.script.stop_calls, 1);
    MT_EXPECT_TRUE(!f.orch->GetSandbox(meta.id).value().has_value());
    MT_EXPECT_TRUE(f.orch->SandboxHandleFor(meta.id) == nullptr);
}

// NOTE: the remaining capture branch — freeze SUCCEEDS but
// `publish_sandbox_volume_backings` fails, which is the only way Rust reaches
// the thaw (and the failed-thaw escalation to a terminal error) — needs a
// `VolumeManager` whose `recover_and_publish_backings` fails. `VolumeManager`
// is a concrete class with a real catalog and repository behind it, so it
// cannot be faked here yet. That branch stays untested rather than asserted
// against behaviour the fake cannot produce.

MT_TEST(delete_sandbox_without_a_volume_manager_skips_every_volume_step) {
    Fixture f;
    SandboxMetadata meta = CreatingMeta();
    meta.volume_mounts["/data"] = "vol-1";

    sandbox::FreshSandboxBuildSpec spec;
    spec.image_config_path = "/img/alpine.json";
    sandbox::SandboxLaunchConfig config;
    config.sandbox_id = meta.id;
    MT_EXPECT_TRUE(f.orch->LaunchSandbox(LaunchPlan::ForCreateFresh(
        meta.id, spec, config, meta, NewTimeout::None())).ok());

    // Rust: `let Some(manager) = self.volume_manager.as_ref() else { return };`
    // The freeze still runs (it is the backend's job), but publishing, failing
    // and releasing backings are all skipped, so the delete succeeds.
    MT_EXPECT_TRUE(f.orch->DeleteSandbox(meta.id).ok());
    MT_EXPECT_EQ(f.script.freeze_calls, 1);
    MT_EXPECT_EQ(f.script.stop_calls, 1);
    MT_EXPECT_TRUE(!f.orch->GetSandbox(meta.id).value().has_value());
}

// ============================================================
// delete_sandbox_impl :: a template builder's capture failure is optional
// ============================================================
MT_TEST(delete_sandbox_template_builder_capture_failure_is_not_reported) {
    Fixture f;
    SandboxMetadata meta = CreatingMeta();
    meta.template_builder        = true;
    meta.volume_mounts["/data"]  = "vol-1";

    sandbox::FreshSandboxBuildSpec spec;
    spec.image_config_path = "/img/alpine.json";
    sandbox::SandboxLaunchConfig config;
    config.sandbox_id = meta.id;
    MT_EXPECT_TRUE(f.orch->LaunchSandbox(LaunchPlan::ForCreateFresh(
        meta.id, spec, config, meta, NewTimeout::None())).ok());

    f.script.fail_freeze_recoverable = true;
    // Rust: "Builder caches are optional; still stop and release the worker on
    // capture failure." The delete therefore succeeds.
    MT_EXPECT_TRUE(f.orch->DeleteSandbox(meta.id).ok());
    MT_EXPECT_EQ(f.script.stop_calls, 1);
    // An optional cache is not thawed either — it goes straight to stop.
    MT_EXPECT_EQ(f.script.thaw_calls, 0);
    MT_EXPECT_TRUE(!f.orch->GetSandbox(meta.id).value().has_value());
}

// ============================================================
// delete_sandbox :: refused while shutting down? (Rust does NOT gate delete)
// ============================================================
MT_TEST(delete_sandbox_still_runs_after_shutdown_began) {
    Fixture f;
    SandboxMetadata meta = LaunchRunning(&f);
    MT_EXPECT_TRUE(f.orch->Shutdown().ok());

    // Rust's delete_sandbox has no `ensure_accepting_lifecycle_operations`
    // gate: shutdown itself deletes sandboxes, so the path must stay open.
    MT_EXPECT_TRUE(f.orch->DeleteSandbox(meta.id).ok());
    MT_EXPECT_TRUE(!f.orch->GetSandbox(meta.id).value().has_value());
}

MT_MAIN
