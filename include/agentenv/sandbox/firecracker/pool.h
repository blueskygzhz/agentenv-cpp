// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/pool.rs.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_POOL_H_
#define AGENTENV_SANDBOX_FIRECRACKER_POOL_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/firecracker/config.h"
#include "agentenv/sandbox/firecracker/instance.h"
// `kPoolFirecrackerStopTimeoutMs`, `kPoolPrimePollIntervalMs`, `WarmStdioPaths`.
#include "agentenv/sandbox/firecracker/lifecycle.h"
#include "agentenv/sandbox/network/slot.h"
#include "agentenv/warm-pool/pool.h"

namespace agentenv {
namespace sandbox {
namespace network {
class SlotNetworkManager;
}  // namespace network

namespace firecracker {

/// Rust `struct WarmFirecracker` — one warm entry, handed to a sandbox as an
/// indivisible ownership unit.
///
/// The three members cannot be separated. The VMM process was spawned *inside*
/// `slot`'s network namespace and its tap device belongs to that slot, so a
/// process paired with a different slot has no connectivity; `work_dir` holds
/// the API socket that process is listening on. Handing out any one of them
/// alone produces a VM that looks healthy and is not reachable.
///
/// Ownership transfers on acquire: a warm entry is never returned to the pool.
/// By the time a sandbox is done with it the process has loaded a snapshot and
/// run guest code, so it is no longer a blank VM and cannot be given to the
/// next tenant. `Cleanup` destroys all three.
class WarmFirecracker {
 public:
    WarmFirecracker(network::Slot* slot, std::unique_ptr<FirecrackerInstance> instance,
                    const std::string& work_dir)
        : slot_(slot), instance_(std::move(instance)), work_dir_(work_dir) {}

    network::Slot* GetSlot() const { return slot_; }
    FirecrackerInstance* GetInstance() const { return instance_.get(); }
    const std::string& WorkDir() const { return work_dir_; }

    /// Rust's destructuring move out of the entry, as `adopt_warm_firecracker`
    /// does: the sandbox takes the process, and the entry is left empty.
    std::unique_ptr<FirecrackerInstance> TakeInstance() { return std::move(instance_); }

 private:
    network::Slot* slot_;
    std::unique_ptr<FirecrackerInstance> instance_;
    std::string work_dir_;
};

/// Rust: firecracker/pool.rs :: a warm pool of pre-booted instances.
class Pool {
 public:
    virtual ~Pool() {}
    /// Takes ownership of the returned entry; see [`WarmFirecracker`].
    virtual core::Expected<std::shared_ptr<WarmFirecracker>, std::string> Acquire() = 0;
};

/// Rust `struct FirecrackerPool`.
///
/// Keeps a set of Firecracker processes that are spawned, inside their own
/// network namespace, and already answering on their API socket — everything a
/// microVM needs before it is told what to run. A request that finds a warm
/// entry skips all four of those steps, which is where the bulk of a cold
/// create goes.
///
/// `SpawnFn` exists so the watermark logic can be exercised without KVM. The
/// default (`RealSpawn`) does exactly what Rust's `create_warm_async` does.
class FirecrackerPool : public Pool {
 public:
    /// Rust `create_warm_async` — produces one warm entry or an error.
    typedef std::function<core::Expected<std::shared_ptr<WarmFirecracker>, std::string>()>
        SpawnFn;
    /// Rust `cleanup_warm_async`/`cleanup_warm_blocking` — destroys one entry.
    typedef std::function<core::Expected<core::Unit, std::string>(
        const std::shared_ptr<WarmFirecracker>&, bool)>
        CleanupFn;

    struct Options {
        warmpool::PoolConfig pool;
        /// Rust `fill_concurrency` — entries created per refill batch.
        std::size_t fill_concurrency;
        /// Rust `firecracker.work_dir`, the parent of each entry's work dir.
        core::Optional<std::string> work_base_dir;
        /// Rust `capture_output` — whether warm processes write their stdio to
        /// files. Fixed at spawn time, hence part of the acquire key.
        bool capture_output;
        std::string firecracker_bin;
        int64_t socket_timeout_ms;
        int64_t socket_poll_interval_ms;

        Options() : fill_concurrency(1), capture_output(false),
                    socket_timeout_ms(10 * 1000),
                    socket_poll_interval_ms(kPoolPrimePollIntervalMs) {}
    };

    /// `network` and `connector_factory` are what `RealSpawn` needs; either may
    /// be null when a `SpawnFn` is injected instead.
    FirecrackerPool(const Options& options, network::SlotNetworkManager* network,
                    std::function<std::shared_ptr<Connector>()> connector_factory);
    ~FirecrackerPool();

