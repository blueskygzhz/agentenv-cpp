// SPDX-License-Identifier: MIT
// Rust: src/image/cache/graph.rs `mod tests`.
#include "agentenv/image/cache_metadata_store.h"

#include <set>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/config.h"
#include "microtest.h"

using namespace agentenv;              // NOLINT
using namespace agentenv::image::cache;  // NOLINT

namespace {

struct TempRoot {
    std::string path;
    TempRoot() {
        auto d = core::fs::CreateTempDir("agentenv-image-cache-meta-");
        path = d.ok() ? d.value() : std::string("/tmp/agentenv-image-cache-fallback");
        core::fs::CreateDirAll(path);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
};

std::shared_ptr<ImageCacheMetadataStore> TestStore(const std::string& dir) {
    // Memory durability: these tests never restart the store, so the WAL
    // would only add fsync cost.
    auto s = ImageCacheMetadataStore::Open(dir + "/meta.db",
                                           local_store::Durability::Memory);
    MT_EXPECT_TRUE(s.ok());
    return s.value();
}

HardCommitId Hard(const std::string& digest) {
    auto h = HardCommitId::New(digest);
    MT_EXPECT_TRUE(h.ok());
    return h.value();
}

ImageCacheConfigId ConfigId(const std::string& name) {
    auto c = ImageCacheConfigId::FromFilename(name);
    MT_EXPECT_TRUE(c.ok());
    return c.value();
}

ImageCacheHoldOwner Owner(const std::string& ns, const std::string& key) {
    auto o = ImageCacheHoldOwner::New(ns, key);
    MT_EXPECT_TRUE(o.ok());
    return o.value();
}

/// Writes a cache-owned image config whose lowers are `file=` entries, i.e.
/// the shape that roots hard commits.
void WriteConfig(const std::string& path,
                 const std::vector<std::pair<std::string, uint64_t> >& lowers,
                 const std::string& file_dir) {
    storage::overlaybd::ImageConfig config;
    for (std::size_t i = 0; i < lowers.size(); ++i) {
        storage::overlaybd::LayerConfig layer;
        layer.digest = lowers[i].first;
        layer.size   = lowers[i].second;
        layer.file   = file_dir + "/" + lowers[i].first;
        config.lowers.push_back(layer);
    }
    auto w = core::fs::Write(path, storage::overlaybd::ImageConfigToJson(config));
    MT_EXPECT_TRUE(w.ok());
}

}  // namespace

// Rust `hard_commit_p2p_keys_are_persisted_and_preserved_by_config_reconcile`.
MT_TEST(image_cache_meta_p2p_keys_survive_config_reconcile) {
    TempRoot root;
    auto store = TestStore(root.path);

    const HardCommitId digest = Hard("sha256:aaa");
    std::set<p2p::P2pArtifactKey> keys;
    keys.insert("p2p-key-1");
    MT_EXPECT_TRUE(store->RecordHardCommitObject(
        digest, core::Optional<std::string>(root.path + "/aaa"),
        core::Optional<uint64_t>(11), keys).ok());

    // A config reconcile re-writes the object record; the P2P key set must be
    // merged rather than replaced.
    const std::string configs = root.path + "/configs";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(configs).ok());
    const std::string config_path = configs + "/app-image.json";
    std::vector<std::pair<std::string, uint64_t> > lowers;
    lowers.push_back(std::make_pair(std::string("sha256:aaa"), static_cast<uint64_t>(11)));
    WriteConfig(config_path, lowers, root.path);

    MT_EXPECT_TRUE(store->RecordConfigRefsFromConfigPath(config_path).ok());

