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
      default_sandbox_timeout_ms_(15 * 60 * 1000),
      virtualization_mode_(core::VirtualizationModeDefault()),
      has_access_tokens_(false) {
    // Rust `default_fresh_sandbox_resources` leaves the disk size to be filled
    // from the backend's runtime info after the rootfs device is created.
    default_fresh_resources_.disk_size_mib = 0;
}

Orchestrator::Orchestrator(std::shared_ptr<MetadataStore> store,
                           std::shared_ptr<sandbox::SandboxBackendFactory> factory,
                           std::shared_ptr<SandboxPersister> persister)
    : store_(std::move(store)),
      factory_(std::move(factory)),
      persister_(std::move(persister)),
      next_proxy_route_version_(1),   // Rust: AtomicU64::new(1)
      is_shutting_down_(false),
      default_sandbox_timeout_ms_(15 * 60 * 1000),
      virtualization_mode_(core::VirtualizationModeDefault()),
      has_access_tokens_(false) {
    default_fresh_resources_.disk_size_mib = 0;
}

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

        // Rust: after claiming, execute the timeout action.
        if (metadata.timeout_action == SandboxTimeoutAction::Pause) {
            // Rust `pause_sandbox_impl` — the Pausing CAS was already done by
            // `claim_expired_running_sandbox`.
            OrchestratorResult<core::Unit> paused = PauseSandboxImpl(metadata.id);
            if (!paused.ok()) {
                AGENTENV_WARN("failed to auto-evict (pause) expired sandbox "
                              << metadata.id.ToString() << ": "
                              << paused.error().Message());
                continue;
            }
        } else {
            // Rust `delete_sandbox_impl(id, Running, &mut progress)`.
            DeleteProgress progress = DeletionProgress(metadata.id);
            OrchestratorResult<core::Unit> deleted =
                DeleteSandboxImpl(metadata.id, SandboxState::Running, &progress);
            if (!deleted.ok()) {
                AGENTENV_WARN("failed to auto-evict (delete) expired sandbox "
                              << metadata.id.ToString() << ": "
                              << deleted.error().Message());
                continue;
            }
        }
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

// ---- create ----------------------------------------------------------------

// Rust `create_sandbox`.
OrchestratorResult<SandboxMetadata>
Orchestrator::CreateSandbox(const CreateSandboxRequest& request) {
    return CreateSandboxInner(core::SandboxId::Fresh(), request, false);
}

// Rust `create_template_builder`.
OrchestratorResult<SandboxMetadata>
Orchestrator::CreateTemplateBuilder(const core::SandboxId& build_id,
                                    const CreateSandboxRequest& request) {
    return CreateSandboxInner(build_id, request, true);
}

// Rust `create_sandbox_inner`.
OrchestratorResult<SandboxMetadata>
Orchestrator::CreateSandboxInner(const core::SandboxId& sandbox_id,
                                 const CreateSandboxRequest& request,
                                 bool template_builder) {
    OrchestratorResult<core::Unit> accepting = EnsureAcceptingLifecycleOperations();
    if (!accepting.ok()) {
        counters_.RecordCreateFail(1);
        return core::make_unexpected(accepting.error());
    }

    const core::Optional<sandbox::EnvdAccessToken> envd_access_token =
        (request.secure && has_access_tokens_)
            ? core::Optional<sandbox::EnvdAccessToken>(
                  access_tokens_.Generate(sandbox_id))
            : core::Optional<sandbox::EnvdAccessToken>();

    AGENTENV_INFO("creating sandbox " << sandbox_id.ToString());

    const SandboxLaunchSource& source = request.source;

    // Rust builds the launch config and the transitional metadata per source
    // variant, then funnels both into `launch_sandbox`.
    sandbox::SandboxLaunchConfig launch_config;
    launch_config.sandbox_id              = sandbox_id;
    launch_config.env_vars                = request.env_vars;
    launch_config.envd_access_token       = envd_access_token;
    launch_config.extra_drives_in_snapshot = request.extra_drives_in_snapshot;

    SandboxMetadata metadata;
    metadata.id               = sandbox_id;
    metadata.template_builder = template_builder;
    metadata.state            = SandboxState::Creating;
    metadata.created_at_ms    = core::SystemTime::Now().unix_nanos / 1000000;
    metadata.timeout_action   = request.timeout_action;
    metadata.auto_resume      = request.auto_resume;
    metadata.user_metadata    = request.user_metadata;
    metadata.network_policy   = request.network_policy;
    metadata.volume_mounts    = request.volume_mounts;
    metadata.secure           = request.secure;
    metadata.context          = source.context;
    metadata.image_configs    = source.image_configs;

    // Rust `network_policy.runtime_policy()` — empty when nothing must be
    // installed in the guest.
    if (request.network_policy.RuntimePolicy()) {
        launch_config.network = core::Optional<std::string>(std::string("policy"));
    }

    LaunchPlan plan;

    if (source.kind == SandboxLaunchSource::Kind::Snapshot) {
        // A snapshot pins the virtualization mode it was captured under.
        if (source.snapshot_virtualization_mode != virtualization_mode_) {
            counters_.RecordCreateFail(1);
            return core::make_unexpected(
                OrchestratorError::VirtualizationModeMismatch(
                    "snapshot " + source.snapshot_id,
                    source.snapshot_virtualization_mode, virtualization_mode_));
        }

        // Effective custom config: a launch-provided value overrides the one
        // persisted in the source snapshot; otherwise inherit it. Storing the
        // effective value keeps the inherited config when a snapshot is later
        // published from this sandbox instead of dropping it.
        const core::Optional<sandbox::custom_extension::Params> effective_params =
            request.custom_extension_params
                ? request.custom_extension_params
                : source.snapshot_custom_extension_params;

        launch_config.snapshot_id = source.snapshot_id;
        launch_config.extra_drives.clear();
        for (std::size_t i = 0; i < request.extra_drives.size(); ++i) {
            launch_config.extra_drives.push_back(request.extra_drives[i].drive_id);
        }
        if (effective_params) {
            launch_config.custom_extension_params =
                core::Optional<std::string>(effective_params->json_bytes);
        }

        metadata.snapshot_id             = source.snapshot_id;
        metadata.snapshot_alias          = source.snapshot_alias;
        metadata.virtualization_mode     = source.snapshot_virtualization_mode;
        metadata.runtime_versions        = source.snapshot_runtime_versions;
        metadata.resources               = source.snapshot_resources;
        metadata.startup                 = source.snapshot_startup;
        metadata.custom_extension_params = effective_params;

        plan = LaunchPlan::ForCreateFromSnapshot(
            sandbox_id, source.snapshot_id, launch_config, metadata,
            NewTimeout::Set(request.timeout_ms ? *request.timeout_ms
                                               : default_sandbox_timeout_ms_));
    } else {
        // Rust: `extra_drives.extend(launch_extra_drives)` — the image's own
        // drives come first, then the launch-time ones.
        std::vector<sandbox::ExtraDrive> extra_drives = source.extra_drives;
        for (std::size_t i = 0; i < request.extra_drives.size(); ++i) {
            extra_drives.push_back(request.extra_drives[i]);
        }
        // Rust `resources.unwrap_or_else(default_fresh_sandbox_resources)`.
        const sandbox::SandboxResources resources =
            source.image_resources ? *source.image_resources
                                   : default_fresh_resources_;

        launch_config.snapshot_id = source.image_ref;
        launch_config.extra_drives.clear();
        for (std::size_t i = 0; i < extra_drives.size(); ++i) {
            launch_config.extra_drives.push_back(extra_drives[i].drive_id);
        }
        // Rust's fresh path does NOT inherit anything: the request's value is
        // the only source.
        if (request.custom_extension_params) {
            launch_config.custom_extension_params =
                core::Optional<std::string>(
                    request.custom_extension_params->json_bytes);
        }
        // Rust `extra_drives_in_snapshot` is hardcoded false on a fresh boot.
        launch_config.extra_drives_in_snapshot = false;

        sandbox::FreshSandboxBuildSpec build_spec;
        build_spec.image_config_path = source.overlaybd_config_path;
        build_spec.resources         = resources;
        build_spec.extra_boot_args   = source.extra_boot_args;
        for (std::size_t i = 0; i < extra_drives.size(); ++i) {
            build_spec.extra_drives.push_back(extra_drives[i].image_config_path);
        }

        metadata.snapshot_id             = source.image_ref;
        metadata.virtualization_mode     = virtualization_mode_;
        metadata.resources               = resources;
        metadata.custom_extension_params = request.custom_extension_params;

        plan = LaunchPlan::ForCreateFresh(
            sandbox_id, build_spec, launch_config, metadata,
            NewTimeout::Set(request.timeout_ms ? *request.timeout_ms
                                               : default_sandbox_timeout_ms_));
    }

    OrchestratorResult<SandboxMetadata> result = LaunchSandbox(plan);
    if (!result.ok()) {
        counters_.RecordCreateFail(1);
        return result;
    }
    counters_.RecordCreateSuccess(1);
    PublishSandboxEvent(SandboxLifecycleEventType::Create, result.value().id,
                        result.value().resources);
    return result;
}

