// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/cache/full_file_cache/cache_entry.rs
#ifndef AGENTENV_STORAGE_OVERLAYBD_CACHE_ENTRY_H_
#define AGENTENV_STORAGE_OVERLAYBD_CACHE_ENTRY_H_

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/cache_meta.h"
#include "agentenv/storage/overlaybd/roaring.h"
#include "agentenv/storage/util/mmap_region.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `FileCacheBackendOptions`, the part `CacheEntry` needs.
///
/// The full options struct lives with the pool; only `block_size` reaches an
/// entry, and it is load-bearing: bitmap indices are only meaningful under the
/// block size they were recorded with.
struct CacheEntryOptions {
    uint64_t block_size;

    CacheEntryOptions() : block_size(kCacheDefaultBlockSize) {}
};

/// Rust `RangeAllocation` — outcome of preallocating a byte range.
enum RangeAllocation {
    /// Disk blocks are reserved; a punch can roll the range back.
    kRangeReserved,
    /// The platform or filesystem cannot reserve a range. Nothing was
    /// reserved and writes through the mmap keep their pre-reservation
    /// behaviour -- meaning the SIGBUS-on-full-disk hazard the reservation
    /// exists to remove is still present.
    kRangeUnsupported
};

class CacheEntry;

/// Rust `ReservedRange` — RAII guard over a reserved-but-unpublished range.
///
/// Destroying an armed guard punches the range back to a hole, so a failed
/// refill leaves behind neither allocated disk blocks that capacity
/// accounting never saw nor partially written pages. Call `Commit` once the
/// covered blocks are published.
///
/// Armed only when the reservation actually happened, so on a platform
/// reporting `kRangeUnsupported` the whole mechanism is inert.
///
/// While a guard is armed, no block overlapping the range may be published
/// and the holder must be the sole loader for every covered block --
/// releasing the guard zeroes the range.
class ReservedRange {
 public:
    ReservedRange(CacheEntry* entry, uint64_t offset, uint64_t len, bool armed)
        : entry_(entry), offset_(offset), len_(len), armed_(armed) {}
    ~ReservedRange();

    /// Keep the reservation. Call after the covered data is fully written and
    /// before publishing the blocks, so an armed guard never coexists with
    /// published blocks.
    void Commit() { armed_ = false; }

 private:
    ReservedRange(const ReservedRange&);
    ReservedRange& operator=(const ReservedRange&);

    CacheEntry* entry_;
    uint64_t offset_;
    uint64_t len_;
    bool armed_;
};

/// Rust `AcquireRefillResult`.
enum AcquireRefillResult {
    /// Block is already cached.
    kRefillInCache,
    /// Caller became the loader; proceed to fetch from the source.
    kRefillShouldLoad,
    /// Another loader holds this block.
    ///
    /// Rust has no such variant: its `AcquireRefillFut` returns `Pending` and
    /// parks the caller's waker until the elected loader finishes. A
    /// synchronous caller cannot be parked, so the contention is reported and
    /// the caller reads straight from the source instead -- same data, one
    /// redundant fetch.
    kRefillInProgress
};

/// Rust `CacheEntry` — one cached file: its data, block bitmap and stats.
///
/// Data lives in a *sparse* file mapped shared, so a refill writes blocks
/// through the mapping and a read hands back a pointer into it with no copy.
/// The bitmap records which blocks are present; everything else is recovered
/// or re-fetched.
class CacheEntry {
 public:
    /// Rust `CacheEntry::create` — a new entry for a source of known size.
    ///
    /// A zero source size is refused: the mapping would be empty and every
    /// block index out of range, so there is nothing such an entry could
    /// serve.
    static core::Expected<std::shared_ptr<CacheEntry>, std::string> Create(
        const std::string& cache_id, const std::string& key, uint64_t source_size,
        const CacheEntryOptions& options, const CacheEntryPaths& paths);

