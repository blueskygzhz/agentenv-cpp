// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/service.rs — THE semantic core of AgentENV.
//
// Concurrency model (per project decision): synchronous + thread pool, no
// std::future in the public API. Rust's `async fn -> Result<T>` maps onto a
// plain blocking `Expected<T, OrchestratorError>`; `tokio::spawn` background
// loops map onto dedicated std::thread workers guarded by a shutdown flag.
//
// Rust's `broadcast::Sender<SandboxLifecycleEvent>` maps onto a callback
// subscriber list, since there is no async receiver to await on.
#ifndef AGENTENV_ORCHESTRATOR_SERVICE_H_
#define AGENTENV_ORCHESTRATOR_SERVICE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "agentenv/cfg.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/orchestrator/launch_plan.h"
#include "agentenv/orchestrator/metrics.h"
#include "agentenv/orchestrator/sandbox_metrics.h"
#include "agentenv/orchestrator/persistence.h"
#include "agentenv/orchestrator/proxy.h"
#include "agentenv/orchestrator/store.h"
#include "agentenv/orchestrator/types.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/volume.h"

namespace agentenv {

/// Rust `use crate::image::cache::{RuntimeImageOwner, RuntimeImageRefs}`.
///
/// Declared rather than included: `image/cache_store.h` pulls the whole image
/// cache service in, and all this header needs is a pointer to the port and
/// the owner type the reconcile names. The one translation unit that calls
/// through them includes the real header.
namespace image {
namespace cache {
class RuntimeImageRefs;
struct RuntimeImageOwner;
}  // namespace cache
}  // namespace image

namespace orchestrator {

/// Rust: `const WAIT_TRANSITION_TIMEOUT: Duration = Duration::from_secs(60)`.
static const int64_t kWaitTransitionTimeoutMs = 60 * 1000;
/// Rust: `const SANDBOX_EVENT_CHANNEL_CAPACITY: usize = 1024`.
static const std::size_t kSandboxEventChannelCapacity = 1024;

/// Rust `type Result<T> = std::result::Result<T, OrchestratorError>`.
template <typename T>
using OrchestratorResult = core::Expected<T, OrchestratorError>;

/// Rust `type SandboxHandle = Arc<Mutex<Box<dyn SandboxBackend>>>`.
///
/// Handle *identity* matters: the launch and cleanup paths compare handles
/// with `Arc::ptr_eq` to detect one that was swapped out underneath them, so
/// the port keeps the same "shared_ptr identity + inner mutex" shape.
class SandboxHandle {
 public:
    explicit SandboxHandle(std::unique_ptr<sandbox::SandboxBackend> backend)
        : backend_(std::move(backend)) {}

    /// RAII equivalent of Rust `handle.lock().await`.
    class Guard {
     public:
        Guard(std::mutex& mu, sandbox::SandboxBackend* backend)
            : lock_(mu), backend_(backend) {}
        /// Adopts a lock the caller already holds — `std::adopt_lock`.
        ///
        /// Needed by `TryLock`: acquiring with `try_lock` and then letting the
        /// guard lock again would release the mutex in between, which is
        /// exactly the window the try was meant to close.
        Guard(std::mutex& mu, sandbox::SandboxBackend* backend, std::adopt_lock_t)
            : lock_(mu, std::adopt_lock), backend_(backend) {}
        sandbox::SandboxBackend* operator->() const { return backend_; }
        sandbox::SandboxBackend& operator*()  const { return *backend_; }
        sandbox::SandboxBackend* get()        const { return backend_; }

     private:
        std::unique_lock<std::mutex> lock_;
        sandbox::SandboxBackend*     backend_;
    };

    Guard Lock() { return Guard(mu_, backend_.get()); }

    /// Rust `handle.try_lock()` — nothing when the backend is busy.
    ///
    /// Used by the metrics sweep, which must *skip* a contended sandbox rather
    /// than wait for it: a lifecycle operation holds this lock across a VMM
    /// call, and a sweep queueing behind one would both miss its interval and
    /// delay the operation it queued behind. A missed sample costs one point
    /// in a graph.
    std::shared_ptr<Guard> TryLock() {
        if (!mu_.try_lock()) return std::shared_ptr<Guard>();
        return std::shared_ptr<Guard>(new Guard(mu_, backend_.get(), std::adopt_lock));
    }

