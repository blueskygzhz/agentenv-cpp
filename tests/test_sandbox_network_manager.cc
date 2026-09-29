// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/manager.rs :: tests
//
// Covers the allocation bitmap, pool interaction and shutdown semantics, plus
// the pure helpers (`slot_index_from_host_veth_name`,
// `collect_conflict_report`, the host iptables rule sets).
//
// Tests that need a real namespace are skipped: `AllocateAny` falls through to
// `CreateNetwork`, which needs CAP_NET_ADMIN. What is asserted here is
// everything reachable without it — index bookkeeping via `AllocateSlot`,
// which builds a `Slot` but never touches the kernel.
#include "microtest.h"

#include <string>
#include <vector>

#include "agentenv/cfg/network.h"
#include "agentenv/sandbox/network/manager.h"

using namespace agentenv;
using namespace agentenv::sandbox::network;

namespace {

/// Rust `manager_with_capacity` — maintenance off so the tests drive
/// fill/drain explicitly rather than racing a worker.
NetworkManagerConfig TestConfig(std::size_t low, std::size_t high) {
    NetworkManagerConfig config;
    config.pool.low_watermark       = low;
    config.pool.high_watermark      = high;
    config.pool.maintenance_enabled = false;
    config.pool.startup_prewarm     = false;
    config.address_plan             = NetworkAddressPlan::Default();
    config.netns_dir                = "/tmp/aenv-network-tests/netns";
    return config;
}

}  // namespace

// ============================================================
// manager.rs :: slot_index_from_host_veth_name_*
// ============================================================
MT_TEST(slot_index_from_host_veth_name_parses_valid_slot) {
    uint32_t idx = 0;
    MT_EXPECT_TRUE(SlotIndexFromHostVethName("veth-42", &idx));
    MT_EXPECT_EQ(idx, static_cast<uint32_t>(42));

    MT_EXPECT_TRUE(SlotIndexFromHostVethName("veth-1", &idx));
    MT_EXPECT_EQ(idx, static_cast<uint32_t>(1));
}

MT_TEST(slot_index_from_host_veth_name_rejects_non_matching_names) {
    uint32_t idx = 0;
    // No prefix at all.
    MT_EXPECT_TRUE(!SlotIndexFromHostVethName("eth0", &idx));
    // Prefix but not a number.
    MT_EXPECT_TRUE(!SlotIndexFromHostVethName("veth-nope", &idx));
    // Slot 0 is reserved, so a `veth-0` must not reserve anything.
    MT_EXPECT_TRUE(!SlotIndexFromHostVethName("veth-0", &idx));
    // Prefix only.
    MT_EXPECT_TRUE(!SlotIndexFromHostVethName("veth-", &idx));
    // At or past the ceiling.
    std::ostringstream too_big;
    too_big << "veth-" << cfg::kNetworkMaxSlots;
    MT_EXPECT_TRUE(!SlotIndexFromHostVethName(too_big.str(), &idx));
    // A trailing sign or space must not parse.
    MT_EXPECT_TRUE(!SlotIndexFromHostVethName("veth--1", &idx));
    MT_EXPECT_TRUE(!SlotIndexFromHostVethName("veth-1 ", &idx));
}

// ============================================================
// manager.rs :: collect_conflict_report_keeps_samples_and_total_count
// ============================================================
MT_TEST(collect_conflict_report_keeps_samples_and_total_count) {
    const std::string output =
        "10.11.0.1 via 10.12.0.3 dev veth-1\n"
        "iifname \"veth-1\" tcp dport 443 redirect to :5017\n"
        "unrelated line\n"
        "ip saddr 10.11.0.2 oifname \"eth0\" masquerade\n";

    std::vector<std::string> patterns;
    patterns.push_back("10.11.");
    patterns.push_back("veth-");

    ConflictReport report = CollectConflictReport(output, patterns);

    // Three of the four lines match; the "unrelated line" does not.
    MT_EXPECT_EQ(report.total_matches, static_cast<size_t>(3));
    MT_EXPECT_EQ(report.samples.size(), static_cast<size_t>(3));
    MT_EXPECT_TRUE(report.samples[0].find("veth-1") != std::string::npos);
}

MT_TEST(collect_conflict_report_caps_samples_but_not_the_count) {
    // More matches than the sample limit: the count keeps rising while the
    // sample list stops growing.
    std::string output;
    for (int i = 0; i < 12; ++i) {
        std::ostringstream line;
        line << "10.11.0." << i << " dev veth-" << i << "\n";
        output += line.str();
    }

    std::vector<std::string> patterns;
    patterns.push_back("10.11.");

    ConflictReport report = CollectConflictReport(output, patterns);
    MT_EXPECT_EQ(report.total_matches, static_cast<size_t>(12));
    MT_EXPECT_EQ(report.samples.size(), kConflictSampleLimit);
}

