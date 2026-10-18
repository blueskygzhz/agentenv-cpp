// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/service.rs :: fork_sandbox, capture_snapshot,
//       snapshot_volume_mounts, replace_sandbox_network_policy,
//       patch_sandbox_custom_extension_params + token methods
#include "microtest.h"

#include <memory>
#include <set>
#include <string>

#include "orchestrator_fakes.h"

using namespace agentenv;
using namespace agentenv::orchestrator;
using agentenv::testing::Fixture;
using agentenv::testing::BackendScript;
using agentenv::testing::FakeSandbox;
using agentenv::testing::NowMs;

namespace {

/// Adds a Running sandbox to the store and registers its handle.
SandboxMetadata RegisterRunning(Fixture& f, uint32_t cpu, uint32_t mib) {
    SandboxMetadata m;
    m.id    = core::SandboxId::Fresh();
    m.state = SandboxState::Running;
    m.resources.cpu_count  = cpu;
    m.resources.memory_mib = mib;
    m.created_at_ms = NowMs();
    m.timeout_ms = core::Optional<int64_t>(60 * 1000);
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    std::unique_ptr<sandbox::SandboxBackend> backend(new FakeSandbox(&f.script));
    f.orch->RegisterSandboxHandle(
        m.id, SandboxHandlePtr(new SandboxHandle(std::move(backend))));
    f.orch->UpsertProxyRoute(m.id, ProxyTarget("10.0.0.1"));
    return m;
}

}  // namespace

// ============================================================
// fork_sandbox :: recoverable backend failure restores source to Running
// ============================================================
// FakeSandbox::Fork returns Recoverable by default. The orchestrator must
// restore the source to Running (CAS Forking → Running) and surface
// SandboxOperationFailed.
MT_TEST(fork_sandbox_recoverable_failure_restores_source_running) {
    Fixture f;
    SandboxMetadata source = RegisterRunning(f, 2, 512);

    OrchestratorResult<std::vector<SandboxForkOutcome> > forked =
        f.orch->ForkSandbox(source.id, 2, NewTimeout::Set(30 * 1000));
    MT_EXPECT_TRUE(!forked.ok());
    MT_EXPECT_TRUE(forked.error().kind ==
                   OrchestratorErrorKind::SandboxOperationFailed);

    // Source must be back to Running after a recoverable backend error.
    MT_EXPECT_TRUE(f.orch->GetSandbox(source.id).value()->state ==
                   SandboxState::Running);
    // Source handle must still be registered.
    MT_EXPECT_TRUE(f.orch->SandboxHandleFor(source.id) != nullptr);
}

// ============================================================
// fork_sandbox :: terminal backend failure removes the source sandbox
// ============================================================
MT_TEST(fork_sandbox_terminal_failure_removes_source_sandbox) {
    Fixture f;
    f.script.fail_fork_terminal = true;
    SandboxMetadata source = RegisterRunning(f, 2, 512);

    OrchestratorResult<std::vector<SandboxForkOutcome> > forked =
        f.orch->ForkSandbox(source.id, 1, NewTimeout::Set(30 * 1000));
    MT_EXPECT_TRUE(!forked.ok());
    MT_EXPECT_TRUE(forked.error().kind ==
                   OrchestratorErrorKind::SandboxOperationFailed);

    // Terminal: source sandbox must have been removed from the store.
    MT_EXPECT_TRUE(!f.orch->GetSandbox(source.id).value().has_value());
    MT_EXPECT_TRUE(f.orch->SandboxHandleFor(source.id) == nullptr);
}

// ============================================================
// fork_sandbox :: refuses source that has volume mounts
// ============================================================
MT_TEST(fork_sandbox_refuses_source_with_volume_mounts) {
    Fixture f;
    SandboxMetadata source = RegisterRunning(f, 2, 512);

    // Update the already-registered record to add volume mounts.
    std::vector<SandboxState> running;
    running.push_back(SandboxState::Running);
    MT_EXPECT_TRUE(f.store->UpdateIfState(
        source.id, running,
        [](SandboxMetadata* m) { m->volume_mounts["/data"] = "vol-123"; }).ok());

    OrchestratorResult<std::vector<SandboxForkOutcome> > forked =
        f.orch->ForkSandbox(source.id, 1, NewTimeout::Set(30 * 1000));
    MT_EXPECT_TRUE(!forked.ok());
    MT_EXPECT_TRUE(forked.error().kind == OrchestratorErrorKind::InternalError);
}

