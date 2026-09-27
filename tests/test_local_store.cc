// SPDX-License-Identifier: MIT
// Rust: src/local_store.rs — behaviour of the durability policies, batch
// atomicity, ordered iteration and prefix scans.
#include <string>
#include <vector>

#include "agentenv/core/capability.h"
#include "agentenv/core/fs.h"
#include "agentenv/local_store.h"
#include "agentenv/privileges.h"
#include "microtest.h"

namespace {

using agentenv::core::Optional;
using agentenv::local_store::BatchOp;
using agentenv::local_store::Durability;
using agentenv::local_store::Entry;
using agentenv::local_store::KvStore;
namespace cap = agentenv::core::cap;
namespace fs = agentenv::core::fs;

struct TempRoot {
    std::string path;

    TempRoot() {
        const agentenv::core::Expected<std::string, std::string> temp =
            fs::CreateTempDir("agentenv-store-");
        MT_EXPECT_TRUE(temp.ok());
        path = temp.value();
    }
    ~TempRoot() { fs::RemoveDirAll(path); }

    std::string Join(const std::string& leaf) const { return fs::Join(path, leaf); }
};

std::shared_ptr<KvStore> OpenAt(const std::string& path, Durability durability) {
    const agentenv::core::Expected<std::shared_ptr<KvStore>, std::string> store =
        KvStore::Open(path, durability);
    MT_EXPECT_TRUE(store.ok());
    return store.value();
}

}  // namespace

// ---------------------------------------------------------------------------
// KvStore
// ---------------------------------------------------------------------------

MT_TEST(put_get_delete_round_trip) {
    TempRoot root;
    const std::shared_ptr<KvStore> store = OpenAt(root.Join("db/store.log"), Durability::Wal);

    MT_EXPECT_TRUE(store->Put("alpha", "1").ok());
    const agentenv::core::Expected<Optional<std::string>, std::string> got =
        store->Get("alpha");
    MT_EXPECT_TRUE(got.ok());
    MT_EXPECT_TRUE(got.value().has_value());
    MT_EXPECT_EQ(*got.value(), std::string("1"));

    // An absent key is an empty optional, not an error.
    MT_EXPECT_TRUE(store->Get("missing").ok());
    MT_EXPECT_TRUE(!store->Get("missing").value().has_value());

    MT_EXPECT_TRUE(store->Delete("alpha").ok());
    MT_EXPECT_TRUE(!store->Get("alpha").value().has_value());
    // Deleting a missing key succeeds, as RocksDB does.
    MT_EXPECT_TRUE(store->Delete("alpha").ok());
}

MT_TEST(put_replaces_an_existing_value) {
    TempRoot root;
    const std::shared_ptr<KvStore> store = OpenAt(root.Join("store.log"), Durability::Wal);

    MT_EXPECT_TRUE(store->Put("k", "first").ok());
    MT_EXPECT_TRUE(store->Put("k", "second").ok());
    MT_EXPECT_EQ(*store->Get("k").value(), std::string("second"));
    MT_EXPECT_EQ(store->Entries().value().size(), static_cast<std::size_t>(1));
}

MT_TEST(keys_and_values_are_binary_safe) {
    TempRoot root;
    const std::shared_ptr<KvStore> store = OpenAt(root.Join("store.log"), Durability::Wal);

    // Embedded NULs and newlines must survive the log framing, which is why it
    // uses explicit lengths rather than delimiters.
    const std::string key("a\0b\nc", 5);
    const std::string value("\0\xff\n\r", 4);
    MT_EXPECT_TRUE(store->Put(key, value).ok());

    const std::shared_ptr<KvStore> reopened =
        OpenAt(root.Join("store.log"), Durability::Wal);
    MT_EXPECT_TRUE(reopened->Get(key).value().has_value());
    MT_EXPECT_EQ(*reopened->Get(key).value(), value);
}