    /// Rust `FirecrackerPool::global` — the process-wide pool when
    /// `[pool.firecracker].enabled = true`, NULL otherwise. Built on first use
    /// from `ConfigManager::GlobalConfig()` and `SlotNetworkManager::Global()`,
    /// with a process-exit hook; every call ensures the maintenance worker.
    static FirecrackerPool* Global();

    /// Rust `FirecrackerPool::prime` — eagerly initialize the global pool and
    /// wait until `low_watermark` warm entries exist, or until `timeout_ms`
    /// elapses. Best-effort: a timeout is not an error, the cold path stays
    /// available.
    static core::Expected<core::Unit, std::string> PrimeGlobal(int64_t timeout_ms);

    /// Rust `ensure_worker_started`.
    void EnsureWorkerStarted();

    /// The manager this pool's slots come from; an adopted entry's slot is
    /// released back to it.
    network::SlotNetworkManager* NetworkManager() const { return network_; }

    /// Overrides the spawn/cleanup pair. Both or neither: a test spawner whose
    /// entries are destroyed by the real cleanup would try to tear down
    /// kernel state it never created.
    void SetSpawnForTesting(SpawnFn spawn, CleanupFn cleanup);

    /// Rust `try_acquire` — hand out a warm entry and request a refill when
    /// the pool drops below the low watermark.
    ///
    /// Fails when the pool is empty: the caller's fallback is a cold spawn,
    /// which is slower but correct, so an empty pool must not block.
    core::Expected<std::shared_ptr<WarmFirecracker>, std::string> Acquire() override;

    /// Rust `try_acquire(capture_output)`.
    ///
    /// A mismatch is refused rather than served. stdout/stderr destinations are
    /// fixed when the process is spawned, so an entry spawned with capture off
    /// can never produce the log file a caller asking for capture expects.
    core::Expected<std::shared_ptr<WarmFirecracker>, std::string> AcquireFor(
        bool capture_output);

    /// Rust `warm_len`.
    std::size_t WarmLen() const;

    /// Rust `run_maintenance_cycle` — one Fill/Drain/Idle decision.
    /// Returns entries created (Fill) or destroyed (Drain); cleanup failures
    /// are logged as the Rust worker logs the cycle's error.
    std::size_t RunMaintenanceCycle();

    /// Rust `prime`'s loop for this pool: waits (polling every
    /// `kPoolPrimePollIntervalMs`) for the maintenance worker to reach the low
    /// watermark. Skipped when maintenance or startup prewarm is disabled.
    void PrimeFor(int64_t timeout_ms);

    /// Synchronous fill up to the low watermark without a worker. Test helper:
    /// production primes through the worker (`PrimeFor`).
    void Prime();

    /// Rust `shutdown` — drain and destroy every entry, reporting *all*
    /// failures joined, not just the first: each entry holds a distinct slot
    /// and work dir, and an operator needs to know everything that leaked.
    ///
    /// `sync_network_cleanup` mirrors Rust's process-exit path, where the
    /// async teardown cannot be used.
    core::Expected<core::Unit, std::string> Shutdown(bool sync_network_cleanup);

 private:
    /// Rust `create_warm_async`: allocate slot, make work dir, spawn into the
    /// namespace, wait for the API socket. Unwinds in reverse on any failure,
    /// releasing the slot back to the network pool.
    core::Expected<std::shared_ptr<WarmFirecracker>, std::string> RealSpawn();

    /// Rust `cleanup_warm_*`: stop the process, drop the work dir, tear the
    /// slot down. All three run even if an earlier one fails; as in Rust only
    /// the slot teardown's failure is reported (a failed stop is logged, the
    /// work-dir removal is a `TempDir` drop).
    core::Expected<core::Unit, std::string> RealCleanup(
        const std::shared_ptr<WarmFirecracker>& warm, bool sync_network_cleanup);

    /// Rust `fill_warm_entries` — returns entries queued, and the joined
    /// cleanup failures through `*error`.
    std::size_t FillWarmEntries(std::size_t to_fill, std::string* error);
    core::Expected<core::Unit, std::string> CleanupWarm(
        const std::shared_ptr<WarmFirecracker>& warm, bool sync_network_cleanup);

    Options options_;
    network::SlotNetworkManager* network_;
    std::function<std::shared_ptr<Connector>()> connector_factory_;
    SpawnFn spawn_;
    CleanupFn cleanup_;
    warmpool::WarmPool<std::shared_ptr<WarmFirecracker> > pool_;
};

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_POOL_H_