// ============================================================
// fork_sandbox :: the success path
// ============================================================
// Rust `fork_sandbox_creates_running_children_from_one_source`.
//
// Nothing below this comment was reachable before `FakeSandbox::Fork` could
// succeed, so the whole child-registration loop went unasserted: removing
// `metadata.id = child_id` -- which registers every child under the source's
// id -- used to pass the entire suite.
MT_TEST(fork_sandbox_creates_running_children_from_one_source) {
    Fixture f;
    f.script.succeed_fork = true;
    SandboxMetadata source = RegisterRunning(f, 2, 512);
    source.snapshot_id = "snap-src";
    source.user_metadata["team"] = "batch-fork-source";
    // A paused capture on the source, so the child clearing it is observable.
    // Without this the field is empty on both sides and the clone could keep
    // it without any assertion noticing.
    source.paused_state =
        std::shared_ptr<sandbox::PausedSandboxState>(new agentenv::testing::FakePausedState());
    // Volume mounts on the source that the children must *not* inherit: each
    // child gets its own set, and silently sharing the parent's would hand two
    // sandboxes the same exclusive volume.
    source.volume_mounts["/data"] = "vol-parent";
    MT_EXPECT_TRUE(f.store->Update(source).ok());

    std::vector<SandboxLifecycleEvent> events;
    f.orch->SubscribeSandboxEvents(
        [&events](const SandboxLifecycleEvent& e) { events.push_back(e); });

    std::vector<SandboxForkChildSpec> specs;
    for (int i = 0; i < 3; ++i) {
        SandboxForkChildSpec spec;
        spec.sandbox_id = core::SandboxId::Fresh();
        // Per-child mounts, which is what `SandboxForkChildSpec` exists for.
        spec.volume_mounts["/child"] = "vol-child";
        specs.push_back(spec);
    }

    OrchestratorResult<std::vector<SandboxForkOutcome> > forked =
        f.orch->ForkSandboxWithSpecs(source.id, specs, NewTimeout::UseExisting());
    MT_EXPECT_TRUE(forked.ok());
    if (!forked.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(forked.value().size()), 3);

    std::set<std::string> child_ids;
    for (std::size_t i = 0; i < forked.value().size(); ++i) {
        MT_EXPECT_TRUE(forked.value()[i].ok());
        if (!forked.value()[i].ok()) continue;
        const SandboxMetadata& child = forked.value()[i].value();

        // A fresh identity per child, distinct from the source and from each
        // other. Sharing the source's id would make the store registration
        // overwrite the parent's record.
        MT_EXPECT_TRUE(!(child.id == source.id));
        MT_EXPECT_TRUE(child_ids.insert(child.id.ToString()).second);

        // Running, not Creating: the backend already started, so a child left
        // in a pre-running state would never be reaped by the timeout sweep
        // nor answer requests.
        MT_EXPECT_TRUE(child.state == SandboxState::Running);

        // Inherited from the source record, which is the point of cloning it.
        MT_EXPECT_EQ(child.snapshot_id, source.snapshot_id);
        MT_EXPECT_TRUE(child.user_metadata == source.user_metadata);
        MT_EXPECT_EQ(child.resources.cpu_count, source.resources.cpu_count);

        // `UseExisting` keeps the source's timeout rather than clearing it.
        MT_EXPECT_TRUE(child.timeout_ms.has_value());
        if (child.timeout_ms.has_value() && source.timeout_ms.has_value()) {
            MT_EXPECT_EQ(*child.timeout_ms, *source.timeout_ms);
        }

        // Paused state must not survive the clone: the child is running, and
        // a stale capture here would let a later resume load the parent's VM
        // image under the child's id.
        MT_EXPECT_TRUE(!child.paused_state);

        // The child's own mounts replace the source's wholesale. Inheriting
        // `/data -> vol-parent` would give the child a volume the parent still
        // holds exclusively.
        MT_EXPECT_EQ(static_cast<int>(child.volume_mounts.size()), 1);
        MT_EXPECT_TRUE(child.volume_mounts.count("/child") == 1);
        MT_EXPECT_TRUE(child.volume_mounts.count("/data") == 0);

        // Registered in the store and the handle map, which is what makes the
        // child addressable at all.
        core::Expected<core::Optional<SandboxMetadata>, StoreError> stored =
            f.store->Get(child.id);
        MT_EXPECT_TRUE(stored.ok() && stored.value());
        MT_EXPECT_TRUE(f.orch->SandboxHandleFor(child.id));
    }

    // One Fork event per registered child. The events drive the live-resource
    // gauges, so a missing one leaves a sandbox running that the metrics do
    // not account for.
    int fork_events = 0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].event_type == SandboxLifecycleEventType::Fork) ++fork_events;
    }
    MT_EXPECT_EQ(fork_events, 3);

    // The source is handed back to Running; it was only Forking for the
    // duration of the backend call.
    core::Expected<core::Optional<SandboxMetadata>, StoreError> after =
        f.store->Get(source.id);
    MT_EXPECT_TRUE(after.ok() && after.value());
    if (after.ok() && after.value()) {
        MT_EXPECT_TRUE(after.value()->state == SandboxState::Running);
    }

    MT_EXPECT_EQ(static_cast<int>(f.orch->counters().CreateSuccesses()), 3);
    MT_EXPECT_EQ(static_cast<int>(f.orch->counters().CreateFails()), 0);
}

