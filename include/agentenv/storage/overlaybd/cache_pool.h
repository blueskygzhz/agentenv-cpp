// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/cache/full_file_cache/{cache_pool,
// cache_store}.rs — the file cache pool and the cached-file handle.
#ifndef AGENTENV_STORAGE_OVERLAYBD_CACHE_POOL_H_
#define AGENTENV_STORAGE_OVERLAYBD_CACHE_POOL_H_

#include <cstdint>
#include <functional>
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

/// Rust `CacheFnTransFunc` — maps a source name to the cache id to store it
/// under. Returning an empty string means "no opinion".
typedef std::function<std::string(const std::string&)> CacheFnTransFunc;

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

    /// Node-level cap on concurrently downloading chunks, enforced by this
    /// backend's download scheduler. Bounds total scratch memory to
    /// `bk_download_max_inflight_blocks * bk_download_block_size`. A
    /// per-image `maxInflightBlocks` override never resizes it.
    std::size_t bk_download_max_inflight_blocks;
    /// Node-level cap on concurrently running background-download layer
    /// tasks.
    std::size_t bk_download_max_concurrent_files;
    /// The largest chunk size the scheduler accepts from any task, so the
    /// scratch budget above holds regardless of per-image overrides.
    uint32_t bk_download_block_size;
    /// Per-block read timeout for background downloads, in milliseconds: a
    /// slow read is dropped and reissued rather than holding a block slot
    /// forever. Rust carries a `Duration`; milliseconds here because that is
    /// what `BkDownloadScheduler::New` takes.
    uint64_t bk_download_hedge_timeout_ms;

    /// Defined out-of-line: the hedge-timeout default lives with the
    /// scheduler in `bk_download.h`, which includes *this* header, so naming
    /// the constant here would close the include cycle. Duplicating its value
    /// would be worse -- two defaults that can drift apart silently.
    FileCacheBackendOptions();

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
/// Defined in `bk_download.h`, which includes this header. The backend owns
/// one through a `shared_ptr`, and that needs no definition here.
class BkDownloadScheduler;

