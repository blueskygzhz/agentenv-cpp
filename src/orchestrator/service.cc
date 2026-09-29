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
      is_shutting_down_(false),
      // Rust reads config.orchestrator.default_sandbox_timeout_secs; callers
      // override via SetDefaultSandboxTimeoutMs once config is wired.
      default_sandbox_timeout_ms_(15 * 60 * 1000) {}

Orchestrator::Orchestrator(std::shared_ptr<MetadataStore> store,
                           std::shared_ptr<sandbox::SandboxBackendFactory> factory,
                           std::shared_ptr<SandboxPersister> persister)
    : store_(std::move(store)),
      factory_(std::move(factory)),
      persister_(std::move(persister)),
      next_proxy_route_version_(1),   // Rust: AtomicU64::new(1)
      is_shutting_down_(false),
      default_sandbox_timeout_ms_(15 * 60 * 1000) {}

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

// ---- lifecycle writes ------------------------------------------------------

// Rust `wait_for_transition`.
OrchestratorResult<SandboxMetadata>
Orchestrator::WaitForTransition(const core::SandboxId& id,
                                SandboxState transitional_state) {
    std::vector<SandboxState> states;
    states.push_back(transitional_state);

    core::Expected<core::Optional<SandboxMetadata>, StoreError> r =
        store_->WaitWhileInStates(id, states, kWaitTransitionTimeoutMs);

    if (!r.ok()) {
        // Rust: the `tokio::time::timeout` Elapsed arm warns and returns
        // InvalidSandboxState; every other store error is propagated.
        if (r.error().kind == StoreError::Kind::Backend &&
            r.error().detail == "wait timed out") {
            AGENTENV_WARN("timed out waiting for sandbox " << id.ToString()
                          << " to leave transitional state "
                          << SandboxStateName(transitional_state));
            return core::make_unexpected(
                OrchestratorError::InvalidSandboxState(id, transitional_state));
        }
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(r.error().Message()));
    }
    // Rust: `Ok(Ok(None))` means the sandbox was removed while waiting.
    if (!r.value()) {
        return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
    }
    return *r.value();
}

// Rust `maybe_update_running_timeout`.
OrchestratorResult<SandboxMetadata>
Orchestrator::MaybeUpdateRunningTimeout(const core::SandboxId& id,
                                        NewTimeout timeout) {
    std::vector<SandboxState> expected;
    expected.push_back(SandboxState::Running);

    core::Expected<MetadataUpdateResult, StoreError> r = store_->UpdateIfState(
        id, expected,
        [timeout](SandboxMetadata* m) { m->UpdateTimeout(timeout); });

    if (!r.ok()) {
        // Rust maps StateConflict onto InvalidSandboxState carrying the actual
        // state, and defers every other variant to `OrchestratorError::from`.
        if (r.error().kind == StoreError::Kind::StateConflict) {
            return core::make_unexpected(OrchestratorError::InvalidSandboxState(
                id, r.error().actual_state));
        }
        if (r.error().kind == StoreError::Kind::SandboxNotFound) {
            return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
        }
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(r.error().Message()));
    }
    return r.value().current;
}

// Rust `keep_alive_for`.
OrchestratorResult<core::Optional<SandboxMetadata> >
Orchestrator::KeepAliveFor(const core::SandboxId& id,
                           const core::Optional<int64_t>& timeout_ms,
                           bool allow_shorter) {
    OrchestratorResult<core::Unit> gate = EnsureAcceptingLifecycleOperations();
    if (!gate.ok()) return core::make_unexpected(gate.error());

    // Rust: an unset timeout falls back to `default_sandbox_timeout`.
    const int64_t valid_timeout_ms =
        timeout_ms ? *timeout_ms : default_sandbox_timeout_ms_;

    OrchestratorResult<core::Optional<SandboxMetadata> > got = GetSandbox(id);
    if (!got.ok()) return core::make_unexpected(got.error());
    if (!got.value()) {
        return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
    }
    SandboxMetadata metadata = *got.value();

    // Rust: transitional states that may still lead to Running are awaited
    // before the keep-alive applicability check.
    if (metadata.state == SandboxState::Creating ||
        metadata.state == SandboxState::Resuming ||
        metadata.state == SandboxState::Snapshotting ||
        metadata.state == SandboxState::Forking) {
        OrchestratorResult<SandboxMetadata> waited =
            WaitForTransition(id, metadata.state);
        if (!waited.ok()) return core::make_unexpected(waited.error());
        metadata = waited.value();
    }

    if (metadata.state != SandboxState::Running) {
        return core::make_unexpected(
            OrchestratorError::InvalidSandboxState(id, metadata.state));
    }

    std::vector<SandboxState> expected;
    expected.push_back(SandboxState::Running);

    const int64_t now_ms = core::SystemTime::Now().unix_nanos / 1000000;
    core::Expected<MetadataUpdateResult, StoreError> r = store_->UpdateIfState(
        id, expected,
        [valid_timeout_ms, allow_shorter, now_ms](SandboxMetadata* m) {
            const int64_t new_expire = now_ms + valid_timeout_ms;
            // Rust: unless `allow_shorter`, a deadline that is not strictly
            // later than the current one is skipped entirely.
            if (!allow_shorter && m->expires_at_ms &&
                new_expire <= *m->expires_at_ms) {
                return;
            }
            // Rust `set_timeout` anchors on now; created_at_ms is this port's
            // anchor field, so it is realigned before applying the TTL.
            m->created_at_ms = now_ms;
            m->SetTimeout(core::Optional<int64_t>(valid_timeout_ms));
        });

    if (!r.ok()) {
        if (r.error().kind == StoreError::Kind::StateConflict) {
            return core::make_unexpected(OrchestratorError::InvalidSandboxState(
                id, r.error().actual_state));
        }
        if (r.error().kind == StoreError::Kind::SandboxNotFound) {
            return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
        }
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(r.error().Message()));
    }
    return core::Optional<SandboxMetadata>(r.value().current);
}

