// SPDX-License-Identifier: MIT
// Tests for firecracker::MmdsMetadata (SHA-512 + e2b JSON) and FirecrackerPool.
#include "microtest.h"

#include "agentenv/core/json.h"
#include "agentenv/sandbox/firecracker/mmds.h"
#include "agentenv/sandbox/firecracker/pool.h"

using namespace agentenv::sandbox::firecracker;
namespace warmpool = agentenv::warmpool;
namespace core = agentenv::core;

// SHA-512("") known FIPS vector.
MT_TEST(sha512_empty_vector) {
    std::string h = MmdsMetadata::HashAccessToken("");
    MT_EXPECT_TRUE(h ==
   "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
}

// SHA-512("abc") known FIPS vector.
MT_TEST(sha512_abc_vector) {
    std::string h = MmdsMetadata::HashAccessToken("abc");
    MT_EXPECT_TRUE(h ==
      "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
}

namespace {
agentenv::core::SandboxId NilSandboxId() {
    agentenv::core::Uuid nil;  // default-constructed == all zero
    return agentenv::core::SandboxId(nil);
}
agentenv::core::Json ParsedValue(const MmdsMetadata& m) {
    agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> v =
        agentenv::core::Json::Parse(m.ToJson());
    MT_EXPECT_TRUE(v.ok());
    return v.ok() ? v.value() : agentenv::core::Json();
}
agentenv::core::Json J(const std::string& text) {
    return agentenv::core::Json::Parse(text).value();
}
agentenv::sandbox::EnvdAccessToken Token(const std::string& v) {
    return agentenv::sandbox::EnvdAccessToken(v);
}
}  // namespace

// mmds.rs :: serializes_with_e2b_mmds_field_names — exactly four keys.
MT_TEST(mmds_serializes_with_e2b_mmds_field_names) {
    MmdsMetadata m(NilSandboxId(), "snapshot-123");
    MT_EXPECT_EQ(m.ToJson(),
                 J("{\"instanceID\":\"00000000-0000-0000-0000-000000000000\","
                   "\"envID\":\"snapshot-123\",\"address\":\"\","
                   "\"accessTokenHash\":\"" + MmdsMetadata::HashAccessToken("") + "\"}")
                     .ToString());
}

// mmds.rs :: hashes_secure_access_token_with_sha512
MT_TEST(mmds_hashes_secure_access_token_with_sha512) {
    const std::string token = "super-secret-token";
    MmdsMetadata m(NilSandboxId(), "snapshot-123");
    // Seeded with the hash of the empty token, not an empty string.
    MT_EXPECT_EQ(m.AccessTokenHash(), MmdsMetadata::HashAccessToken(""));
    m.WithAccessToken(agentenv::core::Optional<agentenv::sandbox::EnvdAccessToken>(Token(token)));
    MT_EXPECT_EQ(m.AccessTokenHash(), MmdsMetadata::HashAccessToken(token));
    MT_EXPECT_TRUE(m.ToJson().find(token) == std::string::npos);
    // `set_access_token(None)` goes back to the empty-token digest.
    m.SetAccessToken(agentenv::core::nullopt);
    MT_EXPECT_EQ(m.AccessTokenHash(), MmdsMetadata::HashAccessToken(""));
}

// mmds.rs :: extra_fields_are_flattened_into_top_level
MT_TEST(mmds_extra_fields_are_flattened_into_top_level) {
    agentenv::core::JsonObject extra;
    extra["imageConfigs"] = J("{\"rootfs\":{\"Cmd\":[\"/bin/sh\"]}}");
    MmdsMetadata m(NilSandboxId(), "snap-1");
    m.WithExtra(extra);
    agentenv::core::Json v = ParsedValue(m);
    MT_EXPECT_EQ(v["imageConfigs"].ToString(), J("{\"rootfs\":{\"Cmd\":[\"/bin/sh\"]}}").ToString());
    MT_EXPECT_EQ(v["instanceID"].as_string(), std::string("00000000-0000-0000-0000-000000000000"));
}

// mmds.rs :: extra_fields_cannot_override_reserved_fields
MT_TEST(mmds_extra_fields_cannot_override_reserved_fields) {
    MmdsMetadata expected(NilSandboxId(), "snapshot-123");
    expected.WithAccessToken(
        agentenv::core::Optional<agentenv::sandbox::EnvdAccessToken>(Token("a-real-token")));
    agentenv::core::JsonObject extra;
    for (int i = 0; i < 4; ++i) {
        extra[kMmdsReservedFields[i]] = agentenv::core::Json("attacker-controlled");
    }
    MmdsMetadata m = expected;
    m.WithExtra(extra);
    MT_EXPECT_TRUE(m == expected);
    MT_EXPECT_EQ(m.ExtraCount(), static_cast<std::size_t>(0));
    MT_EXPECT_TRUE(m.ToJson().find("attacker-controlled") == std::string::npos);
}

// The previous port pasted extras into the document as raw text, so a value
// that was not a single JSON value could close the object early and add its
// own `accessTokenHash` — defeating the reserved-key filter. Values are JSON
// now; a string full of JSON syntax stays a string.
MT_TEST(mmds_extra_value_cannot_inject_fields) {
    MmdsMetadata m(NilSandboxId(), "snap-1");
    agentenv::core::JsonObject extra;
    extra["custom"] = agentenv::core::Json("1,\"accessTokenHash\":\"forged\"");
    extra["line\nbreak\"key"] = agentenv::core::Json("tab\there");
    m.WithExtra(extra);
    agentenv::core::Json v = ParsedValue(m);
    MT_EXPECT_EQ(v["accessTokenHash"].as_string(), MmdsMetadata::HashAccessToken(""));
    MT_EXPECT_EQ(v["custom"].as_string(), std::string("1,\"accessTokenHash\":\"forged\""));
    MT_EXPECT_EQ(v["line\nbreak\"key"].as_string(), std::string("tab\there"));
}

// Rust `Map::extend` replaces an existing key; the guest must never see two.
MT_TEST(mmds_with_extra_replaces_existing_key) {
    MmdsMetadata m(NilSandboxId(), "snap-1");
    agentenv::core::JsonObject first, second;
    first["custom"] = agentenv::core::Json(static_cast<int64_t>(1));
    second["custom"] = agentenv::core::Json(static_cast<int64_t>(2));
    m.WithExtra(first).WithExtra(second);
    MT_EXPECT_EQ(m.ExtraCount(), static_cast<std::size_t>(1));
    const std::string j = m.ToJson();
    MT_EXPECT_EQ(j.find("\"custom\""), j.rfind("\"custom\""));
    MT_EXPECT_EQ(ParsedValue(m)["custom"].as_int(), static_cast<int64_t>(2));
}

// mmds.rs :: round_trips_with_extra_fields
MT_TEST(mmds_round_trips_with_extra_fields) {
    agentenv::core::JsonObject extra;
    extra["custom"] = agentenv::core::Json(static_cast<int64_t>(42));
    MmdsMetadata original(NilSandboxId(), "snap-1");
    original.WithExtra(extra);
    agentenv::core::Expected<MmdsMetadata, std::string> restored =
        MmdsMetadata::FromValue(J(original.ToJson()));
    MT_EXPECT_TRUE(restored.ok());
    MT_EXPECT_TRUE(restored.value() == original);
}

// mmds.rs :: deserializes_legacy_json_without_extra_fields
MT_TEST(mmds_deserializes_legacy_json_without_extra_fields) {
    agentenv::core::Expected<MmdsMetadata, std::string> m = MmdsMetadata::FromValue(
        J("{\"instanceID\":\"00000000-0000-0000-0000-000000000000\",\"envID\":\"snapshot-123\","
          "\"address\":\"\",\"accessTokenHash\":\"" + MmdsMetadata::HashAccessToken("") + "\"}"));
    MT_EXPECT_TRUE(m.ok());
    MT_EXPECT_EQ(m.value().ExtraCount(), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(m.value().SandboxId(), std::string("00000000-0000-0000-0000-000000000000"));
}

MT_TEST(mmds_deserialize_requires_core_fields) {
    MT_EXPECT_TRUE(!MmdsMetadata::FromValue(
                        J("{\"instanceID\":\"x\",\"envID\":\"y\",\"address\":\"\"}"))
                        .ok());
    MT_EXPECT_TRUE(!MmdsMetadata::FromValue(J("{\"instanceID\":1,\"envID\":\"y\","
                                              "\"address\":\"\",\"accessTokenHash\":\"h\"}"))
                        .ok());
    MT_EXPECT_TRUE(!MmdsMetadata::FromValue(J("[]")).ok());
}

// ---- FirecrackerPool ----
//
// A warm entry is `slot + VMM process + work dir` and is handed out as one
// unit, never returned. These cover the two things that makes easy to get
// wrong: that every entry taken out of the pool is destroyed exactly once
// (nothing leaks a slot or a work dir), and that an entry is only handed to a
// caller whose stdio expectations match the one it was spawned with.

namespace {

/// A warm entry with no live kernel state behind it. Paired with a fake
/// cleanup, so the null slot is never dereferenced.
std::shared_ptr<WarmFirecracker> FakeWarm(const std::string& tag) {
    std::shared_ptr<FirecrackerInstance> inst(
        new FirecrackerInstance("/tmp/agentenv-fc-test-" + tag, std::shared_ptr<Connector>()));
    return std::shared_ptr<WarmFirecracker>(
        new WarmFirecracker(NULL, inst, "/tmp/agentenv-fc-test-" + tag));
}

FirecrackerPool::Options OptionsFor(std::size_t low, std::size_t high) {
    FirecrackerPool::Options o;
    o.pool.low_watermark = low;
    o.pool.high_watermark = high;
    o.pool.maintenance_enabled = true;
    return o;
}

/// Records what the pool created and destroyed, so a test can assert that the
/// two balance rather than only that the count came out right.
///
/// Held by `shared_ptr` and captured by value, because `~FirecrackerPool` runs
/// `Shutdown` and therefore calls these callbacks: anything they touch has to
/// outlive the pool, which a stack local declared after it does not.
struct SpawnLedger {
    int spawned;
    int cleaned;
    int sync_cleanups;
    int fail_after;          ///< spawn fails once this many have succeeded
    std::string cleanup_err; ///< non-empty makes every cleanup fail

    SpawnLedger() : spawned(0), cleaned(0), sync_cleanups(0), fail_after(-1) {}
};

std::shared_ptr<SpawnLedger> InstallLedger(FirecrackerPool* pool) {
    std::shared_ptr<SpawnLedger> self(new SpawnLedger());
    pool->SetSpawnForTesting(
        [self]() -> core::Expected<std::shared_ptr<WarmFirecracker>, std::string> {
            if (self->fail_after >= 0 && self->spawned >= self->fail_after) {
                return core::make_unexpected(std::string("no free slot"));
            }
            ++self->spawned;
            char tag[32];
            std::snprintf(tag, sizeof(tag), "%d", self->spawned);
            return FakeWarm(tag);
        },
        [self](const std::shared_ptr<WarmFirecracker>&, bool sync)
            -> core::Expected<core::Unit, std::string> {
            ++self->cleaned;
            if (sync) ++self->sync_cleanups;
            if (!self->cleanup_err.empty()) {
                return core::make_unexpected(self->cleanup_err);
            }
            return core::Unit();
        });
    return self;
}

}  // namespace

MT_TEST(pool_prime_fills_to_low_watermark) {
    FirecrackerPool pool(OptionsFor(2, 4), NULL, NULL);
    std::shared_ptr<SpawnLedger> ledger = InstallLedger(&pool);

    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 0);
    pool.Prime();
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 2);
    MT_EXPECT_EQ(ledger->spawned, 2);
    // Nothing was destroyed on the way: an entry cleaned up during a fill is
    // one whose slot and work dir were allocated for nothing.
    MT_EXPECT_EQ(ledger->cleaned, 0);
}

MT_TEST(pool_acquire_hands_out_the_whole_unit_and_empties) {
    FirecrackerPool pool(OptionsFor(2, 4), NULL, NULL);
    std::shared_ptr<SpawnLedger> ledger = InstallLedger(&pool);
    pool.Prime();
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 2);

    core::Expected<std::shared_ptr<WarmFirecracker>, std::string> a = pool.Acquire();
    MT_EXPECT_TRUE(a.ok());
    if (a.ok()) {
        // The three members travel together. A caller given the process alone
        // would have a VM with no namespace and no socket directory.
        MT_EXPECT_TRUE(a.value()->GetInstance() != NULL);
        MT_EXPECT_TRUE(!a.value()->WorkDir().empty());
    }
    MT_EXPECT_TRUE(pool.Acquire().ok());
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 0);

    // Empty is an error, not a wait: the caller's fallback is a cold spawn,
    // which is slower but correct, so blocking here would be worse.
    core::Expected<std::shared_ptr<WarmFirecracker>, std::string> c = pool.Acquire();
    MT_EXPECT_TRUE(!c.ok());
    MT_EXPECT_TRUE(c.error().find("cold path") != std::string::npos);

    // Acquired entries are the caller's now; the pool never cleaned them up.
    MT_EXPECT_EQ(ledger->cleaned, 0);
}