    /// Rust `CacheEntry::load_from_disk` — reconstruct from persisted state.
    ///
    /// The persisted bitmap may be a *subset* of what is actually on disk, if
    /// a crash fell between a data write and a checkpoint. That direction is
    /// safe -- the missing blocks are simply re-fetched. The reverse would
    /// not be, which is why eviction clears the bitmap before punching.
    ///
    /// Refuses an entry whose data file size or block size disagrees with the
    /// metadata: bitmap indices only mean anything under the block size they
    /// were written with, and a different one maps them to wrong offsets.
    static core::Expected<std::shared_ptr<CacheEntry>, std::string> LoadFromDisk(
        const std::string& cache_id, const CacheEntryPaths& paths,
        const CacheEntryOptions& options);

    ~CacheEntry();

    // -- identity ----------------------------------------------------------

    const std::string& CacheId() const { return cache_id_; }
    const std::string& Key() const { return key_; }
    const CacheEntryPaths& Paths() const { return paths_; }
    uint64_t BlockSize() const { return block_size_; }
    uint64_t SourceSize() const { return source_size_; }

    // -- block I/O ---------------------------------------------------------

    /// Rust `read_block` — a zero-copy view of a cached block.
    ///
    /// `*data` points into the mapping and stays valid until this entry is
    /// evicted or destroyed. Reports `found = false` when the block is absent
    /// or an eviction is in progress; the caller then refills or reads the
    /// source.
    core::Expected<core::Unit, std::string> ReadBlock(uint64_t block_id, const uint8_t** data,
                                                         size_t* len, bool* found);

    /// Rust `ensure_range_allocated`.
    ///
    /// Use `ReserveRange` instead when a fallible step sits between the
    /// reservation and publication of the blocks.
    core::Expected<RangeAllocation, std::string> EnsureRangeAllocated(uint64_t offset,
                                                                         uint64_t len);

    /// Rust `reserve_range` — reserve with rollback on failure.
    core::Expected<std::shared_ptr<ReservedRange>, std::string> ReserveRange(uint64_t offset,
                                                                               uint64_t len);

    /// Rust `write_block` — write through the mapping and publish the block.
    ///
    /// No rollback guard: nothing between the reservation and the bitmap
    /// publication can fail, and callers may overwrite an already cached
    /// block, which a punch-based rollback would corrupt.
    core::Expected<core::Unit, std::string> WriteBlock(uint64_t block_id, const uint8_t* data,
                                                         size_t len);

    /// Rust `mmap_mut_for_block` — a writable view for a source to fill
    /// directly, avoiding an intermediate buffer.
    ///
    /// The caller must have reserved the range and must be the sole loader
    /// for the block.
    core::Expected<core::Unit, std::string> MmapMutForBlock(uint64_t block_id,
                                                               uint64_t source_size,
                                                               uint8_t** data, size_t* len,
                                                               bool* available);

    /// Rust `mark_block_cached` — publish a block with no I/O.
    void MarkBlockCached(uint64_t block_id);

    /// Rust `evict_all_blocks` — drop every cached block, return bytes freed.
    ///
    /// Ordering is the point: the bitmap is cleared first so every later read
    /// misses, then `meta.bin` is removed, then the data file is punched. A
    /// crash at any step leaves a bitmap that under-claims, never one that
    /// claims blocks already released.
    core::Expected<uint64_t, std::string> EvictAllBlocks();

    /// Rust `set_source_size` — refuses any actual change.
    ///
    /// The mapping and every bitmap index are sized from the original value,
    /// so a resize would invalidate both. Kept as a method because callers
    /// re-assert the same size on reopen.
    core::Expected<core::Unit, std::string> SetSourceSize(uint64_t new_size);

    // -- refill election ---------------------------------------------------

    /// Rust `acquire_refill` — claim the right to load a block.
    AcquireRefillResult AcquireRefill(uint64_t block_id);

