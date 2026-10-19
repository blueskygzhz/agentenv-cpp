// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/cache/full_file_cache/{cache_pool,
// cache_store}.rs — the file cache pool and the cached-file handle.
#ifndef AGENTENV_STORAGE_OVERLAYBD_CACHE_POOL_H_
#define AGENTENV_STORAGE_OVERLAYBD_CACHE_POOL_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/cache_entry.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `GIB`, `EVICTION_MARK_BYTES`, `WATERMARK_RATIO` and friends.
const uint64_t kCacheGib = 1024ULL * 1024 * 1024;
const uint64_t kCacheMaxFreeSpaceBytes = 50ULL * kCacheGib;
const uint64_t kCacheEvictionMarkBytes = 5ULL * kCacheGib;
/// Percent of capacity the cache aims to stay under.
const uint64_t kCacheWatermarkRatio = 90;
/// Rust `DEFAULT_MAX_REFILLING` — refills in flight before reads stop
/// refilling and go straight to the source.
const uint32_t kCacheDefaultMaxRefilling = 128;

/// Rust `O_CACHE_ONLY` / `RW_V2_CACHE_ONLY` — ask for cached data only.
///
/// A read carrying either must never reach the source: the caller is probing
/// what is cached, and a transparent refill would turn the probe into the
/// fetch it was trying to avoid.
const uint32_t kOCacheOnly = 0x01000000;
const uint32_t kRwV2CacheOnly = 0x00000004;

/// Rust `PREMERGED_INDEX_DIR` — shares the cache root but is not an entry.
extern const char* const kPremergedIndexDir;

/// Rust `FileCacheBackendOptions`.
struct FileCacheBackendOptions {
    std::string cache_dir;
    uint64_t capacity_bytes;
    /// The refill and management unit.
    uint64_t block_size;
    /// Rust `open_evict_idle`, in seconds. Entries held open but idle longer
    /// than this may have their blocks reclaimed under pressure; the entry
    /// survives and refills on demand. Zero disables open-entry eviction.
    uint64_t open_evict_idle_secs;

    FileCacheBackendOptions()
        : cache_dir(kCacheDefaultDir),
          capacity_bytes(kCacheDefaultCapacityBytes),
          block_size(kCacheDefaultBlockSize),
          open_evict_idle_secs(600) {}

    /// Rust `from_cache_config`.
    static core::Expected<FileCacheBackendOptions, std::string> FromCacheConfig(
        const CacheConfig& cfg);

    /// Rust `normalize`.
    ///
    /// The block size must be a page-aligned power of two: block ids index
    /// both the bitmap and file offsets, and a size that is neither makes the
    /// two disagree. A zero is replaced by the default rather than refused,
    /// since it means "unset".
    core::Expected<core::Unit, std::string> Normalize();
};

/// Rust `CacheStats`.
struct CacheStats {
    size_t entries;
    uint64_t bytes_used;
    uint64_t hits;
    uint64_t misses;
    uint64_t refills;

    CacheStats() : entries(0), bytes_used(0), hits(0), misses(0), refills(0) {}
};

/// Rust `CachedFileStats`.
struct CachedFileStats {
    std::string key;
    uint64_t bytes_used;
    uint64_t hits;
    uint64_t misses;
    uint64_t refills;

    CachedFileStats() : bytes_used(0), hits(0), misses(0), refills(0) {}
};

/// Rust `CacheListType`.
enum CacheListType { kCacheListAll, kCacheListFiles, kCacheListDirs };

/// Rust `CachePoolStat`.
///
/// The sizes are `u32` *in units of the refill unit*, not bytes — that is the
/// shape the upstream ioctl reports, and widening them here would silently
/// change the interface.
struct CachePoolStat {
    uint32_t refill_unit;
    uint32_t total_size;
    uint32_t used_size;
    uint64_t evict_other;
    uint64_t evict_global;
    uint64_t evict_user;

    CachePoolStat()
        : refill_unit(0), total_size(0), used_size(0), evict_other(0), evict_global(0),
          evict_user(0) {}
};

class FileCacheBackend;

/// Rust `CachedFile` — a handle onto one cached file.
///
/// Reads are served from the cache and refill from `source` on a miss. The
/// source is replaceable (`SetSource`) so a layer being downloaded can be
/// read from the registry meanwhile and from the local copy afterwards.
class CachedFile : public VirtualFile {
 public:
    ~CachedFile();

    /// Rust `get_source` / `set_source`.
    std::shared_ptr<VirtualFile> GetSource() const { return source_; }
    void SetSource(std::shared_ptr<VirtualFile> source) { source_ = source; }

    const std::string& CacheId() const { return cache_id_; }

