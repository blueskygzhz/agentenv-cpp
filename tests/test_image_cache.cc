// SPDX-License-Identifier: MIT
// Tests for image::cache — the pure-logic subset of graph.rs + gc.rs.
#include "microtest.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "agentenv/image/cache.h"

using namespace agentenv;
using namespace agentenv::image;

namespace {

cache::HardCommitId Hard(const std::string& d) {
  auto r = cache::HardCommitId::New(d);
  return r.value();
}

cache::ImageCacheConfigId Config(const std::string& name) {
    auto r = cache::ImageCacheConfigId::FromFilename(name);
    return r.value();
}

cache::ImageCacheHoldOwner Owner(const std::string& ns, const std::string& k) {
    auto r = cache::ImageCacheHoldOwner::New(ns, k);
    return r.value();
}

}  // namespace

MT_TEST(hard_commit_id_rejects_blank) {
    MT_EXPECT_TRUE(cache::HardCommitId::New("sha256:x").ok());
    MT_EXPECT_TRUE(!cache::HardCommitId::New("   ").ok());
    MT_EXPECT_TRUE(!cache::HardCommitId::New("").ok());
}

MT_TEST(config_id_requires_image_json_suffix) {
    MT_EXPECT_TRUE(cache::ImageCacheConfigId::FromFilename("foo-image.json").ok());
    MT_EXPECT_TRUE(!cache::ImageCacheConfigId::FromFilename("foo.json").ok());
    MT_EXPECT_TRUE(cache::IsRegularConfigFilename("a-image.json"));
    MT_EXPECT_TRUE(!cache::IsRegularConfigFilename("a-image.jsonx"));
}

MT_TEST(config_id_from_path_takes_basename) {
    auto r = cache::ImageCacheConfigId::FromConfigPath("/var/cache/x-image.json");
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().AsStr() == "x-image.json");
    MT_EXPECT_TRUE(!cache::ImageCacheConfigId::FromConfigPath("/var/cache/x.json").ok());
}

MT_TEST(hold_owner_validates_and_displays) {
    auto ok = cache::ImageCacheHoldOwner::New("ns", "k");
    MT_EXPECT_TRUE(ok.ok());
    MT_EXPECT_TRUE(ok.value().ToString() == "ns/k");
    MT_EXPECT_TRUE(!cache::ImageCacheHoldOwner::New("", "k").ok());
    MT_EXPECT_TRUE(!cache::ImageCacheHoldOwner::New("ns", "  ").ok());
}

MT_TEST(hex_encode_matches_rust) {
    // 'a' = 0x61, 'b' = 0x62.
    MT_EXPECT_TRUE(cache::HexEncode("ab") == "6162");
    MT_EXPECT_TRUE(cache::HexEncode("") == "");
}

MT_TEST(key_encoding_is_prefix_plus_hex_joined) {
    cache::HardCommitId d = Hard("sha256:one");
    // "sha256:one" hex.
    std::string hex = cache::HexEncode("sha256:one");
    MT_EXPECT_TRUE(cache::HardCommitObjectKey(d) ==
    std::string("object/hard-commit/") + hex);

    cache::ImageCacheConfigId cfg = Config("a-image.json");
    std::string cfg_hex = cache::HexEncode("a-image.json");
    MT_EXPECT_TRUE(cache::ConfigToHardKey(cfg, d) ==
      std::string("ref/config-to-hard/") + cfg_hex + "/" + hex);
    // Prefix scans end with '/'.
    MT_EXPECT_TRUE(cache::ConfigToHardPrefixForConfig(cfg) ==
       std::string("ref/config-to-hard/") + cfg_hex + "/");

    // Mirror keys are distinct forward/reverse orderings.
    cache::ImageCacheHoldOwner o = Owner("ns", "key");
 MT_EXPECT_TRUE(cache::HoldToHardKey(o, d) != cache::HardToHoldKey(d, o));
}