 private:
    std::mutex                               mu_;
    std::unique_ptr<sandbox::SandboxBackend> backend_;
};

using SandboxHandlePtr = std::shared_ptr<SandboxHandle>;

/// Rust free function `resources_with_runtime_info` (service.rs).
///
/// The API's `disk_size_mib` tracks the rootfs block device size. Attached
/// drives are separately configured storage and are not folded into it.
sandbox::SandboxResources ResourcesWithRuntimeInfo(
    sandbox::SandboxResources resources,
    const sandbox::SandboxRuntimeInfo& runtime_info);

/// Rust enum `FailedLaunchStage` (service.rs, private).
enum class FailedLaunchStage {
    Registered,
    TransitionalPersisted,
    RunningPersisted,
};

/// Rust `FailedLaunchStage::rollback_expected_state`.
/// Returns an unset optional for `Registered` (nothing to roll back to).
core::Optional<SandboxState> RollbackExpectedState(FailedLaunchStage stage,
                                                   const LaunchPlan& plan);

/// Rust `FailedLaunchStage::should_detach_proxy_route`.
bool ShouldDetachProxyRoute(FailedLaunchStage stage);

/// Rust `persisted_sandboxes_require_managed_seed` — any restored sandbox
/// whose access depends on a token derived from the seed: a `secure` one (envd
/// token) or one that refuses public traffic (traffic token).
bool PersistedSandboxesRequireManagedSeed(const std::vector<SandboxMetadata>& persisted);

/// Rust `default_fresh_sandbox_resources` — the configured machine shape.
/// `disk_size_mib` is 0 and is filled from the backend's runtime info once the
/// rootfs device exists.
sandbox::SandboxResources DefaultFreshSandboxResources(const cfg::AppConfig& config);

/// Rust `configured_runtime_versions` — the releases this node boots fresh
/// sandboxes with. An unset kernel/firecracker version is recorded as
/// `"unknown"` rather than left empty, as upstream.
snapshot::SnapshotRuntimeVersions ConfiguredRuntimeVersions(const cfg::AppConfig& config);

/// Rust enum `DeleteProgress` (service.rs, private) — the three-phase delete.
///
/// Rust:
/// ```ignore
/// enum DeleteProgress {
///     #[default] Capture,
///     Stop    { capture_failed: bool },
///     Release { capture_failed: bool },
///     Done,
/// }
/// ```
/// The `capture_failed` payload must survive across phases: it is what tells
/// the Release phase to fail the volume backings instead of publishing them.
struct DeleteProgress {
    enum class Kind { Capture, Stop, Release, Done };

    Kind kind = Kind::Capture;   // Rust `#[default]`
    /// Live when kind == Stop or Release.
    bool capture_failed = false;

    DeleteProgress() {}

    static DeleteProgress Capture() { return DeleteProgress(); }
    static DeleteProgress Stop(bool capture_failed) {
        DeleteProgress p; p.kind = Kind::Stop; p.capture_failed = capture_failed; return p;
    }
    static DeleteProgress Release(bool capture_failed) {
        DeleteProgress p; p.kind = Kind::Release; p.capture_failed = capture_failed; return p;
    }
    static DeleteProgress Done() {
        DeleteProgress p; p.kind = Kind::Done; return p;
    }

    bool operator==(const DeleteProgress& o) const {
        if (kind != o.kind) return false;
        if (kind == Kind::Stop || kind == Kind::Release) {
            return capture_failed == o.capture_failed;
        }
        return true;
    }
    bool operator!=(const DeleteProgress& o) const { return !(*this == o); }
};

/// Rust enum `ShutdownOutcome` (service.rs, private).
struct ShutdownOutcome {
    bool        success = true;
    std::string message;  // live when !success

    /// Rust `ShutdownOutcome::from_result`.
    static ShutdownOutcome FromResult(const OrchestratorResult<core::Unit>& r);
    /// Rust `ShutdownOutcome::as_result`.
    OrchestratorResult<core::Unit> AsResult() const;
};

/// Rust struct `Orchestrator<S, F, P>`.
///
/// The Rust type is generic over the metadata store, the backend factory and
/// the persister. C++11 models that with runtime polymorphism instead, so the
/// three collaborators are injected as interface pointers.
class Orchestrator {
 public:
    /// Rust `subscribe_sandbox_events` returns a `broadcast::Receiver`; in the
    /// synchronous model a subscriber is a callback invoked inline by
    /// `publish_sandbox_event`.
    using SandboxEventSubscriber = std::function<void(const SandboxLifecycleEvent&)>;

    Orchestrator(std::shared_ptr<MetadataStore> store,
                 std::shared_ptr<sandbox::Backend> backend);

    /// Rust `Orchestrator<S, F, P>` proper: store + backend factory +
    /// persister. This is the constructor the lifecycle write paths need.
    Orchestrator(std::shared_ptr<MetadataStore> store,
                 std::shared_ptr<sandbox::SandboxBackendFactory> factory,
                 std::shared_ptr<SandboxPersister> persister);
    ~Orchestrator();

    /// Rust `Orchestrator::new` / `new_inner_with_volumes` — the startup path.
    ///
    /// Restores the previous run before the orchestrator exists: every record
    /// `persister.load_all(factory)` yields is added to `store`, so a paused
    /// sandbox survives a restart. The access-token seed is loaded *after*
    /// that scan because whether it may be regenerated depends on it — a
    /// persisted sandbox that carries tokens would be locked out by a new
    /// seed, so `LoadOrCreate` is told the seed must already exist.
    ///
    /// Rust reads `ConfigManager::global_config()`; the config is a parameter
    /// here so a test can supply its own. Not started (its subsystem is not
    /// ported): the metrics task.
    ///
    /// Local image maintenance is started the way upstream does
    /// (`service.rs:263-284`): only when `image.cache.gc.enabled`, and only
    /// after `ReconcilePausedAtStartup` succeeds. That gate is fail-closed —
    /// a failed reconcile leaves maintenance off rather than letting a GC pass
    /// run against protection it could not establish.
    static OrchestratorResult<std::shared_ptr<Orchestrator> >
        New(std::shared_ptr<MetadataStore> store,
            std::shared_ptr<sandbox::SandboxBackendFactory> factory,
            std::shared_ptr<SandboxPersister> persister,
            const cfg::AppConfig& config,
            std::shared_ptr<volume::VolumeManager> volume_manager =
                std::shared_ptr<volume::VolumeManager>());

