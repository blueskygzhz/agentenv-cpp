// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/sandbox_metrics.rs — the sweep and the two query
// paths, driven over a scripted `EnvdApi`.
//
// This is the half that was previously blocked: `SandboxBackend::Envd()` did
// not exist, so there was nothing to call `metrics()` on. The cache itself is
// asserted in test_sandbox_metrics.cc; what is asserted here is the sweep's
// decisions -- which sandboxes it samples, what it does when a guest is
// unreachable or a handle is contended, and the two re-checks it makes *after*
// the guest call because that call happens without the runtime lock.
#include "microtest.h"
#include "orchestrator_fakes.h"

#include <string>
#include <vector>

using namespace agentenv;                // NOLINT
using namespace agentenv::orchestrator;  // NOLINT
using namespace agentenv::testing;       // NOLINT

namespace {

/// A running sandbox with a published proxy route, which is the state the
/// sweep requires: no route means nothing is reachable yet, so there is no
/// incarnation to attribute a sample to.
core::SandboxId Running(Fixture* f, uint64_t generation) {
    const core::SandboxId id = core::SandboxId::Fresh();
    SandboxMetadata metadata;
    metadata.id = id;
    metadata.state = SandboxState::Running;
    MT_EXPECT_TRUE(f->store->Add(metadata).ok());

    ProxyTarget target("10.0.0.1");
    for (uint64_t v = 0; v < generation; ++v) {
        // `Upsert` is what bumps the version upstream, so the generation is
        // reached the way a resume would reach it rather than being poked in.
        f->orch->UpsertProxyRoute(id, target);
    }
    f->orch->RegisterSandboxHandle(
        id, SandboxHandlePtr(new SandboxHandle(
                std::unique_ptr<sandbox::SandboxBackend>(new FakeSandbox(&f->script)))));
    return id;
}

/// Every state, so a test can move a sandbox without having to name the one it
/// is currently in. `UpdateStateIfState` has no unconditional form.
std::vector<SandboxState> AnyState() {
    std::vector<SandboxState> all;
    all.push_back(SandboxState::Creating);
    all.push_back(SandboxState::Running);
    all.push_back(SandboxState::Pausing);
    all.push_back(SandboxState::Paused);
    all.push_back(SandboxState::Resuming);
    all.push_back(SandboxState::Killing);
    all.push_back(SandboxState::Snapshotting);
    return all;
}

core::Optional<core::SandboxId> NoTarget() { return core::Optional<core::SandboxId>(); }
core::Optional<core::SandboxId> Target(const core::SandboxId& id) {
    return core::Optional<core::SandboxId>(id);
}
core::Optional<int64_t> NoBound() { return core::Optional<int64_t>(); }

std::vector<core::SandboxId> Ids(const core::SandboxId& id) {
    return std::vector<core::SandboxId>(1, id);
}

}  // namespace

// ---- what the sweep samples -------------------------------------------------

