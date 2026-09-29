// SPDX-License-Identifier: MIT
// Rust: src/snapshot/artifact_cache.rs
#include "agentenv/snapshot/artifact_cache.h"

#include <sys/stat.h>
#include <sys/time.h>

#include <cerrno>
#include <cstring>

#include "agentenv/core/fs.h"

namespace agentenv {
namespace snapshot {

// ---- CacheHandle ------------------------------------------------------------

CacheHandle::CacheHandle(std::shared_ptr<LocalArtifactCache> cache,
                         std::string key, std::string local_path)
    : cache_(cache), key_(key), local_path_(local_path) {}

CacheHandle::~CacheHandle() {
    if (cache_) cache_->Release(key_);
}

// ---- CacheArtifactLease -----------------------------------------------------

void CacheArtifactLease::AddHandle(CacheHandle* handle) {
    handles_.push_back(std::unique_ptr<CacheHandle>(handle));
}

// ---- LocalArtifactCache -----------------------------------------------------

uint64_t LocalArtifactCache::NowUs() const {
    struct timeval tv;
    ::gettimeofday(&tv, NULL);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + static_cast<uint64_t>(tv.tv_usec);
}

core::Expected<std::shared_ptr<LocalArtifactCache>, std::string>
LocalArtifactCache::New(const std::string& cache_root,
                        bool has_max_size_gb, uint64_t max_size_gb) {
    // Rust `prepare_cache_root`: create dir.
    core::Expected<core::Unit, std::string> mk = core::fs::CreateDirAll(cache_root);
    if (!mk.ok()) {
        return core::make_unexpected(
            std::string("create snapshot artifact cache root '") + cache_root + "': " +
            mk.error());
    }

    std::shared_ptr<LocalArtifactCache> cache(new LocalArtifactCache());
    cache->cache_root_ = cache_root;
    cache->max_size_bytes_ =
        has_max_size_gb ? (max_size_gb * 1024 * 1024 * 1024)
                        : kArtifactCacheDefaultMaxSizeBytes;
    return cache;
}

core::Expected<std::string, std::string>
LocalArtifactCache::KeyToLocalPath(const std::string& key) const {
    if (key.empty()) {
        return core::make_unexpected(
            std::string("invalid cache key '") + key + "': key is empty");
    }

    // Rust: iterate Path::components() and allow only Normal; reject
    // ParentDir/RootDir/Prefix. We split on '/' and check each component.
    std::string path = cache_root_;
    bool saw_component = false;
    size_t i = 0;
    while (i <= key.size()) {
        size_t slash = key.find('/', i);
        if (slash == std::string::npos) slash = key.size();
        const std::string component = key.substr(i, slash - i);
        if (!component.empty() && component != ".") {
            if (component == "..") {
                return core::make_unexpected(
                    std::string("invalid cache key '") + key +
                    "': path traversal is not allowed");
            }
            // Any other component with a leading '/' also implies RootDir.
            path += "/" + component;
            saw_component = true;
        }
        i = slash + 1;
    }
    if (!saw_component) {
        return core::make_unexpected(
            std::string("invalid cache key '") + key + "': key is empty");
    }
    return path;
}

CacheHandle* LocalArtifactCache::TryAcquire(const std::string& key) {
    std::lock_guard<std::mutex> g(index_mu_);
    std::map<std::string, CacheEntry>::iterator it = entries_.find(key);
    if (it == entries_.end()) return NULL;

    // Rust: `if !entry.local_path.exists()` drop the stale entry.
    if (!core::fs::Exists(it->second.local_path)) {
        total_size_ -= it->second.size;
        entries_.erase(it);
        return NULL;
    }

    it->second.ref_count += 1;
    it->second.last_accessed_us = NowUs();
    return new CacheHandle(shared_from_this(), key, it->second.local_path);
}

void LocalArtifactCache::Release(const std::string& key) {
    std::lock_guard<std::mutex> g(index_mu_);
    std::map<std::string, CacheEntry>::iterator it = entries_.find(key);
    if (it == entries_.end()) return;
    if (it->second.ref_count > 0) --it->second.ref_count;
}

void LocalArtifactCache::InsertEntry(const std::string& key,
                                      const std::string& local_path,
                                      uint64_t size) {
    std::lock_guard<std::mutex> g(index_mu_);
    std::map<std::string, CacheEntry>::iterator it = entries_.find(key);
    if (it != entries_.end()) {
        total_size_ -= it->second.size;
        entries_.erase(it);
    }
    CacheEntry entry;
    entry.local_path = local_path;
    entry.size = size;
    entry.last_accessed_us = NowUs();
    entry.ref_count = 0;  // caller bumps it separately
    entries_[key] = entry;
    total_size_ += size;
}

core::Expected<CacheHandle*, std::string>
LocalArtifactCache::PinLocalFile(const std::string& key,
                                  const std::string& local_path) {
    while (true) {
        CacheHandle* h = TryAcquire(key);
        if (h) return h;

        // Wait for any in-flight materialization on this key, then re-check.
        {
            std::unique_lock<std::mutex> lk(inflight_mu_);
            std::map<std::string, InflightEntry*>::iterator it = inflight_.find(key);
            if (it != inflight_.end()) {
                InflightEntry* pending = it->second;
                inflight_cv_.wait(lk, [pending]() { return pending->done; });
                if (!pending->ok) return core::make_unexpected(pending->error);
                continue;
            }
        }
        break;
    }

    core::Expected<uint64_t, std::string> size = core::fs::FileSize(local_path);
    if (!size.ok()) {
        return core::make_unexpected(
            std::string("stat local cache file '") + local_path + "': " + size.error());
    }

    std::lock_guard<std::mutex> g(index_mu_);
    std::map<std::string, CacheEntry>::iterator it = entries_.find(key);
    if (it != entries_.end()) {
        // Rust `insert` replaces and adjusts total; a second pin on the same
        // key must not double-count.
        total_size_ -= it->second.size;
        it->second.size = size.value();
        it->second.ref_count += 1;
        it->second.last_accessed_us = NowUs();
        total_size_ += size.value();
        return new CacheHandle(shared_from_this(), key, local_path);
    }
    CacheEntry entry;
    entry.local_path = local_path;
    entry.size = size.value();
    entry.last_accessed_us = NowUs();
    entry.ref_count = 1;
    entries_[key] = entry;
    total_size_ += entry.size;
    return new CacheHandle(shared_from_this(), key, local_path);
}

uint64_t LocalArtifactCache::TotalSize() const {
    std::lock_guard<std::mutex> g(index_mu_);
    return total_size_;
}

size_t LocalArtifactCache::EntryCount() const {
    std::lock_guard<std::mutex> g(index_mu_);
    return entries_.size();
}

bool LocalArtifactCache::IsOverLimit() const {
    std::lock_guard<std::mutex> g(index_mu_);
    return total_size_ > max_size_bytes_;
}

core::Expected<core::Unit, std::string> LocalArtifactCache::EvictLru() {
    const uint64_t target =
        static_cast<uint64_t>(static_cast<double>(max_size_bytes_) *
                               kArtifactCacheEvictionTargetRatio);

    // Collect eviction candidates (unpinned), sorted by last_accessed_us asc.
    struct Candidate {
        std::string key;
        uint64_t last_accessed_us;
        uint64_t size;
        std::string local_path;
    };
    std::vector<Candidate> candidates;
    {
        std::lock_guard<std::mutex> g(index_mu_);
        if (total_size_ <= target) return core::Unit();
        for (std::map<std::string, CacheEntry>::const_iterator it = entries_.begin();
             it != entries_.end(); ++it) {
            if (it->second.ref_count == 0) {
                Candidate c;
                c.key = it->first;
                c.last_accessed_us = it->second.last_accessed_us;
                c.size = it->second.size;
                c.local_path = it->second.local_path;
                candidates.push_back(c);
            }
        }
    }
    // Oldest first.
    for (size_t i = 0; i + 1 < candidates.size(); ++i) {
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (candidates[j].last_accessed_us < candidates[i].last_accessed_us) {
                Candidate tmp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = tmp;
            }
        }
    }

    for (size_t i = 0; i < candidates.size(); ++i) {
        {
            std::lock_guard<std::mutex> g(index_mu_);
            if (total_size_ <= target) break;
            std::map<std::string, CacheEntry>::iterator it =
                entries_.find(candidates[i].key);
            if (it == entries_.end() || it->second.ref_count > 0) continue;
            total_size_ -= it->second.size;
            entries_.erase(it);
        }
        core::fs::RemoveFile(candidates[i].local_path);  // best-effort
    }
    return core::Unit();
}

}  // namespace snapshot
}  // namespace agentenv