    /// Rust `query` — how many bytes of `[offset, offset+count)` are cached.
    core::Expected<uint64_t, std::string> Query(uint64_t offset, size_t count);

    /// Rust `refill_from_slice` — publish caller-supplied bytes as cached.
    core::Expected<size_t, std::string> RefillFromSlice(uint64_t offset, const uint8_t* data,
                                                           size_t len);

    /// Rust `try_refill_range` / `refill_range` — pull a range from the
    /// source into the cache.
    core::Expected<size_t, std::string> RefillRange(uint64_t offset, size_t count);

    /// Rust `read_at_with_flags`.
    ///
    /// `rw_flags` carrying `kRwV2CacheOnly` keeps the read inside the cache.
    core::Expected<ByteBuffer, std::string> ReadAtWithFlags(uint64_t offset, size_t len,
                                                               uint32_t rw_flags);

    /// Rust `write_at_with_flags`.
    ///
    /// Requires a page-aligned offset, and a page-aligned length unless the
    /// write reaches the end of the source. Blocks are the unit of both the
    /// bitmap and the mapping, so a misaligned write could not be recorded as
    /// a whole block.
    core::Expected<size_t, std::string> WriteAtWithFlags(uint64_t offset, const uint8_t* data,
                                                            size_t len, uint32_t rw_flags);

    // -- VirtualFile -------------------------------------------------------

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override;
    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst,
                                                     size_t len) override;
    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                  size_t len) override;
    core::Expected<uint64_t, std::string> Size() override;

 private:
    friend class FileCacheBackend;

    CachedFile() : source_size_(0), open_flags_(0) {}
    CachedFile(const CachedFile&);
    CachedFile& operator=(const CachedFile&);

    /// Rust `tryget_size` — the source size, refreshed from the source when
    /// the request reaches past what is known.
    core::Expected<uint64_t, std::string> TryGetSize(uint64_t offset, size_t count);

    FileCacheBackend* backend_;
    std::string cache_id_;
    std::shared_ptr<VirtualFile> source_;
    uint64_t source_size_;
    uint32_t open_flags_;
};

/// Rust `FileCacheBackend` — the pool of cache entries.
///
/// Owns the entry map, the byte accounting and eviction. Capacity is enforced
/// by two tiers: closed entries are removed outright, then entries still held
/// open but idle give up their blocks while the handle keeps working.
class FileCacheBackend {
 public:
    /// Rust `with_options` — load whatever is already in the cache directory.
    ///
    /// An entry that fails to load is *deleted*, not skipped: its metadata is
    /// the only record of which blocks are valid, so without it the data file
    /// is unusable and would otherwise occupy the budget forever.
    static core::Expected<std::shared_ptr<FileCacheBackend>, std::string> WithOptions(
        const FileCacheBackendOptions& options);

    /// Rust `from_cache_config`.
    static core::Expected<std::shared_ptr<FileCacheBackend>, std::string> FromCacheConfig(
        const CacheConfig& cfg);

    ~FileCacheBackend();

    const FileCacheBackendOptions& Options() const { return options_; }

    /// Rust `open_file` / `open_file_with_source_size` / `_with_flags`.
    ///
    /// A source size of 0 means "ask the source", which is why a source is
    /// required unless the size is given: the entry's mapping is sized once
    /// at creation and cannot be grown.
    core::Expected<std::shared_ptr<CachedFile>, std::string> OpenFile(
        const std::string& src_name, std::shared_ptr<VirtualFile> source,
        uint64_t source_size, uint32_t open_flags);

    /// Rust `stats`.
    CacheStats Stats() const;

    /// Rust `file_stats` (by store key) and `file_stats_by_src_name`.
    bool FileStats(const std::string& key, CachedFileStats* out) const;
    bool FileStatsBySrcName(const std::string& src_name, CachedFileStats* out) const;

    /// Rust `stat_path`.
    core::Expected<CachePoolStat, std::string> StatPath() const;

    /// Rust `list`.
    std::vector<std::string> List(CacheListType type) const;

    /// Rust `evict_by_size` — reclaim at least `size` bytes, LRU first.
    core::Expected<uint64_t, std::string> EvictBySize(uint64_t size);

    /// Rust `evict_store_key` / `evict_src_name` / `evict_global`.
    core::Expected<core::Unit, std::string> EvictStoreKey(const std::string& store_key);
    core::Expected<core::Unit, std::string> EvictSrcName(const std::string& src_name);
    core::Expected<core::Unit, std::string> EvictGlobal();

    /// Rust `contains_src_name` — whether an entry exists for this source.
    ///
    /// Presence of the *entry*, not of any particular block: an entry with an
    /// empty bitmap still counts, because it names a source the cache knows
    /// about.
    bool ContainsSrcName(const std::string& src_name) const;