// ---- pause -----------------------------------------------------------------

// Rust `join_concurrent_pause`.
OrchestratorResult<core::Unit>
Orchestrator::JoinConcurrentPause(const core::SandboxId& id) {
    AGENTENV_DEBUG("concurrent pause in progress, waiting for completion");
    OrchestratorResult<SandboxMetadata> waited =
        WaitForTransition(id, SandboxState::Pausing);
    if (!waited.ok()) return core::make_unexpected(waited.error());

    switch (waited.value().state) {
        case SandboxState::Paused:
            AGENTENV_DEBUG("concurrent pause succeeded");
            return core::Unit();
        case SandboxState::Running:
            AGENTENV_INFO("concurrent pause failed; sandbox returned to running state");
            return core::make_unexpected(
                OrchestratorError::InvalidSandboxState(id, SandboxState::Running));
        case SandboxState::Killing:
            AGENTENV_INFO("sandbox is being deleted after concurrent pause attempt");
            return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
        default:
            AGENTENV_INFO("unexpected state after waiting for concurrent pause: "
                          << SandboxStateName(waited.value().state));
            return core::make_unexpected(OrchestratorError::InvalidSandboxState(
                id, waited.value().state));
    }
}

// Rust `pause_sandbox`.
OrchestratorResult<core::Unit>
Orchestrator::PauseSandbox(const core::SandboxId& id) {
    return PauseSandboxInner(id);
}

// Rust `pause_sandbox_inner`.
OrchestratorResult<core::Unit>
Orchestrator::PauseSandboxInner(const core::SandboxId& id) {
    AGENTENV_INFO("pausing sandbox " << id.ToString());

    std::vector<SandboxState> running;
    running.push_back(SandboxState::Running);
    core::Expected<SandboxState, StoreError> claimed =
        store_->UpdateStateIfState(id, SandboxState::Pausing, running);

    if (!claimed.ok()) {
        const StoreError error = claimed.error();
        if (error.kind != StoreError::Kind::StateConflict) {
            if (error.kind == StoreError::Kind::SandboxNotFound) {
                return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
            }
            return core::make_unexpected(
                OrchestratorError::StoreOperationFailed(error.Message()));
        }
        switch (error.actual_state) {
            case SandboxState::Pausing:
                // Another task is already performing the pause.
                return JoinConcurrentPause(id);
            case SandboxState::Paused:
                return core::Unit();
            case SandboxState::Killing:
                AGENTENV_INFO("sandbox is being deleted while pausing");
                return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
            default:
                AGENTENV_INFO("cannot pause sandbox in current state "
                              << SandboxStateName(error.actual_state));
                return core::make_unexpected(OrchestratorError::InvalidSandboxState(
                    id, error.actual_state));
        }
    }

    return PauseSandboxImpl(id);
}

