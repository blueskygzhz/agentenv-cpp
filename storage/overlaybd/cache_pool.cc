// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/cache/full_file_cache/{cache_pool,
// cache_store}.rs
#include "agentenv/storage/overlaybd/cache_pool.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include "agentenv/core/digest.h"
#include "agentenv/storage/overlaybd/bk_download.h"
#include "agentenv/storage/overlaybd/sys.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

const char* const kPremergedIndexDir = "premerged-index";

// Rust seeds the background-download caps from `DownloadConfig::default()`:
// `CacheConfig` does not carry the download config, so a caller that holds
// the global one (image_service) overrides them after construction.
FileCacheBackendOptions::FileCacheBackendOptions()
    : cache_dir(kCacheDefaultDir),
      capacity_bytes(kCacheDefaultCapacityBytes),
      block_size(kCacheDefaultBlockSize),
      open_evict_idle_secs(600),
      bk_download_max_inflight_blocks(DownloadConfig().max_inflight_blocks),
      bk_download_max_concurrent_files(DownloadConfig().max_concurrent_files),
      bk_download_block_size(DownloadConfig().block_size),
      bk_download_hedge_timeout_ms(kBkDefaultHedgeTimeoutMs) {}

namespace {

uint64_t DivRoundUp(uint64_t n, uint64_t d) {
    if (n == 0 || d == 0) return 0;
    return (n - 1) / d + 1;
}

uint64_t AlignDown(uint64_t v, uint64_t a) {
    if (a == 0) return v;
    return (v / a) * a;
}

uint64_t AlignUp(uint64_t v, uint64_t a) {
    if (a == 0 || v == 0) return v;
    const uint64_t rounded = DivRoundUp(v, a);
    const uint64_t max = ~static_cast<uint64_t>(0);
    if (a != 0 && rounded > max / a) return max;
    return rounded * a;
}

bool IsPowerOfTwo(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

std::string Join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir[dir.size() - 1] == '/') return dir + name;
    return dir + "/" + name;
}

bool RemoveTree(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (dir != 0) {
        struct dirent* ent = 0;
        while ((ent = ::readdir(dir)) != 0) {
            const std::string name(ent->d_name);
            if (name == "." || name == "..") continue;
            const std::string child = Join(path, name);
            struct stat st;
            // lstat, not stat: a symlink must be unlinked rather than
            // followed into whatever it points at.
            if (::lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                RemoveTree(child);
            } else {
                ::unlink(child.c_str());
            }
        }
        ::closedir(dir);
    }
    return ::rmdir(path.c_str()) == 0;
}

bool MakeDirs(const std::string& path) {
    if (path.empty()) return true;
    std::string acc;
    size_t i = 0;
    if (path[0] == '/') {
        acc = "/";
        i = 1;
    }
    while (i < path.size()) {
        const size_t slash = path.find('/', i);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        acc += path.substr(i, end - i);
        if (!acc.empty() && ::mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) return false;
        acc += "/";
        i = end + 1;
    }
    return true;
}