// Rust `claim_expired_running_sandbox`.
OrchestratorResult<bool>
Orchestrator::ClaimExpiredRunningSandbox(const core::SandboxId& id,
                                         int64_t cutoff_ms,
                                         SandboxState claimed_state) {
    std::vector<SandboxState> expected;
    expected.push_back(SandboxState::Running);

    core::Expected<MetadataUpdateResult, StoreError> r = store_->UpdateIfState(
        id, expected, [cutoff_ms, claimed_state](SandboxMetadata* m) {
            // Rust only claims the sandbox while it is still expired at the
            // cutoff, so a refreshed TTL cancels the eviction.
            if (m->IsExpired(cutoff_ms)) m->state = claimed_state;
        });

    if (r.ok()) {
        return r.value().current.state == claimed_state;
    }
    // Rust treats StateConflict and SandboxNotFound as "skip", not failure.
    if (r.error().kind == StoreError::Kind::StateConflict ||
        r.error().kind == StoreError::Kind::SandboxNotFound) {
        return false;
    }
    return core::make_unexpected(
        OrchestratorError::StoreOperationFailed(r.error().Message()));
}

// Rust `deletion_progress` — `entry(id).or_default()`, default is Capture.
DeleteProgress Orchestrator::DeletionProgress(const core::SandboxId& id) const {
    std::lock_guard<std::mutex> g(deletions_mu_);
    const std::string key = id.ToString();
    std::map<std::string, DeleteProgress>::iterator it = deletions_.find(key);
    if (it == deletions_.end()) {
        deletions_[key] = DeleteProgress::Capture();
        return DeleteProgress::Capture();
    }
    return it->second;
}

// Rust `self.deletions.lock().await.remove(&sandbox_id)`.
void Orchestrator::ForgetDeletionProgress(const core::SandboxId& id) {
    std::lock_guard<std::mutex> g(deletions_mu_);
    deletions_.erase(id.ToString());
}

void Orchestrator::SetDeletionProgress(const core::SandboxId& id,
                                       DeleteProgress progress) {
    std::lock_guard<std::mutex> g(deletions_mu_);
    deletions_[id.ToString()] = progress;
}

// Rust `evict_expired_sandboxes`.
OrchestratorResult<std::vector<core::SandboxId> >
Orchestrator::EvictExpiredSandboxes() {
    std::vector<core::SandboxId> evicted;

    // Rust skips the whole sweep once shutdown has begun.
    if (IsShuttingDown()) {
        return evicted;
    }

    const int64_t eviction_cutoff_ms = core::SystemTime::Now().unix_nanos / 1000000;
    core::Expected<std::vector<SandboxMetadata>, StoreError> expired =
        store_->ListExpired(eviction_cutoff_ms);
    if (!expired.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(expired.error().Message()));
    }

    const std::vector<SandboxMetadata>& candidates = expired.value();
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const SandboxMetadata& metadata = candidates[i];
        // Rust only evicts Running sandboxes.
        if (metadata.state != SandboxState::Running) continue;

        const SandboxState claimed_state =
            (metadata.timeout_action == SandboxTimeoutAction::Pause)
                ? SandboxState::Pausing
                : SandboxState::Killing;

        // Rust takes the same per-sandbox delete lock as explicit deletion
        // before claiming Killing.
        DeletionProgress(metadata.id);

        OrchestratorResult<bool> claimed = ClaimExpiredRunningSandbox(
            metadata.id, eviction_cutoff_ms, claimed_state);
        if (!claimed.ok()) {
            // Rust logs and continues rather than aborting the sweep.
            AGENTENV_WARN("failed to auto-evict expired sandbox "
                          << metadata.id.ToString() << ": "
                          << claimed.error().Message());
            continue;
        }
        if (!claimed.value()) continue;

        evicted.push_back(metadata.id);
    }

    return evicted;
}

// ---- launch path -----------------------------------------------------------

