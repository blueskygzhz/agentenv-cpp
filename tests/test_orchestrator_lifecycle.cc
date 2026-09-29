// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/service.rs write paths +
//       src/orchestrator/store/in_memory.rs (update_if_state / wait_while_in_states)
#include "microtest.h"

#include <thread>

#include "agentenv/orchestrator/persistence.h"
#include "agentenv/orchestrator/service.h"
#include "agentenv/sandbox/mock.h"

using namespace agentenv;
using namespace agentenv::orchestrator;

namespace {

struct Fixture {
    std::shared_ptr<InMemoryMetadataStore> store;
    std::shared_ptr<sandbox::Backend>      backend;
    std::shared_ptr<Orchestrator>          orch;

    Fixture()
        : store(new InMemoryMetadataStore()),
          backend(new sandbox::MockBackend()),
          orch(new Orchestrator(store, backend)) {}
};

int64_t NowMs() {
    return core::SystemTime::Now().unix_nanos / 1000000;
}

SandboxMetadata Running(uint32_t cpu, uint32_t mib) {
    SandboxMetadata m;
    m.id    = core::SandboxId::Fresh();
    m.state = SandboxState::Running;
    m.resources.cpu_count  = cpu;
    m.resources.memory_mib = mib;
    m.created_at_ms = NowMs();
    return m;
}

std::vector<SandboxState> States1(SandboxState s) {
    std::vector<SandboxState> v;
    v.push_back(s);
    return v;
}

}  // namespace

// ============================================================
// in_memory.rs :: update_if_state_applies_only_on_expected_state
// ============================================================
MT_TEST(update_if_state_applies_only_on_expected_state) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    // Matching state: the callback runs and the change is visible.
    core::Expected<MetadataUpdateResult, StoreError> ok = f.store->UpdateIfState(
        m.id, States1(SandboxState::Running),
        [](SandboxMetadata* md) { md->resources.cpu_count = 9; });
    MT_EXPECT_TRUE(ok.ok());
    MT_EXPECT_EQ(ok.value().previous.resources.cpu_count, static_cast<uint32_t>(1));
    MT_EXPECT_EQ(ok.value().current.resources.cpu_count,  static_cast<uint32_t>(9));

    // Non-matching state: StateConflict and the callback must NOT have run.
    core::Expected<MetadataUpdateResult, StoreError> bad = f.store->UpdateIfState(
        m.id, States1(SandboxState::Paused),
        [](SandboxMetadata* md) { md->resources.cpu_count = 77; });
    MT_EXPECT_TRUE(!bad.ok());
    MT_EXPECT_TRUE(bad.error().kind == StoreError::Kind::StateConflict);
    MT_EXPECT_TRUE(bad.error().actual_state == SandboxState::Running);
    MT_EXPECT_EQ(f.store->Get(m.id).value()->resources.cpu_count,
                 static_cast<uint32_t>(9));
}

MT_TEST(update_if_state_on_missing_sandbox_is_not_found) {
    Fixture f;
    core::Expected<MetadataUpdateResult, StoreError> r = f.store->UpdateIfState(
        core::SandboxId::Fresh(), States1(SandboxState::Running),
        [](SandboxMetadata*) {});
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().kind == StoreError::Kind::SandboxNotFound);
}

// ============================================================
// in_memory.rs :: wait_while_in_states_returns_immediately_if_already_stable
// ============================================================
MT_TEST(wait_while_in_states_returns_immediately_if_already_stable) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    // Already out of the transitional set, so this must not block.
    core::Expected<core::Optional<SandboxMetadata>, StoreError> r =
        f.store->WaitWhileInStates(m.id, States1(SandboxState::Creating), 1000);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().has_value());
    MT_EXPECT_TRUE(r.value()->state == SandboxState::Running);
}

// ============================================================
// in_memory.rs :: wait_while_in_states_wakes_on_transition
// ============================================================
MT_TEST(wait_while_in_states_wakes_on_transition) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    m.state = SandboxState::Creating;
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    std::thread mover([&f, &m]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        f.store->UpdateStateIfState(m.id, SandboxState::Running,
                                    States1(SandboxState::Creating));
    });

    core::Expected<core::Optional<SandboxMetadata>, StoreError> r =
        f.store->WaitWhileInStates(m.id, States1(SandboxState::Creating), 5000);
    mover.join();

    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().has_value());
    MT_EXPECT_TRUE(r.value()->state == SandboxState::Running);
}

