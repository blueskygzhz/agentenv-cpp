// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/pool.rs
//
// Watermark management sits on the ported WarmPool<T>; the four steps that make
// an entry warm (network slot, work dir, spawn into the namespace, wait for the
// API socket) are real. They need KVM only to run a guest -- spawning the VMM
// and waiting for its socket do not -- so `RealSpawn` is exercised directly and
// `SetSpawnForTesting` exists for the cases that cannot allocate a slot.
#include "agentenv/sandbox/firecracker/pool.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "agentenv/cfg.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/sandbox/firecracker/lifecycle.h"
#include "agentenv/sandbox/firecracker/socket.h"
#include "agentenv/sandbox/network/manager.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

namespace {

/// Rust `static POOL: OnceLock<Option<FirecrackerPool>>` — `g_pool_initialized`
/// is the `OnceLock`, `g_pool` the `Option`.
std::mutex g_pool_mu;
bool g_pool_initialized = false;
FirecrackerPool* g_pool = NULL;

/// Rust `firecracker_pool_exit_hook`.
void FirecrackerPoolExitHook() {
    FirecrackerPool* pool = NULL;
    {
        std::lock_guard<std::mutex> lg(g_pool_mu);
        pool = g_pool;
    }
    if (pool == NULL) return;
    const core::Expected<core::Unit, std::string> done = pool->Shutdown(true);
    if (!done.ok()) {
        AGENTENV_WARN("firecracker pool shutdown on process exit failed: " << done.error());
    }
}

/// Rust `try_init`.
core::Expected<FirecrackerPool*, std::string> TryInit() {
    const cfg::AppConfig* cfg = cfg::ConfigManager::GlobalConfig();
    if (cfg == NULL) return static_cast<FirecrackerPool*>(NULL);
    const core::Optional<cfg::ResolvedFirecrackerPoolConfig> pool_config =
        cfg->FirecrackerPoolConfig();
    if (!pool_config.has_value()) return static_cast<FirecrackerPool*>(NULL);

    // Rust `FirecrackerPool::new`.
    FirecrackerPool::Options options;
    options.pool = pool_config->pool;
    options.fill_concurrency = pool_config->fill_concurrency;
    options.work_base_dir = cfg->firecracker.work_dir;
    options.capture_output = LoggingEnabled(cfg->firecracker.log_level);
    options.firecracker_bin = cfg->ResolvedFirecrackerBinaryPath();
    options.socket_timeout_ms = static_cast<int64_t>(cfg->firecracker.socket_timeout_secs) * 1000;
    options.socket_poll_interval_ms = static_cast<int64_t>(cfg->firecracker.socket_poll_ms);

    if (std::atexit(FirecrackerPoolExitHook) != 0) {
        AGENTENV_WARN("failed to register firecracker pool process-exit hook");
    }
    return new FirecrackerPool(options, &network::SlotNetworkManager::Global(),
                               []() { return MakeUnixConnector(); });
}

}  // namespace

FirecrackerPool::FirecrackerPool(const Options& options,
                                 network::SlotNetworkManager* network,
                                 std::function<std::shared_ptr<Connector>()> connector_factory)
    : options_(options),
      network_(network),
      connector_factory_(connector_factory),
      pool_(options.pool) {
    // Rust's `join_all` over a batch of zero would make no progress.
    if (options_.fill_concurrency == 0) options_.fill_concurrency = 1;
}

FirecrackerPool::~FirecrackerPool() {
    // Rust's process-exit hook. Without this the warm processes outlive the
    // pool and keep their slots and work directories, which no later run can
    // identify as abandoned.
    Shutdown(true);
}

FirecrackerPool* FirecrackerPool::Global() {
    FirecrackerPool* pool = NULL;
    {
        std::lock_guard<std::mutex> lg(g_pool_mu);
        if (!g_pool_initialized) {
            g_pool_initialized = true;
            const core::Expected<FirecrackerPool*, std::string> made = TryInit();
            if (made.ok()) {
                g_pool = made.value();
            } else {
                AGENTENV_WARN("firecracker pool init failed; falling back to cold spawn: "
                              << made.error());
            }
        }
        pool = g_pool;
    }
    if (pool == NULL) return NULL;
    pool->EnsureWorkerStarted();
    return pool;
}

