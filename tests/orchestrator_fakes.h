// SPDX-License-Identifier: MIT
// Shared programmable fakes for the orchestrator write-path tests.
//
// Rust drives these paths with `MockSandbox` / `MockSandboxFactory` in
// `src/orchestrator/service/tests.rs`; this is their C++ counterpart.
#ifndef AGENTENV_TESTS_ORCHESTRATOR_FAKES_H_
#define AGENTENV_TESTS_ORCHESTRATOR_FAKES_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/orchestrator/persistence.h"
#include "agentenv/orchestrator/service.h"

namespace agentenv {
namespace testing {

/// Records what the orchestrator asked of the backend and fails on demand, so
/// each launch/delete branch can be driven deterministically.
struct BackendScript {
    // launch branches
    bool fail_start_nowait = false;
    bool fail_wait_ready   = false;
    bool no_interaction_ip = false;

    // delete branches
    bool fail_stop = false;
    /// `FreezeAndSnapshotVolumes` fails terminally (runtime is unsafe to keep).
    bool fail_freeze_terminal = false;
    /// `FreezeAndSnapshotVolumes` fails recoverably (sandbox still serving).
    bool fail_freeze_recoverable = false;
    /// `ThawVolumes` fails, which escalates a recoverable capture error.
    bool fail_thaw = false;

    // fork branches
    bool fail_fork_terminal   = false;
    bool fail_fork_recoverable = false;

    // snapshot branches
    bool succeed_snapshot = false;  // off by default (FakeSandbox returns unsupported)
    std::string snapshot_id = "fake-snapshot-id";
    bool fail_snapshot_terminal   = false;
    bool fail_snapshot_recoverable = false;  // default when succeed_snapshot==false

    // network / custom-extension branches
    bool fail_update_network = false;

    // observed
    int start_calls    = 0;
    int wait_calls     = 0;
    int stop_calls     = 0;
    int freeze_calls   = 0;
    int thaw_calls     = 0;
    int fork_calls     = 0;
    int snapshot_calls = 0;

    /// Rust `runtime_info().rootfs_virtual_size`.
    core::Optional<uint64_t> rootfs_virtual_size;
};

class FakeSandbox : public sandbox::SandboxBackend {
 public:
    explicit FakeSandbox(BackendScript* script) : script_(script) {}

    core::Expected<core::Unit, core::AnyError> Start() override {
        return StartNowait();
    }
    core::Expected<core::Unit, core::AnyError> StartNowait() override {
        ++script_->start_calls;
        if (script_->fail_start_nowait) {
            return core::make_unexpected(core::err("fake: start refused"));
        }
        return core::Unit();
    }
    core::Expected<core::Unit, core::AnyError> WaitForReady() override {
        ++script_->wait_calls;
        if (script_->fail_wait_ready) {
            return core::make_unexpected(core::err("fake: never became ready"));
        }
        return core::Unit();
    }
    sandbox::SandboxCaptureResult<std::shared_ptr<sandbox::PausedSandboxState> >
        Pause(const core::Optional<std::string>&) override {
        return core::make_unexpected(
            sandbox::SandboxCaptureError::Recoverable("fake: pause unsupported"));
    }
    core::Expected<core::Unit, core::AnyError> Resume() override {
        return core::Unit();
    }
    sandbox::SandboxCaptureResult<std::string> Snapshot() override {
        return core::make_unexpected(
            sandbox::SandboxCaptureError::Recoverable("fake: snapshot unsupported"));
    }
    sandbox::SandboxCaptureResult<core::Unit> SnapshotVolumes() override {
        return core::Unit();
    }
    sandbox::SandboxCaptureResult<core::Unit> FreezeAndSnapshotVolumes() override {
        ++script_->freeze_calls;
        if (script_->fail_freeze_terminal) {
            return core::make_unexpected(
                sandbox::SandboxCaptureError::Terminal("fake: freeze broke the runtime"));
        }
        if (script_->fail_freeze_recoverable) {
            return core::make_unexpected(
                sandbox::SandboxCaptureError::Recoverable("fake: freeze declined"));
        }
        return core::Unit();
    }
    core::Expected<core::Unit, core::AnyError> ThawVolumes() override {
        ++script_->thaw_calls;
        if (script_->fail_thaw) {
            return core::make_unexpected(core::err("fake: thaw refused"));
        }
        return core::Unit();
    }
    sandbox::SandboxCaptureResult<std::vector<sandbox::SandboxForkResult> >
        Fork(const std::vector<sandbox::SandboxForkSpec>& specs) override {
        ++script_->fork_calls;
        if (script_->fail_fork_terminal) {
            return core::make_unexpected(
                sandbox::SandboxCaptureError::Terminal("fake: fork broke the runtime"));
        }
        // Default: recoverable error. Use FakeForkableSandbox for success tests.
        return core::make_unexpected(
            sandbox::SandboxCaptureError::Recoverable("fake: fork unsupported"));
    }
    core::Expected<core::Unit, core::AnyError> Stop() override {
        ++script_->stop_calls;
        if (script_->fail_stop) {
            return core::make_unexpected(core::err("fake: stop refused"));
        }
        return core::Unit();
    }
    core::Optional<std::string> HostInteractionIp() const override {
        if (script_->no_interaction_ip) return core::Optional<std::string>();
        return core::Optional<std::string>(std::string("10.0.0.7"));
    }
    sandbox::SandboxRuntimeInfo RuntimeInfoOf() const override {
        sandbox::SandboxRuntimeInfo info;
        info.rootfs_virtual_size = script_->rootfs_virtual_size;
        return info;
    }
    sandbox::RuntimeArtifactSet StartupArtifacts() const override {
        return sandbox::RuntimeArtifactSet::Empty();
    }
    core::Expected<core::Unit, core::AnyError>
        UpdateNetworkPolicy(const core::Optional<std::string>&) override {
        return core::Unit();
    }
    void UpdateCustomExtensionParams(const core::Optional<std::string>&) override {}