// Rust `pause_sandbox_impl`.
//
// Deviation: Rust first pins the paused runtime artifacts via
// `protect_image_refs(RuntimeImageOwner::PausedSandbox(..))` and restores
// Running if that fails. The runtime image-ref subsystem is not ported, so
// that guard (and its matching `release_image_refs` calls) has no counterpart.
OrchestratorResult<core::Unit>
Orchestrator::PauseSandboxImpl(const core::SandboxId& id) {
    // Allocate persistence space while the running handle and route are still
    // attached. Allocation does not mutate the backend, so a failure only has
    // to restore the metadata.
    core::Optional<std::string> artifact_root;
    if (persister_) {
        PersistenceResult<core::Optional<std::string> > allocated =
            persister_->AllocateArtifactRoot(id);
        if (!allocated.ok()) {
            AGENTENV_WARN("failed to allocate paused sandbox artifact root: "
                          << allocated.error().Message());
            std::vector<SandboxState> pausing;
            pausing.push_back(SandboxState::Pausing);
            store_->UpdateStateIfState(id, SandboxState::Running, pausing);
            return core::make_unexpected(
                OrchestratorError::SandboxPersistenceFailed(
                    allocated.error().Message()));
        }
        artifact_root = allocated.value();
    }

    SandboxHandlePtr           handle;
    core::Optional<ProxyRoute> removed_proxy_route;
    DetachSandboxHandleAndRoute(id, &handle, &removed_proxy_route);

    if (!handle) {
        AGENTENV_WARN("sandbox handle not found while pausing, removing from store");
        core::Expected<core::Optional<SandboxMetadata>, StoreError> got =
            store_->Get(id);
        if (!got.ok()) {
            return core::make_unexpected(
                OrchestratorError::StoreOperationFailed(got.error().Message()));
        }
        if (got.value()) FinalizeTerminalVolumes(*got.value());
        core::Expected<core::Optional<SandboxMetadata>, StoreError> removed =
            store_->Remove(id);
        if (!removed.ok()) {
            return core::make_unexpected(
                OrchestratorError::StoreOperationFailed(removed.error().Message()));
        }
        return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
    }

    // Pause the sandbox and capture the state needed to resume it later.
    sandbox::SandboxCaptureResult<std::shared_ptr<sandbox::PausedSandboxState> >
        paused_state_result = core::make_unexpected(
            sandbox::SandboxCaptureError::Recoverable(""));
    {
        SandboxHandle::Guard sandbox = handle->Lock();
        paused_state_result = sandbox->Pause(artifact_root);
    }

    if (!paused_state_result.ok()) {
        const sandbox::SandboxCaptureError error = paused_state_result.error();
        AGENTENV_WARN("failed to pause sandbox: " << error.Message());
        if (error.IsTerminal()) {
            // The handle was already detached before `Pause()`. Do not
            // reinsert it: the live runtime may have been mutated and is no
            // longer safe to keep serving as a running sandbox.
            {
                SandboxHandle::Guard sandbox = handle->Lock();
                core::Expected<core::Unit, core::AnyError> stopped = sandbox->Stop();
                if (!stopped.ok()) {
                    AGENTENV_WARN("failed to stop sandbox after terminal pause "
                                  "failure: " << stopped.error().chain());
                }
            }
            core::Expected<core::Optional<SandboxMetadata>, StoreError> got =
                store_->Get(id);
            if (!got.ok()) {
                return core::make_unexpected(
                    OrchestratorError::StoreOperationFailed(got.error().Message()));
            }
            if (got.value()) FinalizeTerminalVolumes(*got.value());
            core::Expected<core::Optional<SandboxMetadata>, StoreError> removed =
                store_->Remove(id);
            if (!removed.ok()) {
                return core::make_unexpected(
                    OrchestratorError::StoreOperationFailed(removed.error().Message()));
            }
        } else {
            RegisterSandboxHandle(id, handle);
            RestoreProxyRoute(id, removed_proxy_route);
            std::vector<SandboxState> pausing;
            pausing.push_back(SandboxState::Pausing);
            store_->UpdateStateIfState(id, SandboxState::Running, pausing);
        }
        return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
            id, SandboxOperation::Pause, error.Message()));
    }

    std::shared_ptr<sandbox::PausedSandboxState> paused_state =
        paused_state_result.value();

    core::Expected<core::Optional<SandboxMetadata>, StoreError> got = store_->Get(id);
    if (!got.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(got.error().Message()));
    }
    if (!got.value()) {
        return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
    }
    SandboxMetadata persisted_metadata = *got.value();
    persisted_metadata.state        = SandboxState::Paused;
    persisted_metadata.paused_state = paused_state;

    if (persister_) {
        PersistenceResult<core::Unit> persisted = persister_->PersistPaused(
            persisted_metadata, artifact_root, paused_state.get());
        if (!persisted.ok()) {
            AGENTENV_WARN("failed to persist paused sandbox state: "
                          << persisted.error().Message());
            // Try to bring the sandbox back; if even that fails the runtime is
            // unusable and the record has to go.
            bool resumed_ok = true;
            {
                SandboxHandle::Guard sandbox = handle->Lock();
                core::Expected<core::Unit, core::AnyError> resumed = sandbox->Resume();
                if (!resumed.ok()) {
                    resumed_ok = false;
                    AGENTENV_WARN("failed to resume sandbox after pause failure: "
                                  << resumed.error().chain());
                }
            }
            if (!resumed_ok) {
                {
                    SandboxHandle::Guard sandbox = handle->Lock();
                    core::Expected<core::Unit, core::AnyError> stopped = sandbox->Stop();
                    if (!stopped.ok()) {
                        AGENTENV_WARN("failed to stop sandbox after pause failure: "
                                      << stopped.error().chain());
                    }
                }
                core::Expected<core::Optional<SandboxMetadata>, StoreError> current =
                    store_->Get(id);
                if (current.ok() && current.value()) {
                    FinalizeTerminalVolumes(*current.value());
                }
                core::Expected<core::Optional<SandboxMetadata>, StoreError> removed =
                    store_->Remove(id);
                if (!removed.ok()) {
                    AGENTENV_WARN("failed to remove sandbox after pause failure: "
                                  << removed.error().Message());
                }
            } else {
                RegisterSandboxHandle(id, handle);
                RestoreProxyRoute(id, removed_proxy_route);
                std::vector<SandboxState> pausing;
                pausing.push_back(SandboxState::Pausing);
                store_->UpdateStateIfState(id, SandboxState::Running, pausing);
            }
            return core::make_unexpected(OrchestratorError::InternalError(
                "failed to persist paused sandbox state: " +
                persisted.error().Message()));
        }
    }

    const sandbox::SandboxResources resources = persisted_metadata.resources;
    core::Expected<core::Unit, StoreError> updated =
        store_->Update(persisted_metadata);
    if (!updated.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(updated.error().Message()));
    }

    // Stop the sandbox to free up resources. Rust only warns on failure: the
    // pause itself already succeeded and is recorded.
    {
        SandboxHandle::Guard sandbox = handle->Lock();
        core::Expected<core::Unit, core::AnyError> stopped = sandbox->Stop();
        if (!stopped.ok()) {
            AGENTENV_WARN("failed to stop sandbox after pausing: "
                          << stopped.error().chain());
        }
    }
    PublishSandboxEvent(SandboxLifecycleEventType::Pause, id, resources);
    AGENTENV_INFO("sandbox paused");
    return core::Unit();
}

