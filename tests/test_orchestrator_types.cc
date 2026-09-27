// SPDX-License-Identifier: MIT
// Ports the semantics asserted by src/orchestrator/types.rs + mod.rs:
//   - SandboxState Display (lowercase names)
//   - SandboxLifecycleEvent equality
//   - SandboxOperation debug names
//   - OrchestratorError `#[error(...)]` message formatting
#include "microtest.h"
#include "agentenv/orchestrator/types.h"

using namespace agentenv::orchestrator;
using agentenv::core::SandboxId;
using agentenv::core::Uuid;
using agentenv::sandbox::SandboxResources;

MT_TEST(sandbox_state_display) {
    MT_EXPECT_TRUE(std::string(SandboxStateName(SandboxState::Creating))     == "creating");
    MT_EXPECT_TRUE(std::string(SandboxStateName(SandboxState::Resuming))     == "resuming");
    MT_EXPECT_TRUE(std::string(SandboxStateName(SandboxState::Running))      == "running");
    MT_EXPECT_TRUE(std::string(SandboxStateName(SandboxState::Snapshotting)) == "snapshotting");
    MT_EXPECT_TRUE(std::string(SandboxStateName(SandboxState::Forking))      == "forking");
    MT_EXPECT_TRUE(std::string(SandboxStateName(SandboxState::Pausing))      == "pausing");
    MT_EXPECT_TRUE(std::string(SandboxStateName(SandboxState::Paused))       == "paused");
    MT_EXPECT_TRUE(std::string(SandboxStateName(SandboxState::Killing))      == "killing");
}

MT_TEST(sandbox_resources_default_and_eq) {
    SandboxResources a;  // Rust Default: 1 / 128 / 1024
    MT_EXPECT_EQ(static_cast<int>(a.cpu_count), 1);
    MT_EXPECT_EQ(static_cast<int>(a.memory_mib), 128);
    MT_EXPECT_EQ(static_cast<int>(a.disk_size_mib), 1024);
    SandboxResources b;
    MT_EXPECT_TRUE(a == b);
    b.cpu_count = 4;
    MT_EXPECT_TRUE(a != b);
}

MT_TEST(sandbox_lifecycle_event_eq) {
    SandboxId id = SandboxId::Fresh();
    SandboxResources res;
    SandboxLifecycleEvent e1{SandboxLifecycleEventType::Create, id, res};
    SandboxLifecycleEvent e2{SandboxLifecycleEventType::Create, id, res};
    MT_EXPECT_TRUE(e1 == e2);

    SandboxLifecycleEvent e3{SandboxLifecycleEventType::Delete, id, res};
    MT_EXPECT_TRUE(e1 != e3);
}

MT_TEST(sandbox_operation_debug_names) {
    MT_EXPECT_TRUE(std::string(SandboxOperationName(SandboxOperation::Build))     == "Build");
    MT_EXPECT_TRUE(std::string(SandboxOperationName(SandboxOperation::WaitReady)) == "WaitReady");
    MT_EXPECT_TRUE(std::string(SandboxOperationName(
        SandboxOperation::PatchCustomExtensionParams)) == "PatchCustomExtensionParams");
    MT_EXPECT_TRUE(std::string(SandboxOperationName(SandboxOperation::Stop))      == "Stop");
}

// Reproduces the Rust thiserror `#[error(...)]` strings verbatim.
MT_TEST(orchestrator_error_messages) {
    // Build a deterministic id for message assertions.
    Uuid u;
    MT_EXPECT_TRUE(Uuid::Parse("00000000-0000-7000-8000-000000000001", &u));
    SandboxId id(u);
    std::string ids = id.ToString();

    MT_EXPECT_TRUE(OrchestratorError::ConfigLoadFailed("x").Message()
                   == "failed to load sandbox config");
    MT_EXPECT_TRUE(OrchestratorError::ShuttingDown().Message()
                   == "orchestrator is shutting down");
    MT_EXPECT_TRUE(OrchestratorError::SandboxNotFound(id).Message()
                   == "sandbox " + ids + " not found");
    MT_EXPECT_TRUE(OrchestratorError::InvalidSandboxState(id, SandboxState::Running).Message()
                   == "sandbox " + ids + " is in invalid state Running");
    MT_EXPECT_TRUE(OrchestratorError::SandboxOperationFailed(id, SandboxOperation::Start, "boom").Message()
                   == "sandbox " + ids + " operation Start failed: boom");
    MT_EXPECT_TRUE(OrchestratorError::SandboxOperationConflict(id, SandboxOperation::Pause).Message()
                   == "sandbox " + ids + " operation Pause conflicted with another operation");
    MT_EXPECT_TRUE(OrchestratorError::StoreOperationFailed("db down").Message()
                   == "store operation failed: db down");
    MT_EXPECT_TRUE(OrchestratorError::SandboxPersistenceFailed("disk").Message()
                   == "sandbox persistence failed: disk");
    MT_EXPECT_TRUE(OrchestratorError::InvalidTimeout(id, "-1s").Message()
                   == "invalid timeout for " + ids + ": -1s");
    MT_EXPECT_TRUE(OrchestratorError::InternalError("oops").Message()
                   == "internal error: oops");
}

MT_MAIN