void FirecrackerPool::EnsureWorkerStarted() {
    pool_.StartMaintenanceWorker([this]() { (void)RunMaintenanceCycle(); });
}

core::Expected<core::Unit, std::string> FirecrackerPool::PrimeGlobal(int64_t timeout_ms) {
    FirecrackerPool* pool = Global();
    if (pool == NULL) {
        AGENTENV_DEBUG("firecracker pool disabled; skipping prime");
        return core::Unit();
    }
    pool->PrimeFor(timeout_ms);
    return core::Unit();
}

void FirecrackerPool::SetSpawnForTesting(SpawnFn spawn, CleanupFn cleanup) {
    spawn_ = spawn;
    cleanup_ = cleanup;
}

core::Expected<std::shared_ptr<WarmFirecracker>, std::string>
FirecrackerPool::Acquire() {
    return AcquireFor(options_.capture_output);
}

core::Expected<std::shared_ptr<WarmFirecracker>, std::string>
FirecrackerPool::AcquireFor(bool capture_output) {
    // Rust checks this before touching the pool. Every entry in it was spawned
    // with the same stdio destinations, so a mismatch cannot be satisfied by
    // looking further down the queue.
    if (capture_output != options_.capture_output) {
        return core::make_unexpected(
            std::string("firecracker pool: capture_output mismatch (cold path)"));
    }

    std::shared_ptr<WarmFirecracker> warm;
    if (!pool_.TryAcquire(&warm)) {
        return core::make_unexpected(std::string("firecracker pool empty (cold path)"));
    }

    // Rust requests the refill *after* handing the entry out, so the caller is
    // never made to wait behind a spawn it does not need.
    if (pool_.Len() < pool_.Config().low_watermark) {
        pool_.RequestMaintenance();
    }
    return warm;
}

std::size_t FirecrackerPool::WarmLen() const {
    return pool_.Len();
}

core::Expected<core::Unit, std::string> FirecrackerPool::CleanupWarm(
    const std::shared_ptr<WarmFirecracker>& warm, bool sync_network_cleanup) {
    return cleanup_ ? cleanup_(warm, sync_network_cleanup)
                    : RealCleanup(warm, sync_network_cleanup);
}

std::size_t FirecrackerPool::FillWarmEntries(std::size_t to_fill, std::string* error) {
    std::size_t created = 0;
    std::size_t remaining = to_fill;
    std::vector<std::string> cleanup_failures;
    while (remaining > 0 && !pool_.IsShuttingDown()) {
        // `remaining` tracks refill attempts left in this maintenance action.
        // On any create failure the current batch is finished, then no further
        // batch is launched -- a failing spawn is almost always a resource
        // that will not appear within this cycle (no free slot, no KVM).
        const std::size_t batch_size =
            remaining < options_.fill_concurrency ? remaining : options_.fill_concurrency;

        // Rust `join_all` over `create_warm_async`: the batch is created
        // concurrently, one thread per entry.
        typedef core::Expected<std::shared_ptr<WarmFirecracker>, std::string> Made;
        std::vector<Made> results(
            batch_size, core::make_unexpected(std::string("warm entry was not created")));
        std::vector<std::thread> workers;
        for (std::size_t i = 0; i < batch_size; ++i) {
            Made* slot = &results[i];
            try {
                workers.push_back(
                    std::thread([this, slot]() { *slot = spawn_ ? spawn_() : RealSpawn(); }));
            } catch (const std::system_error& err) {
                *slot = core::make_unexpected(std::string("spawn refill thread: ") + err.what());
            }
        }
        for (std::size_t i = 0; i < workers.size(); ++i) workers[i].join();

        bool saw_failure = false;
        for (std::size_t i = 0; i < results.size(); ++i) {
            if (!results[i].ok()) {
                saw_failure = true;
                AGENTENV_DEBUG("skipping firecracker pool refill attempt: " << results[i].error());
                continue;
            }
            if (pool_.TryPushBounded(results[i].value())) {
                ++created;
                continue;
            }
            // Hit the high watermark (or shutdown) while this batch was in
            // flight. The entry is destroyed rather than dropped: its slot and
            // work dir are live state nothing else holds a handle to.
            const core::Expected<core::Unit, std::string> gone =
                CleanupWarm(results[i].value(), false);
            if (!gone.ok()) {
                AGENTENV_WARN("firecracker pool: cleanup of unqueued warm entry failed: "
                              << gone.error());
                cleanup_failures.push_back(gone.error());
            }
        }

        if (saw_failure) break;
        remaining -= batch_size;
    }

    const core::Expected<core::Unit, std::string> result =
        FirecrackerPoolCleanupResult(cleanup_failures);
    if (!result.ok() && error != NULL) *error = result.error();
    return created;
}