// ============================================================
// fork_sandbox :: one child failing to start leaves its siblings alone
// ============================================================
// Rust `fork_sandbox_keeps_successful_siblings_when_one_start_fails`.
MT_TEST(fork_sandbox_keeps_successful_siblings_when_one_start_fails) {
    Fixture f;
    f.script.succeed_fork = true;
    f.script.fork_child_outcomes.push_back(true);
    f.script.fork_child_outcomes.push_back(false);  // the middle child fails
    f.script.fork_child_outcomes.push_back(true);
    SandboxMetadata source = RegisterRunning(f, 2, 512);

    OrchestratorResult<std::vector<SandboxForkOutcome> > forked =
        f.orch->ForkSandbox(source.id, 3, NewTimeout::Set(30 * 1000));
    // The call as a whole succeeds. A per-child failure is reported inside the
    // outcome vector, not by failing the fork -- otherwise one bad child would
    // discard two perfectly good ones.
    MT_EXPECT_TRUE(forked.ok());
    if (!forked.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(forked.value().size()), 3);

    int ok_count = 0;
    int fail_count = 0;
    for (std::size_t i = 0; i < forked.value().size(); ++i) {
        if (forked.value()[i].ok()) {
            ++ok_count;
            const SandboxMetadata& child = forked.value()[i].value();
            MT_EXPECT_TRUE(child.state == SandboxState::Running);
            // `Set` overrides the source's timeout for the children.
            MT_EXPECT_TRUE(child.timeout_ms.has_value());
            if (child.timeout_ms.has_value()) {
                MT_EXPECT_EQ(*child.timeout_ms, static_cast<int64_t>(30 * 1000));
            }
            MT_EXPECT_TRUE(f.orch->SandboxHandleFor(child.id));
        } else {
            ++fail_count;
            // Reported against the child, as a Fork operation failure.
            MT_EXPECT_TRUE(forked.value()[i].error().kind ==
                           OrchestratorErrorKind::SandboxOperationFailed);
        }
    }
    MT_EXPECT_EQ(ok_count, 2);
    MT_EXPECT_EQ(fail_count, 1);

    // The source survives a child's failure untouched.
    core::Expected<core::Optional<SandboxMetadata>, StoreError> after =
        f.store->Get(source.id);
    MT_EXPECT_TRUE(after.ok() && after.value());
    if (after.ok() && after.value()) {
        MT_EXPECT_TRUE(after.value()->state == SandboxState::Running);
    }

    // Counted per child rather than per call: two created, one failed.
    MT_EXPECT_EQ(static_cast<int>(f.orch->counters().CreateSuccesses()), 2);
    MT_EXPECT_EQ(static_cast<int>(f.orch->counters().CreateFails()), 1);
}

