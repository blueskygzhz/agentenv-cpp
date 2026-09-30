// SPDX-License-Identifier: MIT
// Rust: src/image/cache/service.rs — the `run_gc` / `check_collectable_*` tests.
#include "agentenv/image/cache_gc.h"

#include <set>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/config.h"
#include "microtest.h"

using namespace agentenv;                // NOLINT
using namespace agentenv::image::cache;  // NOLINT

namespace {

struct TempRoot {
    std::string path;
    TempRoot() {
        auto d = core::fs::CreateTempDir("agentenv-image-cache-gc-");
        path = d.ok() ? d.value() : std::string("/tmp/agentenv-image-cache-gc-fallback");
        core::fs::CreateDirAll(path);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
};

HardCommitId Hard(const std::string& digest) {
    auto h = HardCommitId::New(digest);
    MT_EXPECT_TRUE(h.ok());
    return h.value();
}

ImageCacheHoldOwner Owner(const std::string& ns, const std::string& key) {
    auto o = ImageCacheHoldOwner::New(ns, key);
    MT_EXPECT_TRUE(o.ok());
    return o.value();
}

/// A store plus its commit directory, wired into a GC engine.
struct Fixture {
    TempRoot                                 root;
    std::string                              commit_store;
    std::shared_ptr<ImageCacheMetadataStore> metadata;
    std::shared_ptr<p2p::P2pTransport>       transport;

    explicit Fixture(bool with_transport = true) {
        commit_store = root.path + "/commits";
        MT_EXPECT_TRUE(core::fs::CreateDirAll(commit_store).ok());
        auto s = ImageCacheMetadataStore::Open(root.path + "/meta.db",
                                               local_store::Durability::Memory);
        MT_EXPECT_TRUE(s.ok());
        metadata = s.value();
        if (with_transport) transport.reset(new p2p::DisabledP2pTransport());
    }

    ImageCacheGc Gc() { return ImageCacheGc(commit_store, metadata, transport); }

    /// Writes `body` into the commit store and records the matching object.
    HardCommitId SeedCommit(const std::string& digest, const std::string& body) {
        const std::string file = commit_store + "/" + digest;
        MT_EXPECT_TRUE(core::fs::Write(file, body).ok());
        const HardCommitId id = Hard(digest);
        MT_EXPECT_TRUE(metadata->RecordHardCommitObject(
            id, core::Optional<std::string>(file),
            core::Optional<uint64_t>(body.size()),
            std::set<p2p::P2pArtifactKey>()).ok());
        return id;
    }

    std::string CommitPath(const std::string& digest) const {
        return commit_store + "/" + digest;
    }
};

}  // namespace

MT_TEST(image_cache_gc_collects_free_commit) {
    Fixture fx;
    const HardCommitId digest = fx.SeedCommit("sha256:free", "0123456789");

    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(ImageCacheLiveRuntimeRefs());
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(1));
    MT_EXPECT_EQ(report.value().freed_bytes, static_cast<uint64_t>(10));
    MT_EXPECT_TRUE(report.value().blocked.empty());

    // The file, its record, and GC's own operation hold are all gone.
    MT_EXPECT_TRUE(!core::fs::Exists(fx.CommitPath("sha256:free")));
    auto record = fx.metadata->GetHardCommitObject(digest);
    MT_EXPECT_TRUE(record.ok());
    MT_EXPECT_TRUE(!record.value().has_value());
    auto holders = fx.metadata->HardCommitHoldReferrers(digest);
    MT_EXPECT_TRUE(holders.ok());
    MT_EXPECT_TRUE(holders.value().empty());
}

