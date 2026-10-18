// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/cache/meta.rs
#include "agentenv/storage/overlaybd/cache_meta.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>


namespace agentenv {
namespace storage {
namespace overlaybd {

const char* const kCacheDataFileName = "data";
const char* const kCacheMetaFileName = "meta.bin";
const char* const kCacheDefaultDir = "/opt/overlaybd/registry_cache";

namespace {

void PutU32(std::vector<uint8_t>* out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void PutU64(std::vector<uint8_t>* out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

std::string Join(const std::string& dir, const char* name) {
    if (dir.empty()) return std::string(name);
    if (dir[dir.size() - 1] == '/') return dir + name;
    return dir + "/" + name;
}

/// Standard CRC-32C: initial state all-ones, final complement.
///
/// *Not* `zfile::Crc32c`, despite the identical polynomial. Rust reaches two
/// different functions from the same crate:
///
///   * `meta.rs` calls `crc32c::crc32c(data)` — the standard checksum, which
///     is what this is.
///   * `zfile.rs` defines its own `crc32c` as `!crc32c_append(!0, data)`,
///     which cancels the crate's complements and chains the *raw* internal
///     state instead, to stay bit-compatible with upstream OverlayBD.
///
/// The two disagree on every input. Reusing the zfile one here produced a
/// `meta.bin` that no Rust reader would accept, and because the same function
/// would have verified it on read, a C++-only round-trip could not have
/// noticed.
const uint32_t kCrc32cPoly = 0x82F63B78u;

const uint32_t* StandardCrc32cTable() {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int k = 0; k < 8; ++k) {
                crc = (crc & 1) ? ((crc >> 1) ^ kCrc32cPoly) : (crc >> 1);
            }
            table[i] = crc;
        }
        ready = true;
    }
    return table;
}

uint32_t StandardCrc32c(const uint8_t* data, size_t len) {
    const uint32_t* table = StandardCrc32cTable();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

}  // namespace

CacheEntryPaths CacheEntryPaths::New(const std::string& root, const std::string& cache_id) {
    CacheEntryPaths p;
    p.dir = Join(root, cache_id.c_str());
    p.data_path = Join(p.dir, kCacheDataFileName);
    p.meta_path = Join(p.dir, kCacheMetaFileName);
    return p;
}

core::Expected<std::vector<uint8_t>, std::string> CacheMetaDisk::ToBytes() const {
    std::vector<uint8_t> out;
    out.reserve(CacheMetaDiskHeader::kOnDiskSize + 4 + key.size() + bitmap.SerializedSize() + 4);

    // Field by field rather than a struct copy: the Rust record is
    // `#[repr(C, packed)]` little-endian, and this must not inherit whatever
    // padding or byte order the host would give the C++ struct.
    PutU32(&out, header.magic);
    PutU32(&out, header.version);
    PutU64(&out, header.source_size);
    PutU64(&out, header.block_size);
    PutU64(&out, header.last_access_unix_nanos);
    PutU64(&out, header.hits);
    PutU64(&out, header.misses);
    PutU64(&out, header.refills);

    if (key.size() > 0xFFFFFFFFULL) {
        return core::make_unexpected(std::string("key too long"));
    }
    PutU32(&out, static_cast<uint32_t>(key.size()));
    out.insert(out.end(), key.begin(), key.end());

    const std::vector<uint8_t> bits = bitmap.Serialize();
    out.insert(out.end(), bits.begin(), bits.end());

    // crc32c, not plain crc32, and over everything written so far.
    const uint32_t checksum =
        out.empty() ? StandardCrc32c(0, 0) : StandardCrc32c(&out[0], out.size());
    PutU32(&out, checksum);
    return out;
}

core::Expected<CacheMetaDisk, std::string> CacheMetaDisk::FromBytes(const uint8_t* data,
                                                                       size_t len) {
    // Header, key length and CRC are the irreducible minimum.
    if (data == 0 || len < CacheMetaDiskHeader::kOnDiskSize + 4 + 4) {
        return core::make_unexpected(std::string("meta.bin too short for header"));
    }

    CacheMetaDisk out;
    size_t pos = 0;
    out.header.magic = GetU32(data + pos); pos += 4;
    out.header.version = GetU32(data + pos); pos += 4;
    out.header.source_size = GetU64(data + pos); pos += 8;
    out.header.block_size = GetU64(data + pos); pos += 8;
    out.header.last_access_unix_nanos = GetU64(data + pos); pos += 8;
    out.header.hits = GetU64(data + pos); pos += 8;
    out.header.misses = GetU64(data + pos); pos += 8;
    out.header.refills = GetU64(data + pos); pos += 8;

    if (out.header.magic != kCacheMetaMagic) {
        std::ostringstream o;
        o << "meta.bin magic mismatch: expected 0x" << std::hex << kCacheMetaMagic
          << ", got 0x" << out.header.magic;
        return core::make_unexpected(o.str());
    }
    if (out.header.version != kCacheMetaVersion) {
        std::ostringstream o;
        o << "meta.bin version mismatch: expected " << kCacheMetaVersion << ", got "
          << out.header.version;
        return core::make_unexpected(o.str());
    }

    const uint32_t key_len = GetU32(data + pos);
    pos += 4;
    // `key_len + 4` so the trailing CRC is still accounted for.
    if (len < pos + static_cast<size_t>(key_len) + 4) {
        return core::make_unexpected(std::string("meta.bin truncated: key extends past end"));
    }
    out.key.assign(reinterpret_cast<const char*>(data + pos), key_len);
    pos += key_len;

    // The bitmap runs to 4 bytes before the end; the CRC is last.
    const size_t bitmap_len = len - 4 - pos;
    size_t consumed = 0;
    const core::Expected<RoaringBitmap, std::string> bits =
        RoaringBitmap::Deserialize(data + pos, bitmap_len, &consumed);
    if (!bits.ok()) {
        return core::make_unexpected("failed to deserialize bitmap: " + bits.error());
    }
    out.bitmap = bits.value();

    const uint32_t stored = GetU32(data + len - 4);
    const uint32_t computed = StandardCrc32c(data, len - 4);
    if (stored != computed) {
        std::ostringstream o;
        o << "meta CRC mismatch: stored 0x" << std::hex << stored << ", computed 0x"
          << computed;
        return core::make_unexpected(o.str());
    }

    return out;
}

core::Expected<core::Unit, std::string> CacheMetaDisk::WriteToPath(
    const std::string& meta_path) const {
    const core::Expected<std::vector<uint8_t>, std::string> raw = ToBytes();
    if (!raw.ok()) return core::make_unexpected(raw.error());

    const std::string::size_type slash = meta_path.rfind('/');
    if (slash == std::string::npos) {
        return core::make_unexpected(std::string("meta_path has no parent directory"));
    }
    const std::string dir = meta_path.substr(0, slash);

    // A unique temp name per write: two writers racing on the same entry must
    // not share a temp file, or one would rename the other's partial bytes
    // into place.
    std::ostringstream tmp_name;
    tmp_name << dir << "/meta.tmp-" << static_cast<unsigned long>(::getpid()) << "-"
             << static_cast<unsigned long>(::time(0)) << "-" << std::rand();
    const std::string tmp = tmp_name.str();

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return core::make_unexpected("create " + tmp + " failed");

    size_t written = 0;
    const std::vector<uint8_t>& bytes = raw.value();
    while (written < bytes.size()) {
        const ssize_t n = ::write(fd, &bytes[written], bytes.size() - written);
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return core::make_unexpected("write " + tmp + " failed");
        }
        written += static_cast<size_t>(n);
    }

