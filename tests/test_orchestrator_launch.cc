// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/service.rs :: launch_sandbox + its rollback helpers
//       (build_sandbox / cleanup_failed_launch / rollback_failed_launch_metadata
//        / detach_launch_runtime_if_current / upsert_proxy_route_if_current_handle)
#include "microtest.h"

#include <memory>
#include <string>
#include <vector>

#include "orchestrator_fakes.h"

using namespace agentenv;
using namespace agentenv::orchestrator;
using agentenv::testing::Fixture;
using agentenv::testing::FakeSandbox;
using agentenv::testing::FakePausedState;
using agentenv::testing::CreatingMeta;
using agentenv::testing::FreshPlan;

// ============================================================
// resources_with_runtime_info
// ============================================================
MT_TEST(resources_with_runtime_info_overrides_disk_from_rootfs_size) {
    sandbox::SandboxResources res;
    res.disk_size_mib = 1024;

    sandbox::SandboxRuntimeInfo none;
    // Rust: an unset rootfs_virtual_size leaves resources untouched.
    MT_EXPECT_EQ(ResourcesWithRuntimeInfo(res, none).disk_size_mib,
                 static_cast<uint32_t>(1024));

    sandbox::SandboxRuntimeInfo info;
    // 3 MiB + 1 byte must round UP to 4 MiB (Rust `bytes_to_mib_ceil`).
    info.rootfs_virtual_size =
        core::Optional<uint64_t>(3ull * 1024 * 1024 + 1);
    MT_EXPECT_EQ(ResourcesWithRuntimeInfo(res, info).disk_size_mib,
                 static_cast<uint32_t>(4));
}

// ============================================================
// launch_sandbox :: the happy path
// ============================================================
MT_TEST(launch_sandbox_persists_running_and_publishes_route) {
    Fixture f;
    SandboxMetadata meta = CreatingMeta();

    OrchestratorResult<SandboxMetadata> launched =
        f.orch->LaunchSandbox(FreshPlan(meta));
    MT_EXPECT_TRUE(launched.ok());

    // Rust promotes the transitional record to Running and applies the timeout.
    MT_EXPECT_TRUE(launched.value().state == SandboxState::Running);
    MT_EXPECT_TRUE(launched.value().timeout_ms.has_value());
    MT_EXPECT_EQ(*launched.value().timeout_ms, static_cast<int64_t>(60 * 1000));

    // Fresh source dispatches onto `build`, not `build_from_snapshot`.
    MT_EXPECT_EQ(f.factory->build_calls, 1);
    MT_EXPECT_EQ(f.factory->build_from_snapshot_calls, 0);
    MT_EXPECT_EQ(f.factory->last_image_config_path, std::string("/img/alpine.json"));

    // start_nowait then wait_for_ready, and no rollback stop.
    MT_EXPECT_EQ(f.script.start_calls, 1);
    MT_EXPECT_EQ(f.script.wait_calls, 1);
    MT_EXPECT_EQ(f.script.stop_calls, 0);

    // The record is in the store and the proxy route resolves.
    MT_EXPECT_TRUE(f.orch->GetSandbox(meta.id).value().has_value());
    MT_EXPECT_TRUE(f.orch->ProxyLookupFor(meta.id).value().kind ==
                   ProxyLookupKind::Ready);
    MT_EXPECT_EQ(f.orch->ProxyLookupFor(meta.id).value().target.ip,
                 std::string("10.0.0.7"));

    // The live handle is registered.
    MT_EXPECT_TRUE(f.orch->SandboxHandleFor(meta.id) != nullptr);
}

// ============================================================
// launch_sandbox :: build failure persists nothing
// ============================================================
MT_TEST(launch_sandbox_build_failure_leaves_no_metadata) {
    Fixture f;
    f.factory->fail_build = true;
    SandboxMetadata meta = CreatingMeta();

    OrchestratorResult<SandboxMetadata> launched =
        f.orch->LaunchSandbox(FreshPlan(meta));
    MT_EXPECT_TRUE(!launched.ok());
    // Rust maps a factory error onto SandboxOperationFailed{Build}.
    MT_EXPECT_TRUE(launched.error().kind ==
                   OrchestratorErrorKind::SandboxOperationFailed);
    MT_EXPECT_TRUE(launched.error().operation == SandboxOperation::Build);

    // Nothing was ever started, registered or persisted.
    MT_EXPECT_EQ(f.script.start_calls, 0);
    MT_EXPECT_TRUE(!f.orch->GetSandbox(meta.id).value().has_value());
    MT_EXPECT_TRUE(f.orch->SandboxHandleFor(meta.id) == nullptr);
}

