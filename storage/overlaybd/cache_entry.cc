// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/cache/full_file_cache/cache_entry.rs
#include "agentenv/storage/overlaybd/cache_entry.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

#include "agentenv/storage/overlaybd/sys.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

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

bool PathExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// ReservedRange
// ---------------------------------------------------------------------------

ReservedRange::~ReservedRange() {
    if (!armed_ || entry_ == 0) return;
    // Best effort: the range was never published, so leaving it allocated
    // only wastes disk until the entry is evicted, which punches the whole
    // file anyway. Nothing to report it to from a destructor either.
    sys::ReleaseSpaceHint(entry_->data_fd_, offset_, len_);
}

// ---------------------------------------------------------------------------
// CacheEntry
// ---------------------------------------------------------------------------

CacheEntry::CacheEntry()
    : block_size_(kCacheDefaultBlockSize),
      data_fd_(-1),
      source_size_(0),
      last_access_nanos_(0),
      hits_(0),
      misses_(0),
      refills_(0),
      dirty_(false),
      open_count_(0),
      evicting_(false) {}

CacheEntry::~CacheEntry() {
    // The mapping is released first: it borrows the descriptor, and unmapping
    // after the close would leave a window where the fd is reusable while the
    // mapping still refers to it.
    mem_region_.reset();
    if (data_fd_ >= 0) ::close(data_fd_);
}

core::Expected<std::shared_ptr<CacheEntry>, std::string> CacheEntry::Create(
    const std::string& cache_id, const std::string& key, uint64_t source_size,
    const CacheEntryOptions& options, const CacheEntryPaths& paths) {
    if (source_size == 0) {
        return core::make_unexpected(
            std::string("cannot create cache entry with source_size == 0"));
    }
    if (options.block_size == 0) {
        return core::make_unexpected(std::string("cache block_size must be > 0"));
    }
    if (!MakeDirs(paths.dir)) {
        return core::make_unexpected("create cache dir " + paths.dir + " failed");
    }

    // Not truncating: reopening an existing entry whose metadata was lost
    // must not destroy data that a later checkpoint could still have
    // described. The size is asserted below instead.
    const int fd = ::open(paths.data_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return core::make_unexpected("open " + paths.data_path + " failed");

    // Sized up front and never resized: the mapping and every block index are
    // derived from this length.
    if (::ftruncate(fd, static_cast<off_t>(source_size)) != 0) {
        ::close(fd);
        return core::make_unexpected("set length of " + paths.data_path + " failed");
    }

    const core::Expected<util::MMapRegion, std::string> region =
        util::MMapRegion::FromFd(fd, 0, static_cast<size_t>(source_size));
    if (!region.ok()) {
        ::close(fd);
        return core::make_unexpected("mmap cache data file: " + region.error());
    }

    std::shared_ptr<CacheEntry> e(new CacheEntry());
    e->cache_id_ = cache_id;
    e->key_ = key;
    e->paths_ = paths;
    e->block_size_ = options.block_size;
    e->data_fd_ = fd;
    e->mem_region_.reset(new util::MMapRegion(
        const_cast<core::Expected<util::MMapRegion, std::string>&>(region).take_value()));
    e->source_size_ = source_size;
    e->last_access_nanos_ = NowUnixNanos();
    return e;
}

core::Expected<std::shared_ptr<CacheEntry>, std::string> CacheEntry::LoadFromDisk(
    const std::string& cache_id, const CacheEntryPaths& paths,
    const CacheEntryOptions& options) {
    if (!PathExists(paths.meta_path)) {
        return core::make_unexpected(std::string("cache meta not exists"));
    }
    // Corrupt metadata discards the entry rather than being repaired: the
    // bitmap is the only record of which blocks are valid, and guessing it
    // would serve uninitialised bytes as cached content.
    const core::Expected<CacheMetaDisk, std::string> meta =
        CacheMetaDisk::LoadFromPath(paths.meta_path);
    if (!meta.ok()) return core::make_unexpected(meta.error());

    if (!PathExists(paths.data_path)) {
        return core::make_unexpected(std::string("cache data file not exists"));
    }

    const int fd = ::open(paths.data_path.c_str(), O_RDWR);
    if (fd < 0) return core::make_unexpected("open " + paths.data_path + " failed");

    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return core::make_unexpected("stat " + paths.data_path + " failed");
    }

    const uint64_t source_size = meta.value().header.source_size;
    const uint64_t file_len = static_cast<uint64_t>(st.st_size);
    if (file_len != source_size) {
        ::close(fd);
        std::ostringstream o;
        o << "cache data file size " << file_len << " not match those in meta " << source_size;
        return core::make_unexpected(o.str());
    }
    if (source_size == 0) {
        ::close(fd);
        return core::make_unexpected(
            std::string("cannot load cache entry with source_size == 0"));
    }

    // Bitmap indices are only meaningful under the block size they were
    // recorded with; a different one maps them to wrong file offsets, which
    // would hand back the wrong bytes rather than fail.
    const uint64_t meta_block_size = meta.value().header.block_size;
    if (meta_block_size != options.block_size) {
        ::close(fd);
        std::ostringstream o;
        o << "cache block_size mismatch: persisted " << meta_block_size << ", config "
          << options.block_size;
        return core::make_unexpected(o.str());
    }

    const core::Expected<util::MMapRegion, std::string> region =
        util::MMapRegion::FromFd(fd, 0, static_cast<size_t>(source_size));
    if (!region.ok()) {
        ::close(fd);
        return core::make_unexpected("mmap cache data file on load: " + region.error());
    }

    std::shared_ptr<CacheEntry> e(new CacheEntry());
    e->cache_id_ = cache_id;
    e->key_ = meta.value().key;
    e->paths_ = paths;
    e->block_size_ = options.block_size;
    e->index_ = meta.value().bitmap;
    e->data_fd_ = fd;
    e->mem_region_.reset(new util::MMapRegion(
        const_cast<core::Expected<util::MMapRegion, std::string>&>(region).take_value()));
    e->source_size_ = source_size;
    e->last_access_nanos_ = meta.value().header.last_access_unix_nanos;
    e->hits_ = meta.value().header.hits;
    e->misses_ = meta.value().header.misses;
    e->refills_ = meta.value().header.refills;
    // Not dirty: what is in memory is exactly what is on disk.
    e->dirty_ = false;
    return e;
}