    auto record = store->GetHardCommitObject(digest);
    MT_EXPECT_TRUE(record.ok());
    MT_EXPECT_TRUE(record.value().has_value());
    MT_EXPECT_EQ(record.value()->p2p_keys.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(record.value()->p2p_keys.count("p2p-key-1") == 1);
}

// Rust `adding_hard_commit_p2p_key_rejects_missing_object`.
MT_TEST(image_cache_meta_p2p_key_requires_existing_object) {
    TempRoot root;
    auto store = TestStore(root.path);
    MT_EXPECT_TRUE(!store->AddHardCommitP2pKey(Hard("sha256:missing"), "k").ok());
}

MT_TEST(image_cache_meta_add_p2p_key_is_idempotent) {
    TempRoot root;
    auto store = TestStore(root.path);

    const HardCommitId digest = Hard("sha256:bbb");
    MT_EXPECT_TRUE(store->RecordHardCommitObject(
        digest, core::Optional<std::string>(), core::Optional<uint64_t>(),
        std::set<p2p::P2pArtifactKey>()).ok());

    MT_EXPECT_TRUE(store->AddHardCommitP2pKey(digest, "k1").ok());
    MT_EXPECT_TRUE(store->AddHardCommitP2pKey(digest, "k1").ok());

    auto record = store->GetHardCommitObject(digest);
    MT_EXPECT_TRUE(record.ok() && record.value().has_value());
    MT_EXPECT_EQ(record.value()->p2p_keys.size(), static_cast<std::size_t>(1));
}

// Rust `hold_ref_lifecycle_replaces_stale_refs_then_clears_on_release`.
MT_TEST(image_cache_meta_hold_lifecycle) {
    TempRoot root;
    auto store = TestStore(root.path);

    const ImageCacheHoldOwner owner = Owner("sandbox", "sb-1");
    const HardCommitId a = Hard("sha256:a");
    const HardCommitId b = Hard("sha256:b");
    const HardCommitId c = Hard("sha256:c");

    std::set<HardCommitId> first;
    first.insert(a);
    first.insert(b);
    MT_EXPECT_TRUE(store->CreateOrReplaceHold(owner, first).ok());

    // Both directions of the mirror must be queryable.
    auto referrers_a = store->HardCommitHoldReferrers(a);
    MT_EXPECT_TRUE(referrers_a.ok());
    MT_EXPECT_EQ(referrers_a.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(referrers_a.value()[0] == owner);

    // Replacing the set drops the stale edge and adds the new one.
    std::set<HardCommitId> second;
    second.insert(b);
    second.insert(c);
    MT_EXPECT_TRUE(store->CreateOrReplaceHold(owner, second).ok());

    auto stale = store->HardCommitHoldReferrers(a);
    MT_EXPECT_TRUE(stale.ok());
    MT_EXPECT_TRUE(stale.value().empty());

    auto kept = store->HardCommitHoldReferrers(b);
    MT_EXPECT_TRUE(kept.ok());
    MT_EXPECT_EQ(kept.value().size(), static_cast<std::size_t>(1));

    auto added = store->HardCommitHoldReferrers(c);
    MT_EXPECT_TRUE(added.ok());
    MT_EXPECT_EQ(added.value().size(), static_cast<std::size_t>(1));

    // Releasing clears every edge and the hold record itself.
    MT_EXPECT_TRUE(store->ReleaseHold(owner).ok());
    for (int i = 0; i < 3; ++i) {
        const HardCommitId& digest = i == 0 ? b : (i == 1 ? c : a);
        auto refs = store->HardCommitHoldReferrers(digest);
        MT_EXPECT_TRUE(refs.ok());
        MT_EXPECT_TRUE(refs.value().empty());
    }
    std::vector<std::string> namespaces;
    namespaces.push_back("sandbox");
    auto owners = store->ListHoldOwnersInNamespaces(namespaces);
    MT_EXPECT_TRUE(owners.ok());
    MT_EXPECT_TRUE(owners.value().empty());
}

MT_TEST(image_cache_meta_release_holds_in_namespaces_is_scoped) {
    TempRoot root;
    auto store = TestStore(root.path);

    const ImageCacheHoldOwner transient = Owner("sandbox", "sb-1");
    const ImageCacheHoldOwner durable   = Owner("template", "tpl-1");
    std::set<HardCommitId> refs;
    refs.insert(Hard("sha256:a"));

    MT_EXPECT_TRUE(store->CreateOrReplaceHold(transient, refs).ok());
    MT_EXPECT_TRUE(store->CreateOrReplaceHold(durable, refs).ok());

    std::vector<std::string> transient_ns;
    transient_ns.push_back("sandbox");
    auto released = store->ReleaseHoldsInNamespaces(transient_ns);
    MT_EXPECT_TRUE(released.ok());
    MT_EXPECT_EQ(released.value().size(), static_cast<std::size_t>(1));

    // The durable namespace keeps its hold.
    auto remaining = store->HardCommitHoldReferrers(Hard("sha256:a"));
    MT_EXPECT_TRUE(remaining.ok());
    MT_EXPECT_EQ(remaining.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(remaining.value()[0] == durable);
}

// Rust `rebuild_from_configs_records_only_hard_commit_refs`.
MT_TEST(image_cache_meta_rebuild_from_configs) {
    TempRoot root;
    auto store = TestStore(root.path);

    const std::string configs = root.path + "/configs";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(configs).ok());

    std::vector<std::pair<std::string, uint64_t> > lowers;
    lowers.push_back(std::make_pair(std::string("sha256:a"), static_cast<uint64_t>(10)));
    lowers.push_back(std::make_pair(std::string("sha256:b"), static_cast<uint64_t>(20)));
    WriteConfig(configs + "/app-image.json", lowers, root.path);

    MT_EXPECT_TRUE(store->RebuildFromConfigs(configs).ok());

    auto referrers = store->HardCommitConfigReferrers(Hard("sha256:a"));
    MT_EXPECT_TRUE(referrers.ok());
    MT_EXPECT_EQ(referrers.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(referrers.value()[0] == ConfigId("app-image.json"));

    auto objects = store->ListHardCommitObjects();
    MT_EXPECT_TRUE(objects.ok());
    MT_EXPECT_EQ(objects.value().size(), static_cast<std::size_t>(2));
    // Digest-ordered regardless of the hex-encoded key order.
    MT_EXPECT_TRUE(objects.value()[0].digest == Hard("sha256:a"));
    MT_EXPECT_TRUE(objects.value()[1].digest == Hard("sha256:b"));

    // A config that disappears from disk loses its edges on the next rebuild.
    MT_EXPECT_TRUE(core::fs::RemoveFile(configs + "/app-image.json").ok());
    MT_EXPECT_TRUE(store->RebuildFromConfigs(configs).ok());

    auto gone = store->HardCommitConfigReferrers(Hard("sha256:a"));
    MT_EXPECT_TRUE(gone.ok());
    MT_EXPECT_TRUE(gone.value().empty());
}

// Rust `rebuild_from_configs_fails_closed_on_malformed_config`.
MT_TEST(image_cache_meta_rebuild_fails_closed_on_malformed_config) {
    TempRoot root;
    auto store = TestStore(root.path);

    const std::string configs = root.path + "/configs";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(configs).ok());
    MT_EXPECT_TRUE(core::fs::Write(configs + "/bad-image.json", "{not json").ok());

    // Failing closed matters: a partially-built graph would let GC delete a
    // commit that is actually still referenced.
    MT_EXPECT_TRUE(!store->RebuildFromConfigs(configs).ok());
}

MT_TEST(image_cache_meta_rebuild_on_missing_dir_is_noop) {
    TempRoot root;
    auto store = TestStore(root.path);
    MT_EXPECT_TRUE(store->RebuildFromConfigs(root.path + "/absent").ok());
}

// Rust `plan_capacity_eviction_frees_shared_commit_only_when_all_referrers_evicted`.
MT_TEST(image_cache_meta_capacity_eviction_shared_commit) {
    TempRoot root;
    auto store = TestStore(root.path);

    const std::string configs = root.path + "/configs";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(configs).ok());

    // Two configs share `sha256:shared`; each also has a private layer.
    std::vector<std::pair<std::string, uint64_t> > first;
    first.push_back(std::make_pair(std::string("sha256:shared"), static_cast<uint64_t>(100)));
    first.push_back(std::make_pair(std::string("sha256:one"), static_cast<uint64_t>(10)));
    WriteConfig(configs + "/one-image.json", first, root.path);

    std::vector<std::pair<std::string, uint64_t> > second;
    second.push_back(std::make_pair(std::string("sha256:shared"), static_cast<uint64_t>(100)));
    second.push_back(std::make_pair(std::string("sha256:two"), static_cast<uint64_t>(20)));
    WriteConfig(configs + "/two-image.json", second, root.path);

    MT_EXPECT_TRUE(store->RecordConfigRefsFromConfigPath(configs + "/one-image.json").ok());
    MT_EXPECT_TRUE(store->RecordConfigRefsFromConfigPath(configs + "/two-image.json").ok());

    // Total is 130. Under the high watermark, nothing is planned.
    auto idle = store->PlanCapacityEvictionFromStore(1000, 500, UnixNowSecs() + 60);
    MT_EXPECT_TRUE(idle.ok());
    MT_EXPECT_TRUE(idle.value().candidates.empty());
    MT_EXPECT_EQ(idle.value().total_bytes, static_cast<uint64_t>(130));

    // Forcing eviction down to 0 must select both configs, because the shared
    // commit's 100 bytes only count once the second referrer is also evicted.
    auto plan = store->PlanCapacityEvictionFromStore(100, 0, UnixNowSecs() + 60);
    MT_EXPECT_TRUE(plan.ok());
    MT_EXPECT_EQ(plan.value().candidates.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(plan.value().total_bytes, static_cast<uint64_t>(130));
}

MT_TEST(image_cache_meta_capacity_eviction_respects_evictable_before) {
    TempRoot root;
    auto store = TestStore(root.path);

    const std::string configs = root.path + "/configs";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(configs).ok());
    std::vector<std::pair<std::string, uint64_t> > lowers;
    lowers.push_back(std::make_pair(std::string("sha256:a"), static_cast<uint64_t>(100)));
    WriteConfig(configs + "/app-image.json", lowers, root.path);
    MT_EXPECT_TRUE(store->RecordConfigRefsFromConfigPath(configs + "/app-image.json").ok());

    // The config was just touched, so a cutoff in the past excludes it even
    // though the cache is over its watermark.
    auto plan = store->PlanCapacityEvictionFromStore(1, 0, UnixNowSecs() - 3600);
    MT_EXPECT_TRUE(plan.ok());
    MT_EXPECT_TRUE(plan.value().candidates.empty());
}

MT_TEST(image_cache_meta_config_last_used_is_touched_on_record) {
    TempRoot root;
    auto store = TestStore(root.path);

    const std::string configs = root.path + "/configs";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(configs).ok());
    std::vector<std::pair<std::string, uint64_t> > lowers;
    lowers.push_back(std::make_pair(std::string("sha256:a"), static_cast<uint64_t>(10)));
    WriteConfig(configs + "/app-image.json", lowers, root.path);