// ---- resume ----------------------------------------------------------------

// Rust `join_concurrent_resume`.
OrchestratorResult<SandboxMetadata>
Orchestrator::JoinConcurrentResume(const core::SandboxId& id, NewTimeout timeout) {
    AGENTENV_DEBUG("concurrent resume in progress, waiting for completion");
    OrchestratorResult<SandboxMetadata> waited =
        WaitForTransition(id, SandboxState::Resuming);
    if (!waited.ok()) return core::make_unexpected(waited.error());

    switch (waited.value().state) {
        case SandboxState::Running:
            return MaybeUpdateRunningTimeout(id, timeout);
        case SandboxState::Paused:
            AGENTENV_INFO("concurrent resume failed; sandbox returned to paused state");
            return core::make_unexpected(
                OrchestratorError::InvalidSandboxState(id, SandboxState::Paused));
        case SandboxState::Killing:
            AGENTENV_INFO("sandbox is being deleted while resuming");
            return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
        default:
            AGENTENV_INFO("unexpected state after waiting for concurrent resume: "
                          << SandboxStateName(waited.value().state));
            return core::make_unexpected(OrchestratorError::InvalidSandboxState(
                id, waited.value().state));
    }
}

// Rust `resume_sandbox`.
OrchestratorResult<SandboxMetadata>
Orchestrator::ResumeSandbox(const core::SandboxId& id, NewTimeout timeout) {
    return ResumeSandboxInner(id, timeout);
}

// Rust `resume_sandbox_inner`.
OrchestratorResult<SandboxMetadata>
Orchestrator::ResumeSandboxInner(const core::SandboxId& id, NewTimeout timeout) {
    OrchestratorResult<core::Unit> accepting = EnsureAcceptingLifecycleOperations();
    if (!accepting.ok()) return core::make_unexpected(accepting.error());

    AGENTENV_INFO("resuming sandbox " << id.ToString());

    core::Expected<core::Optional<SandboxMetadata>, StoreError> got = store_->Get(id);
    if (!got.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(got.error().Message()));
    }
    if (!got.value()) {
        return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
    }
    SandboxMetadata metadata = *got.value();

    // If another resume is in progress, wait for it and re-evaluate.
    if (metadata.state == SandboxState::Resuming) {
        OrchestratorResult<SandboxMetadata> waited =
            WaitForTransition(id, SandboxState::Resuming);
        if (!waited.ok()) return core::make_unexpected(waited.error());
        metadata = waited.value();
    }

    switch (metadata.state) {
        case SandboxState::Killing:
            return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
        case SandboxState::Running:
            // Already running — just apply the timeout and return.
            return MaybeUpdateRunningTimeout(id, timeout);
        case SandboxState::Paused:
            break;
        default:
            return core::make_unexpected(
                OrchestratorError::InvalidSandboxState(id, metadata.state));
    }

    if (metadata.virtualization_mode != virtualization_mode_) {
        return core::make_unexpected(OrchestratorError::VirtualizationModeMismatch(
            "paused sandbox " + id.ToString(), metadata.virtualization_mode,
            virtualization_mode_));
    }

    std::vector<SandboxState> paused;
    paused.push_back(SandboxState::Paused);
    core::Expected<SandboxState, StoreError> claimed =
        store_->UpdateStateIfState(id, SandboxState::Resuming, paused);
    if (!claimed.ok()) {
        const StoreError error = claimed.error();
        if (error.kind != StoreError::Kind::StateConflict) {
            if (error.kind == StoreError::Kind::SandboxNotFound) {
                return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
            }
            return core::make_unexpected(
                OrchestratorError::StoreOperationFailed(error.Message()));
        }
        switch (error.actual_state) {
            case SandboxState::Running:
                // Another task already completed the resume.
                return MaybeUpdateRunningTimeout(id, timeout);
            case SandboxState::Resuming:
                // A second concurrent resume snuck in between our state read
                // and the CAS. Wait for it and return its outcome.
                return JoinConcurrentResume(id, timeout);
            case SandboxState::Killing:
                AGENTENV_INFO("sandbox is being deleted while resuming");
                return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
            default:
                AGENTENV_INFO("cannot resume sandbox in current state "
                              << SandboxStateName(error.actual_state));
                return core::make_unexpected(OrchestratorError::InvalidSandboxState(
                    id, error.actual_state));
        }
    }

    if (persister_) {
        PersistenceResult<core::Unit> marked = persister_->MarkResuming(id);
        if (!marked.ok()) {
            AGENTENV_WARN("failed to mark persisted sandbox record as resuming: "
                          << marked.error().Message());
            std::vector<SandboxState> resuming;
            resuming.push_back(SandboxState::Resuming);
            store_->UpdateStateIfState(id, SandboxState::Paused, resuming);
            return core::make_unexpected(OrchestratorError::InternalError(
                "failed to mark persisted sandbox record as resuming: " +
                marked.error().Message()));
        }
    }

    if (!metadata.paused_state) {
        AGENTENV_WARN("missing paused state while resuming");
        // Rust returns InternalError via `ok_or_else`, leaving the sandbox in
        // Resuming for the caller to reconcile. The port keeps that behaviour.
        return core::make_unexpected(
            OrchestratorError::InternalError("missing paused state"));
    }

    OrchestratorResult<SandboxMetadata> resumed = LaunchSandbox(LaunchPlan::ForResume(
        id, metadata.paused_state, timeout, metadata.resources,
        (metadata.secure && has_access_tokens_)
            ? core::Optional<sandbox::EnvdAccessToken>(access_tokens_.Generate(id))
            : core::Optional<sandbox::EnvdAccessToken>()));
    if (resumed.ok()) {
        PublishSandboxEvent(SandboxLifecycleEventType::Resume, resumed.value().id,
                            resumed.value().resources);
    }
    return resumed;
}