namespace {

// Rust `bytes_to_mib_ceil`.
uint32_t BytesToMibCeil(uint64_t bytes) {
    const uint64_t kMib = 1024ull * 1024ull;
    return static_cast<uint32_t>((bytes + kMib - 1) / kMib);
}

}  // namespace

// Rust `resources_with_runtime_info`.
sandbox::SandboxResources ResourcesWithRuntimeInfo(
    sandbox::SandboxResources resources,
    const sandbox::SandboxRuntimeInfo& runtime_info) {
    // This API resource field tracks the rootfs block device size. Attached
    // drives are separately configured storage and are not folded into it.
    if (runtime_info.rootfs_virtual_size) {
        resources.disk_size_mib = BytesToMibCeil(*runtime_info.rootfs_virtual_size);
    }
    return resources;
}

// Rust `proxy_target_from_sandbox`.
OrchestratorResult<ProxyTarget>
Orchestrator::ProxyTargetFromSandbox(const sandbox::SandboxBackend& sandbox) {
    core::Optional<std::string> ip = sandbox.HostInteractionIp();
    if (!ip) {
        AGENTENV_WARN("sandbox started without an interaction IP");
        return core::make_unexpected(OrchestratorError::InternalError(
            "sandbox missing host interaction IP after start"));
    }
    return ProxyTarget(*ip);
}

SandboxHandlePtr
Orchestrator::SandboxHandleFor(const core::SandboxId& id) const {
    std::lock_guard<std::mutex> g(sandboxes_mu_);
    std::map<std::string, SandboxHandlePtr>::const_iterator it =
        sandboxes_.find(id.ToString());
    return it == sandboxes_.end() ? SandboxHandlePtr() : it->second;
}

void Orchestrator::RegisterSandboxHandle(const core::SandboxId& id,
                                         SandboxHandlePtr handle) {
    std::lock_guard<std::mutex> g(sandboxes_mu_);
    sandboxes_[id.ToString()] = std::move(handle);
}

// Rust `detach_sandbox_handle_and_route` — the lock order (sandboxes first,
// then proxy_routes) is shared with UpsertProxyRouteIfCurrentHandle.
void Orchestrator::DetachSandboxHandleAndRoute(
    const core::SandboxId& id,
    SandboxHandlePtr* out_handle,
    core::Optional<ProxyRoute>* out_route) {
    std::lock_guard<std::mutex> g(sandboxes_mu_);

    const std::string key = id.ToString();
    std::map<std::string, SandboxHandlePtr>::iterator it = sandboxes_.find(key);
    if (out_handle) {
        *out_handle = (it == sandboxes_.end()) ? SandboxHandlePtr() : it->second;
    }
    if (it != sandboxes_.end()) sandboxes_.erase(it);

    core::Optional<ProxyRoute> removed;
    {
        std::lock_guard<std::mutex> pg(proxy_mu_);
        removed = proxy_routes_.Remove(id);
    }
    if (removed) {
        AGENTENV_DEBUG("removed runtime proxy route version " << removed->version);
    }
    if (out_route) *out_route = removed;
}

// Rust `upsert_proxy_route_if_current_handle`.
bool Orchestrator::UpsertProxyRouteIfCurrentHandle(
    const core::SandboxId& id,
    const SandboxHandlePtr& handle,
    ProxyTarget target) {
    std::lock_guard<std::mutex> g(sandboxes_mu_);

    std::map<std::string, SandboxHandlePtr>::const_iterator it =
        sandboxes_.find(id.ToString());
    if (it == sandboxes_.end()) return false;
    // Rust `Arc::ptr_eq`.
    if (it->second != handle) return false;

    const uint64_t version =
        next_proxy_route_version_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> pg(proxy_mu_);
        proxy_routes_.Upsert(id, std::move(target), version);
    }
    return true;
}

// Rust `build_sandbox` — dispatches the plan onto the backend factory.
core::Expected<std::unique_ptr<sandbox::SandboxBackend>, OrchestratorError>
Orchestrator::BuildSandbox(const LaunchPlan& plan) {
    if (!factory_) {
        return core::make_unexpected(OrchestratorError::InternalError(
            "orchestrator was constructed without a sandbox backend factory"));
    }

    const core::SandboxId sandbox_id = plan.SandboxId();

    if (plan.kind == LaunchPlanKind::Create) {
        const CreateLaunchPlan& create = *plan.create;
        if (create.source.kind == CreateLaunchSourceKind::Snapshot) {
            core::Expected<std::unique_ptr<sandbox::SandboxBackend>, core::AnyError>
                built = factory_->BuildFromSnapshot(create.source.snapshot_id,
                                                    create.launch_config);
            if (!built.ok()) {
                return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
                    sandbox_id, SandboxOperation::Build, built.error().chain()));
            }
            return built.take_value();
        }
        if (!create.source.build_spec) {
            return core::make_unexpected(OrchestratorError::InternalError(
                "fresh launch plan is missing its build spec"));
        }
        core::Expected<std::unique_ptr<sandbox::SandboxBackend>, core::AnyError>
            built = factory_->Build(*create.source.build_spec, create.launch_config);
        if (!built.ok()) {
            return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
                sandbox_id, SandboxOperation::Build, built.error().chain()));
        }
        return built.take_value();
    }

    const ResumeLaunchPlan& resume = *plan.resume;
    if (!resume.paused_state) {
        return core::make_unexpected(OrchestratorError::InternalError(
            "resume launch plan is missing its paused state"));
    }
    core::Expected<std::unique_ptr<sandbox::SandboxBackend>, core::AnyError> built =
        factory_->BuildFromPausedState(resume.sandbox_id, *resume.paused_state,
                                       resume.envd_access_token);
    if (!built.ok()) {
        return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
            sandbox_id, SandboxOperation::Build, built.error().chain()));
    }
    return built.take_value();
}