    const ImageCacheConfigId id = ConfigId("app-image.json");
    auto before = store->ConfigLastUsed(id);
    MT_EXPECT_TRUE(before.ok());
    MT_EXPECT_TRUE(!before.value().has_value());

    MT_EXPECT_TRUE(store->RecordConfigRefsFromConfigPath(configs + "/app-image.json").ok());

    auto after = store->ConfigLastUsed(id);
    MT_EXPECT_TRUE(after.ok());
    MT_EXPECT_TRUE(after.value().has_value());
}

MT_TEST(image_cache_meta_remove_config_refs_clears_edges_and_recency) {
    TempRoot root;
    auto store = TestStore(root.path);

    const std::string configs = root.path + "/configs";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(configs).ok());
    std::vector<std::pair<std::string, uint64_t> > lowers;
    lowers.push_back(std::make_pair(std::string("sha256:a"), static_cast<uint64_t>(10)));
    WriteConfig(configs + "/app-image.json", lowers, root.path);
    MT_EXPECT_TRUE(store->RecordConfigRefsFromConfigPath(configs + "/app-image.json").ok());

    const ImageCacheConfigId id = ConfigId("app-image.json");
    MT_EXPECT_TRUE(store->RemoveConfigRefs(id).ok());

    auto referrers = store->HardCommitConfigReferrers(Hard("sha256:a"));
    MT_EXPECT_TRUE(referrers.ok());
    MT_EXPECT_TRUE(referrers.value().empty());

    auto last_used = store->ConfigLastUsed(id);
    MT_EXPECT_TRUE(last_used.ok());
    MT_EXPECT_TRUE(!last_used.value().has_value());
}

