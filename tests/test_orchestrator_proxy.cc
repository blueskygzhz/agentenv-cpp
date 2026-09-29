// SPDX-License-Identifier: MIT
// Mirrors the #[cfg(test)] block in src/orchestrator/proxy.rs,
// plus LaunchPlan coverage for src/orchestrator/launch_plan.rs.
#include "microtest.h"

#include "agentenv/orchestrator/launch_plan.h"
#include "agentenv/orchestrator/proxy.h"

using namespace agentenv;
using namespace agentenv::orchestrator;

// ============================================================
// proxy.rs :: proxy_table_only_exposes_inserted_routes
// ============================================================
MT_TEST(proxy_table_only_exposes_inserted_routes) {
    core::SandboxId sandbox_id = core::SandboxId::Fresh();
    ProxyTarget target("127.0.0.1");
    ProxyRouteTable table;

    table.Upsert(sandbox_id, target, 1);
    core::Optional<ProxyTarget> got = table.ProxyTargetOf(sandbox_id);
    MT_EXPECT_TRUE(got.has_value());
    MT_EXPECT_TRUE(*got == target);

    // Re-upsert bumps the version but keeps the target.
    table.Upsert(sandbox_id, target, 2);
    got = table.ProxyTargetOf(sandbox_id);
    MT_EXPECT_TRUE(got.has_value());
    MT_EXPECT_TRUE(*got == target);

    core::Optional<ProxyRoute> route = table.Route(sandbox_id);
    MT_EXPECT_TRUE(route.has_value());
    MT_EXPECT_EQ(route->version, static_cast<uint64_t>(2));
}

// ============================================================
// proxy.rs :: proxy_table_remove_drops_route
// ============================================================
MT_TEST(proxy_table_remove_drops_route) {
    core::SandboxId sandbox_id = core::SandboxId::Fresh();
    ProxyRouteTable table;

    table.Upsert(sandbox_id, ProxyTarget("127.0.0.1"), 3);
    core::Optional<ProxyRoute> removed = table.Remove(sandbox_id);
    MT_EXPECT_TRUE(removed.has_value());

    MT_EXPECT_TRUE(!table.Route(sandbox_id).has_value());
    MT_EXPECT_TRUE(!table.ProxyTargetOf(sandbox_id).has_value());
}

// ---- ProxyLookupResult variant equality (Rust derive(PartialEq, Eq)) ----
MT_TEST(proxy_lookup_result_variants_compare_by_payload) {
    MT_EXPECT_TRUE(ProxyLookupResult::NotFound() == ProxyLookupResult::NotFound());
    MT_EXPECT_TRUE(ProxyLookupResult::RouteMissing() == ProxyLookupResult::RouteMissing());
    MT_EXPECT_TRUE(ProxyLookupResult::NotFound() != ProxyLookupResult::RouteMissing());

    MT_EXPECT_TRUE(ProxyLookupResult::Ready(ProxyTarget("10.0.0.1")) ==
                   ProxyLookupResult::Ready(ProxyTarget("10.0.0.1")));
    MT_EXPECT_TRUE(ProxyLookupResult::Ready(ProxyTarget("10.0.0.1")) !=
                   ProxyLookupResult::Ready(ProxyTarget("10.0.0.2")));

    MT_EXPECT_TRUE(ProxyLookupResult::Paused(true) == ProxyLookupResult::Paused(true));
    MT_EXPECT_TRUE(ProxyLookupResult::Paused(true) != ProxyLookupResult::Paused(false));

    MT_EXPECT_TRUE(ProxyLookupResult::Unavailable(SandboxState::Killing) ==
                   ProxyLookupResult::Unavailable(SandboxState::Killing));
    MT_EXPECT_TRUE(ProxyLookupResult::Unavailable(SandboxState::Killing) !=
                   ProxyLookupResult::Unavailable(SandboxState::Pausing));
}

// ============================================================
// launch_plan.rs :: transitional_state / resources / timeout
// ============================================================
MT_TEST(launch_plan_create_reports_creating_state) {
    SandboxMetadata meta;
    meta.id = core::SandboxId::Fresh();
    meta.resources.cpu_count  = 4;
    meta.resources.memory_mib = 512;

    LaunchPlan p = LaunchPlan::ForCreateFresh(
        meta.id, "docker.io/library/alpine", meta, NewTimeout::Set(30 * 1000));

    // Rust: Create => SandboxState::Creating (previously mis-mapped to Booting).
    MT_EXPECT_TRUE(p.TransitionalState() == SandboxState::Creating);
    MT_EXPECT_TRUE(p.SandboxId() == meta.id);
    // Rust `resources()` for Create reads metadata.resources.
    MT_EXPECT_EQ(p.Resources().cpu_count,  static_cast<uint32_t>(4));
    MT_EXPECT_EQ(p.Resources().memory_mib, static_cast<uint32_t>(512));
    // Rust `transitional_metadata()` is Some(..) for Create.
    MT_EXPECT_TRUE(p.TransitionalMetadata() != nullptr);
    MT_EXPECT_TRUE(p.Timeout().kind == NewTimeout::Kind::Set);
}

MT_TEST(launch_plan_resume_reports_resuming_state) {
    core::SandboxId id = core::SandboxId::Fresh();
    sandbox::SandboxResources res;
    res.cpu_count  = 2;
    res.memory_mib = 256;

    LaunchPlan p = LaunchPlan::ForResume(id, NewTimeout::UseExisting(), res);

    // Rust: Resume => SandboxState::Resuming.
    MT_EXPECT_TRUE(p.TransitionalState() == SandboxState::Resuming);
    MT_EXPECT_TRUE(p.SandboxId() == id);
    // Rust `resources()` for Resume reads plan.resources.
    MT_EXPECT_EQ(p.Resources().cpu_count,  static_cast<uint32_t>(2));
    MT_EXPECT_EQ(p.Resources().memory_mib, static_cast<uint32_t>(256));
    // Rust `transitional_metadata()` is None for Resume.
    MT_EXPECT_TRUE(p.TransitionalMetadata() == nullptr);
    MT_EXPECT_TRUE(p.Timeout().kind == NewTimeout::Kind::UseExisting);
}

MT_TEST(launch_plan_create_from_snapshot) {
    SandboxMetadata meta;
    meta.id = core::SandboxId::Fresh();
    meta.resources.cpu_count = 1;

    LaunchPlan p = LaunchPlan::ForCreateFromSnapshot(
        meta.id, "snap-abc", meta, NewTimeout::None());

    MT_EXPECT_TRUE(p.TransitionalState() == SandboxState::Creating);
    MT_EXPECT_TRUE(p.kind == LaunchPlanKind::Create);
    MT_EXPECT_TRUE(p.create->source.kind == CreateLaunchSourceKind::Snapshot);
    MT_EXPECT_EQ(p.create->source.snapshot_id, std::string("snap-abc"));
}

MT_MAIN
