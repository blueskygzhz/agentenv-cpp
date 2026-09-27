// SPDX-License-Identifier: MIT
// Tests for firecracker::MmdsMetadata (SHA-512 + e2b JSON) and FirecrackerPool.
#include "microtest.h"

#include "agentenv/sandbox/firecracker/manifest.h"
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

MT_TEST(mmds_e2b_field_names) {
    // Nil UUID sandbox id.
    agentenv::core::Uuid nil;  // default-constructed == all zero
    agentenv::core::SandboxId sid(nil);
    MmdsMetadata m(sid, "snapshot-123");
    std::string j = m.ToJson();
    MT_EXPECT_TRUE(j.find("\"instanceID\":\"00000000-0000-0000-0000-000000000000\"")
               != std::string::npos);
  MT_EXPECT_TRUE(j.find("\"envID\":\"snapshot-123\"") != std::string::npos);
 MT_EXPECT_TRUE(j.find("\"address\":\"\"") != std::string::npos);
    MT_EXPECT_TRUE(j.find("\"accessTokenHash\":\"cf83e1357") != std::string::npos);
}

MT_TEST(mmds_extra_flattened) {
    agentenv::core::Uuid nil;
    agentenv::core::SandboxId sid(nil);
    MmdsMetadata m(sid, "snap-1");
    m.WithExtra("imageConfigs", "{\"rootfs\":{\"Cmd\":[\"/bin/sh\"]}}");
    std::string j = m.ToJson();
    MT_EXPECT_TRUE(j.find("\"imageConfigs\":{\"rootfs\":{\"Cmd\":[\"/bin/sh\"]}}")
   != std::string::npos);
    // core field still present
    MT_EXPECT_TRUE(j.find("\"instanceID\"") != std::string::npos);
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
    SetMmds(const MmdsData&) override { return agentenv::core::Unit{}; }
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