MT_TEST(image_cache_meta_referrer_map_dedups_and_sorts) {
    TempRoot root;
    auto store = TestStore(root.path);

    const std::string configs = root.path + "/configs";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(configs).ok());

    std::vector<std::pair<std::string, uint64_t> > lowers;
    lowers.push_back(std::make_pair(std::string("sha256:shared"), static_cast<uint64_t>(5)));
    WriteConfig(configs + "/b-image.json", lowers, root.path);
    WriteConfig(configs + "/a-image.json", lowers, root.path);

    MT_EXPECT_TRUE(store->RecordConfigRefsFromConfigPath(configs + "/b-image.json").ok());
    MT_EXPECT_TRUE(store->RecordConfigRefsFromConfigPath(configs + "/a-image.json").ok());

    auto map = store->HardCommitConfigReferrerMap();
    MT_EXPECT_TRUE(map.ok());
    auto entry = map.value().find(Hard("sha256:shared"));
    MT_EXPECT_TRUE(entry != map.value().end());
    MT_EXPECT_EQ(entry->second.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(entry->second[0] == ConfigId("a-image.json"));
    MT_EXPECT_TRUE(entry->second[1] == ConfigId("b-image.json"));
}

MT_TEST(image_cache_meta_commit_store_refs_require_digest_and_size) {
    TempRoot root;
    const std::string commit_store = root.path + "/commits";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(commit_store).ok());

    // A commit-store file with no digest must be rejected rather than
    // silently skipped.
    storage::overlaybd::ImageConfig config;
    storage::overlaybd::LayerConfig layer;
    layer.file = commit_store + "/blob";
    layer.size = 10;
    config.lowers.push_back(layer);
    const std::string path = root.path + "/bad-image.json";
    MT_EXPECT_TRUE(
        core::fs::Write(path, storage::overlaybd::ImageConfigToJson(config)).ok());

    MT_EXPECT_TRUE(
        !ImageCacheMetadataStore::CommitStoreHardCommitDigestsFromConfigPath(
             path, commit_store).ok());
}

