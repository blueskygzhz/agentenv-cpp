// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/service.rs — Orchestrator query/metrics/proxy surface.
#include "microtest.h"

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

SandboxMetadata meta(SandboxState state, uint32_t cpu, uint32_t memory_mib) {
    SandboxMetadata m;
    m.id    = core::SandboxId::Fresh();
    m.state = state;
    m.resources.cpu_count  = cpu;
    m.resources.memory_mib = memory_mib;
    return m;
}

}  // namespace

// ---- get_sandbox: Ok(None) for a missing sandbox, not an error ----
MT_TEST(get_sandbox_missing_is_ok_none) {
    Fixture f;
    core::SandboxId absent = core::SandboxId::Fresh();

    core::Expected<core::Optional<SandboxMetadata>, OrchestratorError> r =
        f.orch->GetSandbox(absent);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(!r.value().has_value());
}

MT_TEST(get_sandbox_returns_stored_record) {
    Fixture f;
    SandboxMetadata m = meta(SandboxState::Running, 2, 256);
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    core::Expected<core::Optional<SandboxMetadata>, OrchestratorError> r =
        f.orch->GetSandbox(m.id);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().has_value());
    MT_EXPECT_TRUE(r.value()->state == SandboxState::Running);
}

// ---- list_sandboxes_filtered drops template_builder records ----
MT_TEST(list_sandboxes_excludes_template_builders) {
    Fixture f;
    SandboxMetadata normal  = meta(SandboxState::Running, 1, 128);
    SandboxMetadata builder = meta(SandboxState::Running, 1, 128);
    builder.template_builder = true;

    MT_EXPECT_TRUE(f.store->Add(normal).ok());
    MT_EXPECT_TRUE(f.store->Add(builder).ok());

    // The store itself still holds both records.
    MT_EXPECT_EQ(f.store->List().value().size(), static_cast<std::size_t>(2));

    // Rust `list_sandboxes_filtered` filters out `template_builder`.
    core::Expected<std::vector<SandboxMetadata>, OrchestratorError> r =
        f.orch->ListSandboxes();
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(r.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(r.value()[0].id == normal.id);
}

// ---- list_sandbox_ids unions store ids with in-flight template builds ----
MT_TEST(list_sandbox_ids_includes_template_builds) {
    Fixture f;
    SandboxMetadata stored = meta(SandboxState::Running, 1, 128);
    MT_EXPECT_TRUE(f.store->Add(stored).ok());

    core::SandboxId building = core::SandboxId::Fresh();
    f.orch->RegisterTemplateBuild(building);

    core::Expected<std::vector<core::SandboxId>, OrchestratorError> r =
        f.orch->ListSandboxIds();
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(r.value().size(), static_cast<std::size_t>(2));

    f.orch->UnregisterTemplateBuild(building);
    MT_EXPECT_EQ(f.orch->ListSandboxIds().value().size(),
                 static_cast<std::size_t>(1));
}

MT_TEST(list_sandbox_ids_dedupes_overlapping_build_id) {
    Fixture f;
    SandboxMetadata stored = meta(SandboxState::Creating, 1, 128);
    MT_EXPECT_TRUE(f.store->Add(stored).ok());
    // Registering the same id as a template build must not double-count it
    // (Rust collects into a HashSet).
    f.orch->RegisterTemplateBuild(stored.id);

    MT_EXPECT_EQ(f.orch->ListSandboxIds().value().size(),
                 static_cast<std::size_t>(1));
}

// ---- metrics_snapshot joins live metadata with the creation counters ----
MT_TEST(metrics_snapshot_derives_from_live_metadata) {
    Fixture f;
    MT_EXPECT_TRUE(f.store->Add(meta(SandboxState::Running,  2, 256)).ok());
    MT_EXPECT_TRUE(f.store->Add(meta(SandboxState::Creating, 4, 512)).ok());
    MT_EXPECT_TRUE(f.store->Add(meta(SandboxState::Paused,   8, 1024)).ok());

    f.orch->counters().RecordCreateSuccess(5);
    f.orch->counters().RecordCreateFail(2);

    core::Expected<OrchestratorMetrics, OrchestratorError> r =
        f.orch->MetricsSnapshot();
    MT_EXPECT_TRUE(r.ok());
    const OrchestratorMetrics& m = r.value();

    MT_EXPECT_EQ(m.running_sandbox_count,  static_cast<uint32_t>(1));
    MT_EXPECT_EQ(m.starting_sandbox_count, static_cast<uint32_t>(1));
    // Paused releases its VM-side resources, so only 2 + 4 stay allocated.
    MT_EXPECT_EQ(m.allocated_cpu,          static_cast<uint32_t>(6));
    MT_EXPECT_EQ(m.paused_sandbox_count,   static_cast<uint32_t>(1));
    MT_EXPECT_EQ(m.paused_allocated_cpu,   static_cast<uint32_t>(8));
    // Counters come from OrchestratorCounters, not from the store.
    MT_EXPECT_EQ(m.create_successes, static_cast<uint64_t>(5));
    MT_EXPECT_EQ(m.create_fails,     static_cast<uint64_t>(2));
}

// ---- publish_sandbox_event fans out to subscribers ----
MT_TEST(publish_sandbox_event_notifies_subscribers) {
    Fixture f;
    std::vector<SandboxLifecycleEvent> seen;
    f.orch->SubscribeSandboxEvents(
        [&seen](const SandboxLifecycleEvent& e) { seen.push_back(e); });

    core::SandboxId id = core::SandboxId::Fresh();
    sandbox::SandboxResources res;
    res.cpu_count  = 2;
    res.memory_mib = 256;
    f.orch->PublishSandboxEvent(SandboxLifecycleEventType::Create, id, res);

    MT_EXPECT_EQ(seen.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(seen[0].event_type == SandboxLifecycleEventType::Create);
    MT_EXPECT_TRUE(seen[0].sandbox_id == id);
    MT_EXPECT_EQ(seen[0].resources.cpu_count, static_cast<uint32_t>(2));
}

// ---- proxy_lookup_for maps state onto the lookup variants ----
MT_TEST(proxy_lookup_missing_sandbox_is_not_found) {
    Fixture f;
    core::Expected<ProxyLookupResult, OrchestratorError> r =
        f.orch->ProxyLookupFor(core::SandboxId::Fresh());
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().kind == ProxyLookupKind::NotFound);
}

MT_TEST(proxy_lookup_paused_reports_auto_resume) {
    Fixture f;
    SandboxMetadata m = meta(SandboxState::Paused, 1, 128);
    m.auto_resume = true;
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    core::Expected<ProxyLookupResult, OrchestratorError> r =
        f.orch->ProxyLookupFor(m.id);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().kind == ProxyLookupKind::Paused);
    MT_EXPECT_TRUE(r.value().auto_resume);
}