// ============================================================
// in_memory.rs :: wait_while_in_states_returns_none_on_remove
// ============================================================
MT_TEST(wait_while_in_states_returns_none_on_remove) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    m.state = SandboxState::Creating;
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    std::thread remover([&f, &m]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        f.store->Remove(m.id);
    });

    core::Expected<core::Optional<SandboxMetadata>, StoreError> r =
        f.store->WaitWhileInStates(m.id, States1(SandboxState::Creating), 5000);
    remover.join();

    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(!r.value().has_value());
}

// ============================================================
// in_memory.rs :: wait_while_in_states_returns_none_for_unknown_sandbox
// ============================================================
MT_TEST(wait_while_in_states_returns_none_for_unknown_sandbox) {
    Fixture f;
    core::Expected<core::Optional<SandboxMetadata>, StoreError> r =
        f.store->WaitWhileInStates(core::SandboxId::Fresh(),
                                   States1(SandboxState::Creating), 1000);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(!r.value().has_value());
}

// ---- wait_for_transition maps the timeout onto InvalidSandboxState ----
MT_TEST(wait_for_transition_times_out_as_invalid_state) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    m.state = SandboxState::Creating;
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    // Nothing ever moves the sandbox, so the store's wait elapses. Verified
    // through the store directly to keep the 60s orchestrator ceiling out of
    // the test's runtime.
    core::Expected<core::Optional<SandboxMetadata>, StoreError> r =
        f.store->WaitWhileInStates(m.id, States1(SandboxState::Creating), 100);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().kind == StoreError::Kind::Backend);
}

MT_TEST(wait_for_transition_removed_sandbox_is_not_found) {
    Fixture f;
    core::Expected<SandboxMetadata, OrchestratorError> r =
        f.orch->WaitForTransition(core::SandboxId::Fresh(), SandboxState::Creating);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().kind == OrchestratorErrorKind::SandboxNotFound);
}

// ---- maybe_update_running_timeout ----
MT_TEST(maybe_update_running_timeout_applies_while_running) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    core::Expected<SandboxMetadata, OrchestratorError> r =
        f.orch->MaybeUpdateRunningTimeout(m.id, NewTimeout::Set(30 * 1000));
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().timeout_ms.has_value());
    MT_EXPECT_EQ(*r.value().timeout_ms, static_cast<int64_t>(30 * 1000));
}

MT_TEST(maybe_update_running_timeout_rejects_non_running) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    m.state = SandboxState::Paused;
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    core::Expected<SandboxMetadata, OrchestratorError> r =
        f.orch->MaybeUpdateRunningTimeout(m.id, NewTimeout::Set(30 * 1000));
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().kind == OrchestratorErrorKind::InvalidSandboxState);
    // Rust carries the actual conflicting state on the error.
    MT_EXPECT_TRUE(r.error().state == SandboxState::Paused);
}

// ---- keep_alive_for ----
MT_TEST(keep_alive_for_missing_sandbox_is_not_found) {
    Fixture f;
    core::Expected<core::Optional<SandboxMetadata>, OrchestratorError> r =
        f.orch->KeepAliveFor(core::SandboxId::Fresh(),
                             core::Optional<int64_t>(1000), true);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().kind == OrchestratorErrorKind::SandboxNotFound);
}

MT_TEST(keep_alive_for_rejects_non_running_sandbox) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    m.state = SandboxState::Paused;
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    core::Expected<core::Optional<SandboxMetadata>, OrchestratorError> r =
        f.orch->KeepAliveFor(m.id, core::Optional<int64_t>(1000), true);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().kind == OrchestratorErrorKind::InvalidSandboxState);
}

MT_TEST(keep_alive_for_extends_deadline) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    core::Expected<core::Optional<SandboxMetadata>, OrchestratorError> r =
        f.orch->KeepAliveFor(m.id, core::Optional<int64_t>(60 * 1000), true);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().has_value());
    MT_EXPECT_TRUE(r.value()->expires_at_ms.has_value());
    MT_EXPECT_TRUE(*r.value()->expires_at_ms > NowMs());
}