MT_TEST(pool_refuses_an_entry_whose_stdio_does_not_match) {
    FirecrackerPool::Options o = OptionsFor(1, 2);
    o.capture_output = false;
    FirecrackerPool pool(o, NULL, NULL);
    std::shared_ptr<SpawnLedger> ledger = InstallLedger(&pool);
    pool.Prime();
    MT_EXPECT_TRUE(static_cast<int>(pool.WarmLen()) > 0);

    // Refused rather than served: stdout/stderr destinations are fixed when
    // the process is spawned, so this entry can never produce the log file the
    // caller is asking for. Serving it would lose the diagnostics silently.
    core::Expected<std::shared_ptr<WarmFirecracker>, std::string> want_capture =
        pool.AcquireFor(true);
    MT_EXPECT_TRUE(!want_capture.ok());
    MT_EXPECT_TRUE(want_capture.error().find("capture_output") != std::string::npos);
    // And the refusal did not consume the entry.
    MT_EXPECT_TRUE(static_cast<int>(pool.WarmLen()) > 0);
    MT_EXPECT_TRUE(pool.AcquireFor(false).ok());
}

MT_TEST(pool_fill_never_overshoots_the_high_watermark) {
    FirecrackerPool pool(OptionsFor(1, 2), NULL, NULL);
    std::shared_ptr<SpawnLedger> ledger = InstallLedger(&pool);

    // Driven well past the point where it should stop.
    for (int i = 0; i < 6; ++i) pool.RunMaintenanceCycle();

    MT_EXPECT_TRUE(pool.WarmLen() <= 2);
    // Nothing was spawned only to be destroyed. Each entry costs a network
    // slot, a work dir and a VMM process, so creating one above the ceiling
    // and immediately tearing it down is pure waste -- and the teardown is
    // where a leaked slot would come from.
    MT_EXPECT_EQ(ledger->cleaned, 0);
    MT_EXPECT_EQ(ledger->spawned, static_cast<int>(pool.WarmLen()));
}