// ---- fork ------------------------------------------------------------------

// Rust `fork_child_error`.
OrchestratorError Orchestrator::ForkChildError(const core::SandboxId& id,
                                               const std::string& source) {
    return OrchestratorError::SandboxOperationFailed(
        id, SandboxOperation::Fork, source);
}

// Rust `stop_failed_fork` — the backend is consumed, so a failure only warns.
void Orchestrator::StopFailedFork(
    std::unique_ptr<sandbox::SandboxBackend> backend,
    const core::SandboxId& id) {
    if (!backend) return;
    core::Expected<core::Unit, core::AnyError> stopped = backend->Stop();
    if (!stopped.ok()) {
        AGENTENV_WARN("failed to stop unsuccessful fork " << id.ToString()
                      << ": " << stopped.error().chain());
    }
}

// Rust `fork_sandbox`.
OrchestratorResult<std::vector<SandboxForkOutcome> >
Orchestrator::ForkSandbox(const core::SandboxId& source_id, uint32_t count,
                          NewTimeout timeout) {
    // Rust refuses the simple form when the source has volume mounts: the
    // children would silently share the parent's volumes.
    core::Expected<core::Optional<SandboxMetadata>, StoreError> source =
        store_->Get(source_id);
    if (!source.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(source.error().Message()));
    }
    if (source.value() && !source.value()->volume_mounts.empty()) {
        return core::make_unexpected(OrchestratorError::InternalError(
            "fork with volume mounts requires volume-aware child specs"));
    }

    std::vector<SandboxForkChildSpec> child_specs;
    for (uint32_t i = 0; i < count; ++i) {
        SandboxForkChildSpec spec;
        spec.sandbox_id = core::SandboxId::Fresh();
        child_specs.push_back(spec);
    }
    return ForkSandboxWithSpecs(source_id, child_specs, timeout);
}

// Rust `fork_sandbox_with_specs`.
OrchestratorResult<std::vector<SandboxForkOutcome> >
Orchestrator::ForkSandboxWithSpecs(
    const core::SandboxId& source_id,
    const std::vector<SandboxForkChildSpec>& child_specs,
    NewTimeout timeout) {
    return ForkSandboxInner(source_id, child_specs, timeout);
}

