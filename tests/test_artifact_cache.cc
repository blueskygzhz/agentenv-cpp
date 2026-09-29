// SPDX-License-Identifier: MIT
// Rust: src/snapshot/artifact_cache.rs `mod tests`.
#include "agentenv/snapshot/artifact_cache.h"

#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "microtest.h"

using namespace agentenv;            // NOLINT
using namespace agentenv::snapshot;  // NOLINT

namespace {

struct TempRoot {
    std::string path;
    TempRoot() {
        auto d = core::fs::CreateTempDir("agentenv-artifact-cache-");
        path = d.ok() ? d.value() : std::string("/tmp/agentenv-cache-fallback");
        core::fs::CreateDirAll(path);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
};

std::shared_ptr<LocalArtifactCache> TestCache(const std::string& dir, uint64_t gb) {
    auto c = LocalArtifactCache::New(dir + "/cache", true, gb);
    return c.ok() ? c.value() : std::shared_ptr<LocalArtifactCache>();
}

/// A fetch callback that writes `body` and reports its length.
struct WriteBody {
    std::string body;
    int* call_count;
    explicit WriteBody(const std::string& b, int* n = NULL) : body(b), call_count(n) {}
    core::Expected<uint64_t, std::string> operator()(const std::string& dest) const {
        if (call_count) ++(*call_count);
        auto w = core::fs::Write(dest, body);
        if (!w.ok()) return core::make_unexpected(w.error());
        return static_cast<uint64_t>(body.size());
    }
};

}  // namespace

// ---- constants ----
MT_TEST(artifact_cache_constants_match_rust) {
    MT_EXPECT_TRUE(kArtifactCacheDefaultMaxSizeBytes == 10ULL * 1024 * 1024 * 1024);
    MT_EXPECT_TRUE(kArtifactCacheEvictionTargetRatio == 0.8);
}

MT_TEST(new_uses_default_when_no_size_given) {
    TempRoot root;
    auto cache = LocalArtifactCache::New(root.path + "/cache", false, 0);
    MT_EXPECT_TRUE(cache.ok());
    MT_EXPECT_TRUE(cache.value()->MaxSizeBytes() == kArtifactCacheDefaultMaxSizeBytes);
    // Rust `prepare_cache_root` creates the directory eagerly.
    MT_EXPECT_TRUE(core::fs::Exists(root.path + "/cache"));
}

MT_TEST(new_converts_gb_to_bytes) {
    TempRoot root;
    auto cache = LocalArtifactCache::New(root.path + "/cache", true, 3);
    MT_EXPECT_TRUE(cache.ok());
    MT_EXPECT_TRUE(cache.value()->MaxSizeBytes() == 3ULL * 1024 * 1024 * 1024);
}

// ---- Rust: ensure_cached_materializes_and_pins ----
MT_TEST(ensure_cached_materializes_and_pins) {
    TempRoot root;
    auto cache = TestCache(root.path, 1);
    MT_EXPECT_TRUE(cache != NULL);

    const std::string key = "artifacts/t1/vm_state.bin";
    auto handle = cache->EnsureCached(key, WriteBody("snap-data"));
    MT_EXPECT_TRUE(handle.ok());
    MT_EXPECT_TRUE(core::fs::Exists(handle.value()->path()));

    auto body = core::fs::ReadToString(handle.value()->path());
    MT_EXPECT_TRUE(body.ok());
    MT_EXPECT_TRUE(body.value() == "snap-data");
    MT_EXPECT_TRUE(cache->TotalSize() == 9);

    // Dropping the handle releases the pin but keeps the entry indexed.
    delete handle.value();
    MT_EXPECT_EQ(static_cast<int>(cache->EntryCount()), 1);
}

// ---- Rust: ensure_cached_reuses_existing_file_after_restart ----
MT_TEST(ensure_cached_reuses_existing_file_after_restart) {
    TempRoot root;
    const std::string cache_root = root.path + "/cache";
    const std::string existing = cache_root + "/artifacts/t1/vm_state.bin";
    core::fs::CreateDirAll(cache_root + "/artifacts/t1");
    core::fs::Write(existing, "warm-cache");

    auto cache = LocalArtifactCache::New(cache_root, true, 1);
    MT_EXPECT_TRUE(cache.ok());

    int fetch_calls = 0;
    auto handle = cache.value()->EnsureCached("artifacts/t1/vm_state.bin",
                                              WriteBody("ignored", &fetch_calls));
    MT_EXPECT_TRUE(handle.ok());

    auto body = core::fs::ReadToString(handle.value()->path());
    MT_EXPECT_TRUE(body.ok());
    MT_EXPECT_TRUE(body.value() == "warm-cache");
    // A warm file on disk must short-circuit the fetch entirely.
    MT_EXPECT_EQ(fetch_calls, 0);
    delete handle.value();
}

// ---- Rust: ensure_cached_rejects_path_traversal_keys ----
MT_TEST(ensure_cached_rejects_path_traversal_keys) {
    TempRoot root;
    auto cache = TestCache(root.path, 1);

    auto escaped = cache->EnsureCached("../escape", WriteBody("x"));
    MT_EXPECT_TRUE(!escaped.ok());
    MT_EXPECT_TRUE(escaped.error().find("path traversal") != std::string::npos);

    // A nested traversal is equally refused.
    auto nested = cache->EnsureCached("a/../../escape", WriteBody("x"));
    MT_EXPECT_TRUE(!nested.ok());
    MT_EXPECT_TRUE(nested.error().find("path traversal") != std::string::npos);

    // An empty key has no components to map.
    auto empty = cache->EnsureCached("", WriteBody("x"));
    MT_EXPECT_TRUE(!empty.ok());
    MT_EXPECT_TRUE(empty.error().find("key is empty") != std::string::npos);

    // "." alone normalizes away to nothing.
    auto dot = cache->EnsureCached(".", WriteBody("x"));
    MT_EXPECT_TRUE(!dot.ok());
}

MT_TEST(key_to_local_path_ignores_curdir_components) {
    TempRoot root;
    auto cache = TestCache(root.path, 1);
    // "./a/./b" and "a/b" must land on the same file, or the cache identity
    // contract (one key -> one path) would be violated.
    auto first = cache->EnsureCached("a/b", WriteBody("body"));
    MT_EXPECT_TRUE(first.ok());
    const std::string path_a = first.value()->path();
    delete first.value();

    auto second = cache->EnsureCached("./a/./b", WriteBody("body"));
    MT_EXPECT_TRUE(second.ok());
    MT_EXPECT_TRUE(second.value()->path() == path_a);
    delete second.value();
}

// ---- deduplication of a repeated key ----
MT_TEST(ensure_cached_fetches_once_per_key) {
    TempRoot root;
    auto cache = TestCache(root.path, 1);

    int calls = 0;
    auto first = cache->EnsureCached("dedup/key", WriteBody("payload", &calls));
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_EQ(calls, 1);

    // Second call hits the index, not the fetch closure.
    auto second = cache->EnsureCached("dedup/key", WriteBody("payload", &calls));
    MT_EXPECT_TRUE(second.ok());
    MT_EXPECT_EQ(calls, 1);

    delete first.value();
    delete second.value();
}

MT_TEST(ensure_cached_propagates_fetch_failure) {
    TempRoot root;
    auto cache = TestCache(root.path, 1);

    struct FailingFetch {
        core::Expected<uint64_t, std::string> operator()(const std::string&) const {
            return core::make_unexpected(std::string("disk on fire"));
        }
    };
    auto failed = cache->EnsureCached("bad/key", FailingFetch());
    MT_EXPECT_TRUE(!failed.ok());
    MT_EXPECT_TRUE(failed.error().find("disk on fire") != std::string::npos);
    // A failed fetch must leave no index entry behind.
    MT_EXPECT_EQ(static_cast<int>(cache->EntryCount()), 0);

    // The in-flight slot must be released so a retry can proceed.
    auto retry = cache->EnsureCached("bad/key", WriteBody("recovered"));
    MT_EXPECT_TRUE(retry.ok());
    delete retry.value();
}

// ---- Rust: pin_local_file_reuses_cached_entry_without_double_counting_size ----
MT_TEST(pin_local_file_reuses_cached_entry_without_double_counting_size) {
    TempRoot root;
    auto cache = TestCache(root.path, 1);

    const std::string local_path = root.path + "/runtime/image.json";
    core::fs::CreateDirAll(root.path + "/runtime");
    core::fs::Write(local_path, "runtime-config");
    auto expected_size = core::fs::FileSize(local_path);
    MT_EXPECT_TRUE(expected_size.ok());

    auto first = cache->PinLocalFile("runtime/snapshot/image.json", local_path);
    MT_EXPECT_TRUE(first.ok());
    auto second = cache->PinLocalFile("runtime/snapshot/image.json", local_path);
    MT_EXPECT_TRUE(second.ok());

    // Two pins on one key: the size must be counted once, not twice.
    MT_EXPECT_TRUE(cache->TotalSize() == expected_size.value());
    MT_EXPECT_EQ(static_cast<int>(cache->EntryCount()), 1);

    delete first.value();
    delete second.value();
}

MT_TEST(pin_local_file_rejects_a_missing_file) {
    TempRoot root;
    auto cache = TestCache(root.path, 1);
    auto pinned = cache->PinLocalFile("missing", root.path + "/nope");
    MT_EXPECT_TRUE(!pinned.ok());
}

// ---- eviction ----
MT_TEST(evict_lru_skips_pinned_entries) {
    TempRoot root;
    auto cache = LocalArtifactCache::New(root.path + "/cache", true, 1);
    MT_EXPECT_TRUE(cache.ok());

    auto handle_a = cache.value()->EnsureCached("file-a", WriteBody("aaa"));
    auto handle_b = cache.value()->EnsureCached("file-b", WriteBody("bbb"));
    MT_EXPECT_TRUE(handle_a.ok() && handle_b.ok());

    const std::string path_b = handle_b.value()->path();
    // Release only A, so B stays pinned.
    delete handle_a.value();

    cache.value()->EvictLru();

    // A pinned entry must survive eviction unconditionally.
    MT_EXPECT_TRUE(core::fs::Exists(path_b));
    delete handle_b.value();
}

MT_TEST(evict_lru_is_a_noop_below_the_target) {
    TempRoot root;
    auto cache = TestCache(root.path, 1);
    auto handle = cache->EnsureCached("small", WriteBody("tiny"));
    MT_EXPECT_TRUE(handle.ok());
    delete handle.value();

    // Well under the limit: nothing should be removed.
    MT_EXPECT_TRUE(!cache->IsOverLimit());
    cache->EvictLru();
    MT_EXPECT_EQ(static_cast<int>(cache->EntryCount()), 1);
}

MT_TEST(is_over_limit_tracks_total_size) {
    TempRoot root;
    // 1 GiB limit; a few bytes can never exceed it.
    auto cache = TestCache(root.path, 1);
    MT_EXPECT_TRUE(!cache->IsOverLimit());
    auto handle = cache->EnsureCached("k", WriteBody("12345"));
    MT_EXPECT_TRUE(handle.ok());
    MT_EXPECT_TRUE(cache->TotalSize() == 5);
    MT_EXPECT_TRUE(!cache->IsOverLimit());
    delete handle.value();
}

// ---- stale entry handling ----
MT_TEST(try_acquire_drops_an_entry_whose_file_vanished) {
    TempRoot root;
    auto cache = TestCache(root.path, 1);

    auto handle = cache->EnsureCached("vanishing", WriteBody("data"));
    MT_EXPECT_TRUE(handle.ok());
    const std::string path = handle.value()->path();
    delete handle.value();
    MT_EXPECT_EQ(static_cast<int>(cache->EntryCount()), 1);

    // Simulate an external deletion (manual cleanup, disk pressure, ...).
    core::fs::RemoveFile(path);

    // The next acquire must notice, drop the stale entry, and re-materialize
    // rather than hand back a handle to a non-existent file.
    int calls = 0;
    auto again = cache->EnsureCached("vanishing", WriteBody("regenerated", &calls));
    MT_EXPECT_TRUE(again.ok());
    MT_EXPECT_EQ(calls, 1);
    auto body = core::fs::ReadToString(again.value()->path());
    MT_EXPECT_TRUE(body.ok());
    MT_EXPECT_TRUE(body.value() == "regenerated");
    delete again.value();
}

// ---- CacheArtifactLease ----
MT_TEST(cache_artifact_lease_holds_handles) {
    TempRoot root;
    auto cache = TestCache(root.path, 1);

    auto handle = cache->EnsureCached("leased", WriteBody("leased-bytes"));
    MT_EXPECT_TRUE(handle.ok());

    {
        CacheArtifactLease lease;
        lease.AddHandle(handle.value());
        // The lease owns the handle now; the entry stays pinned while it lives.
        MT_EXPECT_EQ(static_cast<int>(cache->EntryCount()), 1);
    }
    // Lease destroyed -> handle destroyed -> pin released.
    MT_EXPECT_EQ(static_cast<int>(cache->EntryCount()), 1);
}

MT_MAIN