MT_TEST(plan_eviction_returns_empty_below_high_watermark) {
    std::map<cache::ImageCacheConfigId, std::set<cache::HardCommitId> > refs;
    std::map<cache::HardCommitId, uint64_t> sizes;
    sizes.insert(std::make_pair(Hard("sha256:x"), 100u));
    std::map<cache::ImageCacheConfigId, uint64_t> last_used;

    cache::CapacityEvictionPlan plan =
        cache::PlanCapacityEviction(refs, sizes, last_used, 1000, 0, ~0ull);
    MT_EXPECT_TRUE(plan.total_bytes == 100u);
    MT_EXPECT_TRUE(plan.candidates.empty());
}

MT_TEST(plan_eviction_frees_shared_commit_only_when_all_referrers_evicted) {
    cache::ImageCacheConfigId a = Config("a-image.json");
    cache::ImageCacheConfigId b = Config("b-image.json");
    cache::HardCommitId shared = Hard("sha256:shared");
    cache::HardCommitId a_only = Hard("sha256:a-only");

    std::map<cache::ImageCacheConfigId, std::set<cache::HardCommitId> > refs;
  {
        std::set<cache::HardCommitId> sa;
        sa.insert(shared); sa.insert(a_only);
 refs.insert(std::make_pair(a, sa));
      std::set<cache::HardCommitId> sb;
        sb.insert(shared);
        refs.insert(std::make_pair(b, sb));
    }
    std::map<cache::HardCommitId, uint64_t> sizes;
    sizes.insert(std::make_pair(shared, 500u));
    sizes.insert(std::make_pair(a_only, 50u));
    std::map<cache::ImageCacheConfigId, uint64_t> last_used;
    last_used.insert(std::make_pair(a, 100u));
    last_used.insert(std::make_pair(b, 100u));

    // high=0 forces eviction; low=0 cleans everything eligible.
    cache::CapacityEvictionPlan plan =
        cache::PlanCapacityEviction(refs, sizes, last_used, 0, 0, ~0ull);
    MT_EXPECT_TRUE(plan.total_bytes == 550u);
    MT_EXPECT_TRUE(plan.candidates.size() == 2u);
    // Same last_used -> ordered by config_id: a before b.
    MT_EXPECT_TRUE(plan.candidates[0].config_id == a);
    MT_EXPECT_TRUE(plan.candidates[1].config_id == b);
}

MT_TEST(plan_eviction_skips_configs_used_after_cutoff) {
    cache::ImageCacheConfigId a = Config("a-image.json");
    std::map<cache::ImageCacheConfigId, std::set<cache::HardCommitId> > refs;
    std::set<cache::HardCommitId> sa;
    sa.insert(Hard("sha256:x"));
    refs.insert(std::make_pair(a, sa));
    std::map<cache::HardCommitId, uint64_t> sizes;
    sizes.insert(std::make_pair(Hard("sha256:x"), 100u));
    std::map<cache::ImageCacheConfigId, uint64_t> last_used;
    last_used.insert(std::make_pair(a, 200u));

    // evictable_before=100 < a.last_used=200 -> a is not evictable.
 cache::CapacityEvictionPlan plan =
        cache::PlanCapacityEviction(refs, sizes, last_used, 0, 0, 100);
    MT_EXPECT_TRUE(plan.total_bytes == 100u);
  MT_EXPECT_TRUE(plan.candidates.empty());
}

MT_TEST(gc_summary_counts_blocked_as_retained) {
    cache::ImageCacheGcReport report;
    report.collected = 3;
    report.freed_bytes = 4096;
    cache::ImageCacheGcBlockedReason r1;
    r1.kind = cache::ImageCacheGcBlockedReason::Held;
    cache::ImageCacheGcBlockedReason r2;
    r2.kind = cache::ImageCacheGcBlockedReason::Unverifiable;
    r2.detail = "missing file";
    report.blocked.push_back(cache::ImageCacheGcBlocked(Hard("sha256:a"), r1));
    report.blocked.push_back(cache::ImageCacheGcBlocked(Hard("sha256:b"), r2));

    cache::ImageCacheGcSummary s = cache::SummaryFromReport(report);
    MT_EXPECT_TRUE(s.collected == 3u);
    MT_EXPECT_TRUE(s.freed_bytes == 4096u);
    MT_EXPECT_TRUE(s.retained == 2u);
}

MT_MAIN