MT_TEST(collect_conflict_report_ignores_blank_lines_and_no_patterns) {
    std::vector<std::string> patterns;
    patterns.push_back("10.11.");

    // Blank and whitespace-only lines never count.
    ConflictReport blanks = CollectConflictReport("\n   \n\t\n", patterns);
    MT_EXPECT_EQ(blanks.total_matches, static_cast<size_t>(0));

    // An empty pattern list matches nothing, rather than everything.
    ConflictReport none =
        CollectConflictReport("10.11.0.1 dev veth-1\n", std::vector<std::string>());
    MT_EXPECT_EQ(none.total_matches, static_cast<size_t>(0));
}

// ============================================================
// manager.rs :: global_host_iptables_commands
// ============================================================
MT_TEST(global_host_iptables_commands_match_rust_rule_set) {
    Ipv4Cidr cidr = Ipv4Cidr::Parse("10.11.0.0/16").value();
    std::vector<IptablesRestoreCommand> commands = GlobalHostIptablesCommands(cidr);

    // Rust returns exactly five rules.
    MT_EXPECT_EQ(commands.size(), static_cast<size_t>(5));

    // The two INPUT rules are inserted at fixed positions: the ACCEPT for
    // established flows has to precede the blanket REJECT.
    MT_EXPECT_TRUE(commands[0].kind == IptablesRestoreCommand::Kind::Insert);
    MT_EXPECT_EQ(commands[0].position, 1);
    MT_EXPECT_EQ(commands[0].chain, std::string("INPUT"));
    MT_EXPECT_TRUE(commands[0].rule.find("ESTABLISHED,RELATED -j ACCEPT") !=
                   std::string::npos);

    MT_EXPECT_TRUE(commands[1].kind == IptablesRestoreCommand::Kind::Insert);
    MT_EXPECT_EQ(commands[1].position, 2);
    MT_EXPECT_TRUE(commands[1].rule.find("-j REJECT") != std::string::npos);

    // Every rule scopes itself to our interfaces and our CIDR.
    for (std::size_t i = 0; i < commands.size(); ++i) {
        MT_EXPECT_TRUE(commands[i].rule.find("10.11.0.0/16") != std::string::npos);
    }
    // The wildcard interface match is what makes one rule cover all slots.
    MT_EXPECT_TRUE(commands[2].rule.find("veth-+") != std::string::npos);

    // MASQUERADE lands in nat/POSTROUTING.
    MT_EXPECT_EQ(commands[4].table, std::string("nat"));
    MT_EXPECT_EQ(commands[4].chain, std::string("POSTROUTING"));
    MT_EXPECT_EQ(commands[4].rule,
                 std::string("-s 10.11.0.0/16 -j MASQUERADE"));
}

MT_TEST(global_host_iptables_delete_commands_invert_the_install_set) {
    Ipv4Cidr cidr = Ipv4Cidr::Parse("10.11.0.0/16").value();
    std::vector<IptablesRestoreCommand> install = GlobalHostIptablesCommands(cidr);
    std::vector<IptablesRestoreCommand> remove =
        GlobalHostIptablesDeleteCommands(cidr);

    MT_EXPECT_EQ(remove.size(), install.size());

    // Deletes carry the same table/chain/rule text, differing only in kind —
    // otherwise teardown would leave rules behind.
    for (std::size_t i = 0; i < remove.size(); ++i) {
        MT_EXPECT_TRUE(remove[i].kind == IptablesRestoreCommand::Kind::Delete);
        MT_EXPECT_EQ(remove[i].table, install[i].table);
        MT_EXPECT_EQ(remove[i].chain, install[i].chain);
        MT_EXPECT_EQ(remove[i].rule, install[i].rule);
    }
}

// ============================================================
// manager.rs :: slot_zero_is_reserved_on_init
// ============================================================
MT_TEST(slot_zero_is_reserved_on_init) {
    SlotNetworkManager manager(TestConfig(0, 4));
    // Slot 0 must be taken from the start: it maps to a network address.
    MT_EXPECT_TRUE(manager.IsAllocated(0));
    MT_EXPECT_TRUE(!manager.IsAllocated(1));
}