    /// Rust `with_file_backed_store_and_factory` — in-memory store, and a
    /// file-backed persister at `orchestrator.persisted_sandbox_store_path`
    /// that judges records against this node's virtualization mode.
    static OrchestratorResult<std::shared_ptr<Orchestrator> >
        WithFileBackedStoreAndFactory(std::shared_ptr<sandbox::SandboxBackendFactory> factory,
                                      const cfg::AppConfig& config);

    /// Rust `with_file_backed_store_factory_and_volumes`.
    static OrchestratorResult<std::shared_ptr<Orchestrator> >
        WithFileBackedStoreFactoryAndVolumes(
            std::shared_ptr<sandbox::SandboxBackendFactory> factory,
            std::shared_ptr<volume::VolumeManager> volume_manager,
            const cfg::AppConfig& config);

    // ---- queries (Rust: get_sandbox / list_sandboxes / ...) ---------------

    /// Rust `get_sandbox` — `Ok(None)` when absent, not an error.
    OrchestratorResult<core::Optional<SandboxMetadata> >
        GetSandbox(const core::SandboxId& id) const;

    /// Rust `list_sandboxes` — delegates to the match-all filter.
    OrchestratorResult<std::vector<SandboxMetadata> > ListSandboxes() const;

    /// Rust `list_sandboxes_filtered` — additionally drops `template_builder`
    /// records, which are server-owned and excluded from public sandbox APIs.
    OrchestratorResult<std::vector<SandboxMetadata> >
        ListSandboxesFiltered(const SandboxListFilter& filter) const;

    /// Rust `list_sandbox_ids` — the union of the store's ids and the
    /// in-flight template-build ids, so builder routing stays reserved while
    /// the image resolves and the final template publishes.
    OrchestratorResult<std::vector<core::SandboxId> > ListSandboxIds() const;

    /// Rust `register_template_build`.
    void RegisterTemplateBuild(const core::SandboxId& id);
    /// Rust `unregister_template_build`.
    void UnregisterTemplateBuild(const core::SandboxId& id);

    // ---- metrics (Rust: metrics_snapshot) ---------------------------------

    /// Rust `metrics_snapshot` — resource fields are derived from live
    /// metadata; the two creation counters come from `OrchestratorCounters`.
    OrchestratorResult<OrchestratorMetrics> MetricsSnapshot() const;

    // ---- guest metrics (Rust: orchestrator/sandbox_metrics.rs) ------------

    /// Rust `collect_sandbox_metrics` — one sweep.
    ///
    /// `target` absent means every `Running` sandbox (the periodic tick);
    /// present means just that one (a create/resume/fork event, so a freshly
    /// started sandbox has a sample before the next interval).
    ///
    /// Every failure is a *skip*, never an error: a sweep that aborted on the
    /// first unreachable guest would sample nothing for the rest of the node.
    ///
    /// Deviation from upstream: the per-sandbox calls run sequentially rather
    /// than through `for_each_concurrent(MetricsConcurrency(n), ..)`. There is
    /// no task runtime here, and a thread pool would be a second concurrency
    /// mechanism in a tree that has none. The bound that matters is preserved
    /// — a contended sandbox is skipped via `TryLock` and each guest call is
    /// capped at `kMetricsRequestTimeoutMs` — so the worst-case sweep is
    /// `running × 200 ms` instead of `ceil(running/5) × 200 ms`. At the
    /// default 15 s interval that is a node of 75 sandboxes before the sweep
    /// can no longer finish within one tick; `MetricsConcurrency` is ported
    /// and tested for when a runtime exists to use it.
    void CollectSandboxMetrics(const core::Optional<core::SandboxId>& target);

    /// Rust `sandbox_metrics_history` — retained samples in `[start, end]`
    /// (guest seconds since the epoch), oldest first.
    ///
    /// Errors with `SandboxNotFound` for an unknown id, but returns an *empty*
    /// history for a known sandbox with no samples: the two are different
    /// answers and a client paging a graph needs to tell them apart.
    OrchestratorResult<std::vector<sandbox::SandboxMetric> >
        SandboxMetricsHistory(const core::SandboxId& id,
                              const core::Optional<int64_t>& start_unix_secs,
                              const core::Optional<int64_t>& end_unix_secs) const;

    /// Rust `latest_sandbox_metrics` — the newest sample per requested id.
    ///
    /// An id is omitted entirely when it is not `Running`, has no proxy route,
    /// or its newest sample belongs to a previous incarnation. Omission is the
    /// honest answer: a stale number presented as current is worse than none.
    OrchestratorResult<std::map<std::string, sandbox::SandboxMetric> >
        LatestSandboxMetrics(const std::vector<core::SandboxId>& ids) const;

    // ---- lifecycle events (Rust: broadcast channel) -----------------------

    /// Rust `subscribe_sandbox_events`.
    void SubscribeSandboxEvents(SandboxEventSubscriber subscriber);

    /// Rust `publish_sandbox_event`.
    void PublishSandboxEvent(SandboxLifecycleEventType event_type,
                             const core::SandboxId& sandbox_id,
                             const sandbox::SandboxResources& resources);

    // ---- proxy (Rust: proxy_lookup_for / upsert_proxy_route / ...) --------

    /// Rust `proxy_lookup_for` — resolves proxyability without taking the
    /// per-sandbox mutex.
    OrchestratorResult<ProxyLookupResult>
        ProxyLookupFor(const core::SandboxId& id) const;

