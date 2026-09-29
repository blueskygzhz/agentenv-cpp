// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/service.rs
#include "agentenv/orchestrator/service.h"

#include <algorithm>
#include <set>

#include "agentenv/core/logging.h"

namespace agentenv {
namespace orchestrator {

// ---- FailedLaunchStage -----------------------------------------------------

// Rust `FailedLaunchStage::rollback_expected_state`.
core::Optional<SandboxState> RollbackExpectedState(FailedLaunchStage stage,
                                                   const LaunchPlan& plan) {
    switch (stage) {
        case FailedLaunchStage::Registered:
            return core::nullopt;
        case FailedLaunchStage::TransitionalPersisted:
            return core::Optional<SandboxState>(plan.TransitionalState());
        case FailedLaunchStage::RunningPersisted:
            return core::Optional<SandboxState>(SandboxState::Running);
    }
    return core::nullopt;
}

// Rust `FailedLaunchStage::should_detach_proxy_route` —
// `matches!(self, Self::RunningPersisted)`.
bool ShouldDetachProxyRoute(FailedLaunchStage stage) {
    return stage == FailedLaunchStage::RunningPersisted;
}

// ---- ShutdownOutcome -------------------------------------------------------

// Rust `ShutdownOutcome::from_result`.
ShutdownOutcome ShutdownOutcome::FromResult(
    const OrchestratorResult<core::Unit>& r) {
    ShutdownOutcome o;
    if (r.ok()) {
        o.success = true;
        return o;
    }
    o.success = false;
    // Rust keeps InternalError's bare message and stringifies every other
    // variant through Display.
    const OrchestratorError& err = r.error();
    o.message = (err.kind == OrchestratorErrorKind::InternalError)
                    ? err.detail
                    : err.Message();
    return o;
}

// Rust `ShutdownOutcome::as_result`.
OrchestratorResult<core::Unit> ShutdownOutcome::AsResult() const {
    if (success) return core::Unit{};
    return core::make_unexpected(OrchestratorError::InternalError(message));
}

// ---- Orchestrator ----------------------------------------------------------

Orchestrator::Orchestrator(std::shared_ptr<MetadataStore> store,
                           std::shared_ptr<sandbox::Backend> backend)
    : store_(std::move(store)),
      backend_(std::move(backend)),
      next_proxy_route_version_(1),   // Rust: AtomicU64::new(1)
      is_shutting_down_(false) {}

Orchestrator::~Orchestrator() {}

// ---- queries ---------------------------------------------------------------

// Rust `get_sandbox`.
OrchestratorResult<core::Optional<SandboxMetadata> >
Orchestrator::GetSandbox(const core::SandboxId& id) const {
    core::Expected<core::Optional<SandboxMetadata>, StoreError> r = store_->Get(id);
    if (!r.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(r.error().Message()));
    }
    return r.value();
}

// Rust `list_sandboxes` — `self.list_sandboxes_filtered(matches_all())`.
OrchestratorResult<std::vector<SandboxMetadata> >
Orchestrator::ListSandboxes() const {
    return ListSandboxesFiltered(SandboxListFilter::MatchesAll());
}

// Rust `list_sandboxes_filtered` — drops server-owned template builders.
OrchestratorResult<std::vector<SandboxMetadata> >
Orchestrator::ListSandboxesFiltered(const SandboxListFilter& filter) const {
    core::Expected<std::vector<SandboxMetadata>, StoreError> r =
        store_->ListFiltered(filter);
    if (!r.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(r.error().Message()));
    }
    std::vector<SandboxMetadata> out;
    const std::vector<SandboxMetadata>& all = r.value();
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (!all[i].template_builder) out.push_back(all[i]);
    }
    return out;
}

// Rust `list_sandbox_ids` — union of store ids and in-flight template builds.
OrchestratorResult<std::vector<core::SandboxId> >
Orchestrator::ListSandboxIds() const {
    core::Expected<std::vector<core::SandboxId>, StoreError> r = store_->ListIds();
    if (!r.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(r.error().Message()));
    }

    // Rust builds a HashSet, so duplicates across the two sources collapse.
    std::set<std::string>        seen;
    std::vector<core::SandboxId> out;
    const std::vector<core::SandboxId>& ids = r.value();
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (seen.insert(ids[i].ToString()).second) out.push_back(ids[i]);
    }
    {
        std::lock_guard<std::mutex> g(template_builds_mu_);
        for (std::set<std::string>::const_iterator it = template_build_ids_.begin();
             it != template_build_ids_.end(); ++it) {
            if (!seen.insert(*it).second) continue;
            core::Expected<core::SandboxId, std::string> parsed =
                core::SandboxId::Parse(*it);
            if (parsed.ok()) out.push_back(parsed.value());
        }
    }
    return out;
}