    // Before the rename, not after: a rename that lands while the contents
    // are still only in page cache leaves a valid name over nothing.
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tmp.c_str());
        return core::make_unexpected("fsync " + tmp + " failed");
    }
    ::close(fd);

    if (::rename(tmp.c_str(), meta_path.c_str()) != 0) {
        // The temp file is removed on failure so a retry does not accumulate
        // debris in the cache directory.
        ::unlink(tmp.c_str());
        return core::make_unexpected("rename " + tmp + " -> " + meta_path + " failed");
    }
    return core::Unit();
}

core::Expected<CacheMetaDisk, std::string> CacheMetaDisk::LoadFromPath(
    const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == 0) return core::make_unexpected("open " + path + " failed");

    std::vector<uint8_t> raw;
    uint8_t buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        raw.insert(raw.end(), buf, buf + n);
    }
    std::fclose(f);

    if (raw.empty()) return core::make_unexpected(std::string("meta.bin too short for header"));
    return FromBytes(&raw[0], raw.size());
}

uint64_t NowUnixNanos() {
    struct timespec ts;
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    if (ts.tv_sec < 0) return 0;
    const uint64_t secs = static_cast<uint64_t>(ts.tv_sec);
    // Saturate instead of wrapping: the value orders LRU eviction, so an
    // absurd clock should make an entry look old rather than newest.
    const uint64_t max = ~static_cast<uint64_t>(0);
    if (secs > max / 1000000000ULL) return max;
    const uint64_t base = secs * 1000000000ULL;
    const uint64_t nanos = static_cast<uint64_t>(ts.tv_nsec);
    if (base > max - nanos) return max;
    return base + nanos;
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
