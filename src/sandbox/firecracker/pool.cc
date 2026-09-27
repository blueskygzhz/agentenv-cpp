// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/{pool,factory}.rs
//
// The watermark-driven capacity management is ported faithfully on top of
// WarmPool<T>. The warm entry factory is injected so the semantics can be
// exercised without spawning real microVMs (which require KVM). Pool / Factory
// remain ABCs for the rest of the driver.
#include "agentenv/sandbox/firecracker/pool.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

FirecrackerPool::FirecrackerPool(warmpool::PoolConfig config, EntryFactory factory)
    : pool_(config), factory_(factory) {}

core::Expected<std::shared_ptr<Instance>, std::string>
FirecrackerPool::Acquire() {
    std::shared_ptr<Instance> inst;
    if (pool_.TryAcquire(&inst)) {
        // Rust: request a refill when we dip below the low watermark.
   if (pool_.Len() < pool_.Config().low_watermark) {
     pool_.RequestMaintenance();
     }
      return inst;
 }
    return core::make_unexpected(std::string("firecracker pool empty (cold path)"));
}

void FirecrackerPool::Release(std::shared_ptr<Instance> inst) {
    if (inst) {
        pool_.Release(inst);
    }
}

std::size_t FirecrackerPool::WarmLen() const {
    return pool_.Len();
}

std::size_t FirecrackerPool::RunMaintenanceCycle() {
    warmpool::PoolMaintenanceAction action = pool_.ComputeMaintenanceAction(pool_.Len());
    switch (action.kind) {
   case warmpool::PoolMaintenanceKind::Fill: {
     std::size_t created = 0;
            for (std::size_t i = 0; i < action.count; ++i) {
     if (!factory_) break;
          std::shared_ptr<Instance> inst = factory_();
   if (!inst) break;  // spawn failure -> stop this batch (Rust behavior)
                if (!pool_.TryPushBounded(inst)) break;// hit high watermark
 ++created;
 }
     return created;
 }
 case warmpool::PoolMaintenanceKind::Drain: {
     std::size_t drained = 0;
      for (std::size_t i = 0; i < action.count; ++i) {
  std::shared_ptr<Instance> victim;
          if (!pool_.TryDrainOne(&victim)) break;
        ++drained;  // victim's destructor releases resources
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

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
