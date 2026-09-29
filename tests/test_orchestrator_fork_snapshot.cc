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