    /// Rust `upsert_proxy_route`.
    void UpsertProxyRoute(const core::SandboxId& id, ProxyTarget target);
    /// Rust `restore_proxy_route` — an unset route removes the entry.
    void RestoreProxyRoute(const core::SandboxId& id,
                           const core::Optional<ProxyRoute>& route);

    // ---- lifecycle writes -------------------------------------------------

    /// Rust `keep_alive_for` — refreshes a Running sandbox's TTL.
    ///
    /// An unset `timeout_ms` applies `default_sandbox_timeout`. When
    /// `allow_shorter` is false a new deadline that is not strictly later than
    /// the current one is skipped. Transitional states that may still reach
    /// Running are awaited first.
    OrchestratorResult<core::Optional<SandboxMetadata> >
        KeepAliveFor(const core::SandboxId& id,
                     const core::Optional<int64_t>& timeout_ms,
                     bool allow_shorter);

    /// Rust `wait_for_transition` — waits for `id` to leave
    /// `transitional_state`. A removal yields `SandboxNotFound`; exceeding
    /// `WAIT_TRANSITION_TIMEOUT` yields `InvalidSandboxState`.
    OrchestratorResult<SandboxMetadata>
        WaitForTransition(const core::SandboxId& id,
                          SandboxState transitional_state);

    /// Rust `maybe_update_running_timeout` — applies `timeout` and persists it
    /// only while the sandbox is still Running.
    OrchestratorResult<SandboxMetadata>
        MaybeUpdateRunningTimeout(const core::SandboxId& id, NewTimeout timeout);

    /// Rust `claim_expired_running_sandbox` — flips a still-expired Running
    /// sandbox to `claimed_state`. Returns false when the sandbox's expiry was
    /// refreshed, its state changed, or it no longer exists.
    OrchestratorResult<bool>
        ClaimExpiredRunningSandbox(const core::SandboxId& id,
                                   int64_t cutoff_ms,
                                   SandboxState claimed_state);

    /// Rust `evict_expired_sandboxes` — claims every expired Running sandbox
    /// and applies its `timeout_action`. Returns the evicted ids. A failure on
    /// one sandbox is logged and skipped rather than aborting the sweep.
    OrchestratorResult<std::vector<core::SandboxId> > EvictExpiredSandboxes();

    /// Rust `start_auto_evict_task` — the sweep's only production driver.
    ///
    /// Without it `EvictExpiredSandboxes` was dead code: every sandbox still
    /// got an `expires_at`, the store still answered `ListExpired`, and
    /// `auto_evict_interval_ms` was still parsed from config, so nothing
    /// anywhere reported that no sandbox would ever time out. A leaked sandbox
    /// then holds its VM, network slot, drives and image refs for the life of
    /// the process, and `timeout_action` — the thing a caller sets to choose
    /// between pause and delete — never runs at all.
    ///
    /// Upstream spawns this on the Tokio runtime holding a `Weak<Self>`, so
    /// the task cannot keep the orchestrator alive. Here the thread is a
    /// member joined by `StopAutoEvictTask`, which gives the same guarantee
    /// more directly: `this` stays valid because the destructor joins before
    /// any member dies, and no reference is held that would stop it running.
    ///
    /// Idempotent: a second call is ignored, matching the fact that upstream
    /// spawns it exactly once from the constructor.
    void StartAutoEvictTask(uint64_t evict_interval_ms);

    /// Stops and joins the sweep thread. Called by `Shutdown` and by the
    /// destructor; safe to call twice and safe to call having never started.
    ///
    /// The wait is interruptible rather than a sleep, because a shutdown must
    /// not block for the remainder of a tick — upstream's `tokio::select!` on
    /// `shutdown_rx.changed()` is the same promise.
    void StopAutoEvictTask();

    /// Whether the sweep thread is still joined to this orchestrator.
    ///
    /// Upstream can assert on the task by dropping its `JoinHandle`; the
    /// equivalent observation here is the thread itself. Exposed because
    /// "shutdown stopped the sweep" is otherwise indistinguishable from "the
    /// sweep is still running but `EvictExpiredSandboxes` returns early on
    /// `IsShuttingDown`" — the second is what upstream does *not* do, and
    /// without this a test for the first passes either way.
    bool AutoEvictTaskRunning() const { return auto_evict_thread_.joinable(); }

    // ---- local image maintenance (Rust: image_refs + its sweep) -----------

    /// Rust's `image_refs: Arc<dyn RuntimeImageRefs>` constructor field.
    ///
    /// A setter rather than a constructor parameter, matching how this port
    /// already installs `volume_manager` and the access-token generator: the
    /// three `Orchestrator` constructors and every test that calls them would
    /// otherwise all have to name a port only the maintenance sweep uses.
    ///
    /// `New` installs the production one from config. Left null the sweep
    /// cannot be started at all, which is why `StartLocalImageMaintenanceTask`
    /// refuses instead of running against nothing.
    void SetRuntimeImageRefs(std::shared_ptr<image::cache::RuntimeImageRefs> refs);

    /// Rust `collect_running_artifacts` — the running set's local runtime
    /// artifacts, as the maintenance pass's liveness input.
    ///
    /// Snapshots the handle map under its own lock and only then locks each
    /// sandbox, exactly as upstream does: a lifecycle operation holds the
    /// per-sandbox lock across a VMM call, and holding the map lock while
    /// waiting for one would block every other sandbox's registration.
    ///
    /// Unlike the metrics sweep this *waits* for each sandbox rather than
    /// skipping a contended one. A skipped sandbox is missing from the live
    /// set, and GC treats a missing owner as collectable — it would delete
    /// layers out from under a running VM.
    std::vector<std::pair<core::SandboxId, sandbox::RuntimeArtifactSet> >
        CollectRunningArtifacts() const;