MT_TEST(proxy_lookup_non_running_is_unavailable) {
    Fixture f;
    SandboxMetadata m = meta(SandboxState::Killing, 1, 128);
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    core::Expected<ProxyLookupResult, OrchestratorError> r =
        f.orch->ProxyLookupFor(m.id);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().kind == ProxyLookupKind::Unavailable);
    MT_EXPECT_TRUE(r.value().unavailable_state == SandboxState::Killing);
}

MT_TEST(proxy_lookup_running_without_route_is_route_missing) {
    Fixture f;
    SandboxMetadata m = meta(SandboxState::Running, 1, 128);
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    core::Expected<ProxyLookupResult, OrchestratorError> r =
        f.orch->ProxyLookupFor(m.id);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().kind == ProxyLookupKind::RouteMissing);
}

MT_TEST(proxy_lookup_running_with_route_is_ready) {
    Fixture f;
    SandboxMetadata m = meta(SandboxState::Running, 1, 128);
    MT_EXPECT_TRUE(f.store->Add(m).ok());
    f.orch->UpsertProxyRoute(m.id, ProxyTarget("10.0.0.7"));

    core::Expected<ProxyLookupResult, OrchestratorError> r =
        f.orch->ProxyLookupFor(m.id);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().kind == ProxyLookupKind::Ready);
    MT_EXPECT_EQ(r.value().target.ip, std::string("10.0.0.7"));
}