// ============================================================
// fork_sandbox :: a child whose registration fails is stopped, not leaked
// ============================================================
// Rust `fork_sandbox_register_failure_cleans_up_metrics`.
//
// The store rejects the child because its id is already taken. That is the
// same branch a backend store failure takes, and the part that matters is what
// happens to the backend that was already running: it has to be stopped, or
// the fork leaks a VM that nothing in the store knows to reap.
MT_TEST(fork_sandbox_register_failure_stops_the_orphaned_child) {
    agentenv::testing::ScriptedStoreFixture f;
    f.script.succeed_fork = true;

    // Same setup as `RegisterRunning`, against the scripted store.
    SandboxMetadata source;
    source.id    = core::SandboxId::Fresh();
    source.state = SandboxState::Running;
    source.resources.cpu_count  = 2;
    source.resources.memory_mib = 512;
    source.created_at_ms = NowMs();
    source.timeout_ms = core::Optional<int64_t>(60 * 1000);
    MT_EXPECT_TRUE(f.store->Add(source).ok());
    {
        std::unique_ptr<sandbox::SandboxBackend> backend(new FakeSandbox(&f.script));
        f.orch->RegisterSandboxHandle(
            source.id, SandboxHandlePtr(new SandboxHandle(std::move(backend))));
    }
    f.orch->UpsertProxyRoute(source.id, ProxyTarget("10.0.0.1"));

    std::vector<SandboxForkChildSpec> specs;
    SandboxForkChildSpec c1;
    c1.sandbox_id = core::SandboxId::Fresh();
    specs.push_back(c1);
    SandboxForkChildSpec c2;
    c2.sandbox_id = core::SandboxId::Fresh();
    specs.push_back(c2);

    f.store->fail_add_for = c1.sandbox_id.ToString();

    OrchestratorResult<std::vector<SandboxForkOutcome> > forked =
        f.orch->ForkSandboxWithSpecs(source.id, specs, NewTimeout::Set(15 * 1000));
    MT_EXPECT_TRUE(forked.ok());
    if (!forked.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(forked.value().size()), 2);

    // First child: registration refused, so reported as a Fork failure.
    MT_EXPECT_TRUE(!forked.value()[0].ok());
    if (!forked.value()[0].ok()) {
        MT_EXPECT_TRUE(forked.value()[0].error().kind ==
                       OrchestratorErrorKind::SandboxOperationFailed);
    }
    // No handle was registered for it either; a handle without a store record
    // would never be found by delete or by the timeout sweep.
    MT_EXPECT_TRUE(!f.orch->SandboxHandleFor(c1.sandbox_id));

    // Its backend was stopped rather than dropped while running.
    MT_EXPECT_EQ(static_cast<int>(f.script.fork_children.size()), 2);
    if (f.script.fork_children.size() == 2) {
        MT_EXPECT_EQ(f.script.fork_children[0]->stop_calls, 1);
        // The sibling that registered fine is still running.
        MT_EXPECT_EQ(f.script.fork_children[1]->stop_calls, 0);
    }

    // Second child: unaffected by its sibling's failure.
    MT_EXPECT_TRUE(forked.value()[1].ok());
    if (forked.value()[1].ok()) {
        MT_EXPECT_TRUE(f.orch->SandboxHandleFor(forked.value()[1].value().id));
    }

    core::Expected<core::Optional<SandboxMetadata>, StoreError> after =
        f.store->Get(source.id);
    MT_EXPECT_TRUE(after.ok() && after.value());
    if (after.ok() && after.value()) {
        MT_EXPECT_TRUE(after.value()->state == SandboxState::Running);
    }

    MT_EXPECT_EQ(static_cast<int>(f.orch->counters().CreateSuccesses()), 1);
    MT_EXPECT_EQ(static_cast<int>(f.orch->counters().CreateFails()), 1);
}

// ============================================================
// fork_sandbox_with_specs :: duplicate child IDs are rejected before backend
// ============================================================
MT_TEST(fork_sandbox_with_specs_rejects_duplicate_child_ids) {
    Fixture f;
    SandboxMetadata source = RegisterRunning(f, 2, 512);

    core::SandboxId dup = core::SandboxId::Fresh();
    std::vector<SandboxForkChildSpec> specs;
    SandboxForkChildSpec c1;
    c1.sandbox_id = dup;
    specs.push_back(c1);
    SandboxForkChildSpec c2;
    c2.sandbox_id = dup;  // collision
    specs.push_back(c2);

    OrchestratorResult<std::vector<SandboxForkOutcome> > forked =
        f.orch->ForkSandboxWithSpecs(source.id, specs, NewTimeout::Set(30 * 1000));
    MT_EXPECT_TRUE(!forked.ok());
    MT_EXPECT_TRUE(forked.error().kind == OrchestratorErrorKind::InternalError);
}

// ============================================================
// fork_sandbox_with_specs :: child ID that equals the source is rejected
// ============================================================
MT_TEST(fork_sandbox_with_specs_rejects_child_id_equal_to_source) {
    Fixture f;
    SandboxMetadata source = RegisterRunning(f, 2, 512);

    std::vector<SandboxForkChildSpec> specs;
    SandboxForkChildSpec c;
    c.sandbox_id = source.id;  // same as source
    specs.push_back(c);

    OrchestratorResult<std::vector<SandboxForkOutcome> > forked =
        f.orch->ForkSandboxWithSpecs(source.id, specs, NewTimeout::Set(30 * 1000));
    MT_EXPECT_TRUE(!forked.ok());
    MT_EXPECT_TRUE(forked.error().kind == OrchestratorErrorKind::InternalError);
}

