// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/cache/meta.rs — the per-entry cache
// metadata persisted next to a cached file's data.
#ifndef AGENTENV_STORAGE_OVERLAYBD_CACHE_META_H_
#define AGENTENV_STORAGE_OVERLAYBD_CACHE_META_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/roaring.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `META_MAGIC` — ASCII "OBCH" read as a little-endian u32.
const uint32_t kCacheMetaMagic = 0x4f424348u;
/// Rust `META_VERSION`.
const uint32_t kCacheMetaVersion = 1;

/// Rust `DATA_FILE_NAME` / `META_FILE_NAME`.
extern const char* const kCacheDataFileName;
extern const char* const kCacheMetaFileName;

/// Rust `PAGE_SIZE` / `DEFAULT_BLOCK_SIZE` / capacity defaults.
const uint64_t kCachePageSize = 4096;
const uint64_t kCacheDefaultBlockSize = 256 * 1024;
const uint64_t kCacheDefaultCapacityBytes = 4ULL * 1024 * 1024 * 1024;
const uint64_t kCacheDefaultDiskAvailBytes = 128ULL * 1024 * 1024;
extern const char* const kCacheDefaultDir;

/// Rust `EntryPaths` — where one cache entry's files live.
struct CacheEntryPaths {
    std::string dir;
    std::string data_path;
    std::string meta_path;

    /// Rust `EntryPaths::new` — `<root>/<cache_id>/{data,meta.bin}`.
    static CacheEntryPaths New(const std::string& root, const std::string& cache_id);
};

/// Rust `CacheMetaDiskHeader` — the fixed 56-byte prefix of `meta.bin`.
///
/// `#[repr(C, packed)]` with little-endian fields on the Rust side, so the
/// layout is the format: 4+4+8*6 with no padding anywhere.
struct CacheMetaDiskHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t source_size;
    uint64_t block_size;
    uint64_t last_access_unix_nanos;
    uint64_t hits;
    uint64_t misses;
    uint64_t refills;

    /// The serialized width. Not `sizeof(*this)`: C++ is free to pad this
    /// struct, and the on-disk record is packed.
    static const size_t kOnDiskSize = 56;

    CacheMetaDiskHeader()
        : magic(kCacheMetaMagic),
          version(kCacheMetaVersion),
          source_size(0),
          block_size(0),
          last_access_unix_nanos(0),
          hits(0),
          misses(0),
          refills(0) {}
};

/// Rust `CacheMetaDisk` — what `meta.bin` holds.
///
/// On-disk format, all little-endian:
///
/// ```text
/// [u32 magic][u32 version]
/// [u64 source_size][u64 block_size]
/// [u64 last_access_unix_nanos]
/// [u64 hits][u64 misses][u64 refills]
/// [u32 key_len][u8 × key_len]
/// [serialized roaring bitmap]
/// [u32 crc32c]
/// ```
///
/// The bitmap records which blocks of the source are present in the data
/// file, which is why a corrupt `meta.bin` must be rejected rather than
/// repaired: a wrong bitmap would serve uninitialised data as cached content.
struct CacheMetaDisk {
    CacheMetaDiskHeader header;
    /// The cache key this entry was opened under -- stored so a recovered
    /// entry can be matched back to its source rather than trusted by
    /// directory name alone.
    std::string key;
    RoaringBitmap bitmap;

    /// Rust `to_bytes`.
    core::Expected<std::vector<uint8_t>, std::string> ToBytes() const;

    /// Rust `from_bytes`.
    ///
    /// Checks magic, version and CRC. The CRC covers everything before it,
    /// including the header, so a single flipped bit anywhere fails the parse.
    static core::Expected<CacheMetaDisk, std::string> FromBytes(const uint8_t* data,
                                                                   size_t len);

    /// Rust `write_to_path` — crash-safe: write a temp file, fsync it, rename
    /// over the target.
    ///
    /// The rename is what makes it atomic; without the fsync first, the rename
    /// could land while the contents are still only in page cache, leaving a
    /// valid-looking name pointing at nothing. The *data* file is the caller's
    /// responsibility to sync beforehand -- metadata claiming blocks the data
    /// file does not yet hold is the one ordering that loses data.
    core::Expected<core::Unit, std::string> WriteToPath(const std::string& meta_path) const;

    /// Rust `load_from_path`.
    static core::Expected<CacheMetaDisk, std::string> LoadFromPath(const std::string& path);
};

/// Rust `now_unix_nanos`.
///
/// Saturates rather than wrapping, and a clock before the epoch reads as 0 --
/// the value is only used for LRU ordering, so a bogus clock should make an
/// entry look old, not make it sort unpredictably.
uint64_t NowUnixNanos();

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_CACHE_META_H_