    /// Rust `finish_refill` — release the claim, success or failure.
    ///
    /// On failure the block returns to unclaimed so a later reader retries;
    /// the bitmap is only published by a successful write.
    void FinishRefill(uint64_t block_id);

    // -- metadata ----------------------------------------------------------

    /// Rust `total_cached_bytes` — bytes held according to the bitmap.
    ///
    /// The final block is short whenever the source size is not a block
    /// multiple, so it is measured separately instead of counting every block
    /// as full.
    uint64_t TotalCachedBytes() const;

    /// Rust `checkpoint` — fsync the data, then write `meta.bin` atomically.
    ///
    /// Data before metadata: metadata claiming blocks the data file does not
    /// yet hold is the one ordering that loses data.
    core::Expected<core::Unit, std::string> Checkpoint();

    bool IsDirty() const { return dirty_; }

    // -- LRU / stats -------------------------------------------------------

    void Touch();
    void RecordHit();
    void RecordMiss();
    void RecordRefill();
    uint64_t LastAccess() const { return last_access_nanos_; }
    void StatsSnapshot(uint64_t* hits, uint64_t* misses, uint64_t* refills) const;

    /// Rust `rename_store_key`'s transfer block — take over another entry's
    /// bitmap and counters.
    ///
    /// A move, not a copy: `other`'s bitmap is emptied, matching Rust's
    /// `std::mem::take`. The caller has already renamed the directory on
    /// disk, so leaving both entries claiming the same blocks would mean two
    /// owners of one data file.
    ///
    /// Only valid while `other` is closed. Rust checks `open_count == 0`
    /// before reaching here and notes that block reservation state is
    /// therefore irrelevant -- no refill can be in flight to land in the
    /// entry being abandoned.
    void AdoptFrom(CacheEntry& other);

    /// Open-file reference count, maintained by the pool.
    size_t OpenCount() const { return open_count_; }
    void IncOpenCount() { ++open_count_; }
    void DecOpenCount() { if (open_count_ > 0) --open_count_; }

    /// Block presence, for the pool's accounting and tests.
    bool HasBlock(uint64_t block_id) const;
    uint64_t CachedBlockCount() const { return index_.Cardinality(); }

    /// Block ids present within an inclusive range, ascending.
    ///
    /// Narrowed through the bitmap rather than scanning every block of the
    /// range: a read touches a handful of blocks out of what may be tens of
    /// thousands in a large layer.
    std::vector<uint32_t> PresentBlocksInRange(uint32_t lo, uint32_t hi) const {
        return index_.Range(lo, hi);
    }

 private:
    friend class ReservedRange;

    CacheEntry();
    CacheEntry(const CacheEntry&);
    CacheEntry& operator=(const CacheEntry&);

    std::string cache_id_;
    std::string key_;
    CacheEntryPaths paths_;
    uint64_t block_size_;

    /// One bit per block of the source that has been refilled.
    RoaringBitmap index_;

    /// Blocks with an elected loader. Rust pairs this with a waker list so
    /// contending readers sleep; here it only answers "is someone on it".
    std::set<uint64_t> loading_;

    /// Shared mapping over the sparse data file, for zero-copy I/O.
    std::shared_ptr<util::MMapRegion> mem_region_;
    int data_fd_;

    uint64_t source_size_;
    uint64_t last_access_nanos_;
    uint64_t hits_;
    uint64_t misses_;
    uint64_t refills_;
    bool dirty_;
    size_t open_count_;

    /// Rust's `refill_eviction_barrier` collapses to this flag.
    ///
    /// The barrier is an async RwLock: eviction takes the write side while
    /// refills and outstanding read slices hold read permits, so a punch
    /// cannot run while returned bytes are still being consumed. With
    /// synchronous calls there are no outstanding permits to wait on -- a
    /// read has returned before an eviction can start -- so what remains is
    /// refusing reads *during* the eviction itself.
    bool evicting_;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_CACHE_ENTRY_H_