// ============================================================
// token methods :: GetEnvdAccessToken respects `secure` flag
// ============================================================
MT_TEST(get_envd_access_token_returns_token_iff_secure) {
    Fixture f;
    core::Expected<sandbox::SandboxAccessTokenGenerator, std::string> gen =
        sandbox::SandboxAccessTokenGenerator::New("test-secret-32-bytes-padded!!!!!");
    MT_EXPECT_TRUE(gen.ok());
    f.orch->SetAccessTokenGenerator(gen.value());

    SandboxMetadata secure_meta;
    secure_meta.id     = core::SandboxId::Fresh();
    secure_meta.secure = true;
    core::Optional<sandbox::EnvdAccessToken> tok =
        f.orch->GetEnvdAccessToken(secure_meta);
    MT_EXPECT_TRUE(tok.has_value());
    MT_EXPECT_TRUE(!tok->Expose().empty());

    SandboxMetadata public_meta;
    public_meta.id     = core::SandboxId::Fresh();
    public_meta.secure = false;
    core::Optional<sandbox::EnvdAccessToken> none =
        f.orch->GetEnvdAccessToken(public_meta);
    MT_EXPECT_TRUE(!none.has_value());
}

// ============================================================
// token methods :: ValidateEnvdAccessToken accepts valid candidates
// ============================================================
MT_TEST(validate_envd_access_token_accepts_valid_candidate) {
    Fixture f;
    core::Expected<sandbox::SandboxAccessTokenGenerator, std::string> gen =
        sandbox::SandboxAccessTokenGenerator::New("test-secret-32-bytes-padded!!!!!");
    MT_EXPECT_TRUE(gen.ok());
    f.orch->SetAccessTokenGenerator(gen.value());

    core::SandboxId sid = core::SandboxId::Fresh();
    sandbox::EnvdAccessToken tok = gen.value().Generate(sid);
    MT_EXPECT_TRUE(f.orch->ValidateEnvdAccessToken(sid, tok.Expose()));
    MT_EXPECT_TRUE(!f.orch->ValidateEnvdAccessToken(sid, "bogus"));
}

// ============================================================
// token methods :: TrafficAccessToken round-trips
// ============================================================
MT_TEST(traffic_access_token_generates_and_validates) {
    Fixture f;
    core::Expected<sandbox::SandboxAccessTokenGenerator, std::string> gen =
        sandbox::SandboxAccessTokenGenerator::New("test-secret-32-bytes-padded!!!!!");
    MT_EXPECT_TRUE(gen.ok());
    f.orch->SetAccessTokenGenerator(gen.value());

    core::SandboxId sid = core::SandboxId::Fresh();
    std::string tok = f.orch->TrafficAccessToken(sid);
    MT_EXPECT_TRUE(!tok.empty());
    MT_EXPECT_TRUE(f.orch->ValidateTrafficAccessToken(sid, tok));
    MT_EXPECT_TRUE(!f.orch->ValidateTrafficAccessToken(sid, "wrong"));
}

// ============================================================
// capture_snapshot :: FakeSandbox returns Recoverable — error mapped correctly
// ============================================================
// FakeSandbox::Snapshot() returns a Recoverable error ("fake: snapshot
// unsupported"). The orchestrator should surface SandboxOperationFailed and
// restore the sandbox to Running (recoverable path).
MT_TEST(capture_snapshot_recoverable_failure_restores_running) {
    Fixture f;
    SandboxMetadata m = RegisterRunning(f, 2, 512);

    OrchestratorResult<SnapshotCaptureResult> captured =
        f.orch->CaptureSnapshot(m.id);
    MT_EXPECT_TRUE(!captured.ok());
    MT_EXPECT_TRUE(captured.error().kind ==
                   OrchestratorErrorKind::SandboxOperationFailed);

    // Recoverable: sandbox must be back to Running.
    MT_EXPECT_TRUE(f.orch->GetSandbox(m.id).value()->state ==
                   SandboxState::Running);
}

MT_TEST(capture_snapshot_on_missing_sandbox_is_not_found) {
    Fixture f;
    OrchestratorResult<SnapshotCaptureResult> captured =
        f.orch->CaptureSnapshot(core::SandboxId::Fresh());
    MT_EXPECT_TRUE(!captured.ok());
    MT_EXPECT_TRUE(captured.error().kind ==
                   OrchestratorErrorKind::SandboxNotFound);
}