/// Rust `CachedFile` — a handle onto one cached file.
///
/// Reads are served from the cache and refill from `source` on a miss. The
/// source is replaceable (`SetSource`) so a layer being downloaded can be
/// read from the registry meanwhile and from the local copy afterwards.
///
/// Lifetime, and a deliberate divergence from Rust: the owning
/// `FileCacheBackend` **must outlive every `CachedFile` it handed out**.
/// `~CachedFile` dereferences `backend_` to release the entry's open count.
/// Rust's `CachedFile` instead holds `backend: FileCacheBackend`, a clone
/// sharing the backend's `Arc` state, and so keeps it alive itself. That is
/// not reproducible here: Rust separates the outer handle from the `Arc`
/// state, whereas this `FileCacheBackend` is one object owned by a
/// `shared_ptr`. Holding a strong reference would close the cycle
/// backend -> bk_scheduler_ -> BkDownloadTask -> CachedFile -> backend and
/// `~FileCacheBackend` would never run -- taking the background-download
/// drain down with it. The raw pointer plus this constraint is the price of
/// not having that cycle.
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

    // -- background download -----------------------------------------------

    /// Rust `background_block_size` — the cache's own block size.
    ///
    /// Distinct from the *download* chunk size: a background fetch covers
    /// several of these per request for throughput, while foreground reads
    /// keep this finer granularity so a fault refills only what it touched.
    uint64_t BackgroundBlockSize() const;

    /// Rust `background_source_snapshot` — the source and its recorded size.
    ///
    /// Refused without a source: a background download has nothing to pull
    /// from, and a cache-only handle must not be queued for one.
    core::Expected<core::Unit, std::string> BackgroundSourceSnapshot(
        std::shared_ptr<VirtualFile>* source, uint64_t* source_size) const;

    /// Rust `missing_background_blocks` — block ids not yet cached.
    core::Expected<std::vector<uint64_t>, std::string> MissingBackgroundBlocks(
        uint64_t source_size) const;

    /// Rust `background_is_complete`.
    core::Expected<bool, std::string> BackgroundIsComplete(uint64_t source_size) const;

    /// Rust `actionable_background_chunks` — download chunks with at least
    /// one missing block, as `(start_block, len_blocks)`.
    ///
    /// Chunks that are already complete are skipped rather than re-fetched,
    /// which is what makes a resumed download cheap.
    core::Expected<std::vector<std::pair<uint64_t, uint32_t> >, std::string>
        ActionableBackgroundChunks(uint64_t source_size, uint32_t blocks_per_chunk) const;

    /// Rust `background_refill_range` — fetch a block range from the source.
    ///
    /// Returns the bytes read. Elects a loader for every block up front, then
    /// issues one source read per *contiguous* run of blocks it won, so a
    /// range already partly cached does not re-fetch what it has.
    ///
    /// Refuses with ENOSPC while the cache is full: the caller's retry loop
    /// turns that into a bounded wait instead of churning eviction.
    core::Expected<uint64_t, std::string> BackgroundRefillRange(
        const std::shared_ptr<VirtualFile>& source, uint64_t source_size,
        uint64_t start_block, uint32_t len_blocks);

    // -- VirtualFile -------------------------------------------------------

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override;
    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst,
                                                     size_t len) override;
    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                  size_t len) override;
    core::Expected<uint64_t, std::string> Size() override;

    /// Rust `read_at_with_ctx` / `read_at_into_with_ctx` / `write_at_with_ctx`.
    ///
    /// This is the layer the whole context exists for: on a miss the backend
    /// reads the *source*, and that read is what a ublk queue thread wants on
    /// its own ring. Everything above here only forwards.
    ///
    /// Note what this does and does not buy today: the leaf `LocalFile`
    /// deliberately does not override the ctx variants (see `local_file.h`),
    /// so a local source still lands on a synchronous `pread`. A remote
    /// source is unaffected either way. The forwarding is still required --
    /// without it the context is lost here and could never reach a leaf that
    /// grows the ability to use it.
    core::Expected<ByteBuffer, std::string> ReadAtWithCtx(const IoCtx& ctx, uint64_t offset,
                                                            size_t len) override;
    core::Expected<size_t, std::string> ReadAtIntoWithCtx(const IoCtx& ctx, uint64_t offset,
                                                            uint8_t* dst, size_t len) override;
    core::Expected<size_t, std::string> WriteAtWithCtx(const IoCtx& ctx, uint64_t offset,
                                                         const uint8_t* data,
                                                         size_t len) override;

 private:
    friend class FileCacheBackend;

    CachedFile() : source_size_(0), open_flags_(0) {}
    CachedFile(const CachedFile&);
    CachedFile& operator=(const CachedFile&);

    /// Rust `tryget_size` — the source size, refreshed from the source when
    /// the request reaches past what is known.
    core::Expected<uint64_t, std::string> TryGetSize(uint64_t offset, size_t count);

    /// Rust `background_block_count` — blocks the bitmap must span.
    core::Expected<uint64_t, std::string> BackgroundBlockCount(uint64_t source_size) const;

    /// Rust `refill_block_run` — one source read covering a whole run of
    /// loader-owned blocks, then a publish per block.
    core::Expected<uint64_t, std::string> RefillBlockRun(CacheEntry* entry, VirtualFile* source,
                                                            uint64_t source_size,
                                                            const std::vector<uint64_t>& run);

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

    /// Rust `with_options_and_trans_func`.
    ///
    /// `trans_func` maps a source name to the cache id it is stored under.
    /// The point is that one blob reachable through several URLs must share a
    /// single entry: the image service installs a transform that keeps only
    /// the basename (the blob digest), so switching the P2P accelerator
    /// address -- which rewrites every layer URL -- does not invalidate the
    /// whole cache.
    ///
    /// A transform returning an empty string is treated as "no opinion" and
    /// the default keying applies.
    static core::Expected<std::shared_ptr<FileCacheBackend>, std::string>
    WithOptionsAndTransFunc(const FileCacheBackendOptions& options,
                               const CacheFnTransFunc& trans_func);

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

    // -- background download ----------------------------------------------
    //
    // The scheduler belongs to the backend, as in Rust: a `BkDownloadTask`
    // reaches back into the backend to publish the blocks it fetched, so the
    // backend has to outlive it.
    //
    // Rust needs a `Weak` plus an `_bk_scheduler_owner: Option<Arc<..>>` to
    // pin shutdown to one clone, because its `FileCacheBackend` is a value
    // type that gets cloned. This one is always held through a `shared_ptr`
    // handed out by the factories, so there is a single object and a plain
    // owning pointer says the same thing with no clone bookkeeping.
    //
    // The three *submit* entry points are free functions in `bk_download.h`
    // (`SubmitBkDownloadBatch` and friends) rather than members here. Rust
    // has them on the backend, but their signatures name `BkDownloadRequest`
    // and `StartupPackHandle`, which live in a header that includes this one
    // -- declaring them here would close the include cycle. The semantics,
    // including the closed-scheduler report, are unchanged.

    /// Rust's `_bk_scheduler_owner` / `bk_scheduler`, collapsed into one
    /// owning handle. Incomplete type by design: naming the scheduler's
    /// definition here is what would create the cycle.
    std::shared_ptr<BkDownloadScheduler> BkScheduler() const { return bk_scheduler_; }

    /// Rust `shutdown_bk_downloads` — stop cache-owned background downloads
    /// and wait for in-flight I/O to drain. Idempotent.
    void ShutdownBkDownloads();

    /// Whether a cache id is registered with the scheduler. Rust marks the
    /// equivalent `#[cfg(test)]`; kept here because deregistration is a
    /// property worth asserting rather than reviewing.
    bool BkDownloadRegistered(const std::string& cache_id) const;

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

    /// Rust `get_cache_entry(id).map(|e| e.key.clone())` — the source name an
    /// entry was created with.
    ///
    /// Needed because a background download is registered by `cache_id` (what
    /// a `CachedFile` exposes) but has to be *reopened* by the source name,
    /// and the digest is one-way.
    bool CacheKeyOf(const std::string& cache_id, std::string* out) const;

    /// Rust `transform_store_key` — the cache id for a source name.
    ///
    /// The installed transform wins when it returns a non-empty value;
    /// otherwise the name is hashed.
    ///
    /// Two deviations from Rust here, both deliberate:
    ///
    ///  * Rust falls back to the source name *verbatim*. The id becomes a
    ///    path component under the cache dir, so a URL would produce
    ///    `cache_dir/https:/host/v2/...` -- a deep tree with `:` in it, and
    ///    a top-level `https:` entry that the restart scan would then try to
    ///    load as a cache entry. Hashing keeps it one safe component.
    ///
    ///  * The transform's result has leading separators stripped. Rust joins
    ///    it onto the cache dir, and its transform returns `/<basename>`;
    ///    joining an absolute path in Rust *replaces* the base, so the entry
    ///    would be created at the filesystem root instead of inside the
    ///    cache. That escape also hides the entry from the restart scan and
    ///    from eviction accounting, which both walk the cache dir.
    std::string TransformStoreKey(const std::string& src_name) const;

 private:
    friend class CachedFile;

    FileCacheBackend() : capacity_watermark_(0), current_bytes_(0), is_full_(false),
                         active_refills_(0), evict_global_(0), evict_user_(0) {}
    FileCacheBackend(const FileCacheBackend&);
    FileCacheBackend& operator=(const FileCacheBackend&);

    std::shared_ptr<CacheEntry> GetCacheEntry(const std::string& cache_id) const;

    /// The scheduler this backend owns. Declared last among the handles on
    /// purpose is *not* enough to make teardown safe -- `~FileCacheBackend`
    /// drains it explicitly before touching anything else, because a worker
    /// still running a task holds a raw `FileCacheBackend*`.
    std::shared_ptr<BkDownloadScheduler> bk_scheduler_;

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

    /// Rust `do_preadv2_generic` / `refill_range_generic` /
    /// `cache_refill_with_data_generic` — the same three bodies, generic in the
    /// reader strategy so the ctx path reads its source through the caller's
    /// ring instead of a synchronous `pread`.
    ///
    /// Declared here and defined in the .cc, where the two instantiations are
    /// named explicitly. Templates keep the strategy out of the installed
    /// header, which is also why `internal.h` does not have to be public.
    template <typename Reader>
    core::Expected<ByteBuffer, std::string> DoPreadGeneric(const Reader& reader,
                                                             const std::string& cache_id,
                                                             VirtualFile* source,
                                                             uint64_t source_size,
                                                             uint32_t open_flags, uint32_t rw_flags,
                                                             uint64_t offset, size_t len);
    template <typename Reader>
    core::Expected<core::Unit, std::string> RefillExactRangeGeneric(
        const Reader& reader, const std::string& cache_id, VirtualFile* source,
        uint64_t source_size, uint64_t refill_off, uint64_t refill_size);
    template <typename Reader>
    core::Expected<core::Unit, std::string> DoRefillBlockGeneric(
        const Reader& reader, CacheEntry* entry, const std::string& cache_id, VirtualFile* source,
        uint64_t source_size, uint64_t block_id);

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

    /// Rust `fn_trans_func`. Empty means the default keying applies.
    ///
    /// Set once at construction rather than behind a lock: Rust guards it with
    /// an `RwLock` but never writes it after `with_options_and_trans_func`,
    /// and a key transform that could change at runtime would orphan every
    /// entry stored under the previous one.
    CacheFnTransFunc trans_func_;
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