MT_TEST(a_sweep_samples_a_running_sandbox_and_stores_it_under_its_generation) {
    Fixture f;
    f.script.envd.reset(new FakeEnvd());
    f.script.envd->mem_used = 4096;
    const core::SandboxId id = Running(&f, 1);

    f.orch->CollectSandboxMetrics(NoTarget());
    MT_EXPECT_EQ(f.script.envd->calls, 1);

    const OrchestratorResult<std::vector<sandbox::SandboxMetric> > history =
        f.orch->SandboxMetricsHistory(id, NoBound(), NoBound());
    MT_EXPECT_TRUE(history.ok());
    MT_EXPECT_EQ(history.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(history.value()[0].mem_used == 4096);

    // And it is reachable as "current" under the route's own version.
    const OrchestratorResult<std::map<std::string, sandbox::SandboxMetric> > latest =
        f.orch->LatestSandboxMetrics(Ids(id));
    MT_EXPECT_TRUE(latest.ok());
    MT_EXPECT_EQ(latest.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(latest.value().begin()->second.mem_used == 4096);
}

MT_TEST(a_backend_with_no_guest_agent_is_skipped_not_failed) {
    // `envd` is null until a guest agent is connected. A sweep that treated
    // that as an error would abort and sample nothing for the rest of the node.
    Fixture f;
    const core::SandboxId id = Running(&f, 1);
    MT_EXPECT_TRUE(!f.script.envd);

    f.orch->CollectSandboxMetrics(NoTarget());
    MT_EXPECT_TRUE(f.orch->SandboxMetricsHistory(id, NoBound(), NoBound()).value().empty());
}

MT_TEST(an_unreachable_guest_is_skipped_and_the_sweep_continues) {
    Fixture f;
    f.script.envd.reset(new FakeEnvd());
    f.script.envd->fail = true;
    const core::SandboxId id = Running(&f, 1);

    f.orch->CollectSandboxMetrics(NoTarget());
    MT_EXPECT_EQ(f.script.envd->calls, 1);
    MT_EXPECT_TRUE(f.orch->SandboxMetricsHistory(id, NoBound(), NoBound()).value().empty());

    // The next sweep tries again: a failure is not sticky, since a guest that
    // was briefly busy must not be written off for the process's lifetime.
    f.script.envd->fail = false;
    f.orch->CollectSandboxMetrics(NoTarget());
    MT_EXPECT_EQ(f.script.envd->calls, 2);
    MT_EXPECT_EQ(f.orch->SandboxMetricsHistory(id, NoBound(), NoBound()).value().size(),
                 static_cast<std::size_t>(1));
}

MT_TEST(an_invalidated_handle_refuses_rather_than_reaching_a_reused_address) {
    // The handle outlives the lock by design, so this is the case that
    // matters: a copy taken before teardown must not sample whatever VM now
    // holds that IP.
    Fixture f;
    f.script.envd.reset(new FakeEnvd());
    const core::SandboxId id = Running(&f, 1);

    f.script.envd->live = false;
    f.orch->CollectSandboxMetrics(NoTarget());
    MT_EXPECT_EQ(f.script.envd->calls, 1);
    MT_EXPECT_TRUE(f.orch->SandboxMetricsHistory(id, NoBound(), NoBound()).value().empty());
}

MT_TEST(a_sandbox_that_is_not_running_is_never_sampled) {
    Fixture f;
    f.script.envd.reset(new FakeEnvd());
    const core::SandboxId id = Running(&f, 1);

    const SandboxState others[] = {SandboxState::Paused, SandboxState::Pausing,
                                   SandboxState::Killing, SandboxState::Creating};
    for (std::size_t i = 0; i < sizeof(others) / sizeof(others[0]); ++i) {
        MT_EXPECT_TRUE(f.store->UpdateStateIfState(id, others[i], AnyState()).ok());
        const int before = f.script.envd->calls;
        f.orch->CollectSandboxMetrics(NoTarget());
        // Not even asked: sampling a paused guest would hit an address whose
        // VM is gone, and every such call costs the sweep its timeout.
        MT_EXPECT_EQ(f.script.envd->calls, before);
    }
}

MT_TEST(a_sandbox_with_no_proxy_route_is_skipped) {
    // No route means no published incarnation, so a sample could not be
    // attributed to one -- and `Latest` would have no generation to match.
    Fixture f;
    f.script.envd.reset(new FakeEnvd());
    const core::SandboxId id = core::SandboxId::Fresh();
    SandboxMetadata metadata;
    metadata.id = id;
    metadata.state = SandboxState::Running;
    MT_EXPECT_TRUE(f.store->Add(metadata).ok());
    f.orch->RegisterSandboxHandle(
        id, SandboxHandlePtr(new SandboxHandle(
                std::unique_ptr<sandbox::SandboxBackend>(new FakeSandbox(&f.script)))));

    f.orch->CollectSandboxMetrics(NoTarget());
    MT_EXPECT_EQ(f.script.envd->calls, 0);
}

MT_TEST(a_sandbox_with_no_live_handle_is_skipped) {
    // The record says Running but the handle is gone -- a restored sandbox
    // whose runtime was not re-attached. There is nothing to ask.
    Fixture f;
    f.script.envd.reset(new FakeEnvd());
    const core::SandboxId id = Running(&f, 1);
    SandboxHandlePtr detached;
    core::Optional<ProxyRoute> route;
    f.orch->DetachSandboxHandleAndRoute(id, &detached, &route);

    f.orch->CollectSandboxMetrics(NoTarget());
    MT_EXPECT_EQ(f.script.envd->calls, 0);
}

MT_TEST(a_contended_handle_is_skipped_rather_than_waited_for) {
    // `TryLock`, never `Lock`: a lifecycle operation holds this across a VMM
    // call, and a sweep queueing behind one would both miss its interval and
    // delay the operation it queued behind.
    Fixture f;
    f.script.envd.reset(new FakeEnvd());
    const core::SandboxId id = Running(&f, 1);

    const SandboxHandlePtr handle = f.orch->SandboxHandleFor(id);
    MT_EXPECT_TRUE(handle);
    {
        const SandboxHandle::Guard held = handle->Lock();
        f.orch->CollectSandboxMetrics(NoTarget());
        // Skipped, and -- crucially -- the call returned rather than blocking.
        MT_EXPECT_EQ(f.script.envd->calls, 0);
    }
    // Released, so the next sweep samples it.
    f.orch->CollectSandboxMetrics(NoTarget());
    MT_EXPECT_EQ(f.script.envd->calls, 1);
}

// ---- the targeted sweep -----------------------------------------------------

MT_TEST(a_targeted_sweep_samples_only_its_target) {
    Fixture f;
    BackendScript other;
    other.envd.reset(new FakeEnvd());
    f.script.envd.reset(new FakeEnvd());

    const core::SandboxId first = Running(&f, 1);
    // A second running sandbox over its own script, so the two guests are
    // distinguishable.
    const core::SandboxId second = core::SandboxId::Fresh();
    SandboxMetadata metadata;
    metadata.id = second;
    metadata.state = SandboxState::Running;
    MT_EXPECT_TRUE(f.store->Add(metadata).ok());
    ProxyTarget target("10.0.0.2");
    f.orch->UpsertProxyRoute(second, target);
    f.orch->RegisterSandboxHandle(
        second, SandboxHandlePtr(new SandboxHandle(
                    std::unique_ptr<sandbox::SandboxBackend>(new FakeSandbox(&other)))));

    f.orch->CollectSandboxMetrics(Target(second));
    MT_EXPECT_EQ(f.script.envd->calls, 0);
    MT_EXPECT_EQ(other.envd->calls, 1);

    // An untargeted sweep takes both.
    f.orch->CollectSandboxMetrics(NoTarget());
    MT_EXPECT_EQ(f.script.envd->calls, 1);
    MT_EXPECT_EQ(other.envd->calls, 2);
    (void)first;
}

MT_TEST(a_targeted_sweep_skips_a_generation_already_sampled) {
    // This is what stops a burst of create/resume/fork events multiplying
    // guest calls: the periodic sweep already has a sample for this
    // incarnation, so the event-driven one has nothing to add.
    Fixture f;
    f.script.envd.reset(new FakeEnvd());
    const core::SandboxId id = Running(&f, 1);

    f.orch->CollectSandboxMetrics(NoTarget());
    MT_EXPECT_EQ(f.script.envd->calls, 1);

    f.orch->CollectSandboxMetrics(Target(id));
    f.orch->CollectSandboxMetrics(Target(id));
    MT_EXPECT_EQ(f.script.envd->calls, 1);

    // A new incarnation is a new generation, so the event *does* sample.
    ProxyTarget target("10.0.0.9");
    f.orch->UpsertProxyRoute(id, target);
    f.orch->CollectSandboxMetrics(Target(id));
    MT_EXPECT_EQ(f.script.envd->calls, 2);

    // An untargeted sweep has no such guard -- it is the periodic one, and
    // skipping would mean never taking another sample.
    f.orch->CollectSandboxMetrics(NoTarget());
    MT_EXPECT_EQ(f.script.envd->calls, 3);
}

// ---- the query paths --------------------------------------------------------

MT_TEST(history_distinguishes_an_unknown_sandbox_from_one_with_no_samples) {
    Fixture f;
    const core::SandboxId known = Running(&f, 1);

    // Known but unsampled: an empty history, not an error.
    const OrchestratorResult<std::vector<sandbox::SandboxMetric> > empty =
        f.orch->SandboxMetricsHistory(known, NoBound(), NoBound());
    MT_EXPECT_TRUE(empty.ok());
    MT_EXPECT_TRUE(empty.value().empty());

    // Unknown: an error, because a client paging a graph needs to tell those
    // two answers apart.
    const OrchestratorResult<std::vector<sandbox::SandboxMetric> > missing =
        f.orch->SandboxMetricsHistory(core::SandboxId::Fresh(), NoBound(), NoBound());
    MT_EXPECT_TRUE(!missing.ok());
    MT_EXPECT_TRUE(missing.error().kind == OrchestratorErrorKind::SandboxNotFound);
}

MT_TEST(latest_omits_a_sandbox_whose_incarnation_has_moved_on) {
    Fixture f;
    f.script.envd.reset(new FakeEnvd());
    const core::SandboxId id = Running(&f, 1);
    f.orch->CollectSandboxMetrics(NoTarget());
    MT_EXPECT_EQ(f.orch->LatestSandboxMetrics(Ids(id)).value().size(),
                 static_cast<std::size_t>(1));

    // A resume republishes the route and bumps its version. The stored sample
    // belongs to the previous incarnation, so it must not be served as
    // current -- stale numbers with nothing marking them stale.
    ProxyTarget target("10.0.0.3");
    f.orch->UpsertProxyRoute(id, target);
    MT_EXPECT_TRUE(f.orch->LatestSandboxMetrics(Ids(id)).value().empty());
    // But the history keeps it: that is about what happened, not what is
    // current.
    MT_EXPECT_EQ(f.orch->SandboxMetricsHistory(id, NoBound(), NoBound()).value().size(),
                 static_cast<std::size_t>(1));
}

MT_TEST(latest_omits_an_id_that_is_not_running_or_has_no_route) {
    Fixture f;
    f.script.envd.reset(new FakeEnvd());
    const core::SandboxId id = Running(&f, 1);
    f.orch->CollectSandboxMetrics(NoTarget());

    MT_EXPECT_TRUE(f.store->UpdateStateIfState(id, SandboxState::Paused, AnyState()).ok());
    // Omitted rather than served from the cache: a paused sandbox's last
    // reading is not its current one.
    MT_EXPECT_TRUE(f.orch->LatestSandboxMetrics(Ids(id)).value().empty());

    // An unknown id is simply absent from the map, not an error: the endpoint
    // takes a list and one bad entry must not fail the rest.
    const OrchestratorResult<std::map<std::string, sandbox::SandboxMetric> > unknown =
        f.orch->LatestSandboxMetrics(Ids(core::SandboxId::Fresh()));
    MT_EXPECT_TRUE(unknown.ok());
    MT_EXPECT_TRUE(unknown.value().empty());
}

MT_TEST(retention_drops_samples_from_both_query_paths) {
    Fixture f;
    f.script.envd.reset(new FakeEnvd());
    const core::SandboxId id = Running(&f, 1);
    f.orch->CollectSandboxMetrics(NoTarget());
    MT_EXPECT_EQ(f.orch->SandboxMetricsHistory(id, NoBound(), NoBound()).value().size(),
                 static_cast<std::size_t>(1));

    // Zero retention expires everything already held, and both query paths
    // prune on read -- so a response never contains an over-age sample even if
    // no sweep has run since.
    f.orch->SetMetricsRetentionSecs(0);
    MT_EXPECT_TRUE(f.orch->SandboxMetricsHistory(id, NoBound(), NoBound()).value().empty());
    MT_EXPECT_TRUE(f.orch->LatestSandboxMetrics(Ids(id)).value().empty());
}

MT_TEST(history_filters_on_the_guest_timestamp) {
    Fixture f;
    f.script.envd.reset(new FakeEnvd());
    const core::SandboxId id = Running(&f, 1);

    // Three sweeps with the guest reporting three different seconds. The
    // generation guard only applies to targeted sweeps, so the periodic one
    // keeps appending.
    for (int64_t n = 0; n < 3; ++n) {
        f.script.envd->timestamp_secs = 1700000000LL + n;
        f.script.envd->mem_used = n;
        f.orch->CollectSandboxMetrics(NoTarget());
    }
    MT_EXPECT_EQ(f.orch->SandboxMetricsHistory(id, NoBound(), NoBound()).value().size(),
                 static_cast<std::size_t>(3));

    const std::vector<sandbox::SandboxMetric> window =
        f.orch->SandboxMetricsHistory(id, core::Optional<int64_t>(1700000001LL),
                                      core::Optional<int64_t>(1700000001LL))
            .value();
    MT_EXPECT_EQ(window.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(window[0].mem_used == 1);

    // `Latest` is the most recently *collected*, which is the last sweep.
    MT_EXPECT_TRUE(f.orch->LatestSandboxMetrics(Ids(id)).value().begin()->second.mem_used == 2);
}

MT_MAIN