MT_TEST(keep_alive_for_unset_timeout_uses_default) {
    Fixture f;
    f.orch->SetDefaultSandboxTimeoutMs(45 * 1000);
    SandboxMetadata m = Running(1, 128);
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    // Rust: `timeout.unwrap_or(self.default_sandbox_timeout)`.
    core::Expected<core::Optional<SandboxMetadata>, OrchestratorError> r =
        f.orch->KeepAliveFor(m.id, core::Optional<int64_t>(core::nullopt), true);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value()->timeout_ms.has_value());
    MT_EXPECT_EQ(*r.value()->timeout_ms, static_cast<int64_t>(45 * 1000));
}

MT_TEST(keep_alive_for_without_allow_shorter_keeps_longer_deadline) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    // Establish a long deadline first.
    MT_EXPECT_TRUE(f.orch->KeepAliveFor(
        m.id, core::Optional<int64_t>(600 * 1000), true).ok());
    const int64_t long_deadline = *f.store->Get(m.id).value()->expires_at_ms;

    // A shorter request with allow_shorter = false must be skipped entirely.
    core::Expected<core::Optional<SandboxMetadata>, OrchestratorError> r =
        f.orch->KeepAliveFor(m.id, core::Optional<int64_t>(5 * 1000), false);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(*r.value()->expires_at_ms, long_deadline);

    // With allow_shorter = true the shorter deadline is applied.
    core::Expected<core::Optional<SandboxMetadata>, OrchestratorError> forced =
        f.orch->KeepAliveFor(m.id, core::Optional<int64_t>(5 * 1000), true);
    MT_EXPECT_TRUE(forced.ok());
    MT_EXPECT_TRUE(*forced.value()->expires_at_ms < long_deadline);
}

MT_TEST(keep_alive_for_is_rejected_after_shutdown) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    MT_EXPECT_TRUE(f.store->Add(m).ok());
    MT_EXPECT_TRUE(f.orch->Shutdown().ok());

    // Rust `ensure_accepting_lifecycle_operations` gates this.
    core::Expected<core::Optional<SandboxMetadata>, OrchestratorError> r =
        f.orch->KeepAliveFor(m.id, core::Optional<int64_t>(1000), true);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().kind == OrchestratorErrorKind::ShuttingDown);
}

// ---- claim_expired_running_sandbox ----
MT_TEST(claim_expired_running_sandbox_claims_expired) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    m.created_at_ms = 1000;
    m.SetTimeout(core::Optional<int64_t>(500));  // expires at 1500
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    core::Expected<bool, OrchestratorError> r =
        f.orch->ClaimExpiredRunningSandbox(m.id, 2000, SandboxState::Killing);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value());
    MT_EXPECT_TRUE(f.store->Get(m.id).value()->state == SandboxState::Killing);
}

MT_TEST(claim_expired_running_sandbox_skips_refreshed_expiry) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    m.created_at_ms = 1000;
    m.SetTimeout(core::Optional<int64_t>(5000));  // expires at 6000
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    // Not expired at the cutoff, so the claim is declined and state is intact.
    core::Expected<bool, OrchestratorError> r =
        f.orch->ClaimExpiredRunningSandbox(m.id, 2000, SandboxState::Killing);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(!r.value());
    MT_EXPECT_TRUE(f.store->Get(m.id).value()->state == SandboxState::Running);
}

MT_TEST(claim_expired_running_sandbox_skips_non_running) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    m.state = SandboxState::Pausing;
    m.created_at_ms = 1000;
    m.SetTimeout(core::Optional<int64_t>(500));
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    // Rust folds the StateConflict into Ok(false) rather than an error.
    core::Expected<bool, OrchestratorError> r =
        f.orch->ClaimExpiredRunningSandbox(m.id, 2000, SandboxState::Killing);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(!r.value());
}

MT_TEST(claim_expired_running_sandbox_skips_missing) {
    Fixture f;
    core::Expected<bool, OrchestratorError> r = f.orch->ClaimExpiredRunningSandbox(
        core::SandboxId::Fresh(), 2000, SandboxState::Killing);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(!r.value());
}