MT_TEST(image_cache_gc_skips_commit_rooted_by_config) {
    Fixture fx;
    const HardCommitId digest = fx.SeedCommit("sha256:rooted", "body");

    // Root it through a config edge.
    storage::overlaybd::ImageConfig config;
    storage::overlaybd::LayerConfig layer;
    layer.file   = fx.CommitPath("sha256:rooted");
    layer.digest = "sha256:rooted";
    layer.size   = 4;
    config.lowers.push_back(layer);
    const std::string config_path = fx.root.path + "/app-image.json";
    MT_EXPECT_TRUE(
        core::fs::Write(config_path, storage::overlaybd::ImageConfigToJson(config)).ok());
    MT_EXPECT_TRUE(fx.metadata->RecordConfigRefsFromConfigPath(config_path).ok());

    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(ImageCacheLiveRuntimeRefs());
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(0));
    MT_EXPECT_EQ(report.value().blocked.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(report.value().blocked[0].reason.kind ==
                   ImageCacheGcBlockedReason::RootedByConfig);
    MT_EXPECT_TRUE(core::fs::Exists(fx.CommitPath("sha256:rooted")));
    (void)digest;
}

MT_TEST(image_cache_gc_skips_held_commit) {
    Fixture fx;
    const HardCommitId digest = fx.SeedCommit("sha256:held", "body");

    std::set<HardCommitId> refs;
    refs.insert(digest);
    MT_EXPECT_TRUE(
        fx.metadata->CreateOrReplaceHold(Owner(kRuntimeHoldNamespace, "sb-1"), refs).ok());

    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(ImageCacheLiveRuntimeRefs());
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(0));
    MT_EXPECT_EQ(report.value().blocked.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(report.value().blocked[0].reason.kind == ImageCacheGcBlockedReason::Held);
    MT_EXPECT_TRUE(core::fs::Exists(fx.CommitPath("sha256:held")));
}

MT_TEST(image_cache_gc_skips_live_runtime_commit) {
    Fixture fx;
    const HardCommitId digest = fx.SeedCommit("sha256:live", "body");

    ImageCacheLiveRuntimeRefs live;
    live[digest].push_back(Owner(kRuntimeHoldNamespace, "sb-live"));

    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(live);
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(0));
    MT_EXPECT_EQ(report.value().blocked.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(report.value().blocked[0].reason.kind ==
                   ImageCacheGcBlockedReason::LiveRuntime);
    MT_EXPECT_TRUE(core::fs::Exists(fx.CommitPath("sha256:live")));
}

MT_TEST(image_cache_gc_empty_live_ref_entry_does_not_block) {
    Fixture fx;
    const HardCommitId digest = fx.SeedCommit("sha256:stale", "body");

    // Rust only blocks when the owner list is non-empty; a leftover empty
    // entry must not pin the commit forever.
    ImageCacheLiveRuntimeRefs live;
    live[digest] = std::vector<ImageCacheHoldOwner>();

    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(live);
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(1));
}

MT_TEST(image_cache_gc_blocks_commit_outside_store) {
    Fixture fx;
    // Recorded path lives outside the commit store: GC must refuse to touch it.
    const std::string outside = fx.root.path + "/elsewhere.blob";
    MT_EXPECT_TRUE(core::fs::Write(outside, "body").ok());
    MT_EXPECT_TRUE(fx.metadata->RecordHardCommitObject(
        Hard("sha256:outside"), core::Optional<std::string>(outside),
        core::Optional<uint64_t>(4), std::set<p2p::P2pArtifactKey>()).ok());

    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(ImageCacheLiveRuntimeRefs());
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(0));
    MT_EXPECT_EQ(report.value().blocked.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(report.value().blocked[0].reason.kind ==
                   ImageCacheGcBlockedReason::Unverifiable);
    MT_EXPECT_TRUE(core::fs::Exists(outside));
}

MT_TEST(image_cache_gc_blocks_on_size_mismatch) {
    Fixture fx;
    const std::string file = fx.CommitPath("sha256:resized");
    MT_EXPECT_TRUE(core::fs::Write(file, "four").ok());
    // Record a size that disagrees with the bytes on disk.
    MT_EXPECT_TRUE(fx.metadata->RecordHardCommitObject(
        Hard("sha256:resized"), core::Optional<std::string>(file),
        core::Optional<uint64_t>(999), std::set<p2p::P2pArtifactKey>()).ok());

    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(ImageCacheLiveRuntimeRefs());
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(0));
    MT_EXPECT_TRUE(report.value().blocked[0].reason.kind ==
                   ImageCacheGcBlockedReason::Unverifiable);
    MT_EXPECT_TRUE(core::fs::Exists(file));
}

