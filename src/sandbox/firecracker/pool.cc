// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/pool.rs
//
// Watermark management sits on the ported WarmPool<T>; the four steps that make
// an entry warm (network slot, work dir, spawn into the namespace, wait for the
// API socket) are real. They need KVM only to run a guest -- spawning the VMM
// and waiting for its socket do not -- so `RealSpawn` is exercised directly and
// `SetSpawnForTesting` exists for the cases that cannot allocate a slot.
#include "agentenv/sandbox/firecracker/pool.h"

#include "agentenv/core/fs.h"
#include "agentenv/sandbox/firecracker/lifecycle.h"
#include "agentenv/sandbox/network/manager.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

FirecrackerPool::FirecrackerPool(const Options& options,
                                 network::SlotNetworkManager* network,
                                 std::function<std::shared_ptr<Connector>()> connector_factory)
    : options_(options),
      network_(network),
      connector_factory_(connector_factory),
      pool_(options.pool) {}

FirecrackerPool::~FirecrackerPool() {
    // Rust's process-exit hook. Without this the warm processes outlive the
    // pool and keep their slots and work directories, which no later run can
    // identify as abandoned.
    Shutdown(true);
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

std::size_t FirecrackerPool::RunMaintenanceCycle() {
    const warmpool::PoolMaintenanceAction action =
        pool_.ComputeMaintenanceAction(pool_.Len());
    switch (action.kind) {
   case warmpool::PoolMaintenanceKind::Fill: {
     std::size_t created = 0;
            // Rust fills in batches of `fill_concurrency` and stops launching
            // further batches once one entry fails: a failing spawn is almost
            // always a resource that will not appear within this cycle (no
            // free slot, no KVM), and retrying it immediately only multiplies
            // the cost.
            std::size_t remaining = action.count;
            while (remaining > 0) {
                const std::size_t batch =
                    remaining < options_.fill_concurrency ? remaining : options_.fill_concurrency;
                bool saw_failure = false;
                for (std::size_t i = 0; i < batch; ++i) {
                    const core::Expected<std::shared_ptr<WarmFirecracker>, std::string> made =
                        spawn_ ? spawn_() : RealSpawn();
                    if (!made.ok()) {
                        saw_failure = true;
                        continue;
                    }
                    if (!pool_.TryPushBounded(made.value())) {
                        // Hit the high watermark while this batch was in
                        // flight. The entry is destroyed rather than dropped:
                        // its slot and work dir are live kernel and filesystem
                        // state that nothing else holds a handle to.
                        const core::Expected<core::Unit, std::string> gone =
                            cleanup_ ? cleanup_(made.value(), false)
                                     : RealCleanup(made.value(), false);
                        (void)gone;
                        continue;
                    }
                    ++created;
                }
                remaining -= batch;
                if (saw_failure) break;
            }
     return created;
 }
 case warmpool::PoolMaintenanceKind::Drain: {
     std::size_t drained = 0;
            // Reachable only when two maintenance cycles overlap: the fill
            // target is clamped to the high watermark and acquire only
            // removes, so a single-threaded pool never rises above it. Kept
            // because `ComputeMaintenanceAction` is shared with the network
            // manager, which *does* return slots to its pool and can exceed
            // the watermark -- and because a pool that silently ignored a
            // Drain would hold VMM processes nothing asked it to keep.
      for (std::size_t i = 0; i < action.count; ++i) {
  std::shared_ptr<WarmFirecracker> victim;
          if (!pool_.TryDrainOne(&victim)) break;
                // Explicit, not destructor-driven: the entry owns a raw `Slot*`
                // whose teardown goes through the network manager, which the
                // entry has no reference to.
                const core::Expected<core::Unit, std::string> gone =
                    cleanup_ ? cleanup_(victim, false) : RealCleanup(victim, false);
                (void)gone;
        ++drained;
      }
            return drained;
        }
        case warmpool::PoolMaintenanceKind::Idle:
        default:
            return 0;
    }
}

void FirecrackerPool::Prime() {
    // Fill until we reach the low watermark (best-effort, like Rust prime()).
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
            cleanup_ ? cleanup_(drained[i], sync_network_cleanup)
                     : RealCleanup(drained[i], sync_network_cleanup);
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

    // ① The slot first, because the namespace it creates is what the process
    // must be spawned into -- it cannot be applied afterwards.
    const core::Expected<network::Slot*, std::string> slot = network_->AllocateAny();
    if (!slot.ok()) {
        return core::make_unexpected(
            std::string("firecracker pool: allocate network slot: ") + slot.error());
    }

    // ② The work dir, which is where the API socket will be created.
    const core::Expected<std::string, std::string> work_dir =
        CreateFirecrackerWorkDir(options_.work_base_dir);
    if (!work_dir.ok()) {
        network_->CleanupAllocatedSlot(slot.value(), false);
        return core::make_unexpected(
            std::string("firecracker pool: create work dir: ") + work_dir.error());
    }

    std::shared_ptr<FirecrackerInstance> instance(new FirecrackerInstance(
        work_dir.value(), connector_factory_ ? connector_factory_() : NULL));

    core::Optional<std::string> stdout_path;
    core::Optional<std::string> stderr_path;
    WarmStdioPaths(work_dir.value(), options_.capture_output, &stdout_path, &stderr_path);

    // ③ Spawn into the slot's namespace.
    const core::Expected<core::Unit, std::string> spawned = instance->SpawnWithNetns(
        options_.firecracker_bin,
        stdout_path.has_value() ? *stdout_path : std::string(),
        stderr_path.has_value() ? *stderr_path : std::string(),
        slot.value()->NamespacePath());
    if (!spawned.ok()) {
        // Unwound in reverse, and the process stopped first: `Stop` removes the
        // API socket, which lives inside the work dir being deleted next.
        instance->Stop(kPoolFirecrackerStopTimeoutMs);
        core::fs::RemoveDirAll(work_dir.value());
        network_->CleanupAllocatedSlot(slot.value(), false);
        return core::make_unexpected(
            std::string("spawn warm firecracker process: ") + spawned.error());
    }

    // ④ Wait for the socket. Until this returns the entry is not warm: handing
    // it out earlier only moves the wait into the request that receives it,
    // which is the cost the pool exists to remove.
    const core::Expected<core::Unit, std::string> ready =
        instance->WaitForReady(options_.socket_timeout_ms, options_.socket_poll_interval_ms);
    if (!ready.ok()) {
        instance->Stop(kPoolFirecrackerStopTimeoutMs);
        core::fs::RemoveDirAll(work_dir.value());
        network_->CleanupAllocatedSlot(slot.value(), false);
        return core::make_unexpected(
            std::string("wait for warm firecracker api socket: ") + ready.error());
    }

    return std::shared_ptr<WarmFirecracker>(
        new WarmFirecracker(slot.value(), instance, work_dir.value()));
}

core::Expected<core::Unit, std::string>
FirecrackerPool::RealCleanup(const std::shared_ptr<WarmFirecracker>& warm,
                             bool sync_network_cleanup) {
    if (!warm) return core::Unit();

    // Rust's order, and it is load-bearing: the socket lives in the work dir
    // and the tap device lives in the slot's namespace, so each step removes
    // something the next one is about to delete the container of.
    //
    // A failure in one step does not skip the others. Rust logs a failed stop
    // and carries on for the same reason: the work dir and the slot leak
    // permanently if skipped, while a surviving process is at least visible.
    std::vector<std::string> failures;

    if (!sync_network_cleanup) {
        const core::Expected<core::Unit, std::string> stopped =
            warm->GetInstance()->Stop(kPoolFirecrackerStopTimeoutMs);
        if (!stopped.ok()) {
            failures.push_back(std::string("stop warm process: ") + stopped.error());
        }
    }
    // With `sync_network_cleanup` the caller is a process-exit path: Rust drops
    // the child there rather than awaiting a graceful stop, because the runtime
    // it would need is already being torn down. The destructor signals the
    // child, which is all that is available at that point.

    const core::Expected<core::Unit, std::string> removed =
        core::fs::RemoveDirAll(warm->WorkDir());
    if (!removed.ok()) {
        failures.push_back(std::string("remove warm work dir: ") + removed.error());
    }

    if (network_ != NULL) {
        // `CleanupAllocatedSlot`, not `Release`: a warm entry taken out of the
        // pool is being destroyed, so its slot must go back to the free bitmap
        // rather than into the slot pool holding a namespace whose VMM is gone.
        const core::Expected<core::Unit, std::string> torn =
            network_->CleanupAllocatedSlot(warm->GetSlot(), sync_network_cleanup);
        if (!torn.ok()) {
            failures.push_back(std::string("cleanup warm network slot: ") + torn.error());
        }
    }

    return FirecrackerPoolCleanupResult(failures);
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