// Rust `rollback_failed_launch_metadata`.
//
// Deviation: Rust also releases the `StartingSandbox` image refs here. The
// runtime image-ref protection subsystem is not ported yet, so there is
// nothing to release.
void Orchestrator::RollbackFailedLaunchMetadata(const LaunchPlan& plan,
                                                SandboxState expected_state) {
    const core::SandboxId sandbox_id = plan.SandboxId();

    if (plan.kind == LaunchPlanKind::Create) {
        core::Expected<core::Optional<SandboxMetadata>, StoreError> removed =
            store_->Remove(sandbox_id);
        if (!removed.ok()) {
            AGENTENV_WARN("failed to remove sandbox metadata during launch rollback: "
                          << removed.error().Message());
        }
        return;
    }

    std::vector<SandboxState> expected(1, expected_state);
    core::Expected<SandboxState, StoreError> restored =
        store_->UpdateStateIfState(sandbox_id, SandboxState::Paused, expected);
    if (!restored.ok()) {
        AGENTENV_WARN("failed to restore sandbox metadata during launch rollback: "
                      << restored.error().Message());
    }
    if (persister_) {
        PersistenceResult<core::Unit> rolled = persister_->RollbackResuming(sandbox_id);
        if (!rolled.ok()) {
            AGENTENV_WARN("failed to restore persisted sandbox record lifecycle "
                          "during launch rollback: " << rolled.error().Message());
        }
    }
}

// Rust `detach_launch_runtime_if_current`.
bool Orchestrator::DetachLaunchRuntimeIfCurrent(const core::SandboxId& id,
                                                const SandboxHandlePtr& handle,
                                                bool detach_proxy_route,
                                                FailedLaunchStage stage) {
    std::lock_guard<std::mutex> g(sandboxes_mu_);

    std::map<std::string, SandboxHandlePtr>::iterator it =
        sandboxes_.find(id.ToString());
    // Rust: a missing entry still allows the shared-state rollback.
    if (it == sandboxes_.end()) return true;

    if (it->second != handle) {
        AGENTENV_WARN("sandbox handle was replaced during failed launch cleanup "
                      "(stage " << static_cast<int>(stage)
                      << "); skipping shared state rollback");
        return false;
    }

    sandboxes_.erase(it);

    if (detach_proxy_route) {
        core::Optional<ProxyRoute> removed;
        {
            std::lock_guard<std::mutex> pg(proxy_mu_);
            removed = proxy_routes_.Remove(id);
        }
        if (removed) {
            AGENTENV_DEBUG("removed runtime proxy route version " << removed->version);
        }
    }
    return true;
}

// Rust `cleanup_failed_launch`.
void Orchestrator::CleanupFailedLaunch(const LaunchPlan& plan,
                                       const SandboxHandlePtr& handle,
                                       FailedLaunchStage stage) {
    const bool should_rollback_shared_state = DetachLaunchRuntimeIfCurrent(
        plan.SandboxId(), handle, ShouldDetachProxyRoute(stage), stage);

    {
        SandboxHandle::Guard sandbox = handle->Lock();
        core::Expected<core::Unit, core::AnyError> stopped = sandbox->Stop();
        if (!stopped.ok()) {
            AGENTENV_WARN("failed to stop sandbox while rolling back launch: "
                          << stopped.error().chain());
        }
    }

    if (!should_rollback_shared_state) return;

    core::Optional<SandboxState> expected_state = RollbackExpectedState(stage, plan);
    if (expected_state) {
        RollbackFailedLaunchMetadata(plan, *expected_state);
    }
}