MT_TEST(pool_destructor_destroys_what_is_left) {
    // The entries a running pool still holds are live VMM processes with live
    // network slots. Dropping the pool without tearing them down leaves them
    // with no owner and nothing to identify them as abandoned.
    std::shared_ptr<SpawnLedger> ledger;
    {
        FirecrackerPool pool(OptionsFor(2, 4), NULL, NULL);
        ledger = InstallLedger(&pool);
        pool.Prime();
        MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 2);
        MT_EXPECT_EQ(ledger->cleaned, 0);
    }
    MT_EXPECT_EQ(ledger->cleaned, 2);
    // The destructor is a process-teardown path, so it takes the synchronous
    // route: the graceful stop it would otherwise await may have no runtime
    // left to await on.
    MT_EXPECT_EQ(ledger->sync_cleanups, 2);
}

MT_TEST(pool_spawn_failure_stops_further_batches) {
    FirecrackerPool::Options o = OptionsFor(3, 5);
    o.fill_concurrency = 1;
    FirecrackerPool pool(o, NULL, NULL);
    std::shared_ptr<SpawnLedger> ledger = InstallLedger(&pool);
    ledger->fail_after = 1;  // the second spawn fails

    const std::size_t created = pool.RunMaintenanceCycle();
    MT_EXPECT_EQ(static_cast<int>(created), 1);
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 1);
    // A failing spawn is nearly always a resource that will not appear within
    // this cycle (no free slot, no KVM), so retrying it now only multiplies
    // the cost. Exactly one failed attempt, not three.
    MT_EXPECT_EQ(ledger->spawned, 1);
}