    /// Rust `reconcile_paused_at_startup` — durably protect every restored
    /// paused sandbox, then drop paused protection no restored sandbox claims.
    ///
    /// Fail-closed: the caller must not start maintenance when this fails.
    /// Protection is established *before* the reconcile so a paused sandbox
    /// restored from the previous run cannot be seen as orphaned by its own
    /// reconcile.
    OrchestratorResult<core::Unit> ReconcilePausedAtStartup(
        const std::vector<std::pair<core::SandboxId,
                                    std::shared_ptr<sandbox::PausedSandboxState> > >&
            restored_paused);

    /// Rust `start_local_image_maintenance_task` — the only production driver
    /// of image-cache capacity eviction and GC.
    ///
    /// Without it `ImageCacheService::RunMaintenance`, `RunGc`,
    /// `EvictSourceConfigsOverCapacity` and `ReclaimStaleIndexes` were all
    /// unreachable outside tests: `image.cache.gc` was parsed, watermarks were
    /// computed, holds were written, and the cache still grew without bound
    /// for the life of the process.
    ///
    /// `interval_secs` is upstream's `gc.interval` (a `Duration` there, whole
    /// seconds in this config). Idempotent, like `StartAutoEvictTask`, and a
    /// no-op without a `RuntimeImageRefs` port.
    void StartLocalImageMaintenanceTask(uint64_t interval_secs);

    /// Stops and joins the maintenance thread. Called by `Shutdown` and the
    /// destructor; safe twice and safe having never started.
    void StopLocalImageMaintenanceTask();

    /// Whether the maintenance thread is still joined to this orchestrator.
    bool LocalImageMaintenanceTaskRunning() const {
        return local_image_maintenance_thread_.joinable();
    }

    /// Rust `deletion_progress` — the per-sandbox delete phase, created on
    /// first use. Shared with auto-eviction so both take the same lock.
    DeleteProgress DeletionProgress(const core::SandboxId& id) const;
    void SetDeletionProgress(const core::SandboxId& id, DeleteProgress progress);

    // ---- create / pause / resume -----------------------------------------

    /// Rust `create_sandbox` — allocates a fresh id and launches it.
    OrchestratorResult<SandboxMetadata>
        CreateSandbox(const CreateSandboxRequest& request);

    /// Rust `create_template_builder` — same path with `template_builder` set,
    /// for the server-owned build workers excluded from the public APIs.
    OrchestratorResult<SandboxMetadata>
        CreateTemplateBuilder(const core::SandboxId& build_id,
                              const CreateSandboxRequest& request);

    /// Rust `pause_sandbox` — snapshots the sandbox and stops its VM.
    ///
    /// When another pause is already in flight (`Pausing`), this waits for it
    /// and reports that outcome instead of duplicating the work.
    OrchestratorResult<core::Unit> PauseSandbox(const core::SandboxId& id);

    /// Rust `resume_sandbox` — relaunches a paused sandbox from its captured
    /// state. Joins a concurrent resume rather than duplicating it.
    OrchestratorResult<SandboxMetadata>
        ResumeSandbox(const core::SandboxId& id, NewTimeout timeout);

    /// Rust `default_fresh_sandbox_resources` — the node's configured machine
    /// shape. `disk_size_mib` stays 0 and is filled from the backend's runtime
    /// info once the rootfs device exists.
    void SetDefaultFreshResources(const sandbox::SandboxResources& resources) {
        default_fresh_resources_ = resources;
    }

    /// Rust `ConfigManager::global_config().virtualization_mode`.
    void SetVirtualizationMode(core::VirtualizationMode mode) {
        virtualization_mode_ = mode;
    }
    core::VirtualizationMode VirtualizationMode() const { return virtualization_mode_; }

    /// Rust `access_tokens: SandboxAccessTokenGenerator`.
    void SetAccessTokenGenerator(sandbox::SandboxAccessTokenGenerator generator) {
        access_tokens_ = generator;
        has_access_tokens_ = true;
    }

    // ---- fork ---------------------------------------------------------------

    /// Rust `fork_sandbox` — creates `count` child sandboxes from a running one,
    /// each with a fresh ID and no volume mounts. Rejects sandboxes that have
    /// volume mounts (they need `ForkSandboxWithSpecs`).
    OrchestratorResult<std::vector<SandboxForkOutcome> >
        ForkSandbox(const core::SandboxId& source_id,
                    uint32_t count,
                    NewTimeout timeout);

    /// Rust `fork_sandbox_with_specs` — fork with per-child control over IDs,
    /// volume mounts, extra drives, and drive replacements.
    OrchestratorResult<std::vector<SandboxForkOutcome> >
        ForkSandboxWithSpecs(const core::SandboxId& source_id,
                             const std::vector<SandboxForkChildSpec>& child_specs,
                             NewTimeout timeout);

    // ---- token helpers ------------------------------------------------------

    /// Rust `get_envd_access_token` — returns a token iff `metadata.secure`.
    core::Optional<sandbox::EnvdAccessToken>
        GetEnvdAccessToken(const SandboxMetadata& metadata) const;