std::size_t FirecrackerPool::RunMaintenanceCycle() {
    const warmpool::PoolMaintenanceAction action =
        pool_.ComputeMaintenanceAction(pool_.Len());
    std::string error;
    std::size_t count = 0;
    switch (action.kind) {
        case warmpool::PoolMaintenanceKind::Fill:
            count = FillWarmEntries(action.count, &error);
            break;
        case warmpool::PoolMaintenanceKind::Drain:
            for (std::size_t i = 0; i < action.count; ++i) {
                std::shared_ptr<WarmFirecracker> victim;
                if (!pool_.TryDrainOne(&victim)) break;
                // Rust `cleanup_warm_blocking(warm, false)?`: the first
                // failure ends the cycle.
                const core::Expected<core::Unit, std::string> gone = CleanupWarm(victim, false);
                if (!gone.ok()) {
                    error = gone.error();
                    break;
                }
                ++count;
            }
            break;
        case warmpool::PoolMaintenanceKind::Idle:
        default:
            break;
    }
    if (!error.empty()) {
        AGENTENV_WARN("firecracker pool maintenance cycle failed: " << error);
    }
    return count;
}

void FirecrackerPool::PrimeFor(int64_t timeout_ms) {
    const warmpool::PoolConfig& config = pool_.Config();
    if (!config.maintenance_enabled) {
        AGENTENV_DEBUG("firecracker pool maintenance disabled; skipping prime");
        return;
    }
    if (!config.startup_prewarm) {
        AGENTENV_DEBUG("firecracker pool startup prewarm disabled; skipping prime");
        return;
    }
    const std::size_t target = config.low_watermark;
    if (target == 0 || WarmLen() >= target) return;

    AGENTENV_INFO("priming firecracker pool low_watermark=" << target << " current="
                                                            << WarmLen()
                                                            << " timeout_ms=" << timeout_ms);
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    for (;;) {
        const int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count();
        if (WarmLen() >= target) {
            AGENTENV_INFO("firecracker pool primed warm=" << WarmLen()
                                                          << " elapsed_ms=" << elapsed_ms);
            return;
        }
        if (elapsed_ms >= timeout_ms) {
            AGENTENV_WARN("firecracker pool prime timed out; continuing with partial warm-up warm="
                          << WarmLen() << " target=" << target);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPoolPrimePollIntervalMs));
    }
}

void FirecrackerPool::Prime() {
    while (pool_.Len() < pool_.Config().low_watermark) {
        if (RunMaintenanceCycle() == 0) break;
    }
}

core::Expected<core::Unit, std::string>
FirecrackerPool::Shutdown(bool sync_network_cleanup) {
    const std::vector<std::shared_ptr<WarmFirecracker> > drained = pool_.DrainAll();
    std::vector<std::string> failures;
    for (std::size_t i = 0; i < drained.size(); ++i) {
        const core::Expected<core::Unit, std::string> gone =
            CleanupWarm(drained[i], sync_network_cleanup);
        if (!gone.ok()) failures.push_back(gone.error());
    }
    // Rust `firecracker_pool_cleanup_result`: every failure, joined.
    return FirecrackerPoolCleanupResult(failures);
}