MT_TEST(pool_shutdown_destroys_every_entry_and_reports_all_failures) {
    FirecrackerPool pool(OptionsFor(3, 4), NULL, NULL);
    std::shared_ptr<SpawnLedger> ledger = InstallLedger(&pool);
    pool.Prime();
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 3);

    ledger->cleanup_err = "slot busy";
    core::Expected<core::Unit, std::string> r = pool.Shutdown(true);
    MT_EXPECT_TRUE(!r.ok());
    // Every entry was attempted even though the first failed: each holds a
    // distinct slot and work dir, so stopping at the first failure leaks the
    // rest with no record of them.
    MT_EXPECT_EQ(ledger->cleaned, 3);
    MT_EXPECT_EQ(ledger->sync_cleanups, 3);
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 0);
    // All three reported, not just the first -- an operator needs the full
    // list to know what leaked.
    std::size_t at = 0, joins = 0;
    while ((at = r.error().find(" | ", at)) != std::string::npos) { ++joins; at += 3; }
    MT_EXPECT_EQ(static_cast<int>(joins), 2);
}

MT_TEST(real_spawn_without_a_network_manager_is_an_error) {
    // Not a silent fallback to the parent namespace: a VMM spawned outside its
    // slot gets a working API socket and no guest connectivity, which shows up
    // later as a guest-side fault.
    FirecrackerPool pool(OptionsFor(1, 2), NULL, NULL);
    const std::size_t created = pool.RunMaintenanceCycle();
    MT_EXPECT_EQ(static_cast<int>(created), 0);
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 0);
}

MT_MAIN