// Rust `launch_sandbox`.
OrchestratorResult<SandboxMetadata>
Orchestrator::LaunchSandbox(const LaunchPlan& plan) {
    OrchestratorResult<core::Unit> accepting = EnsureAcceptingLifecycleOperations();
    if (!accepting.ok()) return core::make_unexpected(accepting.error());

    const core::SandboxId sandbox_id        = plan.SandboxId();
    const SandboxState    transitional_state = plan.TransitionalState();

    // Build and start the sandbox before touching any shared state, so a build
    // failure needs no persisted rollback.
    core::Expected<std::unique_ptr<sandbox::SandboxBackend>, OrchestratorError> built =
        BuildSandbox(plan);
    if (!built.ok()) {
        RollbackFailedLaunchMetadata(plan, transitional_state);
        return core::make_unexpected(built.take_error());
    }
    std::unique_ptr<sandbox::SandboxBackend> sandbox = built.take_value();

    core::Expected<core::Unit, core::AnyError> started = sandbox->StartNowait();
    if (!started.ok()) {
        const std::string source = started.error().chain();
        AGENTENV_WARN("failed to start sandbox: " << source);
        core::Expected<core::Unit, core::AnyError> stopped = sandbox->Stop();
        if (!stopped.ok()) {
            AGENTENV_WARN("failed to stop sandbox after start failure: "
                          << stopped.error().chain());
        }
        RollbackFailedLaunchMetadata(plan, transitional_state);
        return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
            sandbox_id, SandboxOperation::Start, source));
    }
    AGENTENV_DEBUG("sandbox start requested");

    // Stop before persisting any state if shutdown started in the meantime.
    if (IsShuttingDown()) {
        AGENTENV_INFO("orchestrator started shutting down just after starting the sandbox");
        core::Expected<core::Unit, core::AnyError> stopped = sandbox->Stop();
        if (!stopped.ok()) {
            AGENTENV_WARN("failed to stop sandbox: " << stopped.error().chain());
        }
        RollbackFailedLaunchMetadata(plan, transitional_state);
        return core::make_unexpected(OrchestratorError::ShuttingDown());
    }

    const sandbox::SandboxResources runtime_resources =
        ResourcesWithRuntimeInfo(plan.Resources(), sandbox->RuntimeInfoOf());

    core::Optional<SandboxMetadata> transitional_metadata;
    if (const SandboxMetadata* planned = plan.TransitionalMetadata()) {
        SandboxMetadata copy = *planned;
        copy.resources = runtime_resources;
        transitional_metadata = core::Optional<SandboxMetadata>(copy);
    }

    // Store the sandbox handle in memory.
    SandboxHandlePtr handle(new SandboxHandle(std::move(sandbox)));
    RegisterSandboxHandle(sandbox_id, handle);

    // Persist the sandbox metadata if needed (during creation).
    if (transitional_metadata) {
        core::Expected<core::Unit, StoreError> added = store_->Add(*transitional_metadata);
        if (!added.ok()) {
            AGENTENV_WARN("failed to persist sandbox metadata; cleaning up: "
                          << added.error().Message());
            const std::string message = added.error().Message();
            CleanupFailedLaunch(plan, handle, FailedLaunchStage::Registered);
            return core::make_unexpected(
                OrchestratorError::StoreOperationFailed(message));
        }
    }

    if (IsShuttingDown()) {
        AGENTENV_INFO("orchestrator started shutting down before sandbox became ready");
        CleanupFailedLaunch(plan, handle, FailedLaunchStage::TransitionalPersisted);
        return core::make_unexpected(OrchestratorError::ShuttingDown());
    }

    // Wait for the sandbox to be ready.
    bool        ready = true;
    std::string ready_error;
    {
        SandboxHandle::Guard guard = handle->Lock();
        core::Expected<core::Unit, core::AnyError> waited = guard->WaitForReady();
        if (!waited.ok()) {
            ready = false;
            ready_error = waited.error().chain();
        }
    }
    if (!ready) {
        AGENTENV_WARN("sandbox failed to become ready: " << ready_error);
        CleanupFailedLaunch(plan, handle, FailedLaunchStage::TransitionalPersisted);
        return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
            sandbox_id, SandboxOperation::WaitReady, ready_error));
    }

    if (IsShuttingDown()) {
        AGENTENV_INFO("orchestrator started shutting down while sandbox was becoming ready");
        CleanupFailedLaunch(plan, handle, FailedLaunchStage::TransitionalPersisted);
        return core::make_unexpected(OrchestratorError::ShuttingDown());
    }

    const NewTimeout launch_timeout = plan.Timeout();
    std::vector<SandboxState> expected_states(1, transitional_state);
    core::Expected<MetadataUpdateResult, StoreError> updated = store_->UpdateIfState(
        sandbox_id, expected_states,
        [runtime_resources, launch_timeout](SandboxMetadata* metadata) {
            metadata->resources = runtime_resources;
            metadata->state     = SandboxState::Running;
            metadata->UpdateTimeout(launch_timeout);
        });
    if (!updated.ok()) {
        const std::string message = updated.error().Message();
        AGENTENV_WARN("failed to persist final sandbox metadata after launch: " << message);
        CleanupFailedLaunch(plan, handle, FailedLaunchStage::TransitionalPersisted);
        return core::make_unexpected(OrchestratorError::StoreOperationFailed(message));
    }
    const SandboxMetadata final_metadata = updated.value().current;

    ProxyTarget       proxy_target;
    bool              has_target = true;
    OrchestratorError target_error;
    {
        SandboxHandle::Guard guard = handle->Lock();
        OrchestratorResult<ProxyTarget> resolved = ProxyTargetFromSandbox(*guard);
        if (resolved.ok()) {
            proxy_target = resolved.value();
        } else {
            has_target   = false;
            target_error = resolved.error();
        }
    }
    if (!has_target) {
        AGENTENV_WARN("sandbox became ready without a proxy target; rolling back launch");
        CleanupFailedLaunch(plan, handle, FailedLaunchStage::RunningPersisted);
        return core::make_unexpected(target_error);
    }

    if (!UpsertProxyRouteIfCurrentHandle(sandbox_id, handle, proxy_target)) {
        AGENTENV_DEBUG("skipping runtime proxy route publication because sandbox "
                       "handle is stale");
    }

    if (plan.kind == LaunchPlanKind::Resume && persister_) {
        PersistenceResult<core::Unit> deleted = persister_->DeleteRecord(sandbox_id);
        if (!deleted.ok()) {
            AGENTENV_WARN("failed to delete persisted sandbox record after resume: "
                          << deleted.error().Message());
        }
    }

    AGENTENV_INFO("sandbox launch completed");
    return final_metadata;
}

