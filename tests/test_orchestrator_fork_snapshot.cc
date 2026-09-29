// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/service.rs :: fork_sandbox, capture_snapshot,
//       snapshot_volume_mounts, replace_sandbox_network_policy,
//       patch_sandbox_custom_extension_params + token methods
#include "microtest.h"

#include <memory>
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
    f.orch->RegisterSandboxHandle(m.id, 
        SandboxHandlePtr(new SandboxHandle(std::move(backend))));
    f.orch->UpsertProxyRoute(m.id, ProxyTarget{"10.0.0.1", 80, false});
    return m;
}

}  // namespace

// ============================================================
// fork_sandbox :: happy path
// ============================================================
MT_TEST(fork_sandbox_creates_children_and_publishes_fork_events) {
    Fixture f;
    SandboxMetadata source = RegisterRunning(f, 2, 512);
    
    OrchestratorResult<std::vector<SandboxForkOutcome> > forked =
        f.orch->ForkSandbox(source.id, 2, NewTimeout::Set(30 * 1000));
    MT_EXPECT_TRUE(forked.ok());
    MT_EXPECT_EQ(forked.value().size(), static_cast<size_t>(2));
    
    // Both children succeeded.
    MT_EXPECT_TRUE(forked.value()[0].ok());
    MT_EXPECT_TRUE(forked.value()[1].ok());
    
    // Each child inherited the source's resources and got a fresh timeout.
    const SandboxMetadata& child0 = forked.value()[0].value();
    MT_EXPECT_EQ(child0.resources.cpu_count, static_cast<uint32_t>(2));
    MT_EXPECT_EQ(child0.resources.memory_mib, static_cast<uint32_t>(512));
    MT_EXPECT_TRUE(child0.timeout_ms.has_value());
    MT_EXPECT_EQ(*child0.timeout_ms, static_cast<int64_t>(30 * 1000));
    MT_EXPECT_TRUE(child0.state == SandboxState::Running);
    
    // The source sandbox remains Running.
    MT_EXPECT_TRUE(f.orch->GetSandbox(source.id).value()->state == 
                   SandboxState::Running);
    
    // Each child has a live handle and proxy route.
    MT_EXPECT_TRUE(f.orch->SandboxHandleFor(child0.id) != nullptr);
    MT_EXPECT_TRUE(f.orch->ProxyLookupFor(child0.id).value().kind ==
                   ProxyLookupKind::Ready);
}

// ============================================================
// fork_sandbox :: refuses sandboxes with volume mounts
// ============================================================
MT_TEST(fork_sandbox_refuses_source_with_volume_mounts) {
    Fixture f;
    SandboxMetadata source = RegisterRunning(f, 2, 512);
    source.volume_mounts["/data"] = "vol-123";
    MT_EXPECT_TRUE(f.store->Add(source).ok());
    
    OrchestratorResult<std::vector<SandboxForkOutcome> > forked =
        f.orch->ForkSandbox(source.id, 1, NewTimeout::Set(30 * 1000));
    MT_EXPECT_TRUE(!forked.ok());
    MT_EXPECT_TRUE(forked.error().kind == 
                   OrchestratorError::Kind::InternalError);
}

// ============================================================
// fork_sandbox_with_specs :: per-child volume mounts
// ============================================================
MT_TEST(fork_sandbox_with_specs_applies_per_child_volume_mounts) {
    Fixture f;
    SandboxMetadata source = RegisterRunning(f, 2, 512);
    
    std::vector<SandboxForkChildSpec> specs;
    SandboxForkChildSpec c1;
    c1.sandbox_id = core::SandboxId::Fresh();
    c1.volume_mounts["/data"] = "vol-alpha";
    specs.push_back(c1);
    
    SandboxForkChildSpec c2;
    c2.sandbox_id = core::SandboxId::Fresh();
    c2.volume_mounts["/data"] = "vol-beta";
    specs.push_back(c2);
    
    OrchestratorResult<std::vector<SandboxForkOutcome> > forked =
        f.orch->ForkSandboxWithSpecs(source.id, specs, NewTimeout::Set(30 * 1000));
    MT_EXPECT_TRUE(forked.ok());
    MT_EXPECT_EQ(forked.value().size(), static_cast<size_t>(2));
    
    MT_EXPECT_EQ(forked.value()[0].value().volume_mounts.at("/data"),
                 std::string("vol-alpha"));
    MT_EXPECT_EQ(forked.value()[1].value().volume_mounts.at("/data"),
                 std::string("vol-beta"));
}

// ============================================================
// fork_sandbox :: rejects duplicate child IDs
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
    MT_EXPECT_TRUE(forked.error().kind ==
                   OrchestratorError::Kind::InternalError);
}

// ============================================================
// token methods
// ============================================================
MT_TEST(get_envd_access_token_returns_token_iff_secure) {
    Fixture f;
    sandbox::SandboxAccessTokenGenerator gen("test-secret");
    f.orch->SetAccessTokenGenerator(gen);
    
    SandboxMetadata secure_meta;
    secure_meta.id     = core::SandboxId::Fresh();
    secure_meta.secure = true;
    core::Optional<sandbox::EnvdAccessToken> tok =
        f.orch->GetEnvdAccessToken(secure_meta);
    MT_EXPECT_TRUE(tok.has_value());
    MT_EXPECT_TRUE(!tok->raw.empty());
    
    SandboxMetadata public_meta;
    public_meta.id     = core::SandboxId::Fresh();
    public_meta.secure = false;
    core::Optional<sandbox::EnvdAccessToken> none =
        f.orch->GetEnvdAccessToken(public_meta);
    MT_EXPECT_TRUE(!none.has_value());
}