    /// Rust `validate_envd_access_token`.
    bool ValidateEnvdAccessToken(const core::SandboxId& id,
                                  const std::string& candidate) const;

    /// Rust `traffic_access_token`.
    std::string TrafficAccessToken(const core::SandboxId& id) const;

    /// Rust `validate_traffic_access_token`.
    bool ValidateTrafficAccessToken(const core::SandboxId& id,
                                     const std::string& candidate) const;

    // ---- snapshot operations ------------------------------------------------

    /// Rust `capture_snapshot` — claims `Snapshotting`, calls `Snapshot()` on
    /// the backend, returns the captured snapshot id and the sandbox metadata.
    OrchestratorResult<SnapshotCaptureResult>
        CaptureSnapshot(const core::SandboxId& id);

    /// Rust `snapshot_volume_mounts` — seals writable persistent-volume uppers
    /// without capturing the VM state.
    OrchestratorResult<core::Unit>
        SnapshotVolumeMounts(const core::SandboxId& id);

    // ---- hot-update operations ----------------------------------------------

    /// Rust `replace_sandbox_network_policy` — updates the network policy on a
    /// running sandbox's backend and persists the new value.
    OrchestratorResult<core::Unit>
        ReplaceSandboxNetworkPolicy(
            const core::SandboxId& id,
            const sandbox::network::SandboxNetworkPolicy& policy);

    /// Rust `patch_sandbox_custom_extension_params` — calls the custom
    /// extension's `hook_patch_params` and stores the approved result.
    ///
    /// Deviation: the Rust implementation contacts an out-of-process extension
    /// server via `CustomExtensionClient::global()`. The C++ port calls the
    /// same method but wraps the client lookup with an optional injection point
    /// (`SetCustomExtensionClient`) so unit tests can drive it without a server.
    OrchestratorResult<core::Optional<sandbox::custom_extension::Params> >
        PatchSandboxCustomExtensionParams(
            const core::SandboxId& id,
            const sandbox::custom_extension::Params& patch);

    /// Inject a custom extension client for testing.  When unset (the default),
    /// `PatchSandboxCustomExtensionParams` returns
    /// `SandboxOperationFailed` with a "not configured" message, which is the
    /// same behaviour as Rust when `CustomExtensionClient::global()` is None.
    void SetCustomExtensionClient(
        std::shared_ptr<sandbox::custom_extension::Client> client) {
        custom_extension_client_ = std::move(client);
    }

    // ---- delete (Rust: delete_sandbox and its three phases) --------------

    /// Rust `delete_sandbox` — stops the sandbox, releases its volumes and
    /// removes its metadata.
    ///
    /// The work is resumable: `DeleteProgress` is kept per sandbox, so a
    /// failed attempt retries from the phase it stopped at instead of redoing
    /// the capture. Deleting an already-deleted sandbox is a no-op.
    OrchestratorResult<core::Unit> DeleteSandbox(const core::SandboxId& id);

    /// Rust `volume_manager: Option<Arc<VolumeManager>>` — optional, and every
    /// volume step is skipped when absent.
    void SetVolumeManager(std::shared_ptr<volume::VolumeManager> manager);

    // ---- launch path (Rust: launch_sandbox and its rollback helpers) ------

    /// Rust `launch_sandbox` — builds, starts and registers a sandbox, then
    /// promotes its metadata from the plan's transitional state to Running.
    ///
    /// Every failure branch rolls back exactly the state it had established,
    /// which is what `FailedLaunchStage` encodes.
    OrchestratorResult<SandboxMetadata> LaunchSandbox(const LaunchPlan& plan);

    /// Rust `sandbox_handle` (test/inspection helper) — the live handle, if any.
    SandboxHandlePtr SandboxHandleFor(const core::SandboxId& id) const;

    /// Rust `sandboxes.write().await.insert(..)` — used by the pause/delete
    /// rollback paths to re-attach a handle they had detached.
    void RegisterSandboxHandle(const core::SandboxId& id, SandboxHandlePtr handle);

    /// Rust `detach_sandbox_handle_and_route` — removes the handle and its
    /// proxy route together, preserving the sandboxes-then-routes lock order.
    void DetachSandboxHandleAndRoute(const core::SandboxId& id,
                                     SandboxHandlePtr* out_handle,
                                     core::Optional<ProxyRoute>* out_route);

    /// Rust `upsert_proxy_route_if_current_handle` — publishes the route only
    /// while `handle` is still the registered one. False means it was stale.
    bool UpsertProxyRouteIfCurrentHandle(const core::SandboxId& id,
                                         const SandboxHandlePtr& handle,
                                         ProxyTarget target);

    /// Rust `proxy_target_from_sandbox`.
    static OrchestratorResult<ProxyTarget>
        ProxyTargetFromSandbox(const sandbox::SandboxBackend& sandbox);

    // ---- shutdown ---------------------------------------------------------

    /// Rust `shutdown` — idempotent; the first call's outcome is memoised in
    /// a `OnceCell` and replayed to later callers.
    OrchestratorResult<core::Unit> Shutdown();

    /// Rust `ensure_accepting_lifecycle_operations` — rejects lifecycle work
    /// once shutdown has begun.
    OrchestratorResult<core::Unit> EnsureAcceptingLifecycleOperations() const;