// ---- delete ----------------------------------------------------------------

namespace {

// Rust `metadata.volume_mounts.values().cloned().collect::<Vec<_>>()`.
std::vector<std::string> VolumeIdsOf(const SandboxMetadata& metadata) {
    std::vector<std::string> ids;
    for (std::unordered_map<std::string, std::string>::const_iterator it =
             metadata.volume_mounts.begin();
         it != metadata.volume_mounts.end(); ++it) {
        ids.push_back(it->second);
    }
    return ids;
}

}  // namespace

void Orchestrator::SetVolumeManager(
    std::shared_ptr<volume::VolumeManager> manager) {
    volume_manager_ = std::move(manager);
}

// Rust `publish_sandbox_volume_backings`.
OrchestratorResult<core::Unit>
Orchestrator::PublishSandboxVolumeBackings(
    const core::SandboxId& id,
    const std::vector<std::string>& volume_ids) {
    // Rust: no volume manager means nothing to publish.
    if (!volume_manager_) return core::Unit();

    core::Expected<core::Unit, volume::VolumeError> published =
        volume_manager_->RecoverAndPublishBackings(id.ToString(), volume_ids);
    if (!published.ok()) {
        return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
            id, SandboxOperation::SnapshotVolumes, published.error().ToString()));
    }
    return core::Unit();
}

// Rust `finalize_terminal_volumes` — both steps are best-effort warnings.
void Orchestrator::FinalizeTerminalVolumes(const SandboxMetadata& metadata) {
    if (!volume_manager_) return;

    const std::vector<std::string> volume_ids = VolumeIdsOf(metadata);
    if (volume_ids.empty()) return;

    const std::string owner = metadata.id.ToString();
    core::Expected<core::Unit, volume::VolumeError> published =
        volume_manager_->RecoverAndPublishBackings(owner, volume_ids);
    if (!published.ok()) {
        AGENTENV_WARN("failed to publish volumes during terminal sandbox cleanup for "
                      << owner << ": " << published.error().ToString());
    }
    core::Expected<core::Unit, volume::VolumeError> released =
        volume_manager_->ReplaceOwnerFor(owner, core::Optional<std::string>(),
                                         volume_ids);
    if (!released.ok()) {
        AGENTENV_WARN("failed to release volumes during terminal sandbox cleanup for "
                      << owner << ": " << released.error().ToString());
    }
}

// Rust `remove_deleted_sandbox`.
OrchestratorResult<core::Unit>
Orchestrator::RemoveDeletedSandbox(const core::SandboxId& id) {
    core::Expected<core::Optional<SandboxMetadata>, StoreError> removed =
        store_->Remove(id);
    if (!removed.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(removed.error().Message()));
    }
    if (removed.value()) {
        PublishSandboxEvent(SandboxLifecycleEventType::Delete,
                            removed.value()->id, removed.value()->resources);
    }

    if (persister_) {
        PersistenceResult<core::Unit> dropped =
            persister_->DeleteRecordAndArtifacts(id);
        if (!dropped.ok()) {
            AGENTENV_WARN("failed to delete persisted sandbox state: "
                          << dropped.error().Message());
        }
    }

    // Deviation: Rust also releases the PausedSandbox image refs here; that
    // subsystem is not ported yet.
    ForgetDeletionProgress(id);
    AGENTENV_INFO("sandbox deleted");
    return core::Unit();
}

// Rust `delete_sandbox`.
//
// Deviation: Rust wraps this in `run_cancellation_safe`, which shields the
// future from a dropped caller. A synchronous call cannot be cancelled
// mid-flight, so the wrapper has no counterpart here.
OrchestratorResult<core::Unit>
Orchestrator::DeleteSandbox(const core::SandboxId& id) {
    return DeleteSandboxInner(id);
}

