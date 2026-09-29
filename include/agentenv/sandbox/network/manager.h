// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/manager.rs
//
// Owns every network slot on the node: an allocation bitmap for indices and a
// warm pool of fully built slots. Allocating from the pool skips namespace and
// veth setup, which is the dominant cost of starting a sandbox.
#ifndef AGENTENV_SANDBOX_NETWORK_MANAGER_H_
#define AGENTENV_SANDBOX_NETWORK_MANAGER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/sandbox/network.h"
#include "agentenv/sandbox/network/slot.h"
#include "agentenv/warm-pool/pool.h"

namespace agentenv {
namespace sandbox {
namespace network {

/// Rust `CONFLICT_SAMPLE_LIMIT`.
const std::size_t kConflictSampleLimit = 5;
/// Rust `ERR_SHUTTING_DOWN`.
extern const char* const kErrShuttingDown;

/// Rust `struct ConflictReport`.
struct ConflictReport {
    std::size_t              total_matches = 0;
    std::vector<std::string> samples;
};

/// Rust `fn collect_conflict_report` — counts every matching line but keeps
/// only the first `kConflictSampleLimit` for the log message.
ConflictReport CollectConflictReport(const std::string& output,
                                     const std::vector<std::string>& patterns);

/// Rust `fn slot_index_from_host_veth_name` — parses `veth-<idx>`.
///
/// Rejects index 0 and anything at or past the ceiling, so an unrelated
/// interface that merely starts with `veth-` cannot reserve a slot.
bool SlotIndexFromHostVethName(const std::string& name, uint32_t* out);

/// Rust `fn global_host_iptables_commands` — the node-wide rules that make
/// slot traffic work: guest replies to host-initiated connections are
/// accepted, all other guest-to-host traffic is rejected, and guest egress is
/// masqueraded.
std::vector<IptablesRestoreCommand> GlobalHostIptablesCommands(
    const Ipv4Cidr& host_interaction_cidr);

/// Rust `fn global_host_iptables_delete_commands` — the exact inverse, used on
/// shutdown.
std::vector<IptablesRestoreCommand> GlobalHostIptablesDeleteCommands(
    const Ipv4Cidr& host_interaction_cidr);

/// Rust `struct NetworkManagerConfig`.
struct NetworkManagerConfig {
    warmpool::PoolConfig pool;
    NetworkAddressPlan    address_plan;
    std::string           netns_dir;

    NetworkManagerConfig()
        : address_plan(NetworkAddressPlan::Default()) {}
};

/// Rust `struct NetworkManager`.
///
/// Slots live in the pool as raw pointers: `Slot` destroys live kernel state
/// in its destructor and is deliberately not copyable, so the pool must move
/// ownership rather than duplicate it.
class SlotNetworkManager {
 public:
    explicit SlotNetworkManager(const NetworkManagerConfig& config);
    ~SlotNetworkManager();

    /// Rust `NetworkManager::global` — the process-wide manager, created on
    /// first use from the global config.
    static SlotNetworkManager& Global();

    /// Rust `NetworkManager::global_if_initialized` — NULL when `Global()`
    /// was never called, so shutdown paths do not construct one just to tear
    /// it down.
    static SlotNetworkManager* GlobalIfInitialized();

    /// Rust `allocate_any` — a warm slot when one is pooled, otherwise a
    /// freshly built one. Never returns slot 0.
    core::Expected<Slot*, std::string> AllocateAny();

    /// Rust `allocate_slot` — a specific index; fails when already taken.
    core::Expected<Slot*, std::string> AllocateSlot(uint32_t idx);

    /// Rust `release` — pools the slot for reuse when there is room, otherwise
    /// tears it down and frees its index.
    core::Expected<core::Unit, std::string> Release(Slot* slot);

    /// Rust `cleanup_allocated_slot` — unconditional teardown plus bit
    /// release, bypassing the pool.
    core::Expected<core::Unit, std::string>
        CleanupAllocatedSlot(Slot* slot, bool sync_cleanup);

    /// Rust `shutdown` — drains the pool, tears every slot down and removes
    /// the node-wide iptables rules. Idempotent; afterwards allocation is
    /// refused and release no longer caches.
    core::Expected<core::Unit, std::string> Shutdown();

    /// Rust `run_pool_maintenance_cycle` — one fill/drain pass against the
    /// configured watermarks.
    core::Expected<core::Unit, std::string> RunPoolMaintenanceCycle();

    /// Rust `compute_maintenance_action`.
    warmpool::PoolMaintenanceAction
        ComputeMaintenanceAction(std::size_t pool_len) const;

    /// Rust `shutting_down`.
    bool ShuttingDown() const;

    std::size_t PoolLen() const { return pool_.Len(); }
    /// Whether `idx` is currently reserved in the allocation bitmap.
    bool IsAllocated(uint32_t idx) const;

 private:
    SlotNetworkManager(const SlotNetworkManager&);
    SlotNetworkManager& operator=(const SlotNetworkManager&);

    /// Rust `allocate_fresh_slot` — takes the next free index and builds the
    /// namespace. Releases the index again if setup fails.
    core::Expected<Slot*, std::string> AllocateFreshSlot();

    /// Rust `release_slot_bit`.
    core::Expected<core::Unit, std::string> ReleaseSlotBit(uint32_t idx);

    /// Rust `cleanup_slot_and_release_bit_inner` — always attempts both, and
    /// reports the bitmap error when both fail (a leaked bit is the worse of
    /// the two, since it permanently shrinks capacity).
    core::Expected<core::Unit, std::string>
        CleanupSlotAndReleaseBit(Slot* slot, bool sync_cleanup);

    /// Rust `reserve_existing_host_veth_slots` — reserves indices whose
    /// `veth-<idx>` already exists, so a stale device or a second runtime
    /// cannot collide with us.
    std::vector<uint32_t> ReserveExistingHostVethSlots();

    /// Rust `log_external_conflicts`.
    void LogExternalConflicts(const std::vector<uint32_t>& existing_slots);

    /// Rust `check_conflicts`.
    void CheckConflicts(const std::string& source, const std::string& command,
                        const std::vector<std::string>& args,
                        const std::vector<int>& capabilities,
                        const std::vector<std::string>& patterns,
                        const std::string& message);

    /// Rust `install_global_host_iptables` — installs once per process.
    core::Expected<core::Unit, std::string> InstallGlobalHostIptables();

    /// Rust `cleanup_global_host_iptables`.
    void CleanupGlobalHostIptables();

    mutable std::mutex   mu_;
    /// Rust `allocated: AtomicBitSet<..>`; a bitmap over slot indices.
    std::vector<uint64_t> allocated_;
    warmpool::WarmPool<Slot*> pool_;
    NetworkAddressPlan    address_plan_;
    std::string           netns_dir_;
    bool                  shutting_down_;
};

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_NETWORK_MANAGER_H_
