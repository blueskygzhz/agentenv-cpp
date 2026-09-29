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
#include "agentenv/orchestrator/proxy.h"
#include "agentenv/orchestrator/store.h"
#include "agentenv/orchestrator/types.h"
#include "agentenv/sandbox/backend.h"

namespace agentenv {
namespace orchestrator {

/// Rust: `const WAIT_TRANSITION_TIMEOUT: Duration = Duration::from_secs(60)`.
static const int64_t kWaitTransitionTimeoutMs = 60 * 1000;
/// Rust: `const SANDBOX_EVENT_CHANNEL_CAPACITY: usize = 1024`.
static const std::size_t kSandboxEventChannelCapacity = 1024;

/// Rust `type Result<T> = std::result::Result<T, OrchestratorError>`.
template <typename T>
using OrchestratorResult = core::Expected<T, OrchestratorError>;

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
enum class DeleteProgress {
    Capture,   // Rust `#[default]`
    Stop,
    Release,
    Done,
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

    // ---- shutdown ---------------------------------------------------------

    /// Rust `shutdown` — idempotent; the first call's outcome is memoised in
    /// a `OnceCell` and replayed to later callers.
    OrchestratorResult<core::Unit> Shutdown();

    /// Rust `is_shutting_down`.
    bool IsShuttingDown() const {
        return is_shutting_down_.load(std::memory_order_relaxed);
    }

    // ---- counters (exposed for the create paths) --------------------------

    OrchestratorCounters&       counters()       { return counters_; }
    const OrchestratorCounters& counters() const { return counters_; }

 private:
    std::shared_ptr<MetadataStore>     store_;
    std::shared_ptr<sandbox::Backend>  backend_;

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
};

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_SERVICE_H_