bool CacheEntry::HasBlock(uint64_t block_id) const {
    if (block_id > 0xFFFFFFFFULL) return false;
    return index_.Contains(static_cast<uint32_t>(block_id));
}

core::Expected<core::Unit, std::string> CacheEntry::ReadBlock(uint64_t block_id,
                                                                 const uint8_t** data,
                                                                 size_t* len, bool* found) {
    if (found) *found = false;

    // Reported as a miss rather than an error: the caller refills or reads
    // the source, which is correct either way, whereas failing would turn a
    // concurrent eviction into a read failure.
    if (evicting_) return core::Unit();
    if (!HasBlock(block_id)) return core::Unit();

    const uint64_t offset = block_id * block_size_;
    if (offset >= source_size_) return core::Unit();

    // The final block is short when the source size is not a block multiple.
    const uint64_t remain = source_size_ - offset;
    const size_t want =
        static_cast<size_t>(remain < block_size_ ? remain : block_size_);

    const core::Expected<const uint8_t*, std::string> slice =
        mem_region_->Slice(static_cast<size_t>(offset), want);
    if (!slice.ok()) {
        std::ostringstream o;
        o << "mmap subslice out of range: offset=" << offset << ", want=" << want
          << ", region_len=" << mem_region_->Len();
        return core::make_unexpected(o.str());
    }

    if (data) *data = slice.value();
    if (len) *len = want;
    if (found) *found = true;
    return core::Unit();
}

