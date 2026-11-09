// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/service.rs write paths +
//       src/orchestrator/store/in_memory.rs (update_if_state / wait_while_in_states)
#include "microtest.h"

#include <atomic>
#include <chrono>
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

/// A store whose first `n` sweeps fail outright.
///
/// Derived from the in-memory store rather than written against
/// `MetadataStore` directly: only one method needs to misbehave, and
/// reimplementing the interface would put a second, divergent store in the
/// tests. `InMemoryMetadataStore::ListExpired` cannot fail — it iterates a map
/// under a lock — so without this the loop's "a failed cycle must not end the
/// sweep" branch has nothing that can exercise it.
class FailingListExpiredStore : public InMemoryMetadataStore {
 public:
    explicit FailingListExpiredStore(int failures) : remaining_(failures), served_(0) {}

    core::Expected<std::vector<SandboxMetadata>, StoreError>
    ListExpired(int64_t now_ms) const override {
        if (remaining_.load() > 0) {
            remaining_.fetch_sub(1);
            served_.fetch_add(1);
            return core::make_unexpected(StoreError::Backend("list expired failed"));
        }
        return InMemoryMetadataStore::ListExpired(now_ms);
    }

    int failures_served() const { return served_.load(); }

 private:
    mutable std::atomic<int> remaining_;
    mutable std::atomic<int> served_;
};

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

    // Rust: after claiming, the orchestrator executes the timeout action.
    // With no live handle registered (sandboxes added directly to the store),
    // the delete path succeeds (no handle → skip stop, still removes record),
    // but the pause path finds no handle and returns SandboxNotFound, which the
    // evict loop treats as a failure, logs, and skips — so only the delete
    // sandbox ends up in the evicted list.
    MT_EXPECT_EQ(r.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(r.value()[0] == to_delete.id);

    // Both records are gone: delete cleaned up normally; pause removed the
    // record too because `pause_sandbox_impl` always removes a sandbox whose
    // handle is missing.
    MT_EXPECT_TRUE(!f.store->Get(to_delete.id).value().has_value());
    MT_EXPECT_TRUE(!f.store->Get(to_pause.id).value().has_value());
    MT_EXPECT_TRUE(f.store->Get(fresh.id).value()->state == SandboxState::Running);
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

// ---- start_auto_evict_task: the sweep's driver ----
//
// The sweep above was dead code: nothing in production ever called it, so no
// sandbox ever timed out. Nothing reported that either — `expires_at` was
// still stamped on every sandbox, `ListExpired` still answered, and
// `auto_evict_interval_ms` was still parsed and defaulted to 1000 — so an
// operator reading the config had every reason to believe eviction was on.

MT_TEST(the_auto_evict_task_sweeps_without_anyone_calling_it) {
    Fixture f;
    SandboxMetadata expired = Running(1, 128);
    expired.created_at_ms   = 1000;
    expired.timeout_action  = SandboxTimeoutAction::Delete;
    expired.SetTimeout(core::Optional<int64_t>(100));
    MT_EXPECT_TRUE(f.store->Add(expired).ok());

    // 1ms so the test does not wait out a production interval. The interval is
    // the only thing that differs from a real node.
    f.orch->StartAutoEvictTask(1);

    // Polled rather than slept on: a fixed sleep either flakes under load or
    // pads every run.
    bool gone = false;
    for (int i = 0; i < 2000 && !gone; ++i) {
        gone = !f.store->Get(expired.id).value().has_value();
        if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    MT_EXPECT_TRUE(gone);
    f.orch->StopAutoEvictTask();
}

MT_TEST(the_auto_evict_task_leaves_unexpired_sandboxes_alone) {
    // The sweep must be driven, not merely run: a driver that evicted
    // everything it found would be worse than none at all.
    Fixture f;
    SandboxMetadata fresh = Running(1, 64);
    fresh.created_at_ms   = NowMs();
    fresh.SetTimeout(core::Optional<int64_t>(10 * 60 * 1000));
    MT_EXPECT_TRUE(f.store->Add(fresh).ok());

    f.orch->StartAutoEvictTask(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    f.orch->StopAutoEvictTask();

    MT_EXPECT_TRUE(f.store->Get(fresh.id).value().has_value());
    MT_EXPECT_TRUE(f.store->Get(fresh.id).value()->state == SandboxState::Running);
}

MT_TEST(the_auto_evict_task_keeps_sweeping_after_a_failed_cycle) {
    // Rust logs `auto-evict task failed` and keeps the loop. Breaking out
    // instead would end eviction for the life of the process over one
    // transient store error — and silently, since the task has no caller to
    // report to.
    //
    // A *cycle* failure needs the store's `ListExpired` to fail, which the
    // only production store cannot do; hence the fake. Note what does **not**
    // work for this: a sandbox whose pause fails. The sweep logs and
    // `continue`s on a per-sandbox error and still returns `Ok`, so the cycle
    // never reports failure and the loop is never asked to survive one.
    std::shared_ptr<FailingListExpiredStore> store(new FailingListExpiredStore(3));
    std::shared_ptr<sandbox::Backend> backend(new sandbox::MockBackend());
    std::shared_ptr<Orchestrator> orch(new Orchestrator(store, backend));

    SandboxMetadata expired = Running(1, 128);
    expired.created_at_ms   = 1000;
    expired.timeout_action  = SandboxTimeoutAction::Delete;
    expired.SetTimeout(core::Optional<int64_t>(100));
    MT_EXPECT_TRUE(store->Add(expired).ok());

    orch->StartAutoEvictTask(1);
    bool gone = false;
    for (int i = 0; i < 2000 && !gone; ++i) {
        gone = !store->Get(expired.id).value().has_value();
        if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    orch->StopAutoEvictTask();

    MT_EXPECT_TRUE(gone);
    // The failures really did happen, or this would pass on a loop that never
    // met one.
    MT_EXPECT_TRUE(store->failures_served() >= 3);
}

MT_TEST(shutdown_stops_the_auto_evict_task) {
    // Rust sends on `shutdown_tx`, which the task selects on, so the sweep
    // ends with the shutdown instead of outliving it.
    //
    // Asserted on the thread, not on behaviour: `EvictExpiredSandboxes`
    // checks `IsShuttingDown` itself and returns early, so "an expired
    // sandbox stays put after shutdown" is true whether or not the task was
    // stopped. That assertion would look like it locked this and would not.
    Fixture f;
    f.orch->StartAutoEvictTask(1);
    MT_EXPECT_TRUE(f.orch->AutoEvictTaskRunning());

    MT_EXPECT_TRUE(f.orch->Shutdown().ok());
    MT_EXPECT_TRUE(!f.orch->AutoEvictTaskRunning());

    // And the sweep is genuinely inert afterwards.
    SandboxMetadata expired = Running(1, 128);
    expired.created_at_ms   = 1000;
    expired.timeout_action  = SandboxTimeoutAction::Delete;
    expired.SetTimeout(core::Optional<int64_t>(100));
    MT_EXPECT_TRUE(f.store->Add(expired).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    MT_EXPECT_TRUE(f.store->Get(expired.id).value().has_value());
}

MT_TEST(stopping_the_auto_evict_task_does_not_wait_out_the_tick) {
    // Upstream's `tokio::select!` on `shutdown_rx.changed()` is a promise that
    // a shutdown is not charged for the remainder of a tick. A plain sleep, or
    // a `wait_for` without the stop predicate, keeps that promise invisibly at
    // a 1ms test interval and breaks it by a full second at the configured
    // default — unbounded for anyone who tuned the sweep slower.
    //
    // A ten-minute interval makes the difference impossible to miss.
    Fixture f;
    f.orch->StartAutoEvictTask(10 * 60 * 1000);

    // The sweep must be *parked* before the stop, or this measures nothing:
    // a stop that lands before the thread first takes the lock is answered by
    // the predicate on entry and returns at once even without a notify.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const int64_t started_ms = NowMs();
    f.orch->StopAutoEvictTask();
    const int64_t elapsed_ms = NowMs() - started_ms;

    MT_EXPECT_TRUE(!f.orch->AutoEvictTaskRunning());
    // Generous by three orders of magnitude against the interval, so this
    // fails only on a stop that actually waited.
    MT_EXPECT_TRUE(elapsed_ms < 1000);
}

MT_TEST(stopping_the_auto_evict_task_before_it_parks_still_returns) {
    // The complement of the test above, and the reason the wait carries a
    // predicate rather than only a post-wait check.
    //
    // Stopping immediately races the thread to the lock. If the stop wins, its
    // `notify_all` reaches nobody — a lost wakeup — and a `wait_for` with no
    // predicate then parks for the whole interval even though the flag it is
    // waiting for is already set. The predicate is evaluated on entry, so the
    // thread never waits at all.
    //
    // No sleep here on purpose: the race *is* the condition under test.
    Fixture f;
    f.orch->StartAutoEvictTask(10 * 60 * 1000);

    const int64_t started_ms = NowMs();
    f.orch->StopAutoEvictTask();
    const int64_t elapsed_ms = NowMs() - started_ms;

    MT_EXPECT_TRUE(!f.orch->AutoEvictTaskRunning());
    MT_EXPECT_TRUE(elapsed_ms < 1000);
}

MT_TEST(starting_the_auto_evict_task_twice_starts_one_sweep) {
    // Upstream spawns it exactly once, from the constructor. A second thread
    // would race the first for the same claim; the CAS keeps that correct but
    // it is still two threads doing one sandbox's work, and only one of them
    // would be joined.
    Fixture f;
    f.orch->StartAutoEvictTask(1);
    f.orch->StartAutoEvictTask(1);
    // The second call must not have replaced the handle, or this join would
    // leave a thread running on a destroyed orchestrator.
    f.orch->StopAutoEvictTask();
    MT_EXPECT_TRUE(!f.orch->AutoEvictTaskRunning());
}

MT_TEST(stopping_an_unstarted_auto_evict_task_is_a_no_op) {
    // The destructor calls this unconditionally, and most orchestrators in
    // these tests never start the task.
    Fixture f;
    f.orch->StopAutoEvictTask();
    f.orch->StopAutoEvictTask();
    MT_EXPECT_TRUE(true);
}

MT_TEST(destroying_an_orchestrator_joins_its_auto_evict_task) {
    // Rust holds a `Weak<Self>` so the task cannot keep the orchestrator
    // alive and stops once the last `Arc` is gone. Here the destructor joins:
    // the thread reads the store, the handle map and the deletion locks, so a
    // thread still running after those members die is a use-after-free.
    //
    // Nothing is asserted beyond surviving — the failure mode is a crash or a
    // sanitizer report, not a wrong value.
    std::shared_ptr<InMemoryMetadataStore> store(new InMemoryMetadataStore());
    {
        std::shared_ptr<sandbox::Backend> backend(new sandbox::MockBackend());
        std::shared_ptr<Orchestrator> orch(new Orchestrator(store, backend));
        SandboxMetadata expired = Running(1, 128);
        expired.created_at_ms   = 1000;
        expired.timeout_action  = SandboxTimeoutAction::Delete;
        expired.SetTimeout(core::Optional<int64_t>(100));
        MT_EXPECT_TRUE(store->Add(expired).ok());
        orch->StartAutoEvictTask(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    MT_EXPECT_TRUE(true);
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

    MT_EXPECT_TRUE(p.LoadAll(NULL).ok());
    MT_EXPECT_EQ(p.LoadAll(NULL).value().size(), static_cast<std::size_t>(0));

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