// ============================================================
// manager.rs :: reject_allocation_at_boundaries
// ============================================================
MT_TEST(allocate_slot_rejects_zero_and_out_of_range) {
    SlotNetworkManager manager(TestConfig(0, 4));

    core::Expected<Slot*, std::string> zero = manager.AllocateSlot(0);
    MT_EXPECT_TRUE(!zero.ok());
    MT_EXPECT_TRUE(zero.error().find("out of range") != std::string::npos);

    core::Expected<Slot*, std::string> over =
        manager.AllocateSlot(static_cast<uint32_t>(cfg::kNetworkMaxSlots));
    MT_EXPECT_TRUE(!over.ok());
    MT_EXPECT_TRUE(over.error().find("out of range") != std::string::npos);
}

// ============================================================
// manager.rs :: duplicate_allocation_is_rejected
// ============================================================
MT_TEST(duplicate_allocation_is_rejected) {
    SlotNetworkManager manager(TestConfig(0, 4));

    core::Expected<Slot*, std::string> first = manager.AllocateSlot(5);
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(manager.IsAllocated(5));

    // The bit is taken, so a second request for the same index must fail
    // rather than hand out a colliding slot.
    core::Expected<Slot*, std::string> second = manager.AllocateSlot(5);
    MT_EXPECT_TRUE(!second.ok());
    MT_EXPECT_TRUE(second.error().find("already allocated") != std::string::npos);

    // Cleanup: the slot never created kernel state, so this only frees the bit.
    MT_EXPECT_TRUE(manager.CleanupAllocatedSlot(first.value(), true).ok());
    MT_EXPECT_TRUE(!manager.IsAllocated(5));
}

// ============================================================
// manager.rs :: release_then_reallocate
// ============================================================
MT_TEST(cleanup_allocated_slot_frees_the_index_for_reuse) {
    SlotNetworkManager manager(TestConfig(0, 4));

    core::Expected<Slot*, std::string> slot = manager.AllocateSlot(7);
    MT_EXPECT_TRUE(slot.ok());
    MT_EXPECT_TRUE(manager.CleanupAllocatedSlot(slot.value(), true).ok());
    MT_EXPECT_TRUE(!manager.IsAllocated(7));

    // The same index is allocatable again.
    core::Expected<Slot*, std::string> again = manager.AllocateSlot(7);
    MT_EXPECT_TRUE(again.ok());
    MT_EXPECT_TRUE(manager.CleanupAllocatedSlot(again.value(), true).ok());
}

// ============================================================
// manager.rs :: double_release_is_rejected / release_slot_zero_is_rejected
// ============================================================
MT_TEST(cleanup_of_an_unallocated_index_is_reported) {
    SlotNetworkManager manager(TestConfig(0, 4));

    core::Expected<Slot*, std::string> slot = manager.AllocateSlot(3);
    MT_EXPECT_TRUE(slot.ok());
    // `Slot*` is consumed by the first cleanup, so build a second handle for
    // the same index to exercise the double-release path.
    core::Expected<Slot*, NetworkError> stale =
        Slot::New(3, NetworkAddressPlan::Default(), "/tmp/aenv-network-tests/netns");
    MT_EXPECT_TRUE(stale.ok());

    MT_EXPECT_TRUE(manager.CleanupAllocatedSlot(slot.value(), true).ok());

    // Second release of the same index: the bit is already clear.
    core::Expected<core::Unit, std::string> again =
        manager.CleanupAllocatedSlot(stale.value(), true);
    MT_EXPECT_TRUE(!again.ok());
    MT_EXPECT_TRUE(again.error().find("not allocated") != std::string::npos);
}

// ============================================================
// manager.rs :: maintenance_action_*
// ============================================================
MT_TEST(maintenance_action_fills_to_low_watermark) {
    SlotNetworkManager manager(TestConfig(3, 6));
    // Below the low watermark: fill up to it.
    warmpool::PoolMaintenanceAction action = manager.ComputeMaintenanceAction(0);
    MT_EXPECT_TRUE(action.kind == warmpool::PoolMaintenanceKind::Fill);
    MT_EXPECT_EQ(action.count, static_cast<size_t>(3));
}

MT_TEST(maintenance_action_drains_above_high_watermark) {
    SlotNetworkManager manager(TestConfig(2, 4));
    // Above the high watermark: drain the excess.
    warmpool::PoolMaintenanceAction action = manager.ComputeMaintenanceAction(7);
    MT_EXPECT_TRUE(action.kind == warmpool::PoolMaintenanceKind::Drain);
    MT_EXPECT_EQ(action.count, static_cast<size_t>(3));
}

MT_TEST(maintenance_action_is_idle_between_watermarks) {
    SlotNetworkManager manager(TestConfig(2, 6));
    warmpool::PoolMaintenanceAction action = manager.ComputeMaintenanceAction(4);
    MT_EXPECT_TRUE(action.kind == warmpool::PoolMaintenanceKind::Idle);
}

