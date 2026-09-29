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
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/orchestrator/launch_plan.h"
#include "agentenv/orchestrator/metrics.h"
#include "agentenv/orchestrator/persistence.h"
#include "agentenv/orchestrator/proxy.h"
#include "agentenv/orchestrator/store.h"
#include "agentenv/orchestrator/types.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/volume.h"

namespace agentenv {
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
        sandbox::SandboxBackend* operator->() const { return backend_; }
        sandbox::SandboxBackend& operator*()  const { return *backend_; }
        sandbox::SandboxBackend* get()        const { return backend_; }

     private:
        std::unique_lock<std::mutex> lock_;
        sandbox::SandboxBackend*     backend_;
    };

    Guard Lock() { return Guard(mu_, backend_.get()); }

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

    /// Rust `deletion_progress` — the per-sandbox delete phase, created on
    /// first use. Shared with auto-eviction so both take the same lock.
    DeleteProgress DeletionProgress(const core::SandboxId& id) const;
    void SetDeletionProgress(const core::SandboxId& id, DeleteProgress progress);

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
    void SetDefaultSandboxTimeoutMs(int64_t ms) { default_sandbox_timeout_ms_ = ms; }
    int64_t DefaultSandboxTimeoutMs() const { return default_sandbox_timeout_ms_; }

    /// Rust `is_shutting_down`.
    bool IsShuttingDown() const {
        return is_shutting_down_.load(std::memory_order_relaxed);
    }

    // ---- counters (exposed for the create paths) --------------------------

    OrchestratorCounters&       counters()       { return counters_; }
    const OrchestratorCounters& counters() const { return counters_; }

 private:
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

    /// Rust `delete_sandbox_inner` — claims `Killing`, waiting out any
    /// transitional state, then runs the three phases.
    OrchestratorResult<core::Unit> DeleteSandboxInner(const core::SandboxId& id);

    /// Rust `delete_sandbox_impl` — Capture -> Stop -> Release.
    OrchestratorResult<core::Unit>
        DeleteSandboxImpl(const core::SandboxId& id,
                          SandboxState previous_state,
                          DeleteProgress* progress);

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

    /// Rust `deletions: Mutex<HashMap<SandboxId, Arc<Mutex<DeleteProgress>>>>`.
    mutable std::mutex                          deletions_mu_;
    mutable std::map<std::string, DeleteProgress> deletions_;

    /// Rust `default_sandbox_timeout`.
    int64_t default_sandbox_timeout_ms_;
};

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_SERVICE_H_