// Rust `delete_sandbox_inner`.
OrchestratorResult<core::Unit>
Orchestrator::DeleteSandboxInner(const core::SandboxId& id) {
    AGENTENV_INFO("deleting sandbox " << id.ToString());

    DeleteProgress progress = DeletionProgress(id);
    if (progress.kind == DeleteProgress::Kind::Done) {
        return core::Unit();
    }
    // A retry that already advanced past Capture resumes from its own phase
    // and must not re-claim Killing.
    if (progress.kind != DeleteProgress::Kind::Capture) {
        return DeleteSandboxImpl(id, SandboxState::Killing, &progress);
    }

    std::vector<SandboxState> deletable;
    deletable.push_back(SandboxState::Running);
    deletable.push_back(SandboxState::Paused);

    // Attempt to transition to Killing, retrying after waiting whenever we
    // find the sandbox in a transitional state.
    SandboxState previous_state = SandboxState::Running;
    for (;;) {
        core::Expected<SandboxState, StoreError> claimed =
            store_->UpdateStateIfState(id, SandboxState::Killing, deletable);
        if (claimed.ok()) {
            previous_state = claimed.value();
            break;
        }

        const StoreError error = claimed.error();
        if (error.kind != StoreError::Kind::StateConflict) {
            if (error.kind == StoreError::Kind::SandboxNotFound) {
                ForgetDeletionProgress(id);
                return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
            }
            return core::make_unexpected(
                OrchestratorError::StoreOperationFailed(error.Message()));
        }

        const SandboxState actual = error.actual_state;
        switch (actual) {
            case SandboxState::Killing:
            case SandboxState::Creating:
            case SandboxState::Snapshotting:
            case SandboxState::Forking:
            case SandboxState::Pausing:
            case SandboxState::Resuming: {
                // An in-progress operation holds the sandbox here. Wait for it
                // to finish so our Killing transition does not race with that
                // operation's final state write, then retry the CAS.
                AGENTENV_DEBUG("sandbox " << id.ToString() << " in transitional state "
                               << SandboxStateName(actual)
                               << ", waiting before deletion");
                OrchestratorResult<SandboxMetadata> waited =
                    WaitForTransition(id, actual);
                if (waited.ok()) continue;   // retry the Killing CAS
                if (waited.error().kind == OrchestratorErrorKind::SandboxNotFound) {
                    // Removed while we waited, e.g. by a concurrent delete.
                    AGENTENV_INFO("sandbox was deleted while waiting for "
                                  "transitional state");
                    return core::Unit();
                }
                return core::make_unexpected(waited.error());
            }
            default:
                // Rust returns the raw StateConflict for any other state.
                return core::make_unexpected(
                    OrchestratorError::StoreOperationFailed(error.Message()));
        }
    }

    return DeleteSandboxImpl(id, previous_state, &progress);
}