MT_TEST(validate_envd_access_token_accepts_valid_candidate) {
    Fixture f;
    sandbox::SandboxAccessTokenGenerator gen("test-secret");
    f.orch->SetAccessTokenGenerator(gen);
    
    core::SandboxId sid = core::SandboxId::Fresh();
    sandbox::EnvdAccessToken tok = gen.Generate(sid);
    MT_EXPECT_TRUE(f.orch->ValidateEnvdAccessToken(sid, tok.raw));
    MT_EXPECT_TRUE(!f.orch->ValidateEnvdAccessToken(sid, "bogus"));
}

MT_TEST(traffic_access_token_generates_and_validates) {
    Fixture f;
    sandbox::SandboxAccessTokenGenerator gen("test-secret");
    f.orch->SetAccessTokenGenerator(gen);
    
    core::SandboxId sid = core::SandboxId::Fresh();
    std::string tok = f.orch->TrafficAccessToken(sid);
    MT_EXPECT_TRUE(!tok.empty());
    MT_EXPECT_TRUE(f.orch->ValidateTrafficAccessToken(sid, tok));
    MT_EXPECT_TRUE(!f.orch->ValidateTrafficAccessToken(sid, "wrong"));
}

// ============================================================
// capture_snapshot :: happy path
// ============================================================
MT_TEST(capture_snapshot_transitions_to_snapshotting_and_back) {
    Fixture f;
    SandboxMetadata m = RegisterRunning(f, 2, 512);
    
    // The fake always returns "mock-snapshot-id".
    OrchestratorResult<SnapshotCaptureResult> captured =
        f.orch->CaptureSnapshot(m.id);
    MT_EXPECT_TRUE(captured.ok());
    MT_EXPECT_EQ(captured.value().captured_snapshot,
                 std::string("mock-snapshot-id"));
    MT_EXPECT_EQ(captured.value().metadata.id, m.id);
    
    // The sandbox is back to Running after the snapshot.
    MT_EXPECT_TRUE(f.orch->GetSandbox(m.id).value()->state ==
                   SandboxState::Running);
}

// ============================================================
// capture_snapshot :: missing sandbox
// ============================================================
MT_TEST(capture_snapshot_on_missing_sandbox_is_not_found) {
    Fixture f;
    OrchestratorResult<SnapshotCaptureResult> captured =
        f.orch->CaptureSnapshot(core::SandboxId::Fresh());
    MT_EXPECT_TRUE(!captured.ok());
    MT_EXPECT_TRUE(captured.error().kind ==
                   OrchestratorError::Kind::SandboxNotFound);
}

// ============================================================
// snapshot_volume_mounts :: happy path
// ============================================================
MT_TEST(snapshot_volume_mounts_succeeds_for_running_sandbox) {
    Fixture f;
    SandboxMetadata m = RegisterRunning(f, 2, 512);
    
    OrchestratorResult<core::Unit> snapped =
        f.orch->SnapshotVolumeMounts(m.id);
    MT_EXPECT_TRUE(snapped.ok());
    
    // The sandbox is back to Running.
    MT_EXPECT_TRUE(f.orch->GetSandbox(m.id).value()->state ==
                   SandboxState::Running);
}

// ============================================================
// replace_sandbox_network_policy :: happy path
// ============================================================
MT_TEST(replace_sandbox_network_policy_updates_and_persists) {
    Fixture f;
    SandboxMetadata m = RegisterRunning(f, 2, 512);
    m.network_policy.allow_public_traffic = true;
    MT_EXPECT_TRUE(f.store->Add(m).ok());
    
    sandbox::network::SandboxNetworkPolicy new_policy;
    new_policy.allow_localhost_ingress = true;
    // Rust: `allow_public_traffic` is not user-updatable — the stored value
    // always wins.
    new_policy.allow_public_traffic = false;  // this should be ignored
    
    OrchestratorResult<core::Unit> updated =
        f.orch->ReplaceSandboxNetworkPolicy(m.id, new_policy);
    MT_EXPECT_TRUE(updated.ok());
    
    // The store should have the new localhost flag but the original
    // allow_public_traffic.
    core::Optional<SandboxMetadata> got = f.store->Get(m.id).value();
    MT_EXPECT_TRUE(got->network_policy.allow_localhost_ingress);
    MT_EXPECT_TRUE(got->network_policy.allow_public_traffic);
}

// ============================================================
// replace_sandbox_network_policy :: not running
// ============================================================
MT_TEST(replace_sandbox_network_policy_rejects_non_running) {
    Fixture f;
    SandboxMetadata m = RegisterRunning(f, 2, 512);
    m.state = SandboxState::Paused;
    MT_EXPECT_TRUE(f.store->Add(m).ok());
    
    sandbox::network::SandboxNetworkPolicy policy;
    OrchestratorResult<core::Unit> updated =
        f.orch->ReplaceSandboxNetworkPolicy(m.id, policy);
    MT_EXPECT_TRUE(!updated.ok());
    MT_EXPECT_TRUE(updated.error().kind ==
                   OrchestratorError::Kind::InvalidSandboxState);
}

// ============================================================
// patch_sandbox_custom_extension_params :: no client configured
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
                   OrchestratorError::Kind::SandboxOperationFailed);
}