core::Expected<RangeAllocation, std::string> CacheEntry::EnsureRangeAllocated(uint64_t offset,
                                                                                uint64_t len) {
    // Rust returns `Unsupported` for a zero length: there is nothing to
    // reserve, and reporting `Reserved` would arm a guard over an empty range.
    if (len == 0) return kRangeUnsupported;

    const core::Expected<core::Unit, sys::SysError> done =
        sys::ReserveSpace(data_fd_, offset, len);
    if (done.ok()) return kRangeReserved;

    // Not an error: the caller has to keep working without a reservation,
    // which is the pre-reservation behaviour. Whole platforms land here, so
    // failing would break every refill on them.
    if (done.error().IsUnsupported()) return kRangeUnsupported;

    std::ostringstream o;
    o << "preallocate cache range failed: offset=" << offset << ", len=" << len;
    return core::make_unexpected(o.str());
}

core::Expected<std::shared_ptr<ReservedRange>, std::string> CacheEntry::ReserveRange(
    uint64_t offset, uint64_t len) {
    const core::Expected<RangeAllocation, std::string> allocated =
        EnsureRangeAllocated(offset, len);
    if (!allocated.ok()) return core::make_unexpected(allocated.error());
    // Armed only if something was actually reserved, so the rollback is inert
    // where the reservation was.
    const bool armed = allocated.value() == kRangeReserved;
    return std::shared_ptr<ReservedRange>(new ReservedRange(this, offset, len, armed));
}

core::Expected<core::Unit, std::string> CacheEntry::MmapMutForBlock(uint64_t block_id,
                                                                       uint64_t source_size,
                                                                       uint8_t** data,
                                                                       size_t* len,
                                                                       bool* available) {
    if (available) *available = false;

    const uint64_t offset = block_id * block_size_;
    if (offset >= source_size) return core::Unit();
    const uint64_t remain = source_size - offset;
    const size_t want = static_cast<size_t>(remain < block_size_ ? remain : block_size_);
    if (want == 0) return core::Unit();

    uint8_t* buf = mem_region_->GetMut(static_cast<size_t>(offset), want);
    if (buf == 0) {
        std::ostringstream o;
        o << "mmap_mut_for_block out of range: offset=" << offset << ", want=" << want
          << ", region_len=" << mem_region_->Len();
        return core::make_unexpected(o.str());
    }

    if (data) *data = buf;
    if (len) *len = want;
    if (available) *available = true;
    return core::Unit();
}

core::Expected<core::Unit, std::string> CacheEntry::WriteBlock(uint64_t block_id,
                                                                  const uint8_t* data,
                                                                  size_t len) {
    const uint64_t offset = block_id * block_size_;

    // Reserved before the store, so a full disk surfaces as ENOSPC here
    // instead of SIGBUS at page-fault time inside the memcpy below.
    const core::Expected<RangeAllocation, std::string> allocated =
        EnsureRangeAllocated(offset, static_cast<uint64_t>(len));
    if (!allocated.ok()) return core::make_unexpected(allocated.error());

    uint8_t* buf = mem_region_->GetMut(static_cast<size_t>(offset), len);
    if (buf == 0) {
        std::ostringstream o;
        o << "mmap get_mut out of range: offset=" << offset << ", len=" << len
          << ", region_len=" << mem_region_->Len();
        return core::make_unexpected(o.str());
    }
    if (len > 0) std::memcpy(buf, data, len);

    // Published only after the bytes are in the mapping. No rollback guard is
    // needed: nothing between the reservation and here can fail, and an
    // overwrite of an already cached block would be corrupted by a punch.
    MarkBlockCached(block_id);
    return core::Unit();
}

void CacheEntry::MarkBlockCached(uint64_t block_id) {
    if (block_id > 0xFFFFFFFFULL) return;
    index_.Insert(static_cast<uint32_t>(block_id));
    dirty_ = true;
}