// ============================================================
// launch_sandbox :: start failure stops the sandbox and rolls back
// ============================================================
MT_TEST(launch_sandbox_start_failure_stops_and_rolls_back) {
    Fixture f;
    f.script.fail_start_nowait = true;
    SandboxMetadata meta = CreatingMeta();

    OrchestratorResult<SandboxMetadata> launched =
        f.orch->LaunchSandbox(FreshPlan(meta));
    MT_EXPECT_TRUE(!launched.ok());
    MT_EXPECT_TRUE(launched.error().operation == SandboxOperation::Start);

    // Rust stops the sandbox it could not start.
    MT_EXPECT_EQ(f.script.stop_calls, 1);
    // wait_for_ready is never reached.
    MT_EXPECT_EQ(f.script.wait_calls, 0);
    MT_EXPECT_TRUE(!f.orch->GetSandbox(meta.id).value().has_value());
    MT_EXPECT_TRUE(f.orch->SandboxHandleFor(meta.id) == nullptr);
}

// ============================================================
// launch_sandbox :: readiness failure detaches handle, route and metadata
// ============================================================
MT_TEST(launch_sandbox_wait_ready_failure_cleans_up_registered_state) {
    Fixture f;
    f.script.fail_wait_ready = true;
    SandboxMetadata meta = CreatingMeta();

    OrchestratorResult<SandboxMetadata> launched =
        f.orch->LaunchSandbox(FreshPlan(meta));
    MT_EXPECT_TRUE(!launched.ok());
    MT_EXPECT_TRUE(launched.error().operation == SandboxOperation::WaitReady);

    // cleanup_failed_launch stops the sandbox ...
    MT_EXPECT_EQ(f.script.stop_calls, 1);
    // ... and rolls the Create record back by removing it.
    MT_EXPECT_TRUE(!f.orch->GetSandbox(meta.id).value().has_value());
    MT_EXPECT_TRUE(f.orch->SandboxHandleFor(meta.id) == nullptr);
    MT_EXPECT_TRUE(f.orch->ProxyLookupFor(meta.id).value().kind ==
                   ProxyLookupKind::NotFound);
}

// ============================================================
// launch_sandbox :: a sandbox without an interaction IP is rolled back
// ============================================================
MT_TEST(launch_sandbox_missing_interaction_ip_rolls_back_running_launch) {
    Fixture f;
    f.script.no_interaction_ip = true;
    SandboxMetadata meta = CreatingMeta();

    OrchestratorResult<SandboxMetadata> launched =
        f.orch->LaunchSandbox(FreshPlan(meta));
    MT_EXPECT_TRUE(!launched.ok());
    // Rust `proxy_target_from_sandbox` yields InternalError.
    MT_EXPECT_TRUE(launched.error().kind == OrchestratorErrorKind::InternalError);

    // The launch had already reached Running, so cleanup removes the record.
    MT_EXPECT_EQ(f.script.stop_calls, 1);
    MT_EXPECT_TRUE(!f.orch->GetSandbox(meta.id).value().has_value());
    MT_EXPECT_TRUE(f.orch->SandboxHandleFor(meta.id) == nullptr);
}

// ============================================================
// launch_sandbox :: shutdown is refused up front
// ============================================================
MT_TEST(launch_sandbox_is_refused_once_shutting_down) {
    Fixture f;
    MT_EXPECT_TRUE(f.orch->Shutdown().ok());

    SandboxMetadata meta = CreatingMeta();
    OrchestratorResult<SandboxMetadata> launched =
        f.orch->LaunchSandbox(FreshPlan(meta));
    MT_EXPECT_TRUE(!launched.ok());
    MT_EXPECT_TRUE(launched.error().kind == OrchestratorErrorKind::ShuttingDown);
    // Rust checks before building, so the factory is never touched.
    MT_EXPECT_EQ(f.factory->build_calls, 0);
}

// ============================================================
// build_sandbox :: source dispatch
// ============================================================
MT_TEST(launch_sandbox_snapshot_source_dispatches_to_build_from_snapshot) {
    Fixture f;
    SandboxMetadata meta = CreatingMeta();
    sandbox::SandboxLaunchConfig config;
    config.sandbox_id = meta.id;

    OrchestratorResult<SandboxMetadata> launched = f.orch->LaunchSandbox(
        LaunchPlan::ForCreateFromSnapshot(meta.id, "snap-42", config, meta,
                                          NewTimeout::None()));
    MT_EXPECT_TRUE(launched.ok());
    MT_EXPECT_EQ(f.factory->build_from_snapshot_calls, 1);
    MT_EXPECT_EQ(f.factory->build_calls, 0);
    MT_EXPECT_EQ(f.factory->last_snapshot_id, std::string("snap-42"));
}

