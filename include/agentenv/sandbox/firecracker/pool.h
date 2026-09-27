// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/{pool,factory}.rs.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_POOL_H_
#define AGENTENV_SANDBOX_FIRECRACKER_POOL_H_

#include <functional>
#include <memory>

#include "agentenv/core/expected.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/firecracker/config.h"
#include "agentenv/sandbox/firecracker/instance.h"
#include "agentenv/warm-pool/pool.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust: firecracker/pool.rs :: a warm pool of pre-booted instances.
class Pool {
 public:
    virtual ~Pool() {}
    virtual core::Expected<std::shared_ptr<Instance>, std::string> Acquire() = 0;
virtual void Release(std::shared_ptr<Instance> inst) = 0;
};

/// Rust: firecracker/factory.rs :: FirecrackerSandboxFactory.
class Factory {
 public:
    virtual ~Factory() {}
    virtual core::Expected<std::shared_ptr<Backend>, std::string>
        BuildBackend(const SandboxConfig& cfg) = 0;
};

/// Rust: firecracker/pool.rs :: FirecrackerPool — warm-entry容量管理.
///
/// The upstream pool spawns real Firecracker processes on a maintenance thread.
/// Spawning a microVM needs KVM, so here the *entry factory is injected*: the
/// watermark-driven capacity management (the semantic core of pool.rs) is fully
/// real and testable on top of the ported WarmPool<T>, while the concrete warm
/// entry is whatever the factory produces (a real driver injects a spawner).
class FirecrackerPool : public Pool {
 public:
    /// Produces one warm instance, or nullptr on failure (mirrors a spawn error).
    typedef std::function<std::shared_ptr<Instance>()> EntryFactory;

    FirecrackerPool(warmpool::PoolConfig config, EntryFactory factory);

    /// Rust `try_acquire` — hand out a warm entry; requests a refill when the
    /// pool drops below the low watermark.
 core::Expected<std::shared_ptr<Instance>, std::string> Acquire() override;
    void Release(std::shared_ptr<Instance> inst) override;

  /// Rust `warm_len`.
    std::size_t WarmLen() const;

    /// Rust `run_maintenance_cycle` — apply one Fill/Drain/Idle decision.
    /// Returns the number of entries created (Fill) or drained (Drain).
    std::size_t RunMaintenanceCycle();

    /// Rust `prime` (synchronous form): fill up to the low watermark.
    void Prime();

 private:
    warmpool::WarmPool<std::shared_ptr<Instance> > pool_;
    EntryFactory factory_;
};

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_POOL_H_