// ============================================================
// manager.rs :: allocate_any_is_rejected_after_shutdown_cleanup
// ============================================================
MT_TEST(allocate_any_is_rejected_after_shutdown) {
    SlotNetworkManager manager(TestConfig(0, 4));
    MT_EXPECT_TRUE(!manager.ShuttingDown());

    MT_EXPECT_TRUE(manager.Shutdown().ok());
    MT_EXPECT_TRUE(manager.ShuttingDown());

    // After shutdown no new slot may be handed out, even though the pool and
    // bitmap could still satisfy the request.
    core::Expected<Slot*, std::string> slot = manager.AllocateAny();
    MT_EXPECT_TRUE(!slot.ok());
    MT_EXPECT_EQ(slot.error(), std::string(kErrShuttingDown));
}

// ============================================================
// manager.rs :: shutdown_cleanup_is_idempotent / noop_for_empty_pool
// ============================================================
MT_TEST(shutdown_is_idempotent_and_noop_for_empty_pool) {
    SlotNetworkManager manager(TestConfig(0, 4));
    MT_EXPECT_EQ(manager.PoolLen(), static_cast<size_t>(0));

    MT_EXPECT_TRUE(manager.Shutdown().ok());
    // A second shutdown must not fail or double-free anything.
    MT_EXPECT_TRUE(manager.Shutdown().ok());
    MT_EXPECT_TRUE(manager.Shutdown().ok());
}

// ============================================================
// manager.rs :: release_does_not_cache_slot_after_shutdown_cleanup
// ============================================================
MT_TEST(release_after_shutdown_cleans_up_instead_of_caching) {
    SlotNetworkManager manager(TestConfig(0, 4));

    core::Expected<Slot*, std::string> slot = manager.AllocateSlot(11);
    MT_EXPECT_TRUE(slot.ok());
    MT_EXPECT_TRUE(manager.Shutdown().ok());

    // Releasing into a shut-down manager must tear the slot down rather than
    // pool it, otherwise the slot would outlive the manager.
    MT_EXPECT_TRUE(manager.Release(slot.value()).ok());
    MT_EXPECT_EQ(manager.PoolLen(), static_cast<size_t>(0));
    MT_EXPECT_TRUE(!manager.IsAllocated(11));
}

// ============================================================
// Release with room in the pool caches the slot for reuse.
// ============================================================
MT_TEST(release_returns_slot_to_pool_when_under_capacity) {
    SlotNetworkManager manager(TestConfig(0, 4));

    core::Expected<Slot*, std::string> slot = manager.AllocateSlot(9);
    MT_EXPECT_TRUE(slot.ok());

    MT_EXPECT_TRUE(manager.Release(slot.value()).ok());
    // Pooled, so the index stays reserved — the slot is still ours.
    MT_EXPECT_EQ(manager.PoolLen(), static_cast<size_t>(1));
    MT_EXPECT_TRUE(manager.IsAllocated(9));

    // The next allocation reuses it instead of building a fresh one, which is
    // the whole point of the warm pool.
    core::Expected<Slot*, std::string> reused = manager.AllocateAny();
    MT_EXPECT_TRUE(reused.ok());
    MT_EXPECT_EQ(reused.value()->Idx(), static_cast<uint32_t>(9));
    MT_EXPECT_EQ(manager.PoolLen(), static_cast<size_t>(0));

    MT_EXPECT_TRUE(manager.CleanupAllocatedSlot(reused.value(), true).ok());
}

MT_TEST(shutdown_drains_pooled_slots_and_releases_their_bits) {
    SlotNetworkManager manager(TestConfig(0, 4));

    core::Expected<Slot*, std::string> a = manager.AllocateSlot(21);
    core::Expected<Slot*, std::string> b = manager.AllocateSlot(22);
    MT_EXPECT_TRUE(a.ok() && b.ok());
    MT_EXPECT_TRUE(manager.Release(a.value()).ok());
    MT_EXPECT_TRUE(manager.Release(b.value()).ok());
    MT_EXPECT_EQ(manager.PoolLen(), static_cast<size_t>(2));

    MT_EXPECT_TRUE(manager.Shutdown().ok());

    // Shutdown must both empty the pool and give the indices back.
    MT_EXPECT_EQ(manager.PoolLen(), static_cast<size_t>(0));
    MT_EXPECT_TRUE(!manager.IsAllocated(21));
    MT_EXPECT_TRUE(!manager.IsAllocated(22));
}

MT_MAIN
