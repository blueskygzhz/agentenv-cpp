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
    /// `SnapshotVolumes` declines, which must abort a volume-aware fork before
    /// any clone is made.
    bool fail_snapshot_volumes = false;

    // fork branches
    bool fail_fork_terminal   = false;
    bool fail_fork_recoverable = false;

    // snapshot branches
    bool succeed_snapshot = false;  // off by default (FakeSandbox returns unsupported)
    /// Where the capture claims to have written its artifacts. Named for what
    /// it is: the manifest the fake hands back points *into* this directory,
    /// so a test that publishes the capture has to create the files there.
    /// It was called `snapshot_id` while nothing read it.
    std::string snapshot_dir = "/tmp/aenv-fake-snapshot";
    bool fail_snapshot_terminal   = false;
    bool fail_snapshot_recoverable = false;  // default when succeed_snapshot==false

    // network / custom-extension branches
    bool fail_update_network = false;

    // fork branches
    //
    // `fork_child_outcomes` scripts one result per child, so a test can make
    // some children start and others fail -- which is the case the orchestrator
    // handles with `continue` rather than by abandoning the whole fork, and the
    // only way to observe that siblings survive.
    bool succeed_fork = false;  // off by default (FakeSandbox returns unsupported)
    /// Per-child: true starts, false fails. Short vectors fall back to `true`.
    std::vector<bool> fork_child_outcomes;
    /// Children the fork handed back, in order, so a test can assert the
    /// orchestrator stopped the ones it could not register.
    std::vector<BackendScript*> fork_children;

    // observed
    int start_calls    = 0;
    int wait_calls     = 0;
    int stop_calls     = 0;
    int freeze_calls   = 0;
    int thaw_calls     = 0;
    int fork_calls     = 0;
    int snapshot_calls = 0;
    int snapshot_volumes_calls = 0;

    /// Rust `runtime_info().rootfs_virtual_size`.
    core::Optional<uint64_t> rootfs_virtual_size;

    /// Rust `runtime_info().runtime_artifacts` — the local overlaybd image
    /// configs the sandbox is reading while it is alive. Empty by default
    /// because most branches do not care; the local image maintenance sweep
    /// does, since an empty set is dropped from the live set it reports.
    sandbox::RuntimeArtifactSet runtime_artifacts;

    /// The specs the last `Fork` call received.
    std::vector<sandbox::SandboxForkSpec> last_fork_specs;

    // ---- guest metrics ----------------------------------------------------
    //
    // `envd` is null by default, which is the honest default: a backend with
    // no guest agent connected has nothing to hand out, and the metrics sweep
    // must skip it rather than fail.

    /// Handed out by `Envd()`. Shared, so a test can observe the call count
    /// and flip the behaviour after the orchestrator has taken a copy -- which
    /// is the whole point of the handle outliving the lock.
    std::shared_ptr<class FakeEnvd> envd;

    BackendScript() {}
    ~BackendScript() {
        for (std::size_t i = 0; i < fork_children.size(); ++i) delete fork_children[i];
    }

 private:
    // Owned child scripts make this non-copyable; it is always held as a local
    // and passed by pointer, so nothing needs the copy.
    BackendScript(const BackendScript&);
    BackendScript& operator=(const BackendScript&);
};

/// A scripted `EnvdApi`.
///
/// Deliberately *not* an `EnvdInstance` with a stubbed transport: what the
/// sweep's behaviour turns on is whether the call succeeds, how long it takes
/// and whether the handle has been invalidated, none of which needs a socket.
class FakeEnvd : public sandbox::EnvdApi {
 public:
    FakeEnvd() : calls(0), fail(false), live(true), mem_used(1) {}

