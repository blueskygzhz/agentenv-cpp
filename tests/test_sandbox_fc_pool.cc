// SPDX-License-Identifier: MIT
// Tests for firecracker::MmdsMetadata (SHA-512 + e2b JSON) and FirecrackerPool.
#include "microtest.h"

#include "agentenv/core/json.h"
#include "agentenv/sandbox/firecracker/mmds.h"
#include "agentenv/sandbox/firecracker/pool.h"

using namespace agentenv::sandbox::firecracker;
namespace warmpool = agentenv::warmpool;

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

// ---- FirecrackerPool watermark management ----
// A trivial Instance stub; the pool only needs to hold/hand out shared_ptrs.
struct StubInstance : public Instance {
    agentenv::core::Expected<agentenv::core::Unit, std::string> Boot() override {
    return agentenv::core::Unit{};
    }
    agentenv::core::Expected<agentenv::core::Unit, std::string> Shutdown() override {
        return agentenv::core::Unit{};
    }
  agentenv::core::Expected<agentenv::core::Unit, std::string>
    SetMmds(const MmdsMetadata&) override { return agentenv::core::Unit{}; }
};

static warmpool::PoolConfig make_cfg(std::size_t low, std::size_t high) {
  warmpool::PoolConfig c;
    c.low_watermark = low;
    c.high_watermark = high;
 c.maintenance_enabled = true;
    return c;
}

MT_TEST(pool_prime_fills_to_low_watermark) {
    int created = 0;
    FirecrackerPool pool(make_cfg(2, 4), [&]() -> std::shared_ptr<Instance> {
        ++created;
    return std::shared_ptr<Instance>(new StubInstance());
    });
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 0);
    pool.Prime();
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 2);
    MT_EXPECT_EQ(created, 2);
}

MT_TEST(pool_acquire_hands_out_and_empties) {
    FirecrackerPool pool(make_cfg(2, 4), []() -> std::shared_ptr<Instance> {
    return std::shared_ptr<Instance>(new StubInstance());
    });
    pool.Prime();
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 2);
    auto a = pool.Acquire();
    MT_EXPECT_TRUE(a.ok());
    auto b = pool.Acquire();
    MT_EXPECT_TRUE(b.ok());
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 0);
    // now empty -> cold path error
    auto c = pool.Acquire();
    MT_EXPECT_TRUE(!c.ok());
}

MT_TEST(pool_maintenance_drains_above_high_watermark) {
    FirecrackerPool pool(make_cfg(1, 2), []() -> std::shared_ptr<Instance> {
      return std::shared_ptr<Instance>(new StubInstance());
    });
    // Release 4 entries (maintenance_enabled lets it exceed high watermark).
    for (int i = 0; i < 4; ++i) pool.Release(std::shared_ptr<Instance>(new StubInstance()));
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 4);
    std::size_t drained = pool.RunMaintenanceCycle();
    MT_EXPECT_EQ(static_cast<int>(drained), 2);  // down to high watermark 2
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 2);
}

MT_TEST(pool_factory_failure_stops_fill) {
    int calls = 0;
    warmpool::PoolConfig fc; fc.low_watermark = 3; fc.high_watermark = 5; fc.maintenance_enabled = true;
    FirecrackerPool pool(fc, [&]() -> std::shared_ptr<Instance> {
        ++calls;
        if (calls >= 2) return std::shared_ptr<Instance>();  // fail on 2nd
        return std::shared_ptr<Instance>(new StubInstance());
    });
    std::size_t created = pool.RunMaintenanceCycle();
    MT_EXPECT_EQ(static_cast<int>(created), 1);  // only first succeeded
    MT_EXPECT_EQ(static_cast<int>(pool.WarmLen()), 1);
}

MT_MAIN