core::Expected<uint64_t, std::string> CacheEntry::EvictAllBlocks() {
    const uint64_t bytes_before = TotalCachedBytes();

    // Set for the whole operation so no read can hand out a pointer into a
    // range about to be punched.
    evicting_ = true;

    // Cleared before any destructive filesystem work: from here every read
    // misses and falls back to the source, so a failure partway can never
    // leave a bitmap claiming data whose blocks are already gone.
    index_.Clear();
    dirty_ = true;

    // The on-disk index goes before the punch, for the same reason in the
    // crash direction: a crash after punching with `meta.bin` still present
    // would reload a bitmap claiming released blocks.
    if (::unlink(paths_.meta_path.c_str()) != 0 && errno != ENOENT) {
        evicting_ = false;
        return core::make_unexpected("remove " + paths_.meta_path + " failed");
    }

    if (source_size_ > 0) {
        const core::Expected<core::Unit, sys::SysError> released =
            sys::ReleaseSpaceHint(data_fd_, 0, source_size_);
        if (!released.ok() && !released.error().IsUnsupported()) {
            // Still propagated: an eviction that reclaimed nothing must not
            // look like it succeeded, or the pool will keep believing it is
            // under its capacity budget.
            evicting_ = false;
            std::ostringstream o;
            o << "release " << source_size_ << " cached bytes failed";
            return core::make_unexpected(o.str());
        }
    }

    evicting_ = false;
    return bytes_before;
}

core::Expected<core::Unit, std::string> CacheEntry::SetSourceSize(uint64_t new_size) {
    // Any actual change is refused: the mapping length and every bitmap index
    // derive from the original size.
    if (source_size_ != new_size) {
        return core::make_unexpected(
            std::string("CacheEntry does not support change set_source_size"));
    }
    return core::Unit();
}

AcquireRefillResult CacheEntry::AcquireRefill(uint64_t block_id) {
    if (HasBlock(block_id)) return kRefillInCache;
    if (loading_.find(block_id) != loading_.end()) return kRefillInProgress;
    loading_.insert(block_id);
    return kRefillShouldLoad;
}

void CacheEntry::FinishRefill(uint64_t block_id) {
    // Unconditional: on failure the block returns to unclaimed so a later
    // reader retries it. Only a successful write publishes the bitmap bit.
    loading_.erase(block_id);
}

uint64_t CacheEntry::TotalCachedBytes() const {
    const uint64_t block_count = index_.Cardinality();
    if (block_count == 0) return 0;

    // All but one block counted as full, then the highest block measured
    // against the source size -- it is the only one that can be short, and
    // counting it as full would over-report every file whose size is not a
    // block multiple.
    const uint64_t full_blocks_bytes = (block_count - 1) * block_size_;

    const std::vector<uint32_t> values = index_.ToVector();
    const uint64_t last_block_id = static_cast<uint64_t>(values[values.size() - 1]);
    const uint64_t last_block_offset = last_block_id * block_size_;
    const uint64_t remain =
        source_size_ > last_block_offset ? source_size_ - last_block_offset : 0;
    const uint64_t last_block_bytes = remain < block_size_ ? remain : block_size_;
    return full_blocks_bytes + last_block_bytes;
}

core::Expected<core::Unit, std::string> CacheEntry::Checkpoint() {
    CacheMetaDisk meta;
    meta.header.source_size = source_size_;
    meta.header.block_size = block_size_;
    meta.header.last_access_unix_nanos = last_access_nanos_;
    meta.header.hits = hits_;
    meta.header.misses = misses_;
    meta.header.refills = refills_;
    meta.key = key_;
    meta.bitmap = index_;

    // Data before metadata. The reverse would publish a bitmap describing
    // blocks the data file does not yet hold, and a crash there serves
    // garbage as cached content. The other direction only loses cache hits.
    if (::fsync(data_fd_) != 0) {
        return core::make_unexpected(std::string("fsync cache data file failed"));
    }
    const core::Expected<core::Unit, std::string> written =
        meta.WriteToPath(paths_.meta_path);
    if (!written.ok()) return written;

    dirty_ = false;
    return core::Unit();
}

void CacheEntry::Touch() { last_access_nanos_ = NowUnixNanos(); }

void CacheEntry::RecordHit() {
    ++hits_;
    Touch();
}

void CacheEntry::RecordMiss() {
    ++misses_;
    Touch();
}

void CacheEntry::RecordRefill() {
    ++refills_;
    Touch();
    // A refill changes the bitmap, so the entry needs a checkpoint; a hit or
    // miss only moves counters, which are not worth a write on their own.
    dirty_ = true;
}

void CacheEntry::StatsSnapshot(uint64_t* hits, uint64_t* misses, uint64_t* refills) const {
    if (hits) *hits = hits_;
    if (misses) *misses = misses_;
    if (refills) *refills = refills_;
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