// ============================================================
// snapshot_volume_mounts :: succeeds for a running sandbox
// ============================================================
MT_TEST(snapshot_volume_mounts_succeeds_for_running_sandbox) {
    Fixture f;
    SandboxMetadata m = RegisterRunning(f, 2, 512);

    OrchestratorResult<core::Unit> snapped = f.orch->SnapshotVolumeMounts(m.id);
    MT_EXPECT_TRUE(snapped.ok());

    // Sandbox is back to Running after the operation.
    MT_EXPECT_TRUE(f.orch->GetSandbox(m.id).value()->state ==
                   SandboxState::Running);
}

MT_TEST(snapshot_volume_mounts_on_missing_sandbox_is_not_found) {
    Fixture f;
    OrchestratorResult<core::Unit> snapped =
        f.orch->SnapshotVolumeMounts(core::SandboxId::Fresh());
    MT_EXPECT_TRUE(!snapped.ok());
    MT_EXPECT_TRUE(snapped.error().kind == OrchestratorErrorKind::SandboxNotFound);
}

// ============================================================
// replace_sandbox_network_policy :: persists the new policy
// ============================================================
MT_TEST(replace_sandbox_network_policy_persists_new_policy) {
    Fixture f;
    SandboxMetadata m = RegisterRunning(f, 2, 512);

    // Ensure allow_public_traffic is true in the store.
    std::vector<SandboxState> running;
    running.push_back(SandboxState::Running);
    MT_EXPECT_TRUE(f.store->UpdateIfState(
        m.id, running,
        [](SandboxMetadata* md) {
            md->network_policy.allow_public_traffic = true;
        }).ok());

    sandbox::network::SandboxNetworkPolicy new_policy;
    new_policy.allow_public_traffic = false;  // Rust: stored value wins, not this
    new_policy.base_policy = sandbox::network::BaseSandboxNetworkPolicy::Default;

    OrchestratorResult<core::Unit> updated =
        f.orch->ReplaceSandboxNetworkPolicy(m.id, new_policy);
    MT_EXPECT_TRUE(updated.ok());

    // The store record must reflect the update.
    core::Optional<SandboxMetadata> got = f.store->Get(m.id).value();
    MT_EXPECT_TRUE(got.has_value());
    // `allow_public_traffic` must stay true — the stored value always wins.
    MT_EXPECT_TRUE(got->network_policy.allow_public_traffic);
}

// ============================================================
// replace_sandbox_network_policy :: missing sandbox
// ============================================================
MT_TEST(replace_sandbox_network_policy_on_missing_sandbox_is_not_found) {
    Fixture f;
    sandbox::network::SandboxNetworkPolicy policy;
    OrchestratorResult<core::Unit> updated =
        f.orch->ReplaceSandboxNetworkPolicy(core::SandboxId::Fresh(), policy);
    MT_EXPECT_TRUE(!updated.ok());
    MT_EXPECT_TRUE(updated.error().kind == OrchestratorErrorKind::SandboxNotFound);
}

// ============================================================
// patch_sandbox_custom_extension_params :: fails when no client
// ============================================================
MT_TEST(patch_custom_extension_params_fails_when_client_absent) {
    Fixture f;
    SandboxMetadata m = RegisterRunning(f, 2, 512);

    sandbox::custom_extension::Params patch;
    patch.json_bytes = "{\"key\":\"value\"}";

    OrchestratorResult<core::Optional<sandbox::custom_extension::Params> > result =
        f.orch->PatchSandboxCustomExtensionParams(m.id, patch);
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().kind ==
                   OrchestratorErrorKind::SandboxOperationFailed);
}

// ============================================================
// patch_sandbox_custom_extension_params :: rejects non-Running
// ============================================================
MT_TEST(patch_custom_extension_params_rejects_paused_sandbox) {
    Fixture f;
    SandboxMetadata m = RegisterRunning(f, 2, 512);
    std::vector<SandboxState> running;
    running.push_back(SandboxState::Running);
    MT_EXPECT_TRUE(f.store->UpdateStateIfState(
        m.id, SandboxState::Paused, running).ok());

    sandbox::custom_extension::Params patch;
    OrchestratorResult<core::Optional<sandbox::custom_extension::Params> > result =
        f.orch->PatchSandboxCustomExtensionParams(m.id, patch);
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().kind ==
                   OrchestratorErrorKind::InvalidSandboxState);
}

MT_MAIN