MT_TEST(image_cache_gc_blocks_missing_file_and_fileless_record) {
    Fixture fx;
    // Recorded but absent from disk.
    MT_EXPECT_TRUE(fx.metadata->RecordHardCommitObject(
        Hard("sha256:gone"), core::Optional<std::string>(fx.CommitPath("sha256:gone")),
        core::Optional<uint64_t>(4), std::set<p2p::P2pArtifactKey>()).ok());
    // Digest-only record, with no file at all.
    MT_EXPECT_TRUE(fx.metadata->RecordHardCommitObject(
        Hard("sha256:fileless"), core::Optional<std::string>(),
        core::Optional<uint64_t>(), std::set<p2p::P2pArtifactKey>()).ok());

    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(ImageCacheLiveRuntimeRefs());
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(0));
    MT_EXPECT_EQ(report.value().blocked.size(), static_cast<std::size_t>(2));
    for (std::size_t i = 0; i < report.value().blocked.size(); ++i) {
        MT_EXPECT_TRUE(report.value().blocked[i].reason.kind ==
                       ImageCacheGcBlockedReason::Unverifiable);
    }
}

MT_TEST(image_cache_gc_blocks_directory_recorded_as_commit) {
    Fixture fx;
    const std::string dir = fx.CommitPath("sha256:dir");
    MT_EXPECT_TRUE(core::fs::CreateDirAll(dir).ok());
    MT_EXPECT_TRUE(fx.metadata->RecordHardCommitObject(
        Hard("sha256:dir"), core::Optional<std::string>(dir),
        core::Optional<uint64_t>(), std::set<p2p::P2pArtifactKey>()).ok());

    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(ImageCacheLiveRuntimeRefs());
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(0));
    MT_EXPECT_TRUE(report.value().blocked[0].reason.kind ==
                   ImageCacheGcBlockedReason::Unverifiable);
    MT_EXPECT_TRUE(core::fs::Exists(dir));
}

// Rust `run_gc_refuses_p2p_object_without_initialized_transport`.
MT_TEST(image_cache_gc_refuses_p2p_commit_without_transport) {
    Fixture fx(/*with_transport=*/false);
    const std::string file = fx.CommitPath("sha256:published");
    MT_EXPECT_TRUE(core::fs::Write(file, "body").ok());
    std::set<p2p::P2pArtifactKey> keys;
    keys.insert("overlaybd-layer/v1/sha256:published");
    MT_EXPECT_TRUE(fx.metadata->RecordHardCommitObject(
        Hard("sha256:published"), core::Optional<std::string>(file),
        core::Optional<uint64_t>(4), keys).ok());

    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(ImageCacheLiveRuntimeRefs());
    MT_EXPECT_TRUE(report.ok());
    // Deleting would leave peers advertising bytes that no longer exist, so
    // the commit is reported as DeleteFailed and the file survives.
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(0));
    MT_EXPECT_EQ(report.value().blocked.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(report.value().blocked[0].reason.kind ==
                   ImageCacheGcBlockedReason::DeleteFailed);
    MT_EXPECT_TRUE(core::fs::Exists(file));
}

MT_TEST(image_cache_gc_unpublishes_before_unlinking) {
    Fixture fx;
    const std::string file = fx.CommitPath("sha256:pub");
    MT_EXPECT_TRUE(core::fs::Write(file, "body").ok());
    std::set<p2p::P2pArtifactKey> keys;
    keys.insert("overlaybd-layer/v1/sha256:pub");
    MT_EXPECT_TRUE(fx.metadata->RecordHardCommitObject(
        Hard("sha256:pub"), core::Optional<std::string>(file),
        core::Optional<uint64_t>(4), keys).ok());

    // The disabled transport accepts unpublish as a no-op, so the delete
    // proceeds through the full unpublish-then-unlink path.
    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(ImageCacheLiveRuntimeRefs());
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(!core::fs::Exists(file));
}