core::Expected<std::shared_ptr<WarmFirecracker>, std::string>
FirecrackerPool::RealSpawn() {
    if (network_ == NULL) {
        return core::make_unexpected(
            std::string("firecracker pool: no network manager"));
    }

    // (1) The slot first, because the namespace it creates is what the process
    // must be spawned into -- it cannot be applied afterwards.
    const core::Expected<network::Slot*, std::string> slot = network_->AllocateAny();
    if (!slot.ok()) {
        return core::make_unexpected(
            std::string("firecracker pool: allocate network slot: ") + slot.error());
    }

    // (2) The work dir, which is where the API socket will be created. Rust
    // `release`s the untouched slot back to the network pool on failure.
    const core::Expected<std::string, std::string> work_dir =
        CreateFirecrackerWorkDir(options_.work_base_dir);
    if (!work_dir.ok()) {
        (void)network_->Release(slot.value());
        return core::make_unexpected(
            std::string("firecracker pool: create work dir: ") + work_dir.error());
    }

    std::unique_ptr<FirecrackerInstance> instance(new FirecrackerInstance(
        work_dir.value(), connector_factory_ ? connector_factory_() : NULL));

    core::Optional<std::string> stdout_path;
    core::Optional<std::string> stderr_path;
    WarmStdioPaths(work_dir.value(), options_.capture_output, &stdout_path, &stderr_path);

    // (3) Spawn into the slot's namespace, then (4) wait for the socket. Until
    // the wait returns the entry is not warm: handing it out earlier only
    // moves the wait into the request that receives it.
    core::Expected<core::Unit, std::string> spawn_result = instance->SpawnWithNetns(
        options_.firecracker_bin,
        stdout_path.has_value() ? *stdout_path : std::string(),
        stderr_path.has_value() ? *stderr_path : std::string(),
        slot.value()->NamespacePath());
    if (!spawn_result.ok()) {
        spawn_result = core::make_unexpected("spawn warm firecracker process: " +
                                             spawn_result.error());
    } else {
        spawn_result =
            instance->WaitForReady(options_.socket_timeout_ms, options_.socket_poll_interval_ms);
        if (!spawn_result.ok()) {
            spawn_result = core::make_unexpected("wait for warm firecracker api socket: " +
                                                 spawn_result.error());
        }
    }
    if (!spawn_result.ok()) {
        // Unwound in reverse, and the process stopped first: `Stop` removes the
        // API socket, which lives inside the work dir being deleted next.
        (void)instance->Stop(kPoolFirecrackerStopTimeoutMs);
        (void)network_->Release(slot.value());
        (void)core::fs::RemoveDirAll(work_dir.value());
        return core::make_unexpected(spawn_result.error());
    }

    AGENTENV_DEBUG("firecracker pool warm entry ready slot=" << slot.value()->Idx()
                                                             << " work_dir=" << work_dir.value());
    return std::shared_ptr<WarmFirecracker>(
        new WarmFirecracker(slot.value(), std::move(instance), work_dir.value()));
}

core::Expected<core::Unit, std::string>
FirecrackerPool::RealCleanup(const std::shared_ptr<WarmFirecracker>& warm,
                             bool sync_network_cleanup) {
    if (!warm) return core::Unit();
    const uint32_t slot_idx = warm->GetSlot() != NULL ? warm->GetSlot()->Idx() : 0;

    // Rust's order, and it is load-bearing: the socket lives in the work dir
    // and the tap device lives in the slot's namespace, so each step removes
    // something the next one is about to delete the container of.
    std::unique_ptr<FirecrackerInstance> instance = warm->TakeInstance();
    if (sync_network_cleanup) {
        // Process-exit path: Rust drops the instance rather than awaiting a
        // graceful stop; the destructor kills and reaps the child.
        instance.reset();
    } else if (instance) {
        const core::Expected<core::Unit, std::string> stopped =
            instance->Stop(kPoolFirecrackerStopTimeoutMs);
        if (!stopped.ok()) {
            AGENTENV_WARN("firecracker pool: stop warm process failed slot="
                          << slot_idx << ": " << stopped.error());
        }
        instance.reset();
    }

    // Rust `drop(work_dir)`: a `TempDir` drop, which ignores removal errors.
    (void)core::fs::RemoveDirAll(warm->WorkDir());

    if (network_ == NULL || warm->GetSlot() == NULL) return core::Unit();
    // `CleanupAllocatedSlot`, not `Release`: a warm entry being destroyed must
    // not return a namespace whose VMM just died to the slot pool.
    const core::Expected<core::Unit, std::string> torn =
        network_->CleanupAllocatedSlot(warm->GetSlot(), sync_network_cleanup);
    if (!torn.ok()) {
        return core::make_unexpected("firecracker pool: cleanup warm network slot: " +
                                     torn.error());
    }
    return core::Unit();
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