    /// Rust `default_sandbox_timeout`, from
    /// `config.orchestrator.default_sandbox_timeout_secs`.
    /// Rust `configured_runtime_versions()` — stamped on a fresh sandbox's
    /// metadata. Rust reads the global config at create time; it is fixed
    /// at construction here (`New` sets it from the config it is given).
    void SetConfiguredRuntimeVersions(const snapshot::SnapshotRuntimeVersions& versions) {
        configured_runtime_versions_ = versions;
    }
    void SetDefaultSandboxTimeoutMs(int64_t ms) { default_sandbox_timeout_ms_ = ms; }
    int64_t DefaultSandboxTimeoutMs() const { return default_sandbox_timeout_ms_; }

    /// Rust `config.orchestrator.metrics_retention_secs` (default 3600).
    ///
    /// Injected rather than read from a global, following this class's existing
    /// convention — upstream reaches for `ConfigManager::global_config()` at
    /// each use, which there is no equivalent of here.
    void SetMetricsRetentionSecs(uint64_t secs) { metrics_retention_secs_ = secs; }
    uint64_t MetricsRetentionSecs() const { return metrics_retention_secs_; }

    /// Rust `is_shutting_down`.
    bool IsShuttingDown() const {
        return is_shutting_down_.load(std::memory_order_relaxed);
    }

    // ---- counters (exposed for the create paths) --------------------------

    OrchestratorCounters&       counters()       { return counters_; }
    const OrchestratorCounters& counters() const { return counters_; }

 private:
    /// Rust `protect_image_refs` — pin one owner's artifacts, turning the
    /// port's string error into `InternalError` with the same context prefix.
    OrchestratorResult<core::Unit> ProtectImageRefs(
        const image::cache::RuntimeImageOwner& owner,
        const sandbox::RuntimeArtifactSet& artifacts, const char* context);

    /// Rust `build_sandbox` — dispatches the plan onto the backend factory.
    core::Expected<std::unique_ptr<sandbox::SandboxBackend>, OrchestratorError>
        BuildSandbox(const LaunchPlan& plan);

    /// Rust `rollback_failed_launch_metadata` — Create removes the record,
    /// Resume restores Paused and rolls the persisted lifecycle back.
    void RollbackFailedLaunchMetadata(const LaunchPlan& plan,
                                      SandboxState expected_state);

    /// Rust `detach_launch_runtime_if_current` — returns false when the handle
    /// was replaced, meaning shared state must NOT be rolled back.
    bool DetachLaunchRuntimeIfCurrent(const core::SandboxId& id,
                                      const SandboxHandlePtr& handle,
                                      bool detach_proxy_route,
                                      FailedLaunchStage stage);

    /// Rust `cleanup_failed_launch`.
    void CleanupFailedLaunch(const LaunchPlan& plan,
                             const SandboxHandlePtr& handle,
                             FailedLaunchStage stage);

    /// Rust `create_sandbox_inner`.
    OrchestratorResult<SandboxMetadata>
        CreateSandboxInner(const core::SandboxId& sandbox_id,
                           const CreateSandboxRequest& request,
                           bool template_builder);

    /// Rust `pause_sandbox_inner` — claims `Pausing` from `Running`.
    OrchestratorResult<core::Unit> PauseSandboxInner(const core::SandboxId& id);

    /// Rust `pause_sandbox_impl` — the actual pause, also reached by the
    /// auto-eviction sweep once it has claimed `Pausing`.
    OrchestratorResult<core::Unit> PauseSandboxImpl(const core::SandboxId& id);

    /// Rust `join_concurrent_pause`.
    OrchestratorResult<core::Unit> JoinConcurrentPause(const core::SandboxId& id);

    /// Rust `resume_sandbox_inner`.
    OrchestratorResult<SandboxMetadata>
        ResumeSandboxInner(const core::SandboxId& id, NewTimeout timeout);

    /// Rust `join_concurrent_resume`.
    OrchestratorResult<SandboxMetadata>
        JoinConcurrentResume(const core::SandboxId& id, NewTimeout timeout);

    /// Rust `delete_sandbox_inner` — claims `Killing`, waiting out any
    /// transitional state, then runs the three phases.
    OrchestratorResult<core::Unit> DeleteSandboxInner(const core::SandboxId& id);

    /// Rust `delete_sandbox_impl` — Capture -> Stop -> Release.
    OrchestratorResult<core::Unit>
        DeleteSandboxImpl(const core::SandboxId& id,
                          SandboxState previous_state,
                          DeleteProgress* progress);

    /// Rust `fork_sandbox_inner`.
    OrchestratorResult<std::vector<SandboxForkOutcome> >
        ForkSandboxInner(const core::SandboxId& source_id,
                         const std::vector<SandboxForkChildSpec>& child_specs,
                         NewTimeout timeout);

    /// Rust `fork_child_error`.
    static OrchestratorError ForkChildError(const core::SandboxId& id,
                                            const std::string& source);

    /// Rust `stop_failed_fork` — best-effort stop of a child that could not be
    /// registered.
    static void StopFailedFork(std::unique_ptr<sandbox::SandboxBackend> backend,
                                const core::SandboxId& id);

    /// Rust `begin_snapshot_operation` — claims `Snapshotting` and returns the
    /// live handle, removing the record if the handle has gone missing.
    OrchestratorResult<SandboxHandlePtr>
        BeginSnapshotOperation(const core::SandboxId& id);