/// Rust's `is_enospc`: an out-of-space refill is retried after an eviction
/// rather than failed, so an incoming image still gets cached under pressure.
bool IsEnospc(const std::string& message) {
    return message.find("ENOSPC") != std::string::npos ||
           message.find("no space") != std::string::npos ||
           message.find("cache is full") != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// FileCacheBackendOptions
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> FileCacheBackendOptions::Normalize() {
    if (cache_dir.empty()) {
        return core::make_unexpected(std::string("cache_dir cannot be empty"));
    }
    // Zero means "unset", so it takes the default instead of being refused.
    if (block_size == 0) block_size = kCacheDefaultBlockSize;

    // Block ids index both the bitmap and file offsets. A size that is not a
    // page-aligned power of two makes those two views disagree, which hands
    // back the wrong bytes rather than failing.
    if (block_size < kCachePageSize || (block_size % kCachePageSize) != 0 ||
        !IsPowerOfTwo(block_size)) {
        std::ostringstream o;
        o << "block_size must be >= " << kCachePageSize
          << ", page-aligned, and a power of two; got " << block_size;
        return core::make_unexpected(o.str());
    }
    return core::Unit();
}

core::Expected<FileCacheBackendOptions, std::string>
FileCacheBackendOptions::FromCacheConfig(const CacheConfig& cfg) {
    FileCacheBackendOptions opt;
    opt.cache_dir = cfg.cache_dir;
    const uint64_t max = ~static_cast<uint64_t>(0);
    opt.capacity_bytes = cfg.cache_size_gb > max / kCacheGib
                             ? max
                             : static_cast<uint64_t>(cfg.cache_size_gb) * kCacheGib;
    opt.block_size =
        cfg.refill_size > 0 ? static_cast<uint64_t>(cfg.refill_size) : kCacheDefaultBlockSize;
    opt.open_evict_idle_secs = cfg.open_evict_idle_secs;

    const core::Expected<core::Unit, std::string> ok = opt.Normalize();
    if (!ok.ok()) return core::make_unexpected(ok.error());
    return opt;
}

// ---------------------------------------------------------------------------
// FileCacheBackend — construction
// ---------------------------------------------------------------------------

core::Expected<std::shared_ptr<FileCacheBackend>, std::string>
FileCacheBackend::WithOptions(const FileCacheBackendOptions& options) {
    FileCacheBackendOptions normalized = options;
    const core::Expected<core::Unit, std::string> ok = normalized.Normalize();
    if (!ok.ok()) return core::make_unexpected(ok.error());

    if (!MakeDirs(normalized.cache_dir)) {
        return core::make_unexpected("create cache dir " + normalized.cache_dir + " failed");
    }

    std::shared_ptr<FileCacheBackend> backend(new FileCacheBackend());
    backend->options_ = normalized;

    CacheEntryOptions entry_options;
    entry_options.block_size = normalized.block_size;

    DIR* dir = ::opendir(normalized.cache_dir.c_str());
    if (dir != 0) {
        struct dirent* ent = 0;
        while ((ent = ::readdir(dir)) != 0) {
            const std::string cache_id(ent->d_name);
            if (cache_id == "." || cache_id == "..") continue;
            // The LSMT premerged-index artifacts share this root but are not
            // cache entries; leaving them alone is why the name is checked
            // rather than every directory being treated as an entry.
            if (cache_id == kPremergedIndexDir) continue;

            const std::string child = Join(normalized.cache_dir, cache_id);
            struct stat st;
            if (::stat(child.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

            const CacheEntryPaths paths =
                CacheEntryPaths::New(normalized.cache_dir, cache_id);
            const core::Expected<std::shared_ptr<CacheEntry>, std::string> entry =
                CacheEntry::LoadFromDisk(cache_id, paths, entry_options);
            if (entry.ok()) {
                backend->current_bytes_ += entry.value()->TotalCachedBytes();
                backend->entries_[cache_id] = entry.value();
            } else {
                // Deleted rather than skipped: the metadata is the only
                // record of which blocks are valid, so without it the data
                // file is unusable and would occupy the budget forever.
                RemoveTree(child);
            }
        }
        ::closedir(dir);
    }

    backend->PublishPressure();

    // Rust creates the scheduler inside the constructor and keeps it in
    // `_bk_scheduler_owner`. Created after the restart scan so a task
    // submitted immediately cannot race the entry table being populated.
    backend->bk_scheduler_ = BkDownloadScheduler::New(
        normalized.bk_download_max_inflight_blocks,
        normalized.bk_download_max_concurrent_files,
        normalized.bk_download_block_size,
        normalized.bk_download_hedge_timeout_ms);
    return backend;
}

core::Expected<std::shared_ptr<FileCacheBackend>, std::string>
FileCacheBackend::FromCacheConfig(const CacheConfig& cfg) {
    const core::Expected<FileCacheBackendOptions, std::string> opt =
        FileCacheBackendOptions::FromCacheConfig(cfg);
    if (!opt.ok()) return core::make_unexpected(opt.error());
    return WithOptions(opt.value());
}

core::Expected<std::shared_ptr<FileCacheBackend>, std::string>
FileCacheBackend::WithOptionsAndTransFunc(const FileCacheBackendOptions& options,
                                             const CacheFnTransFunc& trans_func) {
    const core::Expected<std::shared_ptr<FileCacheBackend>, std::string> backend =
        WithOptions(options);
    if (!backend.ok()) return backend;
    // Installed after construction but before the backend is handed out, so
    // the restart scan above has already keyed existing entries by their
    // on-disk directory names -- which is what the transform produces too.
    backend.value()->trans_func_ = trans_func;
    return backend;
}

FileCacheBackend::~FileCacheBackend() {
    // Before anything else, and unlike Rust, which only calls the
    // non-blocking `stop()` here.
    //
    // The difference is forced by how a task reaches the backend: Rust's
    // `BkDownloadTask` holds `backend: FileCacheBackend`, a clone that keeps
    // the shared cache state alive on its own, so a worker outliving this
    // value is harmless. The C++ `BkDownloadTask` holds a raw
    // `FileCacheBackend*`, so a worker still inside a task after this
    // destructor returns would dereference freed memory. `Shutdown` waits
    // for the in-flight count to reach zero and clears the registry, which
    // is what makes that impossible.
    ShutdownBkDownloads();

    // Entries whose bitmap moved are persisted on the way out: the data is
    // already on disk, and losing the bitmap would discard it.
    CheckpointDirtyEntries();
}

void FileCacheBackend::ShutdownBkDownloads() {
    if (bk_scheduler_) bk_scheduler_->Shutdown();
}

bool FileCacheBackend::BkDownloadRegistered(const std::string& cache_id) const {
    return bk_scheduler_ && bk_scheduler_->IsRegistered(cache_id);
}

std::shared_ptr<CacheEntry> FileCacheBackend::GetCacheEntry(
    const std::string& cache_id) const {
    const std::map<std::string, std::shared_ptr<CacheEntry> >::const_iterator found =
        entries_.find(cache_id);
    if (found == entries_.end()) return std::shared_ptr<CacheEntry>();
    return found->second;
}

bool FileCacheBackend::CacheKeyOf(const std::string& cache_id, std::string* out) const {
    const std::shared_ptr<CacheEntry> entry = GetCacheEntry(cache_id);
    if (!entry) return false;
    if (out != 0) *out = entry->Key();
    return true;
}

std::string FileCacheBackend::TransformStoreKey(const std::string& src_name) const {
    if (trans_func_) {
        std::string key = trans_func_(src_name);
        // Leading separators stripped: the key is joined onto the cache dir,
        // and an absolute one would place the entry outside it -- where the
        // restart scan and eviction accounting, which both walk the cache
        // dir, would never see it again. Rust joins the transform's result
        // directly and its own transform returns `/<basename>`, so there the
        // entry lands at the filesystem root.
        std::string::size_type begin = 0;
        while (begin < key.size() && key[begin] == '/') ++begin;
        if (begin > 0) key = key.substr(begin);
        if (!key.empty() && key.find('/') == std::string::npos && key != "." &&
            key != ".." && key != kPremergedIndexDir) {
            return key;
        }
        // Anything else falls through to hashing: a multi-component key would
        // nest entries, `.`/`..` would resolve to the cache dir or its
        // parent, and the premerged-index name is already taken by the index
        // cache that lives beside the entries.
    }

    // A source name is a URL; the cache id has to be one safe path
    // component, so it is hashed rather than escaped.
    core::Sha256Stream h;
    h.Update(src_name.data(), src_name.size());
    uint8_t digest[32];
    h.PeekFinal(digest);

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 32; ++i) {
        out.push_back(kHex[(digest[i] >> 4) & 0xF]);
        out.push_back(kHex[digest[i] & 0xF]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Capacity accounting and pressure
// ---------------------------------------------------------------------------

uint64_t FileCacheBackend::CalcWaterMark() const {
    const uint64_t capacity = options_.capacity_bytes;
    const uint64_t ratio_mark = capacity / 100 * kCacheWatermarkRatio;
    // On a very large cache the ratio would leave tens of gigabytes unused,
    // so the free-space form takes over: whichever mark is *higher* wins.
    const uint64_t free_space_mark =
        capacity > kCacheMaxFreeSpaceBytes ? capacity - kCacheMaxFreeSpaceBytes : 0;
    return ratio_mark > free_space_mark ? ratio_mark : free_space_mark;
}

uint64_t FileCacheBackend::RiskMark() const {
    const uint64_t capacity = options_.capacity_bytes;
    const uint64_t water_mark = CalcWaterMark();
    const uint64_t by_margin =
        capacity > kCacheEvictionMarkBytes ? capacity - kCacheEvictionMarkBytes : 0;
    const uint64_t midpoint = water_mark / 2 + capacity / 2;
    return by_margin > midpoint ? by_margin : midpoint;
}

void FileCacheBackend::CaptureDiskPressure(uint64_t* evict_bytes, bool* suppress) const {
    if (evict_bytes) *evict_bytes = 0;
    if (suppress) *suppress = false;

    const core::Expected<sys::FsSpace, sys::SysError> space =
        sys::FsSpaceOf(options_.cache_dir);
    // No answer means no disk-derived pressure, rather than assuming the
    // worst: a failed statvfs should not trigger a cache-wide eviction.
    if (!space.ok()) return;

    if (space.value().avail_bytes < kCacheDefaultDiskAvailBytes) {
        // The disk is nearly full regardless of the cache's own budget, so
        // enough is evicted to restore the reserve.
        if (evict_bytes) {
            *evict_bytes = kCacheDefaultDiskAvailBytes - space.value().avail_bytes;
        }
        return;
    }

    // The configured capacity exceeds the filesystem itself, so the cache's
    // watermark can never be reached and enforcing it would evict forever.
    if (suppress) *suppress = space.value().capacity_bytes <= CalcWaterMark();
}

uint64_t FileCacheBackend::PressureEvictTarget(uint64_t current_bytes,
                                                  uint64_t disk_evict_bytes,
                                                  bool suppress) const {
    const uint64_t water_mark = CalcWaterMark();
    uint64_t evict_by_cache = 0;
    if (!suppress && current_bytes >= water_mark) {
        evict_by_cache = current_bytes - water_mark;
    }
    // Whichever demand is larger: the disk reserve and the cache budget are
    // independent reasons to reclaim.
    return evict_by_cache > disk_evict_bytes ? evict_by_cache : disk_evict_bytes;
}

void FileCacheBackend::PublishPressure() {
    uint64_t disk_evict = 0;
    bool suppress = false;
    CaptureDiskPressure(&disk_evict, &suppress);
    // Full either because something must be evicted now, or because usage has
    // passed the risk mark -- the second keeps a burst of refills from
    // overshooting capacity before eviction catches up.
    is_full_ = PressureEvictTarget(current_bytes_, disk_evict, suppress) > 0 ||
               current_bytes_ >= RiskMark();
}

void FileCacheBackend::AddCurrentBytes(uint64_t bytes) {
    current_bytes_ += bytes;
    PublishPressure();
}

void FileCacheBackend::SubtractCurrentBytes(uint64_t bytes) {
    current_bytes_ = current_bytes_ > bytes ? current_bytes_ - bytes : 0;
    PublishPressure();
}

// ---------------------------------------------------------------------------
// Eviction
// ---------------------------------------------------------------------------

bool FileCacheBackend::EntryIsBusy(const CacheEntry& entry) {
    // An open handle or an elected loader means someone is mid-operation;
    // removing the entry under them would invalidate a mapping still in use.
    return entry.OpenCount() > 0;
}

std::vector<std::string> FileCacheBackend::EvictableCacheIdsByLru() const {
    std::vector<std::pair<uint64_t, std::string> > candidates;
    for (std::map<std::string, std::shared_ptr<CacheEntry> >::const_iterator it =
             entries_.begin();
         it != entries_.end(); ++it) {
        const CacheEntry& entry = *it->second;
        // Nothing cached means nothing to reclaim, so such entries are not
        // candidates even though they are idle.
        if (entry.TotalCachedBytes() > 0 && !EntryIsBusy(entry)) {
            candidates.push_back(std::make_pair(entry.LastAccess(), it->first));
        }
    }
    std::sort(candidates.begin(), candidates.end());

    std::vector<std::string> out;
    out.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) out.push_back(candidates[i].second);
    return out;
}

std::vector<std::string> FileCacheBackend::IdleOpenCacheIdsByLru(
    uint64_t idle_cutoff) const {
    std::vector<std::pair<uint64_t, std::string> > candidates;
    for (std::map<std::string, std::shared_ptr<CacheEntry> >::const_iterator it =
             entries_.begin();
         it != entries_.end(); ++it) {
        const CacheEntry& entry = *it->second;
        // This tier is specifically the *open* entries; closed ones were
        // already handled by the first tier.
        if (entry.OpenCount() == 0) continue;
        if (entry.LastAccess() >= idle_cutoff) continue;
        if (entry.TotalCachedBytes() == 0) continue;
        candidates.push_back(std::make_pair(entry.LastAccess(), it->first));
    }
    std::sort(candidates.begin(), candidates.end());

    std::vector<std::string> out;
    out.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) out.push_back(candidates[i].second);
    return out;
}

uint64_t FileCacheBackend::EvictOpenEntryBlocks(const std::string& cache_id,
                                                   bool user_initiated) {
    const std::shared_ptr<CacheEntry> entry = GetCacheEntry(cache_id);
    if (!entry) return 0;
    // Re-validated after the lookup: the entry may have been closed since the
    // candidate list was built, which would make it a first-tier case.
    if (entry->OpenCount() == 0) return 0;

    const core::Expected<uint64_t, std::string> released = entry->EvictAllBlocks();
    if (!released.ok()) return 0;

    if (released.value() > 0) {
        SubtractCurrentBytes(released.value());
        if (user_initiated) {
            evict_user_ += released.value();
        } else {
            evict_global_ += released.value();
        }
    }
    return released.value();
}

uint64_t FileCacheBackend::EvictEntry(const std::string& cache_id, bool user_initiated) {
    const std::shared_ptr<CacheEntry> entry = GetCacheEntry(cache_id);
    if (!entry) return 0;
    if (EntryIsBusy(*entry)) return 0;

    const core::Expected<uint64_t, std::string> released = entry->EvictAllBlocks();
    if (!released.ok()) {
        // The entry stays in the map: a failed eviction must not lose track
        // of bytes that are still on disk.
        return 0;
    }

    // Removed from the map before the directory, so no later lookup can hand
    // out an entry whose files are being deleted.
    entries_.erase(cache_id);
    RemoveTree(entry->Paths().dir);

    if (released.value() > 0) {
        SubtractCurrentBytes(released.value());
        if (user_initiated) {
            evict_user_ += released.value();
        } else {
            evict_global_ += released.value();
        }
    }
    return released.value();
}

void FileCacheBackend::EvictionInner() {
    uint64_t disk_evict = 0;
    bool suppress = false;
    CaptureDiskPressure(&disk_evict, &suppress);
    uint64_t actual_evict = PressureEvictTarget(current_bytes_, disk_evict, suppress);

    if (actual_evict > 0) {
        is_full_ = true;

        // Tier 1: closed entries, oldest first, removed outright.
        const std::vector<std::string> closed = EvictableCacheIdsByLru();
        for (size_t i = 0; i < closed.size() && actual_evict > 0; ++i) {
            const uint64_t bytes = EvictEntry(closed[i], false);
            actual_evict = actual_evict > bytes ? actual_evict - bytes : 0;
        }

        // Tier 2: entries held open but idle give up their blocks. The entry
        // survives, so open handles keep working and refill on their next
        // read -- which is why this tier exists rather than forcing those
        // handles to fail.
        if (actual_evict > 0 && options_.open_evict_idle_secs > 0) {
            const uint64_t now = NowUnixNanos();
            const uint64_t idle_nanos = options_.open_evict_idle_secs > (~0ULL) / 1000000000ULL
                                            ? ~0ULL
                                            : options_.open_evict_idle_secs * 1000000000ULL;
            const uint64_t idle_cutoff = now > idle_nanos ? now - idle_nanos : 0;
            const std::vector<std::string> idle = IdleOpenCacheIdsByLru(idle_cutoff);
            for (size_t i = 0; i < idle.size() && actual_evict > 0; ++i) {
                const uint64_t bytes = EvictOpenEntryBlocks(idle[i], false);
                actual_evict = actual_evict > bytes ? actual_evict - bytes : 0;
            }
        }
    }

    // Recomputed from the post-eviction state rather than publishing the
    // stale initial verdict: bytes may have moved while evicting, including
    // when the initial target was zero.
    PublishPressure();
}

core::Expected<uint64_t, std::string> FileCacheBackend::EvictBySize(uint64_t size) {
    uint64_t freed = 0;
    const std::vector<std::string> closed = EvictableCacheIdsByLru();
    for (size_t i = 0; i < closed.size() && freed < size; ++i) {
        freed += EvictEntry(closed[i], true);
    }
    return freed;
}

core::Expected<core::Unit, std::string> FileCacheBackend::EvictStoreKey(
    const std::string& store_key) {
    EvictEntry(store_key, true);
    return core::Unit();
}

core::Expected<core::Unit, std::string> FileCacheBackend::EvictSrcName(
    const std::string& src_name) {
    return EvictStoreKey(TransformStoreKey(src_name));
}

core::Expected<core::Unit, std::string> FileCacheBackend::EvictGlobal() {
    // A copy of the keys: evicting mutates the map.
    std::vector<std::string> ids;
    ids.reserve(entries_.size());
    for (std::map<std::string, std::shared_ptr<CacheEntry> >::const_iterator it =
             entries_.begin();
         it != entries_.end(); ++it) {
        ids.push_back(it->first);
    }
    for (size_t i = 0; i < ids.size(); ++i) EvictEntry(ids[i], false);
    return core::Unit();
}

bool FileCacheBackend::ContainsSrcName(const std::string& src_name) const {
    return GetCacheEntry(TransformStoreKey(src_name)) != 0;
}

bool FileCacheBackend::CachedSizeBySrcName(const std::string& src_name,
                                              uint64_t* out) const {
    const std::shared_ptr<CacheEntry> entry = GetCacheEntry(TransformStoreKey(src_name));
    if (!entry) return false;
    if (out) *out = entry->SourceSize();
    return true;
}

core::Expected<core::Unit, std::string> FileCacheBackend::RenameSrcName(
    const std::string& old_src, const std::string& new_src) {
    const std::string old_id = TransformStoreKey(old_src);
    const std::string new_id = TransformStoreKey(new_src);
    // Not an error: the caller asked for a state that already holds.
    if (old_id == new_id) return core::Unit();

    // Whatever occupies the destination is dropped first. Rust calls
    // `force_recycle` here; the entry about to move in owns that directory,
    // so a survivor would be silently overwritten by the rename below.
    EvictEntry(new_id, true);

    const std::shared_ptr<CacheEntry> old_entry = GetCacheEntry(old_id);
    if (!old_entry) {
        return core::make_unexpected("cache entry not found for key " + old_src);
    }
    // Refused rather than deferred. Live handles hold a mapping over the data
    // file at the old path; moving it out from under them would leave them
    // writing into a directory that no longer names the entry.
    if (old_entry->OpenCount() > 0) {
        return core::make_unexpected(std::string("cache entry is busy (open_count > 0)"));
    }

    const uint64_t source_size = old_entry->SourceSize();
    const CacheEntryPaths new_paths = CacheEntryPaths::New(options_.cache_dir, new_id);
    const std::string old_dir = old_entry->Paths().dir;

    struct stat st;
    if (::stat(old_dir.c_str(), &st) == 0) {
        // Cleared first: `rename` onto a non-empty directory fails, and the
        // eviction above only removed the entry from the map.
        RemoveTree(new_paths.dir);
        const std::string::size_type slash = new_paths.dir.rfind('/');
        if (slash != std::string::npos && slash > 0) {
            MakeDirs(new_paths.dir.substr(0, slash));
        }
        if (::rename(old_dir.c_str(), new_paths.dir.c_str()) != 0) {
            std::ostringstream o;
            o << "rename cache dir " << old_dir << " -> " << new_paths.dir << " failed: "
              << ::strerror(errno);
            return core::make_unexpected(o.str());
        }
    } else if (!MakeDirs(new_paths.dir)) {
        // The old entry had no directory on disk -- it held nothing cached --
        // so the destination is simply created empty.
        return core::make_unexpected("create cache dir " + new_paths.dir + " failed");
    }

    // The accounting is undone before the old entry goes: its bytes are about
    // to be re-counted under the new entry, and leaving both would
    // double-count them against the budget.
    current_bytes_ -= old_entry->TotalCachedBytes();
    entries_.erase(old_id);

    CacheEntryOptions entry_options;
    entry_options.block_size = options_.block_size;
    const core::Expected<std::shared_ptr<CacheEntry>, std::string> created =
        CacheEntry::Create(new_id, new_src, source_size, entry_options, new_paths);
    if (!created.ok()) return core::make_unexpected(created.error());

    created.value()->AdoptFrom(*old_entry);
    current_bytes_ += created.value()->TotalCachedBytes();

    // Persisted now rather than left to the periodic checkpoint: the bitmap
    // only exists under the new id in memory, and a crash before the next
    // checkpoint would lose every cached block the rename just preserved.
    const core::Expected<core::Unit, std::string> checkpointed =
        created.value()->Checkpoint();
    if (!checkpointed.ok()) return core::make_unexpected(checkpointed.error());

    entries_[new_id] = created.value();
    return core::Unit();
}

void FileCacheBackend::CheckpointDirtyEntries() {
    for (std::map<std::string, std::shared_ptr<CacheEntry> >::iterator it = entries_.begin();
         it != entries_.end(); ++it) {
        if (!it->second->IsDirty()) continue;
        // Best effort: a failed checkpoint only costs cache hits after a
        // restart, so it must not stop the remaining entries.
        it->second->Checkpoint();
    }
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

CacheStats FileCacheBackend::Stats() const {
    CacheStats out;
    out.entries = entries_.size();
    out.bytes_used = current_bytes_;
    for (std::map<std::string, std::shared_ptr<CacheEntry> >::const_iterator it =
             entries_.begin();
         it != entries_.end(); ++it) {
        uint64_t h = 0, m = 0, r = 0;
        it->second->StatsSnapshot(&h, &m, &r);
        out.hits += h;
        out.misses += m;
        out.refills += r;
    }
    return out;
}

bool FileCacheBackend::FileStats(const std::string& key, CachedFileStats* out) const {
    const std::shared_ptr<CacheEntry> entry = GetCacheEntry(key);
    if (!entry || out == 0) return false;
    out->key = entry->Key();
    out->bytes_used = entry->TotalCachedBytes();
    entry->StatsSnapshot(&out->hits, &out->misses, &out->refills);
    return true;
}

bool FileCacheBackend::FileStatsBySrcName(const std::string& src_name,
                                             CachedFileStats* out) const {
    return FileStats(TransformStoreKey(src_name), out);
}

core::Expected<CachePoolStat, std::string> FileCacheBackend::StatPath() const {
    CachePoolStat out;
    const uint64_t unit = options_.block_size;
    out.refill_unit = static_cast<uint32_t>(unit);
    // Reported in refill units, not bytes: that is the shape of the upstream
    // interface, and a byte count would not fit the u32 fields anyway.
    out.total_size = static_cast<uint32_t>(unit == 0 ? 0 : options_.capacity_bytes / unit);
    out.used_size = static_cast<uint32_t>(unit == 0 ? 0 : current_bytes_ / unit);
    out.evict_global = evict_global_;
    out.evict_user = evict_user_;
    out.evict_other = 0;
    return out;
}

std::vector<std::string> FileCacheBackend::List(CacheListType type) const {
    std::vector<std::string> out;
    // Every entry is a file in this cache; there are no directories to
    // report, so the `Dirs` filter yields nothing rather than erroring.
    if (type == kCacheListDirs) return out;
    out.reserve(entries_.size());
    for (std::map<std::string, std::shared_ptr<CacheEntry> >::const_iterator it =
             entries_.begin();
         it != entries_.end(); ++it) {
        out.push_back(it->first);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Refill range planning
// ---------------------------------------------------------------------------

bool FileCacheBackend::QueryRefillRange(const CacheEntry& entry, uint64_t offset,
                                           uint64_t size, uint64_t source_size,
                                           uint64_t* out_off, uint64_t* out_len) const {
    if (size == 0 || offset >= source_size) return false;

    uint64_t req_end = offset + size;
    if (req_end < offset) return false;  // overflow
    if (req_end > source_size) req_end = source_size;
    if (req_end <= offset) return false;

    const uint64_t block_size = options_.block_size;
    // Widened to block boundaries: a refill writes whole blocks, because the
    // bitmap records presence per block and a partial one could not be
    // published.
    const uint64_t align_left = AlignDown(offset, block_size);
    uint64_t align_right = AlignUp(req_end, block_size);
    if (align_right > source_size) align_right = source_size;
    if (align_right <= align_left) return false;

    const uint64_t start_block = align_left / block_size;
    const uint64_t end_block = DivRoundUp(align_right, block_size) - 1;
    if (start_block > 0xFFFFFFFFULL || end_block > 0xFFFFFFFFULL) return false;

    // The already-cached spans inside the widened window.
    std::vector<std::pair<uint64_t, uint64_t> > extents;
    const std::vector<uint32_t> present = entry.PresentBlocksInRange(
        static_cast<uint32_t>(start_block), static_cast<uint32_t>(end_block));
    for (size_t i = 0; i < present.size(); ++i) {
        const uint64_t block_id = static_cast<uint64_t>(present[i]);
        const uint64_t extent_start = block_id * block_size;
        uint64_t extent_end = extent_start + block_size;
        if (extent_end > source_size) extent_end = source_size;
        if (extent_end <= align_left || extent_start >= align_right) continue;
        extents.push_back(std::make_pair(extent_start > align_left ? extent_start : align_left,
                                         extent_end < align_right ? extent_end : align_right));
    }

    uint64_t hole_start = align_left;
    uint64_t hole_end = align_right;

    // One contiguous hole is returned, not a list: the caller refills a
    // single range, and interior cached blocks are cheap to rewrite compared
    // with issuing a separate source read per gap. Only the fully cached
    // *edges* are trimmed.
    for (size_t i = extents.size(); i-- > 0;) {
        if (extents[i].first < hole_end) {
            if (extents[i].second >= hole_end) {
                hole_end = extents[i].first;
            } else {
                break;
            }
        }
    }
    for (size_t i = 0; i < extents.size(); ++i) {
        if (extents[i].second > hole_start) {
            if (extents[i].first <= hole_start) {
                hole_start = extents[i].second;
            } else {
                break;
            }
        }
    }

    if (hole_start >= hole_end) return false;
    if (out_off) *out_off = hole_start;
    if (out_len) *out_len = hole_end - hole_start;
    return true;
}

// ---------------------------------------------------------------------------
// Refill
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> FileCacheBackend::DoRefillBlock(
    CacheEntry* entry, const std::string& cache_id, VirtualFile* source,
    uint64_t source_size, uint64_t block_id) {
    const uint64_t block_size = options_.block_size;
    uint64_t offset = block_id * block_size;
    if (offset > source_size) offset = source_size;
    const uint64_t remain = source_size - offset;
    const size_t want = static_cast<size_t>(remain < block_size ? remain : block_size);
    if (want == 0) return core::Unit();

    // Refused before any work when there is no room: the alternative is
    // writing a block and immediately evicting it.
    if (options_.capacity_bytes == 0) {
        return core::make_unexpected(std::string("cache is full (ENOSPC)"));
    }
    if (is_full_) {
        return core::make_unexpected(std::string("cache is full (ENOSPC)"));
    }

    // Reserved up front with a rollback guard: the source read below can
    // fail, and without the guard a failed refill would leave allocated disk
    // blocks the accounting never saw plus partially written pages.
    const core::Expected<std::shared_ptr<ReservedRange>, std::string> reservation =
        entry->ReserveRange(offset, static_cast<uint64_t>(want));
    if (!reservation.ok()) return core::make_unexpected(reservation.error());

    uint8_t* buf = 0;
    size_t buf_len = 0;
    bool available = false;
    const core::Expected<core::Unit, std::string> mapped =
        entry->MmapMutForBlock(block_id, source_size, &buf, &buf_len, &available);
    if (!mapped.ok()) return core::make_unexpected(mapped.error());
    if (!available) {
        return core::make_unexpected(std::string("block offset beyond source size"));
    }

    // Read straight into the mapping: no intermediate buffer, and the bytes
    // land in the page cache that backs the file.
    size_t done = 0;
    while (done < buf_len) {
        const core::Expected<size_t, std::string> n =
            source->ReadAtInto(offset + done, buf + done, buf_len - done);
        if (!n.ok()) return core::make_unexpected(n.error());
        if (n.value() == 0) break;
        done += n.value();
    }
    if (done != buf_len) {
        std::ostringstream o;
        o << "short refill read for cache_id=" << cache_id << ", offset=" << offset
          << ", got " << done << ", want " << buf_len;
        return core::make_unexpected(o.str());
    }

    // Committed before publishing, so the rollback can never run against a
    // range whose blocks are already visible.
    reservation.value()->Commit();

    entry->MarkBlockCached(block_id);
    entry->RecordRefill();
    AddCurrentBytes(static_cast<uint64_t>(want));
    return core::Unit();
}

core::Expected<core::Unit, std::string> FileCacheBackend::RefillExactRange(
    const std::string& cache_id, VirtualFile* source, uint64_t source_size,
    uint64_t refill_off, uint64_t refill_size) {
    if (refill_size == 0) return core::Unit();

    const std::shared_ptr<CacheEntry> entry = GetCacheEntry(cache_id);
    if (!entry) {
        return core::make_unexpected("cache entry evicted: cache_id=" + cache_id);
    }

    const uint64_t block_size = options_.block_size;
    const uint64_t group_start = refill_off / block_size;
    const uint64_t group_end = DivRoundUp(refill_off + refill_size, block_size);

    for (uint64_t block_id = group_start; block_id < group_end; ++block_id) {
        bool retried_after_eviction = false;
        for (;;) {
            const AcquireRefillResult acquired = entry->AcquireRefill(block_id);
            if (acquired == kRefillInCache) break;
            // Another loader holds it. Rust parks the caller; here the block
            // is left to that loader and this reader falls back to the
            // source, which costs one redundant fetch and no correctness.
            if (acquired == kRefillInProgress) break;

            ++active_refills_;
            const core::Expected<core::Unit, std::string> filled =
                DoRefillBlock(entry.get(), cache_id, source, source_size, block_id);
            --active_refills_;
            entry->FinishRefill(block_id);

            if (filled.ok()) break;
            // One eviction-and-retry on ENOSPC, so an incoming image still
            // gets cached under pressure instead of permanently bypassing the
            // cache. Only once: a second failure means eviction freed nothing.
            if (!retried_after_eviction && IsEnospc(filled.error())) {
                retried_after_eviction = true;
                EvictionInner();
                continue;
            }
            return core::make_unexpected(filled.error());
        }
    }

    EvictionInner();
    return core::Unit();
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

core::Expected<ByteBuffer, std::string> FileCacheBackend::DoPread(
    const std::string& cache_id, VirtualFile* source, uint64_t source_size,
    uint32_t open_flags, uint32_t rw_flags, uint64_t offset, size_t len) {
    ByteBuffer out;
    if (len == 0 || offset >= source_size) return out;

    const uint64_t max_len = source_size - offset;
    const size_t want = static_cast<size_t>(max_len < len ? max_len : len);
    if (want == 0) return out;

    const uint64_t block_size = options_.block_size;
    // Either flag means the caller is probing the cache; a refill would turn
    // the probe into the fetch it was avoiding.
    const bool cache_only =
        (rw_flags & kRwV2CacheOnly) != 0 || (open_flags & kOCacheOnly) != 0;
    // Past this many refills in flight, reads stop refilling and go to the
    // source: a burst would otherwise queue behind each other's I/O.
    const bool allow_refill = active_refills_ < kCacheDefaultMaxRefilling;

    if (options_.capacity_bytes > 0 && !cache_only && allow_refill && source != 0) {
        const std::shared_ptr<CacheEntry> entry = GetCacheEntry(cache_id);
        if (entry) {
            uint64_t hole_off = 0, hole_len = 0;
            if (QueryRefillRange(*entry, offset, static_cast<uint64_t>(want), source_size,
                                 &hole_off, &hole_len)) {
                // Counted once per logical read that had a gap, not once per
                // missing block.
                entry->RecordMiss();
                // Failure is ignored: the read below still serves the request
                // from the source, so a refill problem must not fail it.
                RefillExactRange(cache_id, source, source_size, hole_off, hole_len);
            }
        }
    }

    out.assign(want, 0);
    size_t written = 0;
    const uint64_t read_end = offset + want;
    const uint64_t start_block = offset / block_size;
    const uint64_t end_block = (read_end - 1) / block_size;

    const std::shared_ptr<CacheEntry> entry = GetCacheEntry(cache_id);

    for (uint64_t block_id = start_block; block_id <= end_block && written < want;
         ++block_id) {
        const uint64_t block_base = block_id * block_size;
        const uint64_t from = offset > block_base ? offset - block_base : 0;
        uint64_t to = read_end - block_base;
        if (to > block_size) to = block_size;
        if (to <= from) continue;
        const size_t copy_len = static_cast<size_t>(to - from);

        bool served = false;
        if (entry) {
            const uint8_t* data = 0;
            size_t data_len = 0;
            bool found = false;
            const core::Expected<core::Unit, std::string> got =
                entry->ReadBlock(block_id, &data, &data_len, &found);
            if (got.ok() && found && from + copy_len <= data_len) {
                std::memcpy(&out[written], data + from, copy_len);
                entry->RecordHit();
                served = true;
            }
        }

        if (!served) {
            // Not cached (or cache-only found nothing): straight from the
            // source. A cache-only read with no source yields zeros for the
            // gap rather than failing, matching a sparse read.
            if (source == 0) {
                if (cache_only) {
                    written += copy_len;
                    continue;
                }
                return core::make_unexpected(std::string("cache miss without source"));
            }
            size_t done = 0;
            while (done < copy_len) {
                const core::Expected<size_t, std::string> n = source->ReadAtInto(
                    block_base + from + done, &out[written + done], copy_len - done);
                if (!n.ok()) return core::make_unexpected(n.error());
                if (n.value() == 0) break;
                done += n.value();
            }
            if (done != copy_len) {
                std::ostringstream o;
                o << "short source read at " << (block_base + from) << ": got " << done
                  << ", want " << copy_len;
                return core::make_unexpected(o.str());
            }
        }

        written += copy_len;
    }

    out.resize(written);
    return out;
}

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------

core::Expected<std::shared_ptr<CachedFile>, std::string> FileCacheBackend::OpenFile(
    const std::string& src_name, std::shared_ptr<VirtualFile> source, uint64_t source_size,
    uint32_t open_flags) {
    const std::string cache_id = TransformStoreKey(src_name);

    // Three sources of truth, in Rust's order: the existing entry, then an
    // explicit size, then the source itself.
    //
    // The existing entry comes first because probing the source is not free
    // -- for a remote backend it is a request, and `LazySourceFile` would
    // have to open it. Rust is explicit that this ordering exists to keep
    // lazy opens lazy on a cache hit: the entry's mapping is already set up,
    // so nothing needs the source's size.
    uint64_t size = 0;
    if (!CachedSizeBySrcName(src_name, &size)) {
        size = source_size;
    }
    if (size == 0) {
        // Only now. The entry's mapping is sized once at creation and cannot
        // grow, so the size has to be known before the entry can be created.
        if (!source) {
            return core::make_unexpected(
                std::string("open cached file requires a source or an explicit size"));
        }
        const core::Expected<uint64_t, std::string> probed = source->Size();
        if (!probed.ok()) return core::make_unexpected(probed.error());
        size = probed.value();
    }
    if (size == 0) {
        return core::make_unexpected(std::string("cannot cache a zero-sized source"));
    }

    std::shared_ptr<CacheEntry> entry = GetCacheEntry(cache_id);
    if (!entry) {
        CacheEntryOptions entry_options;
        entry_options.block_size = options_.block_size;
        const CacheEntryPaths paths = CacheEntryPaths::New(options_.cache_dir, cache_id);

        // Tried from disk first: an entry may exist from a previous run with
        // blocks still valid, and recreating it would discard them.
        const core::Expected<std::shared_ptr<CacheEntry>, std::string> loaded =
            CacheEntry::LoadFromDisk(cache_id, paths, entry_options);
        if (loaded.ok() && loaded.value()->SourceSize() == size) {
            entry = loaded.value();
            current_bytes_ += entry->TotalCachedBytes();
        } else {
            // A stale directory is removed rather than reused: its data file
            // is sized for a different source.
            RemoveTree(paths.dir);
            const core::Expected<std::shared_ptr<CacheEntry>, std::string> created =
                CacheEntry::Create(cache_id, src_name, size, entry_options, paths);
            if (!created.ok()) return core::make_unexpected(created.error());
            entry = created.value();
        }
        entries_[cache_id] = entry;
        PublishPressure();
    } else {
        const core::Expected<core::Unit, std::string> same = entry->SetSourceSize(size);
        if (!same.ok()) return core::make_unexpected(same.error());
    }

    entry->IncOpenCount();

    std::shared_ptr<CachedFile> file(new CachedFile());
    file->backend_ = this;
    file->cache_id_ = cache_id;
    file->source_ = source;
    file->source_size_ = size;
    file->open_flags_ = open_flags;
    return file;
}

// ---------------------------------------------------------------------------
// CachedFile
// ---------------------------------------------------------------------------

CachedFile::~CachedFile() {
    const std::shared_ptr<CacheEntry> entry = backend_->GetCacheEntry(cache_id_);
    // Decremented so the entry becomes evictable again. A missing entry is
    // not an error: it may have been evicted while this handle was open.
    if (entry) {
        entry->DecOpenCount();
        // Rust `remove_open_file` touches as well. Closing is a use: without
        // this the entry re-enters the LRU carrying its last *read* timestamp,
        // so a file held open across a quiet period is evicted ahead of
        // genuinely colder ones.
        entry->Touch();
    }
}

core::Expected<uint64_t, std::string> CachedFile::TryGetSize(uint64_t offset, size_t count) {
    const uint64_t req_end = offset + static_cast<uint64_t>(count);
    // Only re-probed when the request reaches past what is known: the source
    // may have grown, and the common case must not pay for a stat.
    if (req_end <= source_size_ || !source_) return source_size_;

    const core::Expected<uint64_t, std::string> probed = source_->Size();
    if (!probed.ok()) return source_size_;
    if (probed.value() > source_size_) {
        // The cached entry keeps its original size -- it cannot be remapped
        // -- so only this handle's view grows. Reads past the entry's extent
        // come from the source.
        source_size_ = probed.value();
    }
    return source_size_;
}

core::Expected<ByteBuffer, std::string> CachedFile::ReadAtWithFlags(uint64_t offset,
                                                                       size_t len,
                                                                       uint32_t rw_flags) {
    const core::Expected<uint64_t, std::string> size = TryGetSize(offset, len);
    if (!size.ok()) return core::make_unexpected(size.error());
    return backend_->DoPread(cache_id_, source_.get(), size.value(), open_flags_, rw_flags,
                             offset, len);
}

core::Expected<ByteBuffer, std::string> CachedFile::ReadAt(uint64_t offset, size_t len) {
    return ReadAtWithFlags(offset, len, 0);
}

core::Expected<size_t, std::string> CachedFile::ReadAtInto(uint64_t offset, uint8_t* dst,
                                                              size_t len) {
    if (len == 0) return static_cast<size_t>(0);
    const core::Expected<ByteBuffer, std::string> data = ReadAtWithFlags(offset, len, 0);
    if (!data.ok()) return core::make_unexpected(data.error());
    const size_t n = data.value().size() < len ? data.value().size() : len;
    if (n > 0) std::memcpy(dst, &data.value()[0], n);
    return n;
}

core::Expected<size_t, std::string> CachedFile::WriteAtWithFlags(uint64_t offset,
                                                                    const uint8_t* data,
                                                                    size_t len, uint32_t) {
    if (len == 0) return static_cast<size_t>(0);

    const core::Expected<uint64_t, std::string> size = TryGetSize(offset, len);
    if (!size.ok()) return core::make_unexpected(size.error());
    const uint64_t source_size = size.value();

    const uint64_t write_end = offset + static_cast<uint64_t>(len);
    if (write_end < offset) return core::make_unexpected(std::string("write range overflow"));

    // Blocks are the unit of both the bitmap and the mapping, so a write that
    // does not start on a page boundary could not be recorded as whole blocks.
    if ((offset % kCachePageSize) != 0) {
        std::ostringstream o;
        o << "offset " << offset << " is not aligned to " << kCachePageSize;
        return core::make_unexpected(o.str());
    }
    if (offset >= source_size) return static_cast<size_t>(0);
    // The final write may be short, since the source itself ends mid-page.
    if ((static_cast<uint64_t>(len) % kCachePageSize) != 0 && write_end < source_size) {
        std::ostringstream o;
        o << "size " << len << " is not aligned to " << kCachePageSize;
        return core::make_unexpected(o.str());
    }

    const uint64_t remain = source_size - offset;
    const size_t write_len = static_cast<size_t>(remain < len ? remain : len);
    if (write_len == 0) return static_cast<size_t>(0);

    return RefillFromSlice(offset, data, write_len);
}

core::Expected<size_t, std::string> CachedFile::WriteAt(uint64_t offset, const uint8_t* data,
                                                           size_t len) {
    return WriteAtWithFlags(offset, data, len, 0);
}

core::Expected<uint64_t, std::string> CachedFile::Size() { return source_size_; }

core::Expected<size_t, std::string> CachedFile::RefillFromSlice(uint64_t offset,
                                                                   const uint8_t* data,
                                                                   size_t len) {
    const std::shared_ptr<CacheEntry> entry = backend_->GetCacheEntry(cache_id_);
    if (!entry) {
        return core::make_unexpected("cache entry evicted: cache_id=" + cache_id_);
    }

    const uint64_t block_size = backend_->options_.block_size;
    // Only whole blocks are published: a partial block recorded as present
    // would serve its unwritten remainder as cached data.
    if ((offset % block_size) != 0) return static_cast<size_t>(0);

    size_t written = 0;
    uint64_t block_id = offset / block_size;
    while (written < len) {
        const size_t chunk = static_cast<size_t>(
            (len - written) < block_size ? (len - written) : block_size);
        const uint64_t block_off = block_id * block_size;
        const uint64_t remain =
            entry->SourceSize() > block_off ? entry->SourceSize() - block_off : 0;
        const size_t expect = static_cast<size_t>(remain < block_size ? remain : block_size);
        // A trailing partial chunk is only publishable when it is the
        // source's own short last block.
        if (chunk < block_size && chunk != expect) break;
        if (expect == 0) break;

        const core::Expected<core::Unit, std::string> ok =
            entry->WriteBlock(block_id, data + written, chunk);
        if (!ok.ok()) return core::make_unexpected(ok.error());
        entry->RecordRefill();
        backend_->AddCurrentBytes(static_cast<uint64_t>(chunk));

        written += chunk;
        ++block_id;
    }

    backend_->EvictionInner();
    return written;
}

core::Expected<size_t, std::string> CachedFile::RefillRange(uint64_t offset, size_t count) {
    const core::Expected<uint64_t, std::string> size = TryGetSize(offset, count);
    if (!size.ok()) return core::make_unexpected(size.error());
    const uint64_t source_size = size.value();

    if (count == 0 || offset >= source_size) return static_cast<size_t>(0);
    const uint64_t remain = source_size - offset;
    const size_t capped = static_cast<size_t>(remain < count ? remain : count);
    if (capped == 0) return static_cast<size_t>(0);
    // Reported as satisfied: with no capacity there is nothing to refill, and
    // the caller's reads will go to the source anyway.
    if (backend_->options_.capacity_bytes == 0) return capped;
    if (!source_) {
        return core::make_unexpected(
            std::string("refill_range requires source file on cache miss"));
    }

    const uint64_t end = offset + capped;
    uint64_t cursor = offset;
    while (cursor < end) {
        const std::shared_ptr<CacheEntry> entry = backend_->GetCacheEntry(cache_id_);
        if (!entry) break;
        uint64_t hole_off = 0, hole_len = 0;
        if (!backend_->QueryRefillRange(*entry, cursor, end - cursor, source_size, &hole_off,
                                        &hole_len)) {
            break;
        }
        const core::Expected<core::Unit, std::string> ok = backend_->RefillExactRange(
            cache_id_, source_.get(), source_size, hole_off, hole_len);
        if (!ok.ok()) return core::make_unexpected(ok.error());
        const uint64_t next = hole_off + hole_len < end ? hole_off + hole_len : end;
        // Guards against a planner that fails to advance, which would spin.
        if (next <= cursor) break;
        cursor = next;
    }
    return capped;
}

// ---------------------------------------------------------------------------
// Rust: cache_store.rs — the background download surface
// ---------------------------------------------------------------------------

uint64_t CachedFile::BackgroundBlockSize() const { return backend_->options_.block_size; }

core::Expected<core::Unit, std::string> CachedFile::BackgroundSourceSnapshot(
    std::shared_ptr<VirtualFile>* source, uint64_t* source_size) const {
    if (!source_) {
        return core::make_unexpected(
            std::string("background cache download requires source file"));
    }
    if (source != 0) *source = source_;
    // The recorded size, not a fresh probe: a background download plans
    // against the size the entry was created with, and the entry's mapping
    // cannot grow past it anyway.
    if (source_size != 0) *source_size = source_size_;
    return core::Unit();
}

core::Expected<uint64_t, std::string> CachedFile::BackgroundBlockCount(
    uint64_t source_size) const {
    const uint64_t block_size = BackgroundBlockSize();
    if (block_size == 0) {
        return core::make_unexpected(std::string("cache block size is zero"));
    }
    const uint64_t block_count = DivRoundUp(source_size, block_size);
    // The bitmap indexes blocks with a u32, so a source needing more than
    // that cannot be tracked at all -- refused here rather than silently
    // wrapping into another block's bit.
    if (block_count > static_cast<uint64_t>(0xFFFFFFFFull) + 1) {
        std::ostringstream o;
        o << "background cache block count " << block_count << " exceeds bitmap range";
        return core::make_unexpected(o.str());
    }
    return block_count;
}

core::Expected<std::vector<uint64_t>, std::string> CachedFile::MissingBackgroundBlocks(
    uint64_t source_size) const {
    const core::Expected<uint64_t, std::string> block_count =
        BackgroundBlockCount(source_size);
    if (!block_count.ok()) return core::make_unexpected(block_count.error());

    const std::shared_ptr<CacheEntry> entry = backend_->GetCacheEntry(cache_id_);
    if (!entry) {
        return core::make_unexpected("cache entry evicted: cache_id=" + cache_id_);
    }

    std::vector<uint64_t> missing;
    for (uint64_t block_id = 0; block_id < block_count.value(); ++block_id) {
        if (!entry->HasBlock(block_id)) missing.push_back(block_id);
    }
    return missing;
}

core::Expected<bool, std::string> CachedFile::BackgroundIsComplete(
    uint64_t source_size) const {
    const core::Expected<std::vector<uint64_t>, std::string> missing =
        MissingBackgroundBlocks(source_size);
    if (!missing.ok()) return core::make_unexpected(missing.error());
    return missing.value().empty();
}

core::Expected<std::vector<std::pair<uint64_t, uint32_t> >, std::string>
CachedFile::ActionableBackgroundChunks(uint64_t source_size,
                                          uint32_t blocks_per_chunk) const {
    const uint64_t per_chunk = blocks_per_chunk < 1 ? 1 : blocks_per_chunk;
    const core::Expected<uint64_t, std::string> block_count =
        BackgroundBlockCount(source_size);
    if (!block_count.ok()) return core::make_unexpected(block_count.error());

    const std::shared_ptr<CacheEntry> entry = backend_->GetCacheEntry(cache_id_);
    if (!entry) {
        return core::make_unexpected("cache entry evicted: cache_id=" + cache_id_);
    }

    std::vector<std::pair<uint64_t, uint32_t> > chunks;
    for (uint64_t start = 0; start < block_count.value(); start += per_chunk) {
        uint64_t end = start + per_chunk;
        // The last chunk is short whenever the source is not a chunk
        // multiple; clamping here is what keeps the read inside the source.
        if (end > block_count.value()) end = block_count.value();

        bool any_missing = false;
        for (uint64_t block_id = start; block_id < end; ++block_id) {
            if (!entry->HasBlock(block_id)) {
                any_missing = true;
                break;
            }
        }
        // Complete chunks are skipped, not re-fetched: that is what makes a
        // resumed download pay only for what it still lacks.
        if (!any_missing) continue;
        chunks.push_back(std::make_pair(start, static_cast<uint32_t>(end - start)));
    }
    return chunks;
}

core::Expected<uint64_t, std::string> CachedFile::BackgroundRefillRange(
    const std::shared_ptr<VirtualFile>& source, uint64_t source_size, uint64_t start_block,
    uint32_t len_blocks) {
    if (!source) {
        return core::make_unexpected(
            std::string("background cache refill requires source file"));
    }
    const uint64_t end_block = start_block + static_cast<uint64_t>(len_blocks);
    if (end_block < start_block) {
        return core::make_unexpected(std::string("background cache block range overflow"));
    }
    const core::Expected<uint64_t, std::string> block_count =
        BackgroundBlockCount(source_size);
    if (!block_count.ok()) return core::make_unexpected(block_count.error());
    if (end_block > block_count.value()) {
        return core::make_unexpected(
            std::string("background cache block range out of bounds"));
    }

    // Backed off while full rather than evicting to make room. Rust's comment
    // says why: the caller's retry loop turns this into a bounded ENOSPC wait,
    // whereas evicting here would have a background download churn the cache
    // against the foreground reads that put it under pressure.
    if (backend_->options_.capacity_bytes == 0 || backend_->is_full_) {
        return core::make_unexpected(std::string("cache is full (ENOSPC)"));
    }

    const std::shared_ptr<CacheEntry> entry = backend_->GetCacheEntry(cache_id_);
    if (!entry) {
        return core::make_unexpected("cache entry evicted: cache_id=" + cache_id_);
    }

    // Loader election for the whole range *before* any source I/O, then one
    // read per contiguous run of won blocks. Electing first is what lets the
    // runs be contiguous: deciding per block would interleave with other
    // loaders and fragment every read.
    std::vector<std::vector<uint64_t> > runs;
    for (uint64_t block_id = start_block; block_id < end_block; ++block_id) {
        const AcquireRefillResult acquired = entry->AcquireRefill(block_id);
        if (acquired == kRefillInCache) continue;
        // Left to whoever holds it. Unlike the foreground path there is no
        // fallback read to make: a background download that skips a block
        // simply leaves it to the loader that owns it.
        if (acquired == kRefillInProgress) continue;

        if (!runs.empty() && runs.back().back() + 1 == block_id) {
            runs.back().push_back(block_id);
        } else {
            runs.push_back(std::vector<uint64_t>(1, block_id));
        }
    }

    uint64_t read_bytes = 0;
    for (size_t r = 0; r < runs.size(); ++r) {
        const core::Expected<uint64_t, std::string> got =
            RefillBlockRun(entry.get(), source.get(), source_size, runs[r]);
        if (!got.ok()) {
            // The failed run's elections are released so a later attempt can
            // retry it; runs already written stay published, because their
            // bytes are in the mapping and the bitmap already says so.
            for (size_t i = r; i < runs.size(); ++i) {
                for (size_t b = 0; b < runs[i].size(); ++b) {
                    entry->FinishRefill(runs[i][b]);
                }
            }
            return core::make_unexpected(got.error());
        }
        read_bytes += got.value();
    }
    return read_bytes;
}

core::Expected<uint64_t, std::string> CachedFile::RefillBlockRun(
    CacheEntry* entry, VirtualFile* source, uint64_t source_size,
    const std::vector<uint64_t>& run) {
    if (run.empty()) return static_cast<uint64_t>(0);

    const uint64_t block_size = BackgroundBlockSize();
    const uint64_t first = run.front();
    const uint64_t last = run.back();
    const uint64_t offset = first * block_size;
    if (offset > source_size) {
        for (size_t i = 0; i < run.size(); ++i) entry->FinishRefill(run[i]);
        return static_cast<uint64_t>(0);
    }

    const uint64_t span = (last - first + 1) * block_size;
    const uint64_t remain = source_size - offset;
    const uint64_t read_len = remain < span ? remain : span;
    if (read_len == 0) {
        for (size_t i = 0; i < run.size(); ++i) entry->FinishRefill(run[i]);
        return static_cast<uint64_t>(0);
    }

    // One source request for the whole run, into scratch. This is the point
    // of running in the background: a single large read instead of one per
    // block, which is also why the bytes cannot go straight into the mapping
    // (they span several blocks' mappings).
    std::vector<uint8_t> scratch(static_cast<size_t>(read_len), 0);
    size_t done = 0;
    while (done < scratch.size()) {
        const core::Expected<size_t, std::string> n =
            source->ReadAtInto(offset + done, &scratch[done], scratch.size() - done);
        if (!n.ok()) {
            for (size_t i = 0; i < run.size(); ++i) entry->FinishRefill(run[i]);
            return core::make_unexpected(n.error());
        }
        if (n.value() == 0) break;
        done += n.value();
    }
    if (static_cast<uint64_t>(done) != read_len) {
        for (size_t i = 0; i < run.size(); ++i) entry->FinishRefill(run[i]);
        std::ostringstream o;
        o << "short background chunk read at offset " << offset << ": got " << done
          << ", want " << read_len;
        return core::make_unexpected(o.str());
    }

    // The whole run is reserved before any block is published: the copies
    // below write through the mapping, and an allocation failure at that
    // point would fault rather than return. Reserving first means a failure
    // happens here, before the batched accounting below.
    const core::Expected<RangeAllocation, std::string> allocated =
        entry->EnsureRangeAllocated(offset, read_len);
    if (!allocated.ok()) {
        for (size_t i = 0; i < run.size(); ++i) entry->FinishRefill(run[i]);
        return core::make_unexpected(allocated.error());
    }

    uint64_t committed = 0;
    for (size_t i = 0; i < run.size(); ++i) {
        const uint64_t block_id = run[i];
        const uint64_t block_off = block_id * block_size;
        const uint64_t block_remain =
            block_off >= source_size ? 0 : source_size - block_off;
        const size_t want =
            static_cast<size_t>(block_remain < block_size ? block_remain : block_size);
        if (want == 0) {
            entry->FinishRefill(block_id);
            continue;
        }

        uint8_t* dst = 0;
        size_t dst_len = 0;
        bool available = false;
        const core::Expected<core::Unit, std::string> mapped =
            entry->MmapMutForBlock(block_id, source_size, &dst, &dst_len, &available);
        if (!mapped.ok() || !available) {
            entry->FinishRefill(block_id);
            if (!mapped.ok()) return core::make_unexpected(mapped.error());
            return core::make_unexpected(
                std::string("background cache block offset beyond source size"));
        }

        const size_t copy_len = dst_len < want ? dst_len : want;
        const size_t src_off = static_cast<size_t>(block_off - offset);
        std::memcpy(dst, &scratch[src_off], copy_len);
        entry->MarkBlockCached(block_id);
        entry->RecordRefill();
        committed += copy_len;
        // Released right after publication, not at the end of the run: the
        // block is readable now, and holding the election would block a
        // foreground reader that wants it.
        entry->FinishRefill(block_id);
    }

    // One accounting update per run rather than per block, matching Rust.
    if (committed > 0) backend_->current_bytes_ += committed;
    return committed;
}

core::Expected<uint64_t, std::string> CachedFile::Query(uint64_t offset, size_t count) {
    const core::Expected<uint64_t, std::string> size = TryGetSize(offset, count);
    if (!size.ok()) return core::make_unexpected(size.error());
    const uint64_t source_size = size.value();
    if (count == 0 || offset >= source_size) return static_cast<uint64_t>(0);

    const std::shared_ptr<CacheEntry> entry = backend_->GetCacheEntry(cache_id_);
    if (!entry) return static_cast<uint64_t>(0);

    const uint64_t block_size = backend_->options_.block_size;
    const uint64_t remain = source_size - offset;
    const uint64_t want = remain < count ? remain : count;
    const uint64_t end = offset + want;

    // Counted as the overlap of each present block with the request, so a
    // request covering half a cached block counts half.
    uint64_t cached = 0;
    for (uint64_t block_id = offset / block_size; block_id * block_size < end; ++block_id) {
        if (!entry->HasBlock(block_id)) continue;
        const uint64_t block_start = block_id * block_size;
        uint64_t block_end = block_start + block_size;
        if (block_end > source_size) block_end = source_size;
        const uint64_t lo = block_start > offset ? block_start : offset;
        const uint64_t hi = block_end < end ? block_end : end;
        if (hi > lo) cached += hi - lo;
    }
    return cached;
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