 private:
    BackendScript* script_;
};

class FakeFactory : public sandbox::SandboxBackendFactory {
 public:
    explicit FakeFactory(BackendScript* script) : script_(script) {}

    bool        fail_build = false;
    std::string last_snapshot_id;
    std::string last_image_config_path;
    int         build_calls = 0;
    int         build_from_snapshot_calls = 0;
    int         build_from_paused_calls = 0;

    core::Expected<std::unique_ptr<sandbox::SandboxBackend>, core::AnyError>
        Build(const sandbox::FreshSandboxBuildSpec& spec,
              const sandbox::SandboxLaunchConfig&) override {
        ++build_calls;
        last_image_config_path = spec.image_config_path;
        return Make();
    }
    core::Expected<std::unique_ptr<sandbox::SandboxBackend>, core::AnyError>
        BuildFromSnapshot(const std::string& snapshot_id,
                          const sandbox::SandboxLaunchConfig&) override {
        ++build_from_snapshot_calls;
        last_snapshot_id = snapshot_id;
        return Make();
    }
    core::Expected<std::unique_ptr<sandbox::SandboxBackend>, core::AnyError>
        BuildFromPausedState(const core::SandboxId&,
                             const sandbox::PausedSandboxState&,
                             const core::Optional<sandbox::EnvdAccessToken>&) override {
        ++build_from_paused_calls;
        return Make();
    }
    core::Expected<std::shared_ptr<sandbox::PausedSandboxState>, core::AnyError>
        DecodePausedState(const std::string&, const std::string&) override {
        ++decode_paused_calls;
        if (fail_decode) {
            return core::make_unexpected(core::err("fake: decode refused"));
        }
        return std::shared_ptr<sandbox::PausedSandboxState>(new FakePausedState());
    }

    int  decode_paused_calls = 0;
    bool fail_decode = false;

 private:
    /// The minimum a persister round-trip needs: something that re-encodes.
    class FakePausedState : public sandbox::PausedSandboxState {
     public:
        std::string Encode() const override { return "{}"; }
        sandbox::RuntimeArtifactSet RuntimeArtifacts() const override {
            return sandbox::RuntimeArtifactSet();
        }
    };

    core::Expected<std::unique_ptr<sandbox::SandboxBackend>, core::AnyError> Make() {
        if (fail_build) {
            return core::make_unexpected(core::err("fake: build refused"));
        }
        return std::unique_ptr<sandbox::SandboxBackend>(new FakeSandbox(script_));
    }

    BackendScript* script_;
};

/// Minimal `PausedSandboxState` so a resume plan can be built.
class FakePausedState : public sandbox::PausedSandboxState {
 public:
    std::string Encode() const override { return "{}"; }
    sandbox::RuntimeArtifactSet RuntimeArtifacts() const override {
        return sandbox::RuntimeArtifactSet::Empty();
    }
};

struct Fixture {
    BackendScript                                       script;
    std::shared_ptr<orchestrator::InMemoryMetadataStore> store;
    std::shared_ptr<FakeFactory>                        factory;
    std::shared_ptr<orchestrator::SandboxPersister>     persister;
    std::shared_ptr<orchestrator::Orchestrator>         orch;

    Fixture()
        : store(new orchestrator::InMemoryMetadataStore()),
          factory(new FakeFactory(&script)),
          persister(new orchestrator::DisabledSandboxPersister()),
          orch(new orchestrator::Orchestrator(store, factory, persister)) {}
};

inline int64_t NowMs() { return core::SystemTime::Now().unix_nanos / 1000000; }

inline orchestrator::SandboxMetadata CreatingMeta() {
    orchestrator::SandboxMetadata m;
    m.id            = core::SandboxId::Fresh();
    m.state         = orchestrator::SandboxState::Creating;
    m.created_at_ms = NowMs();
    m.resources.cpu_count     = 2;
    m.resources.memory_mib    = 512;
    m.resources.disk_size_mib = 1024;
    return m;
}

inline orchestrator::LaunchPlan FreshPlan(const orchestrator::SandboxMetadata& meta) {
    sandbox::FreshSandboxBuildSpec spec;
    spec.image_config_path = "/img/alpine.json";
    spec.resources         = meta.resources;
    sandbox::SandboxLaunchConfig config;
    config.sandbox_id = meta.id;
    return orchestrator::LaunchPlan::ForCreateFresh(
        meta.id, spec, config, meta, orchestrator::NewTimeout::Set(60 * 1000));
}

}  // namespace testing
}  // namespace agentenv
#endif  // AGENTENV_TESTS_ORCHESTRATOR_FAKES_H_