// Rust `delete_sandbox_impl` — the Capture / Stop / Release phases.
OrchestratorResult<core::Unit>
Orchestrator::DeleteSandboxImpl(const core::SandboxId& id,
                                SandboxState previous_state,
                                DeleteProgress* progress) {
    core::Expected<core::Optional<SandboxMetadata>, StoreError> got = store_->Get(id);
    if (!got.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(got.error().Message()));
    }
    // Builder caches are optional: a capture failure must not block the delete.
    const bool optional_cache =
        got.value() ? got.value()->template_builder : false;
    const std::vector<std::string> volume_ids =
        got.value() ? VolumeIdsOf(*got.value()) : std::vector<std::string>();

    SandboxHandlePtr           handle;
    core::Optional<ProxyRoute> removed_route;
    DetachSandboxHandleAndRoute(id, &handle, &removed_route);

    bool                have_capture_error = false;
    sandbox::SandboxCaptureError capture_error =
        sandbox::SandboxCaptureError::Recoverable("");

    // ---- phase 1: capture ------------------------------------------------
    if (progress->kind == DeleteProgress::Kind::Capture) {
        bool volumes_frozen = false;
        bool capture_ok     = true;
        sandbox::SandboxCaptureError error =
            sandbox::SandboxCaptureError::Recoverable("");

        if (handle && previous_state == SandboxState::Running &&
            !volume_ids.empty()) {
            SandboxHandle::Guard sandbox = handle->Lock();
            sandbox::SandboxCaptureResult<core::Unit> frozen =
                sandbox->FreezeAndSnapshotVolumes();
            if (!frozen.ok()) {
                capture_ok = false;
                error      = frozen.error();
            } else {
                volumes_frozen = true;
            }
        }
        if (capture_ok) {
            OrchestratorResult<core::Unit> published =
                PublishSandboxVolumeBackings(id, volume_ids);
            if (!published.ok()) {
                capture_ok = false;
                // Rust wraps this in `SandboxCaptureError::recoverable`.
                error = sandbox::SandboxCaptureError::Recoverable(
                    published.error().Message());
            }
        }

        if (!capture_ok) {
            // A frozen filesystem must be thawed before the sandbox can keep
            // serving; a failed thaw escalates the error to terminal.
            if (volumes_frozen && !error.IsTerminal() && !optional_cache && handle) {
                SandboxHandle::Guard sandbox = handle->Lock();
                core::Expected<core::Unit, core::AnyError> thawed =
                    sandbox->ThawVolumes();
                if (!thawed.ok()) {
                    error = sandbox::SandboxCaptureError::Terminal(
                        "delete failed: " + error.Message() +
                        "; thaw failed: " + thawed.error().chain());
                }
            }

            if (!error.IsTerminal() && !optional_cache) {
                // Recoverable: put the sandbox back exactly as it was.
                if (handle) RegisterSandboxHandle(id, handle);
                RestoreProxyRoute(id, removed_route);

                std::vector<SandboxState> killing;
                killing.push_back(SandboxState::Killing);
                core::Expected<SandboxState, StoreError> restored =
                    store_->UpdateStateIfState(id, previous_state, killing);
                if (!restored.ok()) {
                    return core::make_unexpected(
                        OrchestratorError::StoreOperationFailed(
                            restored.error().Message()));
                }
                return core::make_unexpected(
                    OrchestratorError::SandboxOperationFailed(
                        id, SandboxOperation::Stop, error.Message()));
            }

            AGENTENV_WARN("volume capture failed; stopping sandbox and failing its "
                          "volumes: " << error.Message());
            have_capture_error = true;
            capture_error      = error;
        }

        *progress = DeleteProgress::Stop(have_capture_error);
        SetDeletionProgress(id, *progress);
    }

    // ---- phases 2 and 3: stop then release -------------------------------
    // Rust runs both inside one fallible block so a failure in either leaves
    // `progress` at the phase that still has work left.
    bool        cleanup_ok = true;
    std::string cleanup_error;

    if (progress->kind == DeleteProgress::Kind::Stop) {
        const bool capture_failed = progress->capture_failed;
        if (handle) {
            SandboxHandle::Guard sandbox = handle->Lock();
            core::Expected<core::Unit, core::AnyError> stopped = sandbox->Stop();
            if (!stopped.ok()) {
                cleanup_ok    = false;
                cleanup_error = stopped.error().chain();
            }
        }
        if (cleanup_ok) {
            *progress = DeleteProgress::Release(capture_failed);
            SetDeletionProgress(id, *progress);
        }
    }

    if (cleanup_ok && progress->kind == DeleteProgress::Kind::Release) {
        const bool capture_failed = progress->capture_failed;
        if (volume_manager_) {
            const std::string owner = id.ToString();
            // An incomplete restack may leave image.json pointing at old
            // layers. Never publish that backing as a successful capture.
            if (capture_failed) {
                core::Expected<core::Unit, volume::VolumeError> failed =
                    volume_manager_->FailBackings(owner, volume_ids);
                if (!failed.ok()) {
                    cleanup_ok    = false;
                    cleanup_error = failed.error().ToString();
                }
            }
            if (cleanup_ok) {
                core::Expected<core::Unit, volume::VolumeError> released =
                    volume_manager_->ReplaceOwnerFor(
                        owner, core::Optional<std::string>(), volume_ids);
                if (!released.ok()) {
                    cleanup_ok    = false;
                    cleanup_error = released.error().ToString();
                }
            }
        }
        if (cleanup_ok) {
            OrchestratorResult<core::Unit> finished = RemoveDeletedSandbox(id);
            if (!finished.ok()) {
                cleanup_ok    = false;
                cleanup_error = finished.error().Message();
            } else {
                // Rust writes Done into the `Arc<Mutex<DeleteProgress>>` it is
                // holding, but `remove_deleted_sandbox` has already dropped
                // that entry from `self.deletions`. The write therefore lands
                // on an orphaned Arc and no Done entry is left behind, so the
                // local value is updated without touching the map.
                *progress = DeleteProgress::Done();
            }
        }
    }

    if (!cleanup_ok) {
        // Rust re-inserts the handle only while still in the Stop phase, so a
        // retry can stop the sandbox again.
        if (progress->kind == DeleteProgress::Kind::Stop && handle) {
            RegisterSandboxHandle(id, handle);
        }
        AGENTENV_WARN("sandbox deletion needs a cleanup retry: " << cleanup_error);
        return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
            id, SandboxOperation::Stop, cleanup_error));
    }

    // A capture failure is still reported, unless the sandbox was an optional
    // builder cache.
    if (have_capture_error && !optional_cache) {
        return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
            id, SandboxOperation::Stop, capture_error.Message()));
    }
    return core::Unit();
}

// ---- shutdown --------------------------------------------------------------

// Rust `ensure_accepting_lifecycle_operations`.
OrchestratorResult<core::Unit>
Orchestrator::EnsureAcceptingLifecycleOperations() const {
    if (IsShuttingDown()) {
        return core::make_unexpected(OrchestratorError::ShuttingDown());
    }
    return core::Unit{};
}

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