// Rust `fork_sandbox_inner`.
OrchestratorResult<std::vector<SandboxForkOutcome> >
Orchestrator::ForkSandboxInner(
    const core::SandboxId& source_id,
    const std::vector<SandboxForkChildSpec>& child_specs,
    NewTimeout timeout) {
    OrchestratorResult<core::Unit> accepting = EnsureAcceptingLifecycleOperations();
    if (!accepting.ok()) return core::make_unexpected(accepting.error());

    const uint32_t count = static_cast<uint32_t>(child_specs.size());

    // Every child id must be unique, differ from the source, and not already
    // exist in the store.
    std::set<std::string> seen;
    for (std::size_t i = 0; i < child_specs.size(); ++i) {
        const core::SandboxId& child_id = child_specs[i].sandbox_id;
        if (child_id == source_id || !seen.insert(child_id.ToString()).second) {
            return core::make_unexpected(OrchestratorError::InternalError(
                "fork child sandbox IDs must be unique and differ from the source"));
        }
        core::Expected<core::Optional<SandboxMetadata>, StoreError> existing =
            store_->Get(child_id);
        if (!existing.ok()) {
            return core::make_unexpected(
                OrchestratorError::StoreOperationFailed(existing.error().Message()));
        }
        if (existing.value()) {
            return core::make_unexpected(OrchestratorError::InternalError(
                "fork child sandbox " + child_id.ToString() + " already exists"));
        }
    }

    AGENTENV_INFO("forking sandboxes from " << source_id.ToString());

    SandboxHandlePtr source_handle = SandboxHandleFor(source_id);
    if (!source_handle) {
        return core::make_unexpected(OrchestratorError::SandboxNotFound(source_id));
    }

    // Claim Forking from Running; Rust keeps the *previous* record as the
    // template for every child.
    std::vector<SandboxState> running;
    running.push_back(SandboxState::Running);
    core::Expected<MetadataUpdateResult, StoreError> claimed = store_->UpdateIfState(
        source_id, running,
        [](SandboxMetadata* m) { m->state = SandboxState::Forking; });
    if (!claimed.ok()) {
        const StoreError error = claimed.error();
        if (error.kind == StoreError::Kind::StateConflict) {
            if (error.actual_state == SandboxState::Killing) {
                return core::make_unexpected(
                    OrchestratorError::SandboxNotFound(source_id));
            }
            return core::make_unexpected(OrchestratorError::InvalidSandboxState(
                source_id, error.actual_state));
        }
        if (error.kind == StoreError::Kind::SandboxNotFound) {
            return core::make_unexpected(OrchestratorError::SandboxNotFound(source_id));
        }
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(error.Message()));
    }
    const SandboxMetadata source_metadata = claimed.value().previous;

    std::vector<sandbox::SandboxForkSpec> backend_specs;
    for (std::size_t i = 0; i < child_specs.size(); ++i) {
        sandbox::SandboxForkSpec spec;
        spec.sandbox_id = child_specs[i].sandbox_id;
        if (source_metadata.secure && has_access_tokens_) {
            spec.envd_access_token = core::Optional<sandbox::EnvdAccessToken>(
                access_tokens_.Generate(child_specs[i].sandbox_id));
        }
        spec.extra_drives.clear();
        for (std::size_t d = 0; d < child_specs[i].extra_drives.size(); ++d) {
            spec.extra_drives.push_back(child_specs[i].extra_drives[d].drive_id);
        }
        spec.replace_drive_ids = child_specs[i].replace_drive_ids;
        backend_specs.push_back(spec);
    }

    // One backend call returns a per-child result vector.
    sandbox::SandboxCaptureResult<std::vector<sandbox::SandboxForkResult> >
        fork_result = core::make_unexpected(
            sandbox::SandboxCaptureError::Recoverable(""));
    {
        SandboxHandle::Guard sandbox = source_handle->Lock();
        fork_result = sandbox->Fork(backend_specs);
    }

    if (!fork_result.ok()) {
        const sandbox::SandboxCaptureError error = fork_result.error();
        AGENTENV_WARN("failed to fork sandbox: " << error.Message());
        counters_.RecordCreateFail(count);
        if (error.IsTerminal()) {
            // The source runtime is no longer trustworthy.
            SandboxHandlePtr           detached;
            core::Optional<ProxyRoute> route;
            DetachSandboxHandleAndRoute(source_id, &detached, &route);
            {
                SandboxHandle::Guard sandbox = source_handle->Lock();
                sandbox->Stop();
            }
            FinalizeTerminalVolumes(source_metadata);
            core::Expected<core::Optional<SandboxMetadata>, StoreError> removed =
                store_->Remove(source_id);
            if (!removed.ok()) {
                return core::make_unexpected(
                    OrchestratorError::StoreOperationFailed(removed.error().Message()));
            }
        } else {
            std::vector<SandboxState> forking;
            forking.push_back(SandboxState::Forking);
            store_->UpdateStateIfState(source_id, SandboxState::Running, forking);
        }
        return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
            source_id, SandboxOperation::Fork, error.Message()));
    }

    // Restore the source sandbox to Running; Rust only warns on failure.
    {
        std::vector<SandboxState> forking;
        forking.push_back(SandboxState::Forking);
        core::Expected<SandboxState, StoreError> restored =
            store_->UpdateStateIfState(source_id, SandboxState::Running, forking);
        if (!restored.ok()) {
            AGENTENV_WARN("failed to restore source sandbox metadata after fork: "
                          << restored.error().Message());
        }
    }

    std::vector<sandbox::SandboxForkResult>& forked = fork_result.value();
    std::vector<SandboxForkOutcome> outcomes;
    uint64_t successes = 0;
    const int64_t now_ms = core::SystemTime::Now().unix_nanos / 1000000;

    for (std::size_t i = 0; i < child_specs.size() && i < forked.size(); ++i) {
        const core::SandboxId child_id = child_specs[i].sandbox_id;
        if (!forked[i].ok()) {
            AGENTENV_WARN("failed to start forked sandbox " << child_id.ToString()
                          << ": " << forked[i].error().chain());
            outcomes.push_back(core::make_unexpected(
                ForkChildError(child_id, forked[i].error().chain())));
            continue;
        }
        std::unique_ptr<sandbox::SandboxBackend> backend =
            std::move(forked[i].value());

        // Rust clones the source record and overrides the per-child fields.
        SandboxMetadata metadata = source_metadata;
        metadata.id            = child_id;
        metadata.state         = SandboxState::Running;
        metadata.created_at_ms = now_ms;
        metadata.paused_state.reset();
        metadata.volume_mounts = child_specs[i].volume_mounts;
        metadata.UpdateTimeout(timeout);

        OrchestratorResult<ProxyTarget> target =
            ProxyTargetFromSandbox(*backend);
        if (!target.ok()) {
            StopFailedFork(std::move(backend), child_id);
            outcomes.push_back(core::make_unexpected(
                ForkChildError(child_id, target.error().Message())));
            continue;
        }

        core::Expected<core::Unit, StoreError> added = store_->Add(metadata);
        if (!added.ok()) {
            AGENTENV_WARN("failed to register forked sandbox " << child_id.ToString()
                          << ": " << added.error().Message());
            StopFailedFork(std::move(backend), child_id);
            outcomes.push_back(core::make_unexpected(
                ForkChildError(child_id, added.error().Message())));
            continue;
        }

        RegisterSandboxHandle(child_id,
                              SandboxHandlePtr(new SandboxHandle(std::move(backend))));
        UpsertProxyRoute(child_id, target.value());
        PublishSandboxEvent(SandboxLifecycleEventType::Fork, metadata.id,
                            metadata.resources);
        ++successes;
        outcomes.push_back(metadata);
    }

    counters_.RecordCreateSuccess(successes);
    counters_.RecordCreateFail(static_cast<uint64_t>(count) - successes);
    return outcomes;
}

// ---- token helpers ---------------------------------------------------------

// Rust `get_envd_access_token` — `metadata.secure.then(|| ..)`.
core::Optional<sandbox::EnvdAccessToken>
Orchestrator::GetEnvdAccessToken(const SandboxMetadata& metadata) const {
    if (!metadata.secure || !has_access_tokens_) {
        return core::Optional<sandbox::EnvdAccessToken>();
    }
    return core::Optional<sandbox::EnvdAccessToken>(
        access_tokens_.Generate(metadata.id));
}