    core::Expected<sandbox::SandboxMetric, std::string> Metrics() const override {
        ++calls;
        // Mirrors `EnvdInstance::Metrics`, where `ensure_live()` precedes the
        // request: an invalidated handle must refuse rather than reach the
        // address a different VM now holds.
        if (!live) return core::make_unexpected(std::string("sandbox is no longer active"));
        if (fail) return core::make_unexpected(std::string("fake: guest unreachable"));
        sandbox::SandboxMetric metric;
        metric.timestamp = core::SystemTime(timestamp_secs * 1000000000LL);
        metric.cpu_count = 1;
        metric.mem_used = mem_used;
        return metric;
    }

    mutable int calls;
    bool        fail;
    bool        live;
    int64_t     mem_used;
    int64_t     timestamp_secs = 1700000000LL;
};

class FakeSandbox : public sandbox::SandboxBackend {
 public:
    explicit FakeSandbox(BackendScript* script) : script_(script) {}

    std::shared_ptr<sandbox::EnvdApi> Envd() const override { return script_->envd; }

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
    sandbox::SandboxCaptureResult<sandbox::CapturedSandboxSnapshot> Snapshot() override {
        // The script's `succeed_snapshot` / `snapshot_dir` used to be read by
        // nothing: this returned `Recoverable` unconditionally, so the
        // orchestrator's capture *success* path had no coverage at all. They
        // are honoured now.
        ++script_->snapshot_calls;
        if (script_->fail_snapshot_terminal) {
            return core::make_unexpected(
                sandbox::SandboxCaptureError::Terminal("fake: snapshot refused"));
        }
        if (!script_->succeed_snapshot) {
            return core::make_unexpected(
                sandbox::SandboxCaptureError::Recoverable("fake: snapshot unsupported"));
        }

        // A real manifest, built through the validating constructor rather
        // than assembled field-by-field: a fake that can produce a manifest
        // the real one would reject proves nothing about the publish path it
        // feeds.
        const std::string dir = script_->snapshot_dir;
        const core::Expected<sandbox::SandboxSnapshotManifest, std::string> manifest =
            sandbox::SandboxSnapshotManifest::New("firecracker", dir + "/vm_state.bin",
                                                  dir + "/mem_image.json", 8192,
                                                  dir + "/rootfs/image.json", 4096,
                                                  std::vector<sandbox::ExtraDrive>());
        if (!manifest.ok()) {
            return core::make_unexpected(
                sandbox::SandboxCaptureError::Terminal("fake: " + manifest.error()));
        }
        return sandbox::CapturedSandboxSnapshot(manifest.value(), dir);
    }
    sandbox::SandboxCaptureResult<core::Unit> SnapshotVolumes() override {
        // Counted so a test can assert the volume flush happened *before* the
        // fork capture. A child resumes the parent's volumes, so one caught
        // mid-write would come up with a torn filesystem.
        ++script_->snapshot_volumes_calls;
        if (script_->fail_snapshot_volumes) {
            return core::make_unexpected(
                sandbox::SandboxCaptureError::Recoverable("fake: snapshot volumes declined"));
        }
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
        script_->last_fork_specs = specs;
        if (script_->fail_fork_terminal) {
            return core::make_unexpected(
                sandbox::SandboxCaptureError::Terminal("fake: fork broke the runtime"));
        }
        if (!script_->succeed_fork) {
            return core::make_unexpected(
                sandbox::SandboxCaptureError::Recoverable("fake: fork unsupported"));
        }

        // The whole-call result succeeds and each child carries its own
        // result, which is the shape the real backend returns: a fork can
        // partially succeed, and the orchestrator is expected to keep the
        // children that started rather than unwind the ones that did.
        //
        // Built by moving locals in: `SandboxForkResult` holds a `unique_ptr`,
        // so it is move-only and neither `resize` nor a `push_back` of a
        // copyable temporary compiles.
        std::vector<sandbox::SandboxForkResult> results;
        results.reserve(specs.size());
        for (std::size_t i = 0; i < specs.size(); ++i) {
            const bool starts = i < script_->fork_child_outcomes.size()
                                    ? script_->fork_child_outcomes[i]
                                    : true;
            if (!starts) {
                sandbox::SandboxForkResult failed = core::make_unexpected(
                    core::err("fake: forked child failed to start"));
                results.push_back(std::move(failed));
                // No script is recorded for a child that never existed, so
                // `fork_children` only ever holds the children the caller
                // actually received.
                continue;
            }
            BackendScript* child = new BackendScript();
            script_->fork_children.push_back(child);
            sandbox::SandboxForkResult started(
                std::unique_ptr<sandbox::SandboxBackend>(new FakeSandbox(child)));
            results.push_back(std::move(started));
        }
        // Moved, not returned by name: `Expected`'s `const T&` constructor is
        // viable here and GCC 4.8 picks it, which copies the vector and so
        // copies a move-only element.
        return sandbox::SandboxCaptureResult<std::vector<sandbox::SandboxForkResult> >(
            std::move(results));
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
        info.runtime_artifacts   = script_->runtime_artifacts;
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
    sandbox::firecracker::SnapshotManifestView last_snapshot_manifest;
    std::string last_image_config_path;
    int         build_calls = 0;
    int         build_from_snapshot_calls = 0;
    int         build_from_paused_calls = 0;

    /// What the orchestrator handed the factory on the last build, so a test
    /// can assert the launch inputs and not just that a build happened.
    sandbox::FreshSandboxBuildSpec last_build_spec;
    sandbox::SandboxLaunchConfig   last_launch_config;

    core::Expected<std::unique_ptr<sandbox::SandboxBackend>, core::AnyError>
        Build(const sandbox::FreshSandboxBuildSpec& spec,
              const sandbox::SandboxLaunchConfig& config) override {
        ++build_calls;
        last_image_config_path = spec.image_config_path;
        last_build_spec = spec;
        last_launch_config = config;
        return Make();
    }
    core::Expected<std::unique_ptr<sandbox::SandboxBackend>, core::AnyError>
        BuildFromSnapshot(const sandbox::firecracker::RunnableSnapshotView& snapshot,
                          const sandbox::firecracker::SnapshotManifestView& manifest,
                          const sandbox::SandboxLaunchConfig& config) override {
        ++build_from_snapshot_calls;
        // The id now travels inside the resolved snapshot rather than as the
        // whole argument, so the assertions that read it still work.
        last_snapshot_id = snapshot.snapshot_id;
        last_snapshot_manifest = manifest;
        last_launch_config = config;
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

/// Rust `ScriptedStore` — an in-memory store that can be told to refuse one
/// `Add`.
///
/// Needed because the fork path checks for pre-existing children *before*
/// calling the backend, so a child-registration failure cannot be staged by
/// seeding the id: by the time `Add` runs, the id has already been shown to be
/// free. The failure has to be injected at the call itself.
class ScriptedAddStore : public orchestrator::InMemoryMetadataStore {
 public:
    /// The id whose `Add` must fail. Empty means every `Add` succeeds.
    std::string fail_add_for;

    core::Expected<core::Unit, orchestrator::StoreError>
        Add(orchestrator::SandboxMetadata meta) override {
        if (!fail_add_for.empty() && meta.id.ToString() == fail_add_for) {
            return core::make_unexpected(orchestrator::StoreError::Backend(
                "injected store failure during child registration"));
        }
        return orchestrator::InMemoryMetadataStore::Add(meta);
    }
};

/// `Fixture` over a `ScriptedAddStore`, so a test can reach the branch where a
/// forked child starts but cannot be registered.
struct ScriptedStoreFixture {
    BackendScript                                   script;
    std::shared_ptr<ScriptedAddStore>               store;
    std::shared_ptr<FakeFactory>                    factory;
    std::shared_ptr<orchestrator::SandboxPersister> persister;
    std::shared_ptr<orchestrator::Orchestrator>     orch;

    ScriptedStoreFixture()
        : store(new ScriptedAddStore()),
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