MT_TEST(image_cache_gc_own_operation_hold_does_not_block) {
    Fixture fx;
    const HardCommitId digest = fx.SeedCommit("sha256:self", "body");

    // The second check must ignore GC's own hold; if it did not, nothing would
    // ever be collectable.
    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(ImageCacheLiveRuntimeRefs());
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(1));
    (void)digest;
}

MT_TEST(image_cache_gc_check_respects_ignore_owner) {
    Fixture fx;
    const HardCommitId digest = fx.SeedCommit("sha256:ignored", "body");

    const ImageCacheHoldOwner mine = Owner(kOperationHoldNamespace, "gc/sha256:ignored");
    std::set<HardCommitId> refs;
    refs.insert(digest);
    MT_EXPECT_TRUE(fx.metadata->CreateOrReplaceHold(mine, refs).ok());

    auto record = fx.metadata->GetHardCommitObject(digest);
    MT_EXPECT_TRUE(record.ok() && record.value().has_value());

    ImageCacheGc gc = fx.Gc();

    // Without the ignore, the hold blocks.
    auto blocked = gc.CheckCollectableHardCommit(
        *record.value(), std::vector<ImageCacheConfigId>(), ImageCacheLiveRuntimeRefs(),
        NULL);
    MT_EXPECT_TRUE(blocked.ok());
    MT_EXPECT_TRUE(blocked.value().kind == HardCommitGcDecision::Kind::Blocked);

    // With it, the commit is collectable.
    auto allowed = gc.CheckCollectableHardCommit(
        *record.value(), std::vector<ImageCacheConfigId>(), ImageCacheLiveRuntimeRefs(),
        &mine);
    MT_EXPECT_TRUE(allowed.ok());
    MT_EXPECT_TRUE(allowed.value().kind == HardCommitGcDecision::Kind::Collectable);
    MT_EXPECT_TRUE(*allowed.value().digest == digest);
}

MT_TEST(image_cache_gc_prunes_empty_digest_dir_but_not_store_root) {
    Fixture fx;
    // A commit nested one level down: its parent directory should be pruned,
    // while the store root itself must survive.
    const std::string nested_dir = fx.commit_store + "/ab";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(nested_dir).ok());
    const std::string file = nested_dir + "/blob";
    MT_EXPECT_TRUE(core::fs::Write(file, "body").ok());
    MT_EXPECT_TRUE(fx.metadata->RecordHardCommitObject(
        Hard("sha256:nested"), core::Optional<std::string>(file),
        core::Optional<uint64_t>(4), std::set<p2p::P2pArtifactKey>()).ok());

    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(ImageCacheLiveRuntimeRefs());
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(!core::fs::Exists(nested_dir));
    MT_EXPECT_TRUE(core::fs::Exists(fx.commit_store));
}

MT_TEST(image_cache_gc_empty_store_is_noop) {
    Fixture fx;
    ImageCacheGc gc = fx.Gc();
    auto report = gc.RunGc(ImageCacheLiveRuntimeRefs());
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(0));
    MT_EXPECT_TRUE(report.value().blocked.empty());
}

MT_TEST(image_cache_gc_summary_from_report) {
    ImageCacheGcReport report;
    report.collected   = 2;
    report.freed_bytes = 4096;
    ImageCacheGcBlockedReason reason;
    reason.kind = ImageCacheGcBlockedReason::Held;
    report.blocked.push_back(ImageCacheGcBlocked(Hard("sha256:a"), reason));
    report.blocked.push_back(ImageCacheGcBlocked(Hard("sha256:b"), reason));
    report.blocked.push_back(ImageCacheGcBlocked(Hard("sha256:c"), reason));

    const ImageCacheGcSummary summary = SummaryFromReport(report);
    MT_EXPECT_EQ(summary.collected, static_cast<std::size_t>(2));
    MT_EXPECT_EQ(summary.freed_bytes, static_cast<uint64_t>(4096));
    MT_EXPECT_EQ(summary.retained, static_cast<std::size_t>(3));
}

int main() { return microtest::RunAll(); }