// Rust `validate_envd_access_token`.
bool Orchestrator::ValidateEnvdAccessToken(const core::SandboxId& id,
                                            const std::string& candidate) const {
    if (!has_access_tokens_) return false;
    return access_tokens_.Matches(id, candidate);
}

// Rust `traffic_access_token`.
std::string Orchestrator::TrafficAccessToken(const core::SandboxId& id) const {
    if (!has_access_tokens_) return std::string();
    return access_tokens_.GenerateTraffic(id);
}

// Rust `validate_traffic_access_token`.
bool Orchestrator::ValidateTrafficAccessToken(const core::SandboxId& id,
                                               const std::string& candidate) const {
    if (!has_access_tokens_) return false;
    return access_tokens_.MatchesTraffic(id, candidate);
}

// ---- snapshot operations ---------------------------------------------------

// Rust `begin_snapshot_operation`.
OrchestratorResult<SandboxHandlePtr>
Orchestrator::BeginSnapshotOperation(const core::SandboxId& id) {
    OrchestratorResult<core::Unit> accepting = EnsureAcceptingLifecycleOperations();
    if (!accepting.ok()) return core::make_unexpected(accepting.error());

    std::vector<SandboxState> running;
    running.push_back(SandboxState::Running);
    core::Expected<SandboxState, StoreError> claimed =
        store_->UpdateStateIfState(id, SandboxState::Snapshotting, running);
    if (!claimed.ok()) {
        const StoreError error = claimed.error();
        if (error.kind == StoreError::Kind::StateConflict) {
            if (error.actual_state == SandboxState::Killing) {
                return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
            }
            return core::make_unexpected(
                OrchestratorError::InvalidSandboxState(id, error.actual_state));
        }
        if (error.kind == StoreError::Kind::SandboxNotFound) {
            return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
        }
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(error.Message()));
    }

    SandboxHandlePtr handle = SandboxHandleFor(id);
    if (handle) return handle;

    // The record claims the sandbox is running but no handle is registered.
    AGENTENV_WARN("sandbox handle not found while snapshotting, removing from store");
    SandboxHandlePtr           detached;
    core::Optional<ProxyRoute> route;
    DetachSandboxHandleAndRoute(id, &detached, &route);

    core::Expected<core::Optional<SandboxMetadata>, StoreError> got = store_->Get(id);
    if (!got.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(got.error().Message()));
    }
    if (got.value()) FinalizeTerminalVolumes(*got.value());
    core::Expected<core::Optional<SandboxMetadata>, StoreError> removed =
        store_->Remove(id);
    if (!removed.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(removed.error().Message()));
    }
    return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
}

// Rust `fail_snapshot_operation`.
OrchestratorError
Orchestrator::FailSnapshotOperation(const core::SandboxId& id,
                                    const SandboxHandlePtr& handle,
                                    const sandbox::SandboxCaptureError& error,
                                    SandboxOperation operation) {
    AGENTENV_WARN("sandbox snapshot operation " << SandboxOperationName(operation)
                  << " failed: " << error.Message());
    if (error.IsTerminal()) {
        SandboxHandlePtr           detached;
        core::Optional<ProxyRoute> route;
        DetachSandboxHandleAndRoute(id, &detached, &route);
        if (handle) {
            SandboxHandle::Guard sandbox = handle->Lock();
            core::Expected<core::Unit, core::AnyError> stopped = sandbox->Stop();
            if (!stopped.ok()) {
                AGENTENV_WARN("failed to stop sandbox after terminal snapshot "
                              "failure: " << stopped.error().chain());
            }
        }
        core::Expected<core::Optional<SandboxMetadata>, StoreError> got =
            store_->Get(id);
        if (got.ok() && got.value()) FinalizeTerminalVolumes(*got.value());
        store_->Remove(id);
    } else {
        std::vector<SandboxState> snapshotting;
        snapshotting.push_back(SandboxState::Snapshotting);
        store_->UpdateStateIfState(id, SandboxState::Running, snapshotting);
    }
    return OrchestratorError::SandboxOperationFailed(id, operation, error.Message());
}

// Rust `finish_snapshot_operation`.
OrchestratorResult<core::Unit>
Orchestrator::FinishSnapshotOperation(const core::SandboxId& id) {
    std::vector<SandboxState> snapshotting;
    snapshotting.push_back(SandboxState::Snapshotting);
    core::Expected<SandboxState, StoreError> restored =
        store_->UpdateStateIfState(id, SandboxState::Running, snapshotting);
    if (!restored.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(restored.error().Message()));
    }
    return core::Unit();
}

// Rust `snapshot_volume_mounts` / `snapshot_volume_mounts_inner`.
OrchestratorResult<core::Unit>
Orchestrator::SnapshotVolumeMounts(const core::SandboxId& id) {
    OrchestratorResult<SandboxHandlePtr> handle = BeginSnapshotOperation(id);
    if (!handle.ok()) return core::make_unexpected(handle.error());

    sandbox::SandboxCaptureResult<core::Unit> result = core::Unit();
    {
        SandboxHandle::Guard sandbox = handle.value()->Lock();
        result = sandbox->SnapshotVolumes();
    }
    if (!result.ok()) {
        return core::make_unexpected(FailSnapshotOperation(
            id, handle.value(), result.error(), SandboxOperation::SnapshotVolumes));
    }
    return FinishSnapshotOperation(id);
}