// ---- evict_expired_sandboxes ----
MT_TEST(evict_expired_sandboxes_claims_by_timeout_action) {
    Fixture f;
    // Delete action -> Killing.
    SandboxMetadata to_delete = Running(1, 128);
    to_delete.created_at_ms   = 1000;
    to_delete.timeout_action  = SandboxTimeoutAction::Delete;
    to_delete.SetTimeout(core::Optional<int64_t>(100));

    // Pause action -> Pausing.
    SandboxMetadata to_pause = Running(2, 256);
    to_pause.created_at_ms   = 1000;
    to_pause.timeout_action  = SandboxTimeoutAction::Pause;
    to_pause.SetTimeout(core::Optional<int64_t>(100));

    // Not expired -> untouched.
    SandboxMetadata fresh = Running(1, 64);
    fresh.created_at_ms   = NowMs();
    fresh.SetTimeout(core::Optional<int64_t>(10 * 60 * 1000));

    MT_EXPECT_TRUE(f.store->Add(to_delete).ok());
    MT_EXPECT_TRUE(f.store->Add(to_pause).ok());
    MT_EXPECT_TRUE(f.store->Add(fresh).ok());

    core::Expected<std::vector<core::SandboxId>, OrchestratorError> r =
        f.orch->EvictExpiredSandboxes();
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(r.value().size(), static_cast<std::size_t>(2));

    MT_EXPECT_TRUE(f.store->Get(to_delete.id).value()->state == SandboxState::Killing);
    MT_EXPECT_TRUE(f.store->Get(to_pause.id).value()->state  == SandboxState::Pausing);
    MT_EXPECT_TRUE(f.store->Get(fresh.id).value()->state     == SandboxState::Running);
}

MT_TEST(evict_expired_sandboxes_skips_while_shutting_down) {
    Fixture f;
    SandboxMetadata m = Running(1, 128);
    m.created_at_ms = 1000;
    m.SetTimeout(core::Optional<int64_t>(100));
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    MT_EXPECT_TRUE(f.orch->Shutdown().ok());

    core::Expected<std::vector<core::SandboxId>, OrchestratorError> r =
        f.orch->EvictExpiredSandboxes();
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(r.value().size(), static_cast<std::size_t>(0));
    MT_EXPECT_TRUE(f.store->Get(m.id).value()->state == SandboxState::Running);
}

// ---- deletion progress ----
MT_TEST(deletion_progress_defaults_to_capture) {
    Fixture f;
    core::SandboxId id = core::SandboxId::Fresh();
    // Rust `entry(id).or_default()` — DeleteProgress::Capture is #[default].
    MT_EXPECT_TRUE(f.orch->DeletionProgress(id) == DeleteProgress::Capture());

    // Rust's Release variant carries `capture_failed`, which must round-trip.
    f.orch->SetDeletionProgress(id, DeleteProgress::Release(true));
    MT_EXPECT_TRUE(f.orch->DeletionProgress(id) == DeleteProgress::Release(true));
    MT_EXPECT_TRUE(f.orch->DeletionProgress(id) != DeleteProgress::Release(false));
}

// ---- persistence: DisabledSandboxPersister ----
MT_TEST(disabled_persister_is_inert) {
    DisabledSandboxPersister p;
    core::SandboxId id = core::SandboxId::Fresh();

    MT_EXPECT_TRUE(p.LoadAll().ok());
    MT_EXPECT_EQ(p.LoadAll().value().size(), static_cast<std::size_t>(0));

    // Rust returns Ok(None): persistence disabled.
    core::Expected<core::Optional<std::string>, SandboxPersistenceError> root =
        p.AllocateArtifactRoot(id);
    MT_EXPECT_TRUE(root.ok());
    MT_EXPECT_TRUE(!root.value().has_value());

    MT_EXPECT_TRUE(p.MarkResuming(id).ok());
    MT_EXPECT_TRUE(p.RollbackResuming(id).ok());
    MT_EXPECT_TRUE(p.DeleteRecord(id).ok());
    MT_EXPECT_TRUE(p.DeleteRecordAndArtifacts(id).ok());
}

MT_TEST(persistence_error_messages_match_rust_format) {
    MT_EXPECT_EQ(
        SandboxPersistenceError::Io("read", "/tmp/x", "ENOENT").Message(),
        std::string("failed to read /tmp/x: ENOENT"));
    MT_EXPECT_EQ(
        SandboxPersistenceError::InvalidRecord("bad json", "").Message(),
        std::string("invalid sandbox record: bad json"));
    MT_EXPECT_EQ(
        SandboxPersistenceError::RuntimeState("missing mem file").Message(),
        std::string("invalid paused sandbox runtime state: missing mem file"));
    MT_EXPECT_EQ(
        SandboxPersistenceError::Store("put", "disk full").Message(),
        std::string("paused sandbox store operation failed: put: disk full"));
}

MT_MAIN