MT_TEST(launch_sandbox_resume_plan_dispatches_to_build_from_paused_state) {
    Fixture f;
    // A Resume plan carries no metadata, so the record must already exist.
    SandboxMetadata meta = CreatingMeta();
    meta.state = SandboxState::Resuming;
    MT_EXPECT_TRUE(f.store->Add(meta).ok());

    std::shared_ptr<sandbox::PausedSandboxState> paused(new FakePausedState());
    OrchestratorResult<SandboxMetadata> launched = f.orch->LaunchSandbox(
        LaunchPlan::ForResume(meta.id, paused, NewTimeout::UseExisting(),
                              meta.resources,
                              core::Optional<sandbox::EnvdAccessToken>()));
    MT_EXPECT_TRUE(launched.ok());
    MT_EXPECT_EQ(f.factory->build_from_paused_calls, 1);
    // Rust promotes Resuming -> Running.
    MT_EXPECT_TRUE(launched.value().state == SandboxState::Running);
}

MT_TEST(launch_sandbox_resume_without_paused_state_is_an_internal_error) {
    Fixture f;
    SandboxMetadata meta = CreatingMeta();
    meta.state = SandboxState::Resuming;
    MT_EXPECT_TRUE(f.store->Add(meta).ok());

    OrchestratorResult<SandboxMetadata> launched = f.orch->LaunchSandbox(
        LaunchPlan::ForResume(meta.id,
                              std::shared_ptr<sandbox::PausedSandboxState>(),
                              NewTimeout::UseExisting(), meta.resources,
                              core::Optional<sandbox::EnvdAccessToken>()));
    MT_EXPECT_TRUE(!launched.ok());
    MT_EXPECT_TRUE(launched.error().kind == OrchestratorErrorKind::InternalError);

    // Rust's Resume rollback restores Paused rather than removing the record.
    core::Optional<SandboxMetadata> stored = f.orch->GetSandbox(meta.id).value();
    MT_EXPECT_TRUE(stored.has_value());
    MT_EXPECT_TRUE(stored->state == SandboxState::Paused);
}

// ============================================================
// upsert_proxy_route_if_current_handle :: handle identity
// ============================================================
MT_TEST(upsert_proxy_route_if_current_handle_rejects_a_stale_handle) {
    Fixture f;
    SandboxMetadata meta = CreatingMeta();
    MT_EXPECT_TRUE(f.orch->LaunchSandbox(FreshPlan(meta)).ok());

    SandboxHandlePtr live = f.orch->SandboxHandleFor(meta.id);
    MT_EXPECT_TRUE(live != nullptr);
    // The registered handle still wins.
    MT_EXPECT_TRUE(f.orch->UpsertProxyRouteIfCurrentHandle(
        meta.id, live, ProxyTarget("10.0.0.8")));
    MT_EXPECT_EQ(f.orch->ProxyLookupFor(meta.id).value().target.ip,
                 std::string("10.0.0.8"));

    // A different handle for the same id is stale and must be ignored.
    SandboxHandlePtr stale(new SandboxHandle(
        std::unique_ptr<sandbox::SandboxBackend>(new FakeSandbox(&f.script))));
    MT_EXPECT_TRUE(!f.orch->UpsertProxyRouteIfCurrentHandle(
        meta.id, stale, ProxyTarget("10.0.0.9")));
    MT_EXPECT_EQ(f.orch->ProxyLookupFor(meta.id).value().target.ip,
                 std::string("10.0.0.8"));
}

// ============================================================
// detach_sandbox_handle_and_route
// ============================================================
MT_TEST(detach_sandbox_handle_and_route_returns_and_clears_both) {
    Fixture f;
    SandboxMetadata meta = CreatingMeta();
    MT_EXPECT_TRUE(f.orch->LaunchSandbox(FreshPlan(meta)).ok());

    SandboxHandlePtr           handle;
    core::Optional<ProxyRoute> route;
    f.orch->DetachSandboxHandleAndRoute(meta.id, &handle, &route);

    MT_EXPECT_TRUE(handle != nullptr);
    MT_EXPECT_TRUE(route.has_value());
    MT_EXPECT_EQ(route->target.ip, std::string("10.0.0.7"));

    // Both the registry and the route table are now empty for this id.
    MT_EXPECT_TRUE(f.orch->SandboxHandleFor(meta.id) == nullptr);
    MT_EXPECT_TRUE(f.orch->ProxyLookupFor(meta.id).value().kind ==
                   ProxyLookupKind::RouteMissing);

    // Rust `restore_proxy_route` puts the captured route back verbatim.
    f.orch->RestoreProxyRoute(meta.id, route);
    MT_EXPECT_TRUE(f.orch->ProxyLookupFor(meta.id).value().kind ==
                   ProxyLookupKind::Ready);

    // Detaching an unknown sandbox yields nothing rather than failing.
    SandboxHandlePtr           missing_handle;
    core::Optional<ProxyRoute> missing_route;
    f.orch->DetachSandboxHandleAndRoute(core::SandboxId::Fresh(),
                                        &missing_handle, &missing_route);
    MT_EXPECT_TRUE(missing_handle == nullptr);
    MT_EXPECT_TRUE(!missing_route.has_value());
}

MT_MAIN