MT_TEST(restore_proxy_route_with_none_removes_entry) {
    Fixture f;
    SandboxMetadata m = meta(SandboxState::Running, 1, 128);
    MT_EXPECT_TRUE(f.store->Add(m).ok());
    f.orch->UpsertProxyRoute(m.id, ProxyTarget("10.0.0.7"));
    MT_EXPECT_TRUE(f.orch->ProxyLookupFor(m.id).value().kind ==
                   ProxyLookupKind::Ready);

    f.orch->RestoreProxyRoute(m.id, core::Optional<ProxyRoute>());
    MT_EXPECT_TRUE(f.orch->ProxyLookupFor(m.id).value().kind ==
                   ProxyLookupKind::RouteMissing);
}

// ---- FailedLaunchStage rollback semantics ----
MT_TEST(failed_launch_stage_rollback_expected_state) {
    SandboxMetadata m;
    m.id = core::SandboxId::Fresh();
    LaunchPlan create_plan =
        LaunchPlan::ForCreateFresh(m.id, "alpine", m, NewTimeout::None());

    sandbox::SandboxResources res;
    LaunchPlan resume_plan =
        LaunchPlan::ForResume(m.id, NewTimeout::UseExisting(), res);

    // Registered: nothing was persisted, so there is nothing to roll back to.
    MT_EXPECT_TRUE(!RollbackExpectedState(FailedLaunchStage::Registered,
                                          create_plan).has_value());

    // TransitionalPersisted rolls back to the plan's transitional state.
    core::Optional<SandboxState> t1 = RollbackExpectedState(
        FailedLaunchStage::TransitionalPersisted, create_plan);
    MT_EXPECT_TRUE(t1.has_value());
    MT_EXPECT_TRUE(*t1 == SandboxState::Creating);

    core::Optional<SandboxState> t2 = RollbackExpectedState(
        FailedLaunchStage::TransitionalPersisted, resume_plan);
    MT_EXPECT_TRUE(t2.has_value());
    MT_EXPECT_TRUE(*t2 == SandboxState::Resuming);

    // RunningPersisted always rolls back to Running.
    core::Optional<SandboxState> t3 = RollbackExpectedState(
        FailedLaunchStage::RunningPersisted, resume_plan);
    MT_EXPECT_TRUE(t3.has_value());
    MT_EXPECT_TRUE(*t3 == SandboxState::Running);
}

MT_TEST(failed_launch_stage_detaches_route_only_when_running) {
    MT_EXPECT_TRUE(!ShouldDetachProxyRoute(FailedLaunchStage::Registered));
    MT_EXPECT_TRUE(!ShouldDetachProxyRoute(FailedLaunchStage::TransitionalPersisted));
    MT_EXPECT_TRUE(ShouldDetachProxyRoute(FailedLaunchStage::RunningPersisted));
}

// ---- shutdown is idempotent and memoises its outcome ----
MT_TEST(shutdown_is_idempotent) {
    Fixture f;
    MT_EXPECT_TRUE(!f.orch->IsShuttingDown());

    MT_EXPECT_TRUE(f.orch->Shutdown().ok());
    MT_EXPECT_TRUE(f.orch->IsShuttingDown());

    // Replaying the memoised outcome yields the same result.
    MT_EXPECT_TRUE(f.orch->Shutdown().ok());
}

MT_MAIN