    /// Rust `fail_snapshot_operation` — terminal failures stop and remove the
    /// sandbox; recoverable ones restore `Running`.
    OrchestratorError
        FailSnapshotOperation(const core::SandboxId& id,
                              const SandboxHandlePtr& handle,
                              const sandbox::SandboxCaptureError& error,
                              SandboxOperation operation);

    /// Rust `finish_snapshot_operation` — CAS back to `Running`.
    OrchestratorResult<core::Unit>
        FinishSnapshotOperation(const core::SandboxId& id);

    /// Rust `remove_deleted_sandbox` — removes metadata, publishes the Delete
    /// event, drops persisted state and forgets the delete progress.
    OrchestratorResult<core::Unit> RemoveDeletedSandbox(const core::SandboxId& id);

    /// Rust `publish_sandbox_volume_backings`.
    OrchestratorResult<core::Unit>
        PublishSandboxVolumeBackings(const core::SandboxId& id,
                                     const std::vector<std::string>& volume_ids);

    /// Rust `finalize_terminal_volumes` — best-effort publish-then-release for
    /// a sandbox that is going away.
    void FinalizeTerminalVolumes(const SandboxMetadata& metadata);

    /// Rust `self.deletions.lock().await.remove(&sandbox_id)`.
    void ForgetDeletionProgress(const core::SandboxId& id);

    std::shared_ptr<MetadataStore>     store_;
    std::shared_ptr<sandbox::Backend>  backend_;

    /// Rust `factory: F` and `persister: P`.
    std::shared_ptr<sandbox::SandboxBackendFactory> factory_;
    std::shared_ptr<SandboxPersister>               persister_;

    /// Rust `volume_manager: Option<Arc<VolumeManager>>`.
    std::shared_ptr<volume::VolumeManager>          volume_manager_;

    /// Rust `sandboxes: RwLock<HashMap<SandboxId, SandboxHandle>>`.
    mutable std::mutex                          sandboxes_mu_;
    std::map<std::string, SandboxHandlePtr>     sandboxes_;

    OrchestratorCounters counters_;

    /// Rust `sandbox_metrics: Mutex<SandboxMetrics>`.
    mutable std::mutex         sandbox_metrics_mu_;
    mutable SandboxMetricsCache sandbox_metrics_;

    mutable std::mutex                    template_builds_mu_;
    std::set<std::string>                 template_build_ids_;

    mutable std::mutex                    proxy_mu_;
    ProxyRouteTable                       proxy_routes_;
    std::atomic<uint64_t>                 next_proxy_route_version_;

    mutable std::mutex                    subscribers_mu_;
    std::vector<SandboxEventSubscriber>   subscribers_;

    std::atomic<bool>                     is_shutting_down_;
    mutable std::mutex                    shutdown_mu_;
    core::Optional<ShutdownOutcome>       shutdown_outcome_;  // Rust OnceCell

    /// Rust's auto-evict `tokio::spawn` task and the `watch::Receiver<bool>`
    /// it selects on. `auto_evict_stop_` is separate from
    /// `is_shutting_down_`: the thread must also be stoppable by the
    /// destructor on an orchestrator that was never shut down, and reusing
    /// the shutdown flag would make joining mean "refuse all lifecycle work".
    std::thread                           auto_evict_thread_;
    mutable std::mutex                    auto_evict_mu_;
    std::condition_variable               auto_evict_cv_;
    bool                                  auto_evict_stop_ = false;

    /// Rust `image_refs: Arc<dyn RuntimeImageRefs>`. Null until installed:
    /// upstream's is non-optional because its constructor always builds one,
    /// and the sweep here is skipped rather than crashing without it.
    std::shared_ptr<image::cache::RuntimeImageRefs> image_refs_;

    /// Rust's `start_local_image_maintenance_task` task and the
    /// `watch::Receiver<bool>` it selects on. A second set of primitives
    /// rather than sharing the auto-evict ones: the two sweeps run on
    /// different intervals, and one waiting out the other's tick would make a
    /// 1800s GC interval delay a 1000ms eviction.
    std::thread                           local_image_maintenance_thread_;
    mutable std::mutex                    local_image_maintenance_mu_;
    std::condition_variable               local_image_maintenance_cv_;
    bool                                  local_image_maintenance_stop_ = false;

    /// Rust `deletions: Mutex<HashMap<SandboxId, Arc<Mutex<DeleteProgress>>>>`.
    mutable std::mutex                          deletions_mu_;
    mutable std::map<std::string, DeleteProgress> deletions_;

    /// Rust `default_sandbox_timeout`.
    int64_t default_sandbox_timeout_ms_;
    /// Rust `config.orchestrator.metrics_retention_secs`.
    uint64_t metrics_retention_secs_ = 3600;
    snapshot::SnapshotRuntimeVersions configured_runtime_versions_;

    /// Rust `default_fresh_sandbox_resources()`, read from config.machine.
    sandbox::SandboxResources default_fresh_resources_;
    /// Rust `ConfigManager::global_config().virtualization_mode`.
    core::VirtualizationMode  virtualization_mode_;
    /// Rust `access_tokens: SandboxAccessTokenGenerator`.
    sandbox::SandboxAccessTokenGenerator access_tokens_;
    bool                                 has_access_tokens_;

    /// Rust `CustomExtensionClient::global()`; injected here instead of a
    /// process-wide singleton so tests can supply one.
    std::shared_ptr<sandbox::custom_extension::Client> custom_extension_client_;
};

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_SERVICE_H_