// Rust `register_template_build`.
void Orchestrator::RegisterTemplateBuild(const core::SandboxId& id) {
    std::lock_guard<std::mutex> g(template_builds_mu_);
    template_build_ids_.insert(id.ToString());
}

// Rust `unregister_template_build`.
void Orchestrator::UnregisterTemplateBuild(const core::SandboxId& id) {
    std::lock_guard<std::mutex> g(template_builds_mu_);
    template_build_ids_.erase(id.ToString());
}

// ---- metrics ---------------------------------------------------------------

// Rust `metrics_snapshot`.
OrchestratorResult<OrchestratorMetrics> Orchestrator::MetricsSnapshot() const {
    OrchestratorMetrics metrics;

    // Rust uses `list_with_callback` to fold without materialising a Vec; the
    // aggregate result is identical.
    core::Expected<std::vector<SandboxMetadata>, StoreError> r = store_->List();
    if (!r.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(r.error().Message()));
    }
    const std::vector<SandboxMetadata>& all = r.value();
    for (std::size_t i = 0; i < all.size(); ++i) {
        AggregateResourceMetrics(
            &metrics,
            SandboxContribution::New(all[i].state, all[i].resources));
    }

    metrics.create_successes = counters_.CreateSuccesses();
    metrics.create_fails     = counters_.CreateFails();
    return metrics;
}

// ---- lifecycle events ------------------------------------------------------

// Rust `subscribe_sandbox_events`.
void Orchestrator::SubscribeSandboxEvents(SandboxEventSubscriber subscriber) {
    std::lock_guard<std::mutex> g(subscribers_mu_);
    subscribers_.push_back(subscriber);
}

// Rust `publish_sandbox_event` — the Rust send is best-effort (`let _ = ...`),
// so a subscriber list with no receivers is equally a no-op.
void Orchestrator::PublishSandboxEvent(
    SandboxLifecycleEventType event_type,
    const core::SandboxId& sandbox_id,
    const sandbox::SandboxResources& resources) {
    SandboxLifecycleEvent event;
    event.event_type = event_type;
    event.sandbox_id = sandbox_id;
    event.resources  = resources;

    std::vector<SandboxEventSubscriber> snapshot;
    {
        std::lock_guard<std::mutex> g(subscribers_mu_);
        snapshot = subscribers_;
    }
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        if (snapshot[i]) snapshot[i](event);
    }
}

// ---- proxy -----------------------------------------------------------------

// Rust `proxy_lookup_for`.
OrchestratorResult<ProxyLookupResult>
Orchestrator::ProxyLookupFor(const core::SandboxId& id) const {
    OrchestratorResult<core::Optional<SandboxMetadata> > got = GetSandbox(id);
    if (!got.ok()) return core::make_unexpected(got.error());

    const core::Optional<SandboxMetadata>& meta = got.value();
    if (!meta) {
        return ProxyLookupResult::NotFound();
    }

    switch (meta->state) {
        case SandboxState::Paused:
            return ProxyLookupResult::Paused(meta->auto_resume);
        case SandboxState::Running:
            break;  // fall through to the route lookup
        default:
            // Rust: every non-Running, non-Paused state is Unavailable(state).
            return ProxyLookupResult::Unavailable(meta->state);
    }

    core::Optional<ProxyRoute> route;
    {
        std::lock_guard<std::mutex> g(proxy_mu_);
        route = proxy_routes_.Route(id);
    }
    if (!route) return ProxyLookupResult::RouteMissing();
    return ProxyLookupResult::Ready(route->target);
}

// Rust `upsert_proxy_route`.
void Orchestrator::UpsertProxyRoute(const core::SandboxId& id,
                                    ProxyTarget target) {
    const uint64_t version =
        next_proxy_route_version_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> g(proxy_mu_);
    proxy_routes_.Upsert(id, std::move(target), version);
}

// Rust `restore_proxy_route`.
void Orchestrator::RestoreProxyRoute(const core::SandboxId& id,
                                     const core::Optional<ProxyRoute>& route) {
    std::lock_guard<std::mutex> g(proxy_mu_);
    if (route) {
        proxy_routes_.Upsert(id, route->target, route->version);
    } else {
        proxy_routes_.Remove(id);
    }
}

// ---- shutdown --------------------------------------------------------------

// Rust `shutdown` — memoised in a OnceCell so repeat calls replay the outcome.
OrchestratorResult<core::Unit> Orchestrator::Shutdown() {
    std::lock_guard<std::mutex> g(shutdown_mu_);
    if (shutdown_outcome_) {
        return shutdown_outcome_->AsResult();
    }

    is_shutting_down_.store(true, std::memory_order_relaxed);

    OrchestratorResult<core::Unit> result = core::Unit{};
    shutdown_outcome_ = core::Optional<ShutdownOutcome>(
        ShutdownOutcome::FromResult(result));
    return shutdown_outcome_->AsResult();
}

}  // namespace orchestrator
}  // namespace agentenv