// Rust `capture_snapshot` / `capture_snapshot_inner`.
OrchestratorResult<SnapshotCaptureResult>
Orchestrator::CaptureSnapshot(const core::SandboxId& id) {
    AGENTENV_INFO("capturing sandbox snapshot for " << id.ToString());

    OrchestratorResult<SandboxHandlePtr> handle = BeginSnapshotOperation(id);
    if (!handle.ok()) return core::make_unexpected(handle.error());

    sandbox::SandboxCaptureResult<std::string> result =
        core::make_unexpected(sandbox::SandboxCaptureError::Recoverable(""));
    {
        SandboxHandle::Guard sandbox = handle.value()->Lock();
        result = sandbox->Snapshot();
    }
    if (!result.ok()) {
        return core::make_unexpected(FailSnapshotOperation(
            id, handle.value(), result.error(), SandboxOperation::Snapshot));
    }

    OrchestratorResult<core::Unit> finished = FinishSnapshotOperation(id);
    if (!finished.ok()) return core::make_unexpected(finished.error());

    core::Expected<core::Optional<SandboxMetadata>, StoreError> got = store_->Get(id);
    if (!got.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(got.error().Message()));
    }
    if (!got.value()) {
        AGENTENV_WARN("sandbox disappeared after snapshotting");
        return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
    }

    AGENTENV_INFO("snapshot captured");
    SnapshotCaptureResult captured;
    captured.metadata          = *got.value();
    captured.captured_snapshot = result.value();
    return captured;
}

// ---- hot-update operations -------------------------------------------------

// Rust `replace_sandbox_network_policy` / `_inner`.
OrchestratorResult<core::Unit>
Orchestrator::ReplaceSandboxNetworkPolicy(
    const core::SandboxId& id,
    const sandbox::network::SandboxNetworkPolicy& policy) {
    core::Expected<core::Optional<SandboxMetadata>, StoreError> got = store_->Get(id);
    if (!got.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(got.error().Message()));
    }
    if (!got.value()) {
        return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
    }
    const SandboxMetadata metadata = *got.value();
    if (metadata.state != SandboxState::Running) {
        return core::make_unexpected(
            OrchestratorError::InvalidSandboxState(id, metadata.state));
    }

    // Rust: `allow_public_traffic` is not user-updatable — it is fixed at
    // create time, so the stored value always wins.
    sandbox::network::SandboxNetworkPolicy effective = policy;
    effective.allow_public_traffic = metadata.network_policy.allow_public_traffic;

    SandboxHandlePtr handle = SandboxHandleFor(id);
    if (!handle) {
        return core::make_unexpected(OrchestratorError::SandboxOperationConflict(
            id, SandboxOperation::UpdateNetwork));
    }

    // Rust passes `runtime_policy()` (empty when nothing needs installing).
    core::Optional<std::string> runtime_policy;
    if (effective.RuntimePolicy()) {
        runtime_policy = core::Optional<std::string>(std::string("policy"));
    }

    core::Expected<core::Unit, core::AnyError> updated =
        core::make_unexpected(core::err(""));
    {
        SandboxHandle::Guard sandbox = handle->Lock();
        updated = sandbox->UpdateNetworkPolicy(runtime_policy);
    }
    if (!updated.ok()) {
        return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
            id, SandboxOperation::UpdateNetwork, updated.error().chain()));
    }

    std::vector<SandboxState> running;
    running.push_back(SandboxState::Running);
    core::Expected<MetadataUpdateResult, StoreError> stored = store_->UpdateIfState(
        id, running,
        [effective](SandboxMetadata* m) { m->network_policy = effective; });
    if (!stored.ok()) {
        if (stored.error().kind == StoreError::Kind::StateConflict) {
            return core::make_unexpected(OrchestratorError::InvalidSandboxState(
                id, stored.error().actual_state));
        }
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(stored.error().Message()));
    }
    return core::Unit();
}

// Rust `patch_sandbox_custom_extension_params` / `_inner`.
OrchestratorResult<core::Optional<sandbox::custom_extension::Params> >
Orchestrator::PatchSandboxCustomExtensionParams(
    const core::SandboxId& id,
    const sandbox::custom_extension::Params& patch) {
    core::Expected<core::Optional<SandboxMetadata>, StoreError> got = store_->Get(id);
    if (!got.ok()) {
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(got.error().Message()));
    }
    if (!got.value()) {
        return core::make_unexpected(OrchestratorError::SandboxNotFound(id));
    }
    if (got.value()->state != SandboxState::Running) {
        return core::make_unexpected(
            OrchestratorError::InvalidSandboxState(id, got.value()->state));
    }

    SandboxHandlePtr handle = SandboxHandleFor(id);
    if (!handle) {
        return core::make_unexpected(OrchestratorError::SandboxOperationConflict(
            id, SandboxOperation::PatchCustomExtensionParams));
    }

    // Rust `CustomExtensionClient::global().ok_or_else(..)`.
    if (!custom_extension_client_) {
        return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
            id, SandboxOperation::PatchCustomExtensionParams,
            "custom extension is not configured ([custom_extension].url is unset)"));
    }

    // The sandbox lock is deliberately NOT held across the hook call so that
    // pause/stop are not blocked on extension latency.
    core::Expected<core::Optional<sandbox::custom_extension::Params>, core::AnyError>
        hooked = custom_extension_client_->HookPatchParams(id, patch);
    if (!hooked.ok()) {
        return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
            id, SandboxOperation::PatchCustomExtensionParams,
            hooked.error().chain()));
    }
    const core::Optional<sandbox::custom_extension::Params> new_params =
        hooked.value();

    {
        SandboxHandle::Guard sandbox = handle->Lock();
        sandbox->UpdateCustomExtensionParams(
            new_params ? core::Optional<std::string>(new_params->json_bytes)
                       : core::Optional<std::string>());
    }

    // NOTE: a concurrent pause may have transitioned the sandbox since the
    // entry check, so this may fail. Rust accepts that: extension state is
    // transient, like the network policy.
    std::vector<SandboxState> running;
    running.push_back(SandboxState::Running);
    core::Expected<MetadataUpdateResult, StoreError> stored = store_->UpdateIfState(
        id, running,
        [new_params](SandboxMetadata* m) { m->custom_extension_params = new_params; });
    if (!stored.ok()) {
        // Lost a race against a concurrent state transition (e.g. pause):
        // report it as a conflict instead of a 500.
        if (stored.error().kind == StoreError::Kind::StateConflict) {
            return core::make_unexpected(OrchestratorError::InvalidSandboxState(
                id, stored.error().actual_state));
        }
        return core::make_unexpected(
            OrchestratorError::StoreOperationFailed(stored.error().Message()));
    }
    return new_params;
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
