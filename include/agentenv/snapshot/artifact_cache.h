// SPDX-License-Identifier: MIT
// Rust: src/snapshot/artifact_cache.rs
//
// A reference-counted, LRU-evicting node-local disk cache for runtime
// artifacts. Files are pinned for the lifetime of a CacheHandle; when all
// handles for a key are dropped the entry becomes eviction-eligible.
//
// Deviation from Rust: the Rust version uses Tokio async for `ensure_cached`
// and `pin_local_file`. C++11 has no async executor, so those calls are
// synchronous. Concurrent deduplication uses a per-key mutex+condvar instead
// of a Tokio `watch` channel.
#ifndef AGENTENV_SNAPSHOT_ARTIFACT_CACHE_H_
#define AGENTENV_SNAPSHOT_ARTIFACT_CACHE_H_

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/fs.h"

namespace agentenv {
namespace snapshot {

class LocalArtifactCache;

// ---- constants ---------------------------------------------------------------

/// Rust `DEFAULT_MAX_SIZE_BYTES` = 10 GiB.
const uint64_t kArtifactCacheDefaultMaxSizeBytes = 10ULL * 1024 * 1024 * 1024;
/// Rust `EVICTION_TARGET_RATIO` = 0.8.
const double kArtifactCacheEvictionTargetRatio = 0.8;

// ---- RuntimeArtifactLease ---------------------------------------------------

/// Rust trait `RuntimeArtifactLease`.
class RuntimeArtifactLease {
 public:
    virtual ~RuntimeArtifactLease() {}
};

// ---- CacheHandle ------------------------------------------------------------

/// Rust `struct CacheHandle`.
/// RAII: drops release the pin.
class CacheHandle {
 public:
    ~CacheHandle();
    const std::string& path() const { return local_path_; }
    const std::string& key()  const { return key_; }

 private:
    friend class LocalArtifactCache;
    CacheHandle(std::shared_ptr<LocalArtifactCache> cache,
                std::string key, std::string local_path);

    std::shared_ptr<LocalArtifactCache> cache_;
    std::string key_;
    std::string local_path_;
};

// ---- CacheArtifactLease -----------------------------------------------------

/// Rust `struct CacheArtifactLease`.
class CacheArtifactLease : public RuntimeArtifactLease {
 public:
    void AddHandle(CacheHandle* handle);
 private:
    std::vector<std::unique_ptr<CacheHandle> > handles_;
};

// ---- LocalArtifactCache -----------------------------------------------------

/// Rust `struct LocalArtifactCache`.
class LocalArtifactCache : public std::enable_shared_from_this<LocalArtifactCache> {
 public:
    static core::Expected<std::shared_ptr<LocalArtifactCache>, std::string>
        New(const std::string& cache_root, bool has_max_size_gb, uint64_t max_size_gb);

    /// Rust `ensure_cached`.
    template <typename Fetch>
    core::Expected<CacheHandle*, std::string>
    EnsureCached(const std::string& key, Fetch fetch) {
        core::Expected<std::string, std::string> path = KeyToLocalPath(key);
        if (!path.ok()) return core::make_unexpected(path.error());
        return EnsureCachedAt(key, path.value(), fetch);
    }

    /// Rust `ensure_cached_at`.
    template <typename Fetch>
    core::Expected<CacheHandle*, std::string>
    EnsureCachedAt(const std::string& key, const std::string& local_path, Fetch fetch) {
        while (true) {
            {
                CacheHandle* h = TryAcquire(key);
                if (h) return h;
            }

            if (core::fs::Exists(local_path)) {
                return PinLocalFile(key, local_path);
            }

            // Claim or wait on the in-flight slot.
            bool we_own = false;
            InflightEntry* my_entry = NULL;
            {
                std::unique_lock<std::mutex> lk(inflight_mu_);
                std::map<std::string, InflightEntry*>::iterator it = inflight_.find(key);
                if (it != inflight_.end()) {
                    InflightEntry* pending = it->second;
                    inflight_cv_.wait(lk, [pending]() { return pending->done; });
                    if (!pending->ok) return core::make_unexpected(pending->error);
                    continue;
                }
                // We are the producer.
                we_own = true;
                InflightEntry* e = new InflightEntry();
                inflight_[key] = e;
                my_entry = e;
            }
            (void)we_own;

            // Create parent directory.
            {
                core::Optional<std::string> parent = core::fs::Parent(local_path);
                if (parent.has_value()) {
                    core::Expected<core::Unit, std::string> mk =
                        core::fs::CreateDirAll(*parent);
                    if (!mk.ok()) {
                        std::string err =
                            std::string("create local cache dir '") + *parent +
                            "': " + mk.error();
                        FinishInflight(key, my_entry, false, err);
                        return core::make_unexpected(err);
                    }
                }
            }

            // Run the fetch callback.
            core::Expected<uint64_t, std::string> result = fetch(local_path);
            if (!result.ok()) {
                std::string err =
                    std::string("materialize '") + key +
                    "' into local cache: " + result.error();
                FinishInflight(key, my_entry, false, err);
                return core::make_unexpected(err);
            }

            // Register in index.
            InsertEntry(key, local_path, result.value());
            FinishInflight(key, my_entry, true, std::string());

            if (IsOverLimit()) EvictLru();

            // Acquire the just-inserted entry.
            CacheHandle* h = TryAcquire(key);
            if (h) return h;
            // Extremely unlikely: eviction removed it between insert and acquire.
            continue;
        }
    }

    core::Expected<CacheHandle*, std::string>
        PinLocalFile(const std::string& key, const std::string& local_path);

    // ---- test helpers ----
    uint64_t TotalSize() const;
    size_t   EntryCount() const;
    uint64_t MaxSizeBytes() const { return max_size_bytes_; }
    bool     IsOverLimit() const;
    core::Expected<core::Unit, std::string> EvictLru();

 private:
    friend class CacheHandle;

    struct CacheEntry {
        std::string local_path;
        uint64_t    size = 0;
        uint64_t    last_accessed_us = 0;
        size_t      ref_count = 0;
    };

    struct InflightEntry {
        std::string error;
        bool done = false;
        bool ok   = false;
    };

    LocalArtifactCache() : max_size_bytes_(kArtifactCacheDefaultMaxSizeBytes) {}

    core::Expected<std::string, std::string> KeyToLocalPath(const std::string& key) const;
    CacheHandle* TryAcquire(const std::string& key);
    void Release(const std::string& key);
    void InsertEntry(const std::string& key, const std::string& local_path, uint64_t size);
    void FinishInflight(const std::string& key, InflightEntry* entry,
                        bool ok, const std::string& err);
    uint64_t NowUs() const;

    std::string  cache_root_;
    uint64_t     max_size_bytes_;

    mutable std::mutex index_mu_;
    std::map<std::string, CacheEntry> entries_;
    uint64_t total_size_ = 0;

    std::mutex              inflight_mu_;
    std::condition_variable inflight_cv_;
    std::map<std::string, InflightEntry*> inflight_;
};

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_ARTIFACT_CACHE_H_
