// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/service.rs — `collect_running_artifacts`,
//       `reconcile_paused_at_startup`, `start_local_image_maintenance_task`
//       and the `new_inner_with_volumes` block that gates them.
//
// These three were the missing production drivers of the node-local image
// cache. `RuntimeImageRefs`, `ImageCacheService::RunMaintenance`, `RunGc`,
// `EvictSourceConfigsOverCapacity` and `ReclaimStaleIndexes` were all ported
// and tested, and none of them had a caller: `image.cache.gc` was parsed, the
// watermarks were computed from `capacity_gb`, holds were written by every
// resolve, and the cache still grew without bound for the life of the
// process. Nothing reported it either, which is what these tests pin down.
#include "microtest.h"

#include <chrono>
#include <thread>

#include "agentenv/core/fs.h"
#include "agentenv/image/cache_store.h"
#include "agentenv/orchestrator/persistence.h"
#include "agentenv/orchestrator/service.h"
#include "orchestrator_fakes.h"

using namespace agentenv;
using namespace agentenv::orchestrator;
using agentenv::testing::BackendScript;
using agentenv::testing::FakePausedState;
using agentenv::testing::FakeSandbox;
using agentenv::testing::Fixture;

namespace {

/// Records what the maintenance wiring asks of the image-cache port.
///
/// A fake rather than the real `ImageCacheStore`: the assertions here are
/// about *which* calls the orchestrator makes and with what live set, and the
/// real store answers those by walking a cache directory, which would make
/// "the sweep ran" indistinguishable from "the cache happened to be empty".
class RecordingImageRefs : public image::cache::RuntimeImageRefs {
 public:
    struct Call {
        std::string owner;      // "<kind>:<sandbox id>"
        std::vector<std::string> paths;
    };

    core::Expected<core::Unit, std::string> Pin(
        const image::cache::RuntimeImageOwner& owner,
        const sandbox::RuntimeArtifactSet& artifacts) override {
        std::lock_guard<std::mutex> guard(mu_);
        Call call;
        call.owner = OwnerLabel(owner);
        call.paths = artifacts.overlaybd_image_config_paths;
        pins_.push_back(call);
        if (fail_pin) return core::make_unexpected(std::string("injected pin failure"));
        return core::Unit();
    }

    void UnpinBestEffort(const image::cache::RuntimeImageOwner& owner) override {
        std::lock_guard<std::mutex> guard(mu_);
        unpins_.push_back(OwnerLabel(owner));
    }

    core::Expected<core::Unit, std::string> ReconcilePaused(
        const std::vector<core::SandboxId>& live_paused) override {
        std::lock_guard<std::mutex> guard(mu_);
        ++reconcile_calls_;
        reconciled_.clear();
        for (std::size_t i = 0; i < live_paused.size(); ++i) {
            reconciled_.push_back(live_paused[i].ToString());
        }
        if (fail_reconcile) {
            return core::make_unexpected(std::string("injected reconcile failure"));
        }
        return core::Unit();
    }

    core::Expected<core::Unit, std::string> MaintainRunning(
        const std::vector<std::pair<core::SandboxId, sandbox::RuntimeArtifactSet> >& running)
        override {
        std::lock_guard<std::mutex> guard(mu_);
        ++maintain_calls_;
        last_running_.clear();
        for (std::size_t i = 0; i < running.size(); ++i) {
            Call call;
            call.owner = running[i].first.ToString();
            call.paths = running[i].second.overlaybd_image_config_paths;
            last_running_.push_back(call);
        }
        if (fail_maintain) {
            return core::make_unexpected(std::string("injected maintenance failure"));
        }
        return core::Unit();
    }

    // Injection knobs; set before the call under test.
    bool fail_pin       = false;
    bool fail_reconcile = false;
    bool fail_maintain  = false;

    int MaintainCalls() const {
        std::lock_guard<std::mutex> guard(mu_);
        return maintain_calls_;
    }
    int ReconcileCalls() const {
        std::lock_guard<std::mutex> guard(mu_);
        return reconcile_calls_;
    }
    std::vector<Call> Pins() const {
        std::lock_guard<std::mutex> guard(mu_);
        return pins_;
    }
    std::vector<std::string> Reconciled() const {
        std::lock_guard<std::mutex> guard(mu_);
        return reconciled_;
    }
    std::vector<Call> LastRunning() const {
        std::lock_guard<std::mutex> guard(mu_);
        return last_running_;
    }