MT_TEST(entries_are_returned_in_key_order) {
    TempRoot root;
    const std::shared_ptr<KvStore> store = OpenAt(root.Join("store.log"), Durability::Wal);

    // Inserted out of order on purpose.
    MT_EXPECT_TRUE(store->Put("c", "3").ok());
    MT_EXPECT_TRUE(store->Put("a", "1").ok());
    MT_EXPECT_TRUE(store->Put("b", "2").ok());

    const std::vector<Entry> entries = store->Entries().value();
    MT_EXPECT_EQ(entries.size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(entries[0].first, std::string("a"));
    MT_EXPECT_EQ(entries[1].first, std::string("b"));
    MT_EXPECT_EQ(entries[2].first, std::string("c"));
}

MT_TEST(fold_visits_every_entry_and_stops_on_error) {
    TempRoot root;
    const std::shared_ptr<KvStore> store = OpenAt(root.Join("store.log"), Durability::Wal);
    MT_EXPECT_TRUE(store->Put("a", "1").ok());
    MT_EXPECT_TRUE(store->Put("b", "2").ok());
    MT_EXPECT_TRUE(store->Put("c", "3").ok());

    std::string seen;
    MT_EXPECT_TRUE(store->Fold([&seen](const std::string& key, const std::string&) {
                            seen += key;
                            return std::string();
                        })
                       .ok());
    MT_EXPECT_EQ(seen, std::string("abc"));

    // A visitor error aborts the walk at that entry.
    std::string partial;
    const agentenv::core::Expected<agentenv::core::Unit, std::string> aborted =
        store->Fold([&partial](const std::string& key, const std::string&) {
            partial += key;
            return key == "b" ? std::string("stop at b") : std::string();
        });
    MT_EXPECT_TRUE(!aborted.ok());
    MT_EXPECT_EQ(aborted.error(), std::string("stop at b"));
    MT_EXPECT_EQ(partial, std::string("ab"));
}

MT_TEST(scan_prefix_returns_only_matching_keys) {
    TempRoot root;
    const std::shared_ptr<KvStore> store = OpenAt(root.Join("store.log"), Durability::Wal);
    MT_EXPECT_TRUE(store->Put("sbx:1", "a").ok());
    MT_EXPECT_TRUE(store->Put("sbx:2", "b").ok());
    MT_EXPECT_TRUE(store->Put("tpl:1", "c").ok());
    // Sorts before "sbx:", so lower_bound must not start too early.
    MT_EXPECT_TRUE(store->Put("aaa", "d").ok());

    const std::vector<Entry> scanned = store->ScanPrefix("sbx:").value();
    MT_EXPECT_EQ(scanned.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(scanned[0].first, std::string("sbx:1"));
    MT_EXPECT_EQ(scanned[1].first, std::string("sbx:2"));

    MT_EXPECT_EQ(store->ScanPrefix("zzz").value().size(), static_cast<std::size_t>(0));
    // An empty prefix matches everything.
    MT_EXPECT_EQ(store->ScanPrefix("").value().size(), static_cast<std::size_t>(4));
}

MT_TEST(write_batch_applies_all_mutations) {
    TempRoot root;
    const std::shared_ptr<KvStore> store = OpenAt(root.Join("store.log"), Durability::Wal);
    MT_EXPECT_TRUE(store->Put("stale", "x").ok());

    std::vector<BatchOp> ops;
    ops.push_back(BatchOp::Put("a", "1"));
    ops.push_back(BatchOp::Put("b", "2"));
    ops.push_back(BatchOp::Delete("stale"));
    MT_EXPECT_TRUE(store->WriteBatch(ops).ok());

    MT_EXPECT_EQ(*store->Get("a").value(), std::string("1"));
    MT_EXPECT_EQ(*store->Get("b").value(), std::string("2"));
    MT_EXPECT_TRUE(!store->Get("stale").value().has_value());
}

MT_TEST(empty_batch_is_a_noop) {
    TempRoot root;
    const std::string path = root.Join("store.log");
    const std::shared_ptr<KvStore> store = OpenAt(path, Durability::Wal);

    MT_EXPECT_TRUE(store->WriteBatch(std::vector<BatchOp>()).ok());
    // Rust returns early without touching the database, so no log is created.
    MT_EXPECT_TRUE(!fs::Exists(path));
}

MT_TEST(batch_ops_compare_by_kind_and_payload) {
    MT_EXPECT_TRUE(BatchOp::Put("k", "v") == BatchOp::Put("k", "v"));
    MT_EXPECT_TRUE(BatchOp::Put("k", "v") != BatchOp::Put("k", "w"));
    MT_EXPECT_TRUE(BatchOp::Put("k", "v") != BatchOp::Delete("k"));
    MT_EXPECT_TRUE(BatchOp::Delete("k") == BatchOp::Delete("k"));
    MT_EXPECT_TRUE(BatchOp::Delete("k") != BatchOp::Delete("j"));
}

// ---------------------------------------------------------------------------
// Durability
// ---------------------------------------------------------------------------

MT_TEST(wal_durability_survives_a_reopen) {
    TempRoot root;
    const std::string path = root.Join("store.log");

    {
        const std::shared_ptr<KvStore> store = OpenAt(path, Durability::Wal);
        MT_EXPECT_TRUE(store->Put("kept", "1").ok());
        MT_EXPECT_TRUE(store->Put("removed", "2").ok());
        MT_EXPECT_TRUE(store->Delete("removed").ok());
    }

    const std::shared_ptr<KvStore> reopened = OpenAt(path, Durability::Wal);
    MT_EXPECT_EQ(*reopened->Get("kept").value(), std::string("1"));
    // The delete was logged too, so it must not come back.
    MT_EXPECT_TRUE(!reopened->Get("removed").value().has_value());
}

MT_TEST(sync_durability_survives_a_reopen) {
    TempRoot root;
    const std::string path = root.Join("store.log");
    {
        const std::shared_ptr<KvStore> store = OpenAt(path, Durability::Sync);
        MT_EXPECT_TRUE(store->Put("critical", "value").ok());
    }
    MT_EXPECT_EQ(*OpenAt(path, Durability::Sync)->Get("critical").value(),
                 std::string("value"));
}

MT_TEST(memory_durability_writes_no_log) {
    TempRoot root;
    const std::string path = root.Join("store.log");

    const std::shared_ptr<KvStore> store = OpenAt(path, Durability::Memory);
    MT_EXPECT_TRUE(store->Put("ephemeral", "1").ok());
    // Readable in-process...
    MT_EXPECT_EQ(*store->Get("ephemeral").value(), std::string("1"));
    // ...but nothing hit the disk, so a restart loses it. That is the
    // documented trade-off for regenerable indexes.
    MT_EXPECT_TRUE(!fs::Exists(path));
    MT_EXPECT_TRUE(!OpenAt(path, Durability::Memory)->Get("ephemeral").value().has_value());
}

MT_TEST(durability_is_reported_and_never_leaks_contents) {
    TempRoot root;
    const std::shared_ptr<KvStore> store = OpenAt(root.Join("store.log"), Durability::Sync);
    MT_EXPECT_TRUE(store->Put("secret-key", "secret-value").ok());

    MT_EXPECT_EQ(store->durability(), Durability::Sync);
    // Rust's Debug is `finish_non_exhaustive`: durability only.
    const std::string debug = store->ToDebugString();
    MT_EXPECT_TRUE(debug.find("Sync") != std::string::npos);
    MT_EXPECT_TRUE(debug.find("secret-key") == std::string::npos);
    MT_EXPECT_TRUE(debug.find("secret-value") == std::string::npos);
}

MT_TEST(a_torn_log_tail_is_dropped_not_fatal) {
    TempRoot root;
    const std::string path = root.Join("store.log");
    {
        const std::shared_ptr<KvStore> store = OpenAt(path, Durability::Wal);
        MT_EXPECT_TRUE(store->Put("complete", "1").ok());
    }

    // Simulate a crash mid-append by truncating a partial record onto the end.
    const agentenv::core::Expected<std::string, std::string> log = fs::ReadToString(path);
    MT_EXPECT_TRUE(log.ok());
    MT_EXPECT_TRUE(fs::Write(path, log.value() + "P0000").ok());

    // Everything before the torn tail must still load.
    const std::shared_ptr<KvStore> reopened = OpenAt(path, Durability::Wal);
    MT_EXPECT_EQ(*reopened->Get("complete").value(), std::string("1"));
}

// ---------------------------------------------------------------------------
// Capabilities (crates/linux-cap)
// ---------------------------------------------------------------------------

MT_TEST(parses_capability_sets) {
    // Rust: `parses_capability_sets` in src/privileges.rs.
    const std::string status =
        "CapInh:\t0000000000201000\nCapPrm:\t0000000000201000\nCapEff:\t0000000000201000\n";
    const agentenv::core::Expected<cap::CapabilitySets, std::string> sets =
        cap::CapabilitySets::FromProcStatus(status);
    MT_EXPECT_TRUE(sets.ok());
    MT_EXPECT_TRUE(sets.value().IsDelegable(cap::kCapNetAdmin).value());
    MT_EXPECT_TRUE(sets.value().IsDelegable(cap::kCapSysAdmin).value());
}

MT_TEST(capability_must_be_present_in_all_required_sets) {
    // Rust: `capability_must_be_present_in_all_required_sets`. CAP_SYS_ADMIN
    // is inheritable and permitted but not effective, so it is not delegable.
    const std::string status =
        "CapInh:\t0000000000201000\nCapPrm:\t0000000000201000\nCapEff:\t0000000000001000\n";
    const agentenv::core::Expected<cap::CapabilitySets, std::string> sets =
        cap::CapabilitySets::FromProcStatus(status);
    MT_EXPECT_TRUE(sets.ok());
    MT_EXPECT_TRUE(sets.value().IsDelegable(cap::kCapNetAdmin).value());
    MT_EXPECT_TRUE(!sets.value().IsDelegable(cap::kCapSysAdmin).value());
}

MT_TEST(parses_and_queries_capability_sets) {
    // Rust: `parses_and_queries_capability_sets` in crates/linux-cap.
    const std::string status =
        "CapInh:\t0000000000001000\nCapPrm:\t0000000000201000\nCapEff:\t0000000000201000\n";
    const agentenv::core::Expected<cap::CapabilitySets, std::string> sets =
        cap::CapabilitySets::FromProcStatus(status);
    MT_EXPECT_TRUE(sets.ok());
    // CAP_NET_ADMIN is in all three sets.
    MT_EXPECT_TRUE(sets.value().IsDelegable(cap::kCapNetAdmin).value());
    // CAP_SYS_ADMIN is effective but not inheritable, so not delegable.
    MT_EXPECT_TRUE(!sets.value().IsDelegable(cap::kCapSysAdmin).value());
    MT_EXPECT_TRUE(sets.value().Effective(cap::kCapSysAdmin).value());
}

MT_TEST(builds_masks_for_both_capability_words) {
    // Rust: `builds_masks_for_both_capability_words`.
    std::vector<int> capabilities;
    capabilities.push_back(0);
    capabilities.push_back(31);
    capabilities.push_back(32);
    capabilities.push_back(63);

    const agentenv::core::Expected<std::vector<uint32_t>, std::string> masks =
        cap::CapabilityMasks(capabilities);
    MT_EXPECT_TRUE(masks.ok());
    MT_EXPECT_EQ(masks.value().size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(masks.value()[0], static_cast<uint32_t>(0x80000001));
    MT_EXPECT_EQ(masks.value()[1], static_cast<uint32_t>(0x80000001));
}

MT_TEST(rejects_capabilities_outside_version_three_range) {
    // Rust: `rejects_capabilities_outside_version_three_range`.
    const int invalid[] = {-1, 64, 1000};
    for (std::size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        std::vector<int> capabilities;
        capabilities.push_back(invalid[i]);
        MT_EXPECT_TRUE(!cap::CapabilityMasks(capabilities).ok());
    }
    // Queries reject them too, rather than reading a bit out of range.
    const agentenv::core::Expected<cap::CapabilitySets, std::string> sets =
        cap::CapabilitySets::FromProcStatus(
            "CapInh:\t0\nCapPrm:\t0\nCapEff:\t0\n");
    MT_EXPECT_TRUE(sets.ok());
    MT_EXPECT_TRUE(!sets.value().IsDelegable(64).ok());
    MT_EXPECT_TRUE(!sets.value().Effective(-1).ok());
}

MT_TEST(malformed_proc_status_is_rejected) {
    // A missing field must not silently read as zero capabilities.
    MT_EXPECT_TRUE(!cap::CapabilitySets::FromProcStatus("CapInh:\t0\nCapPrm:\t0\n").ok());
    MT_EXPECT_TRUE(!cap::CapabilitySets::FromProcStatus("").ok());
    // Non-hex values are rejected rather than parsed as a prefix.
    MT_EXPECT_TRUE(
        !cap::CapabilitySets::FromProcStatus("CapInh:\tzz\nCapPrm:\t0\nCapEff:\t0\n").ok());
}

MT_TEST(capability_names_match_the_operator_message) {
    MT_EXPECT_EQ(std::string(cap::CapabilityName(cap::kCapNetAdmin)),
                 std::string("CAP_NET_ADMIN"));
    MT_EXPECT_EQ(std::string(cap::CapabilityName(cap::kCapSysAdmin)),
                 std::string("CAP_SYS_ADMIN"));
    MT_EXPECT_EQ(std::string(cap::CapabilityName(7)), std::string("unknown capability"));
}

MT_TEST(scoped_spawn_requires_a_program) {
    agentenv::privileges::ScopedSpawnRequest request;
    const agentenv::core::Expected<pid_t, std::string> spawned =
        agentenv::privileges::SpawnScoped(request);
    MT_EXPECT_TRUE(!spawned.ok());
}

MT_TEST(scoped_operation_reports_its_own_failure) {
    // With an empty capability set the scoping itself must succeed on any
    // host, so the error seen here is the operation's own.
    const agentenv::core::Expected<agentenv::core::Unit, std::string> result =
        agentenv::privileges::RunWithScopedCapabilities(
            std::vector<int>(), []() { return std::string("operation said no"); });
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_EQ(result.error(), std::string("operation said no"));
}

int main() { return microtest::RunAll(); }