    /// Rust `cached_size_by_src_name` — the recorded source size.
    ///
    /// The size the entry was created with, not how much is cached. That is
    /// what a cache-only open needs: the entry's mapping is sized once and
    /// cannot grow, so reopening it has to re-assert the same value.
    ///
    /// Reports absence rather than zero, mirroring Rust's `Option<u64>`: a
    /// missing entry and a zero-sized one are different situations, and the
    /// latter cannot occur because creation refuses it.
    bool CachedSizeBySrcName(const std::string& src_name, uint64_t* out) const;

    /// Rust `rename_src_name` — move an entry to a new source name.
    ///
    /// The cached blocks move with it: the directory is renamed on disk and
    /// the bitmap and counters are adopted by the new entry, so a rename does
    /// not throw away a warm cache.
    ///
    /// Refused while the entry is open. Its data file is mapped by live
    /// handles, and renaming underneath them would leave those handles
    /// writing into a path that no longer names the entry.
    core::Expected<core::Unit, std::string> RenameSrcName(const std::string& old_src,
                                                             const std::string& new_src);

    /// Rust `checkpoint_dirty_entries` — persist entries whose bitmap moved.
    ///
    /// Rust runs this from a periodic worker; here it is the caller's to
    /// drive, since there is no background task to host it.
    void CheckpointDirtyEntries();

    /// Rust `eviction_inner` — bring usage back under the watermark.
    void EvictionInner();

    /// Rust `is_full` — whether refills are currently refused.
    bool IsFull() const { return is_full_; }

    /// Rust `transform_store_key` — the cache id for a source name.
    ///
    /// A sha256 of the name rather than the name itself: it has to be a
    /// single safe path component, and a source name is a URL.
    std::string TransformStoreKey(const std::string& src_name) const;

 private:
    friend class CachedFile;

    FileCacheBackend() : capacity_watermark_(0), current_bytes_(0), is_full_(false),
                         active_refills_(0), evict_global_(0), evict_user_(0) {}
    FileCacheBackend(const FileCacheBackend&);
    FileCacheBackend& operator=(const FileCacheBackend&);

    std::shared_ptr<CacheEntry> GetCacheEntry(const std::string& cache_id) const;

    /// Rust `query_refill_range_for` — the block-aligned hole covering the
    /// gaps of a request, with fully cached edges trimmed off.
    bool QueryRefillRange(const CacheEntry& entry, uint64_t offset, uint64_t size,
                             uint64_t source_size, uint64_t* out_off, uint64_t* out_len) const;

    core::Expected<core::Unit, std::string> RefillExactRange(const std::string& cache_id,
                                                                VirtualFile* source,
                                                                uint64_t source_size,
                                                                uint64_t refill_off,
                                                                uint64_t refill_size);
    core::Expected<core::Unit, std::string> DoRefillBlock(CacheEntry* entry,
                                                             const std::string& cache_id,
                                                             VirtualFile* source,
                                                             uint64_t source_size,
                                                             uint64_t block_id);

    core::Expected<ByteBuffer, std::string> DoPread(const std::string& cache_id,
                                                       VirtualFile* source,
                                                       uint64_t source_size, uint32_t open_flags,
                                                       uint32_t rw_flags, uint64_t offset,
                                                       size_t len);

    /// Byte accounting, paired with republishing the pressure flag: the two
    /// must move together or a refill admitted between them would see a stale
    /// verdict.
    void AddCurrentBytes(uint64_t bytes);
    void SubtractCurrentBytes(uint64_t bytes);
    void PublishPressure();

    uint64_t CalcWaterMark() const;
    uint64_t RiskMark() const;
    /// Rust `capture_disk_pressure`.
    void CaptureDiskPressure(uint64_t* evict_bytes, bool* suppress) const;
    uint64_t PressureEvictTarget(uint64_t current_bytes, uint64_t disk_evict_bytes,
                                    bool suppress) const;

    static bool EntryIsBusy(const CacheEntry& entry);
    std::vector<std::string> EvictableCacheIdsByLru() const;
    std::vector<std::string> IdleOpenCacheIdsByLru(uint64_t idle_cutoff) const;
    uint64_t EvictEntry(const std::string& cache_id, bool user_initiated);
    uint64_t EvictOpenEntryBlocks(const std::string& cache_id, bool user_initiated);

    FileCacheBackendOptions options_;
    uint64_t capacity_watermark_;
    std::map<std::string, std::shared_ptr<CacheEntry> > entries_;
    uint64_t current_bytes_;
    bool is_full_;
    uint32_t active_refills_;
    uint64_t evict_global_;
    uint64_t evict_user_;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_CACHE_POOL_H_