 private:
    static std::string OwnerLabel(const image::cache::RuntimeImageOwner& owner) {
        const char* kind =
            owner.kind == image::cache::RuntimeImageOwner::Kind::PausedSandbox ? "paused"
                                                                              : "starting";
        return std::string(kind) + ":" + owner.id.ToString();
    }

    mutable std::mutex       mu_;
    int                      maintain_calls_  = 0;
    int                      reconcile_calls_ = 0;
    std::vector<Call>        pins_;
    std::vector<std::string> unpins_;
    std::vector<std::string> reconciled_;
    std::vector<Call>        last_running_;
};

int64_t NowMs() { return core::SystemTime::Now().unix_nanos / 1000000; }

/// A Running record plus a live handle, which is what `CollectRunningArtifacts`
/// reads: the store alone is not enough, the artifacts come off the backend.
core::SandboxId RegisterRunning(Fixture& f, BackendScript* script) {
    SandboxMetadata m;
    m.id    = core::SandboxId::Fresh();
    m.state = SandboxState::Running;
    m.resources.cpu_count  = 1;
    m.resources.memory_mib = 128;
    m.created_at_ms = NowMs();
    MT_EXPECT_TRUE(f.store->Add(m).ok());
    f.orch->RegisterSandboxHandle(
        m.id, SandboxHandlePtr(new SandboxHandle(
                  std::unique_ptr<sandbox::SandboxBackend>(new FakeSandbox(script)))));
    return m.id;
}

/// Polls instead of sleeping a fixed amount: a fixed sleep either flakes under
/// load or pads every run.
bool WaitForMaintainCalls(const RecordingImageRefs& refs, int at_least) {
    for (int i = 0; i < 4000; ++i) {
        if (refs.MaintainCalls() >= at_least) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/aenv-img-maint-XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempDir() { core::fs::RemoveDirAll(path); }
    std::string Join(const std::string& rel) const { return path + "/" + rel; }
};

cfg::AppConfig TestConfig(const TempDir& home) {
    cfg::AppConfig config;
    config.home_path = home.path;
    config.orchestrator.persisted_sandbox_store_path = home.Join("persisted-sandboxes");
    config.sandbox.access_token_hash_seed = std::string("test-seed");
    return config;
}

}  // namespace

// ============================================================
// collect_running_artifacts
// ============================================================

MT_TEST(collect_running_artifacts_reports_each_live_handle) {
    Fixture f;
    f.script.runtime_artifacts.overlaybd_image_config_paths.push_back("/cache/rootfs.json");
    f.script.runtime_artifacts.overlaybd_image_config_paths.push_back("/cache/memory.json");
    const core::SandboxId id = RegisterRunning(f, &f.script);

    const std::vector<std::pair<core::SandboxId, sandbox::RuntimeArtifactSet> > running =
        f.orch->CollectRunningArtifacts();

    MT_EXPECT_EQ(running.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(running[0].first == id);
    MT_EXPECT_EQ(running[0].second.overlaybd_image_config_paths.size(),
                 static_cast<std::size_t>(2));
    MT_EXPECT_EQ(running[0].second.overlaybd_image_config_paths[0],
                 std::string("/cache/rootfs.json"));
}

MT_TEST(collect_running_artifacts_is_empty_without_handles) {
    // A record in the store but no live handle contributes nothing: the
    // artifacts are the backend's, and there is no backend to ask. This is the
    // shape that matters for GC — the live set is handles, not records.
    Fixture f;
    SandboxMetadata m;
    m.id    = core::SandboxId::Fresh();
    m.state = SandboxState::Running;
    m.created_at_ms = NowMs();
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    MT_EXPECT_EQ(f.orch->CollectRunningArtifacts().size(), static_cast<std::size_t>(0));
}

MT_TEST(collect_running_artifacts_reports_every_sandbox_separately) {
    // Two sandboxes, two scripts: the live set is per-owner, and folding them
    // together would let one sandbox's layers keep another's alive.
    Fixture f;
    BackendScript other;
    f.script.runtime_artifacts.overlaybd_image_config_paths.push_back("/cache/a.json");
    other.runtime_artifacts.overlaybd_image_config_paths.push_back("/cache/b.json");

    const core::SandboxId first  = RegisterRunning(f, &f.script);
    const core::SandboxId second = RegisterRunning(f, &other);

    const std::vector<std::pair<core::SandboxId, sandbox::RuntimeArtifactSet> > running =
        f.orch->CollectRunningArtifacts();
    MT_EXPECT_EQ(running.size(), static_cast<std::size_t>(2));

    bool saw_first = false;
    bool saw_second = false;
    for (std::size_t i = 0; i < running.size(); ++i) {
        if (running[i].first == first) {
            saw_first = true;
            MT_EXPECT_EQ(running[i].second.overlaybd_image_config_paths[0],
                         std::string("/cache/a.json"));
        }
        if (running[i].first == second) {
            saw_second = true;
            MT_EXPECT_EQ(running[i].second.overlaybd_image_config_paths[0],
                         std::string("/cache/b.json"));
        }
    }
    MT_EXPECT_TRUE(saw_first && saw_second);
}

// ============================================================
// reconcile_paused_at_startup
// ============================================================

MT_TEST(reconcile_paused_at_startup_pins_then_reconciles) {
    Fixture f;
    std::shared_ptr<RecordingImageRefs> refs(new RecordingImageRefs());
    f.orch->SetRuntimeImageRefs(refs);

    const core::SandboxId id = core::SandboxId::Fresh();
    std::shared_ptr<sandbox::PausedSandboxState> state(new FakePausedState());
    std::vector<std::pair<core::SandboxId, std::shared_ptr<sandbox::PausedSandboxState> > >
        restored;
    restored.push_back(std::make_pair(id, state));

    MT_EXPECT_TRUE(f.orch->ReconcilePausedAtStartup(restored).ok());

    // Pinned as a *paused* owner, which is the namespace the reconcile prunes.
    MT_EXPECT_EQ(refs->Pins().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(refs->Pins()[0].owner, std::string("paused:") + id.ToString());
    // And the reconcile saw it as live, so its own protection is not dropped.
    MT_EXPECT_EQ(refs->ReconcileCalls(), 1);
    MT_EXPECT_EQ(refs->Reconciled().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(refs->Reconciled()[0], id.ToString());
}

MT_TEST(reconcile_paused_at_startup_drops_protection_with_nothing_restored) {
    // The orphan-clearing half: no restored paused sandbox means every paused
    // hold left behind by the previous run is stale.
    Fixture f;
    std::shared_ptr<RecordingImageRefs> refs(new RecordingImageRefs());
    f.orch->SetRuntimeImageRefs(refs);

    MT_EXPECT_TRUE(f.orch
                       ->ReconcilePausedAtStartup(
                           std::vector<std::pair<
                               core::SandboxId,
                               std::shared_ptr<sandbox::PausedSandboxState> > >())
                       .ok());

    MT_EXPECT_EQ(refs->Pins().size(), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(refs->ReconcileCalls(), 1);
    MT_EXPECT_EQ(refs->Reconciled().size(), static_cast<std::size_t>(0));
}

MT_TEST(reconcile_paused_at_startup_fails_closed_when_a_pin_fails) {
    // Fail-closed: the reconcile must not run after a failed pin, or it would
    // prune the protection of the very sandbox that could not be pinned.
    Fixture f;
    std::shared_ptr<RecordingImageRefs> refs(new RecordingImageRefs());
    refs->fail_pin = true;
    f.orch->SetRuntimeImageRefs(refs);

    std::vector<std::pair<core::SandboxId, std::shared_ptr<sandbox::PausedSandboxState> > >
        restored;
    restored.push_back(std::make_pair(
        core::SandboxId::Fresh(),
        std::shared_ptr<sandbox::PausedSandboxState>(new FakePausedState())));

    const OrchestratorResult<core::Unit> r = f.orch->ReconcilePausedAtStartup(restored);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().Message().find("pin paused sandbox image refs") !=
                   std::string::npos);
    MT_EXPECT_EQ(refs->ReconcileCalls(), 0);
}

MT_TEST(reconcile_paused_at_startup_propagates_a_reconcile_failure) {
    Fixture f;
    std::shared_ptr<RecordingImageRefs> refs(new RecordingImageRefs());
    refs->fail_reconcile = true;
    f.orch->SetRuntimeImageRefs(refs);

    const OrchestratorResult<core::Unit> r = f.orch->ReconcilePausedAtStartup(
        std::vector<std::pair<core::SandboxId,
                              std::shared_ptr<sandbox::PausedSandboxState> > >());
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().Message().find("reconcile local image protection") !=
                   std::string::npos);
}

MT_TEST(reconcile_paused_at_startup_without_refs_is_a_no_op) {
    Fixture f;  // no SetRuntimeImageRefs
    MT_EXPECT_TRUE(f.orch
                       ->ReconcilePausedAtStartup(
                           std::vector<std::pair<
                               core::SandboxId,
                               std::shared_ptr<sandbox::PausedSandboxState> > >())
                       .ok());
}

// ============================================================
// start_local_image_maintenance_task: the sweep's driver
// ============================================================

MT_TEST(the_maintenance_task_sweeps_without_anyone_calling_it) {
    // The point of the whole change. Before it, `MaintainRunning` had no
    // production caller at all, so no pass ever ran on a live node.
    Fixture f;
    std::shared_ptr<RecordingImageRefs> refs(new RecordingImageRefs());
    f.orch->SetRuntimeImageRefs(refs);
    f.script.runtime_artifacts.overlaybd_image_config_paths.push_back("/cache/rootfs.json");
    const core::SandboxId id = RegisterRunning(f, &f.script);

    // 1s is the floor this config can express; the interval is the only thing
    // that differs from a real node.
    f.orch->StartLocalImageMaintenanceTask(1);
    MT_EXPECT_TRUE(f.orch->LocalImageMaintenanceTaskRunning());
    MT_EXPECT_TRUE(WaitForMaintainCalls(*refs, 1));

    // The pass carried the live set, not an empty one: an empty live set is
    // exactly what makes GC delete a running sandbox's layers.
    MT_EXPECT_EQ(refs->LastRunning().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(refs->LastRunning()[0].owner, id.ToString());
    MT_EXPECT_EQ(refs->LastRunning()[0].paths[0], std::string("/cache/rootfs.json"));

    f.orch->StopLocalImageMaintenanceTask();
    MT_EXPECT_TRUE(!f.orch->LocalImageMaintenanceTaskRunning());
}

MT_TEST(the_maintenance_task_keeps_sweeping_after_a_failed_pass) {
    // One bad pass must not silently end reclamation for the life of the
    // process, so the loop logs and continues.
    Fixture f;
    std::shared_ptr<RecordingImageRefs> refs(new RecordingImageRefs());
    refs->fail_maintain = true;
    f.orch->SetRuntimeImageRefs(refs);

    f.orch->StartLocalImageMaintenanceTask(1);
    MT_EXPECT_TRUE(WaitForMaintainCalls(*refs, 2));
    f.orch->StopLocalImageMaintenanceTask();
}

MT_TEST(starting_the_maintenance_task_twice_starts_one_sweep) {
    Fixture f;
    std::shared_ptr<RecordingImageRefs> refs(new RecordingImageRefs());
    f.orch->SetRuntimeImageRefs(refs);

    f.orch->StartLocalImageMaintenanceTask(600);
    f.orch->StartLocalImageMaintenanceTask(600);
    MT_EXPECT_TRUE(f.orch->LocalImageMaintenanceTaskRunning());
    // A second thread would be unjoinable through the single member and leak.
    f.orch->StopLocalImageMaintenanceTask();
    MT_EXPECT_TRUE(!f.orch->LocalImageMaintenanceTaskRunning());
}

MT_TEST(the_maintenance_task_is_not_started_without_refs) {
    // Upstream cannot express this state; here the port is installed
    // separately, and a sweep with nothing to call would log a pass per
    // interval while reclaiming nothing.
    Fixture f;
    f.orch->StartLocalImageMaintenanceTask(1);
    MT_EXPECT_TRUE(!f.orch->LocalImageMaintenanceTaskRunning());
}

MT_TEST(stopping_an_unstarted_maintenance_task_is_a_no_op) {
    Fixture f;
    f.orch->StopLocalImageMaintenanceTask();
    MT_EXPECT_TRUE(!f.orch->LocalImageMaintenanceTaskRunning());
}

MT_TEST(stopping_the_maintenance_task_does_not_wait_out_the_interval) {
    // The wait is interruptible rather than a sleep: with a 10-minute GC
    // interval a shutdown that waited out a tick would hang the node.
    Fixture f;
    std::shared_ptr<RecordingImageRefs> refs(new RecordingImageRefs());
    f.orch->SetRuntimeImageRefs(refs);

    f.orch->StartLocalImageMaintenanceTask(600);
    const int64_t before = NowMs();
    f.orch->StopLocalImageMaintenanceTask();
    MT_EXPECT_TRUE(NowMs() - before < 1000);
}

MT_TEST(shutdown_stops_the_maintenance_task) {
    Fixture f;
    std::shared_ptr<RecordingImageRefs> refs(new RecordingImageRefs());
    f.orch->SetRuntimeImageRefs(refs);

    f.orch->StartLocalImageMaintenanceTask(600);
    MT_EXPECT_TRUE(f.orch->LocalImageMaintenanceTaskRunning());
    MT_EXPECT_TRUE(f.orch->Shutdown().ok());
    MT_EXPECT_TRUE(!f.orch->LocalImageMaintenanceTaskRunning());
}

MT_TEST(the_maintenance_task_is_joined_by_the_destructor) {
    // Not stopped explicitly: the thread reads the handle map, so it has to be
    // joined before any member dies.
    std::shared_ptr<RecordingImageRefs> refs(new RecordingImageRefs());
    {
        Fixture f;
        f.orch->SetRuntimeImageRefs(refs);
        f.orch->StartLocalImageMaintenanceTask(1);
        MT_EXPECT_TRUE(WaitForMaintainCalls(*refs, 1));
    }
    const int settled = refs->MaintainCalls();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    MT_EXPECT_EQ(refs->MaintainCalls(), settled);
}

// ============================================================
// the `New` gate (Rust service.rs:263-284)
// ============================================================

MT_TEST(new_does_not_start_maintenance_when_gc_is_disabled) {
    TempDir home;
    cfg::AppConfig config = TestConfig(home);
    config.image.cache.gc.enabled = false;
    BackendScript script;
    std::shared_ptr<agentenv::testing::FakeFactory> factory(
        new agentenv::testing::FakeFactory(&script));

    OrchestratorResult<std::shared_ptr<Orchestrator> > orch = Orchestrator::New(
        std::shared_ptr<MetadataStore>(new InMemoryMetadataStore()), factory,
        std::shared_ptr<SandboxPersister>(new DisabledSandboxPersister()), config);
    MT_EXPECT_TRUE(orch.ok());
    if (!orch.ok()) return;
    MT_EXPECT_TRUE(!orch.value()->LocalImageMaintenanceTaskRunning());
    // The eviction sweep is unaffected: the two are separately gated.
    MT_EXPECT_TRUE(orch.value()->AutoEvictTaskRunning());
}

MT_TEST(new_starts_maintenance_when_gc_is_enabled) {
    TempDir home;
    cfg::AppConfig config = TestConfig(home);
    config.image.cache.gc.enabled = true;
    BackendScript script;
    std::shared_ptr<agentenv::testing::FakeFactory> factory(
        new agentenv::testing::FakeFactory(&script));

    OrchestratorResult<std::shared_ptr<Orchestrator> > orch = Orchestrator::New(
        std::shared_ptr<MetadataStore>(new InMemoryMetadataStore()), factory,
        std::shared_ptr<SandboxPersister>(new DisabledSandboxPersister()), config);
    MT_EXPECT_TRUE(orch.ok());
    if (!orch.ok()) return;
    // `New` installed the production `RuntimeImageRefs` from config and the
    // startup reconcile succeeded, so the sweep is live. Without the port
    // being installed the task refuses to start, which is what this asserts
    // beyond the gate itself.
    MT_EXPECT_TRUE(orch.value()->LocalImageMaintenanceTaskRunning());
    MT_EXPECT_TRUE(orch.value()->Shutdown().ok());
    MT_EXPECT_TRUE(!orch.value()->LocalImageMaintenanceTaskRunning());
}

MT_MAIN