MT_TEST(image_cache_meta_commit_store_refs_skip_outside_files) {
    TempRoot root;
    const std::string commit_store = root.path + "/commits";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(commit_store).ok());

    storage::overlaybd::ImageConfig config;
    storage::overlaybd::LayerConfig inside;
    inside.file   = commit_store + "/blob";
    inside.digest = "sha256:inside";
    inside.size   = 10;
    config.lowers.push_back(inside);

    // Runtime-owned files outside the commit store are not cache-owned.
    storage::overlaybd::LayerConfig outside;
    outside.file   = root.path + "/elsewhere/blob";
    outside.digest = "sha256:outside";
    outside.size   = 20;
    config.lowers.push_back(outside);

    const std::string path = root.path + "/mixed-image.json";
    MT_EXPECT_TRUE(
        core::fs::Write(path, storage::overlaybd::ImageConfigToJson(config)).ok());

    auto digests = ImageCacheMetadataStore::CommitStoreHardCommitDigestsFromConfigPath(
        path, commit_store);
    MT_EXPECT_TRUE(digests.ok());
    MT_EXPECT_EQ(digests.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(digests.value()[0] == Hard("sha256:inside"));
}

MT_TEST(image_cache_meta_path_is_inside_rejects_sibling_prefix) {
    // "/a/bc" must not be considered inside "/a/b".
    MT_EXPECT_TRUE(PathIsInside("/a/b/c", "/a/b"));
    MT_EXPECT_TRUE(PathIsInside("/a/b/c", "/a/b/"));
    MT_EXPECT_TRUE(!PathIsInside("/a/bc", "/a/b"));
    MT_EXPECT_TRUE(!PathIsInside("/a/b", "/a/b"));
    MT_EXPECT_TRUE(!PathIsInside("/a/b/c", ""));
}

MT_TEST(image_cache_meta_remove_hard_commit_object) {
    TempRoot root;
    auto store = TestStore(root.path);

    const HardCommitId digest = Hard("sha256:a");
    MT_EXPECT_TRUE(store->RecordHardCommitObject(
        digest, core::Optional<std::string>(), core::Optional<uint64_t>(7),
        std::set<p2p::P2pArtifactKey>()).ok());
    MT_EXPECT_TRUE(store->RemoveHardCommitObject(digest).ok());

    auto record = store->GetHardCommitObject(digest);
    MT_EXPECT_TRUE(record.ok());
    MT_EXPECT_TRUE(!record.value().has_value());
}

int main() { return microtest::RunAll(); }
