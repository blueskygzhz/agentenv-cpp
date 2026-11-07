// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/compression/zfile.rs
#include "agentenv/storage/overlaybd/zfile.h"

#include <time.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

// `CommitCompressedBatch` recovers the concrete local file to take the
// synchronous-pwrite fast path, which is what Rust's `as_any` downcast does.
#include "agentenv/storage/overlaybd/local_file.h"

#ifdef AGENTENV_WITH_ZSTD
#include <lz4.h>
#include <zstd.h>
#endif

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace zfile {

namespace {

/// Rust `MAX_COALESCED_READ_SIZE`.
///
/// Distinct from `kMaxReadSize`, which bounds one *logical* block. This bounds
/// the combined *encoded* span of a batch, and a single encoded block larger
/// than the budget still forms its own batch — otherwise a 64 KiB
/// incompressible block would be unreadable.
const size_t kMaxCoalescedReadSize = 64 * 1024;

uint32_t ReadU32Le(const uint8_t* b, size_t off) {
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) |
           (static_cast<uint32_t>(b[off + 3]) << 24);
}

uint64_t ReadU64Le(const uint8_t* b, size_t off) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[off + i]) << (8 * i);
    return v;
}

/// Monotonic seconds, for the metric wrappers only.
///
/// Seconds as a double rather than the milliseconds `oss.cc` uses: a single
/// block decode is routinely under a millisecond, and integer milliseconds
/// would report most of this histogram as zero.
double MonotonicSecondsForMetrics() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

void WriteU32Le(uint8_t* b, size_t off, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) b[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

void WriteU64Le(uint8_t* b, size_t off, uint64_t v) {
    for (size_t i = 0; i < 8; ++i) b[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

/// Castagnoli reflected polynomial. The table is built once on first use.
const uint32_t kCrc32cPoly = 0x82F63B78u;

const uint32_t* Crc32cTable() {
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

/// The raw (uncomplemented) CRC state transition, matching Rust's
/// `crc32c_append` once both ends are complemented.
uint32_t Crc32cRaw(uint32_t state, const uint8_t* data, size_t len) {
    const uint32_t* table = Crc32cTable();
    uint32_t crc = ~state;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/// Rust `HeaderTrailer::MAGIC0` — `u64::from_le_bytes(*b"ZFile\0\x01\0")`.
uint64_t Magic0() {
    static const uint8_t bytes[8] = {'Z', 'F', 'i', 'l', 'e', 0x00, 0x01, 0x00};
    return ReadU64Le(bytes, 0);
}

const uint8_t kMagic1[16] = {0x74, 0x75, 0x6a, 0x69, 0x2e, 0x79, 0x79, 0x66,
                             0x40, 0x41, 0x6c, 0x69, 0x62, 0x61, 0x62, 0x61};

/// Read exactly `len` bytes or fail. A short read is an error here: every
/// caller is reading a fixed-size record or a planned extent, so a partial
/// result would be parsed as if complete.
core::Expected<core::Unit, std::string> ReadExactAt(VirtualFile* file, uint8_t* buf, size_t len,
                                                       uint64_t offset) {
    size_t done = 0;
    while (done < len) {
        const core::Expected<size_t, std::string> n =
            file->ReadAtInto(offset + done, buf + done, len - done);
        if (!n.ok()) return core::make_unexpected(n.error());
        if (n.value() == 0) {
            std::ostringstream o;
            o << "short read at " << (offset + done) << ": wanted " << (len - done) << " more";
            return core::make_unexpected(o.str());
        }
        done += n.value();
    }
    return core::Unit();
}

#ifdef AGENTENV_WITH_ZSTD

/// Rust `ZSTD_C_LEVEL`.
const int kZstdCLevel = 3;

/// "destination buffer too small (N), expected at least M" — the guard both
/// encoders apply before handing the buffer to the codec.
///
/// Checked against `MaxDstSize()` rather than against the source length,
/// because that is what the codec is allowed to emit in the worst case: a
/// buffer merely as large as the input is enough for every *compressible*
/// block and silently too small for an incompressible one.
std::string DstTooSmall(size_t have, size_t want) {
    std::ostringstream o;
    o << "destination buffer too small (" << have << "), expected at least " << want;
    return o.str();
}

class Lz4Compressor : public Compressor {
 public:
    Lz4Compressor(size_t max_dst, size_t src_blk) : max_dst_(max_dst), src_blk_(src_blk) {}

    size_t MaxDstSize() const { return max_dst_; }
    size_t SrcBlockSize() const { return src_blk_; }

    core::Expected<size_t, std::string> Compress(const uint8_t* src, size_t src_len,
                                                    uint8_t* dst, size_t dst_cap) {
        if (dst_cap < max_dst_) return core::make_unexpected(DstTooSmall(dst_cap, max_dst_));
        // C liblz4 block format, as upstream OverlayBD writes it. Block sizes
        // are 4 KiB-scale, so the int casts cannot overflow.
        const int n = ::LZ4_compress_default(reinterpret_cast<const char*>(src),
                                             reinterpret_cast<char*>(dst),
                                             static_cast<int>(src_len),
                                             static_cast<int>(dst_cap));
        if (n <= 0) return core::make_unexpected(std::string("lz4 compression failed"));
        return static_cast<size_t>(n);
    }

    core::Expected<size_t, std::string> Decompress(const uint8_t* src, size_t src_len,
                                                      uint8_t* dst, size_t dst_cap) const {
        const int n = ::LZ4_decompress_safe(reinterpret_cast<const char*>(src),
                                            reinterpret_cast<char*>(dst),
                                            static_cast<int>(src_len),
                                            static_cast<int>(dst_cap));
        if (n <= 0) return core::make_unexpected(std::string("lz4 decompress failed"));
        return static_cast<size_t>(n);
    }

 private:
    size_t max_dst_;
    size_t src_blk_;
};

/// Rust's `thread_local! { ZSTD_CCTX }` — one reusable context per thread.
///
/// Creating a context costs more than compressing one 4 KiB block, so it must
/// be reused; it cannot live in the compressor instance because a zstd
/// context is not shareable across threads, and the same class also serves
/// the const, concurrently shared decode side. Thread-local storage satisfies
/// both: reused within a worker, never shared between workers.
class ZstdCCtxHolder {
 public:
    ZstdCCtxHolder() : ctx_(NULL) {}
    ~ZstdCCtxHolder() {
        if (ctx_ != NULL) ::ZSTD_freeCCtx(ctx_);
    }
    ::ZSTD_CCtx* Get() {
        if (ctx_ == NULL) ctx_ = ::ZSTD_createCCtx();
        return ctx_;
    }

 private:
    ZstdCCtxHolder(const ZstdCCtxHolder&);
    ZstdCCtxHolder& operator=(const ZstdCCtxHolder&);
    ::ZSTD_CCtx* ctx_;
};

::ZSTD_CCtx* ThreadZstdCCtx() {
    static thread_local ZstdCCtxHolder holder;
    return holder.Get();
}

class ZstdCompressor : public Compressor {
 public:
    ZstdCompressor(size_t max_dst, size_t src_blk) : max_dst_(max_dst), src_blk_(src_blk) {}

    size_t MaxDstSize() const { return max_dst_; }
    size_t SrcBlockSize() const { return src_blk_; }

    core::Expected<size_t, std::string> Compress(const uint8_t* src, size_t src_len,
                                                    uint8_t* dst, size_t dst_cap) {
        if (dst_cap < max_dst_) return core::make_unexpected(DstTooSmall(dst_cap, max_dst_));
        ::ZSTD_CCtx* ctx = ThreadZstdCCtx();
        if (ctx == NULL) {
            return core::make_unexpected(std::string("create zstd compression context"));
        }
        const size_t n = ::ZSTD_compressCCtx(ctx, dst, dst_cap, src, src_len, kZstdCLevel);
        if (::ZSTD_isError(n)) {
            return core::make_unexpected(std::string("zstd compress failed: ") +
                                         ::ZSTD_getErrorName(n));
        }
        if (n == 0) return core::make_unexpected(std::string("zstd returned empty output"));
        return n;
    }

    core::Expected<size_t, std::string> Decompress(const uint8_t* src, size_t src_len,
                                                      uint8_t* dst, size_t dst_cap) const {
        if (dst_cap < src_blk_) {
            return core::make_unexpected(DstTooSmall(dst_cap, src_blk_));
        }
        const size_t n = ::ZSTD_decompress(dst, dst_cap, src, src_len);
        if (::ZSTD_isError(n)) {
            return core::make_unexpected(std::string("zstd decompress failed: ") +
                                         ::ZSTD_getErrorName(n));
        }
        if (n == 0) {
            return core::make_unexpected(std::string("zstd decompress returned zero bytes"));
        }
        if (n > src_blk_) {
            std::ostringstream o;
            o << "zstd decompress returned more than the block size: " << n;
            return core::make_unexpected(o.str());
        }
        return n;
    }

 private:
    size_t max_dst_;
    size_t src_blk_;
};

#endif  // AGENTENV_WITH_ZSTD

}  // namespace

// ---------------------------------------------------------------------------
// crc32c
// ---------------------------------------------------------------------------

uint32_t Crc32c(const uint8_t* data, size_t len) {
    // Rust: `!crc32c_append(!0, data)`.
    return ~Crc32cRaw(~0u, data, len);
}

uint32_t Crc32cAppend(uint32_t crc, const uint8_t* data, size_t len) {
    // Rust: `crc32c_append(crc, data)`. The running state is the complement of
    // the reported value, which is why the previous result is inverted on the
    // way in and the new one on the way out -- feeding the reported value
    // directly would restart the polynomial from the wrong state.
    return ~Crc32cRaw(~crc, data, len);
}

uint32_t Crc32cSalt(const uint8_t* data, size_t len) {
    // Rust: `!crc32c_append(!NOI_WELL_KNOWN_PRIME, data)`. The prime seeds the
    // running state directly rather than being hashed as data, matching
    // upstream `crc32c_extend()`.
    return ~Crc32cRaw(~kNoiWellKnownPrime, data, len);
}

// ---------------------------------------------------------------------------
// CompressOptions
// ---------------------------------------------------------------------------

core::Expected<CompressOptions, std::string> CompressOptions::DecodeFrom(const uint8_t* buf,
                                                                            size_t len) {
    if (len < 24) {
        return core::make_unexpected(std::string("compress options buffer too small"));
    }
    CompressOptions o;
    o.block_size = ReadU32Le(buf, 0);
    o.algo = buf[4];
    o.level = buf[5];
    o.use_dict = buf[6];
    // Byte 7 is a hole: `reserved` starts at 8, not 7.
    o.reserved = ReadU32Le(buf, 8);
    o.dict_size = ReadU32Le(buf, 12);
    o.verify = buf[16];
    return o;
}

core::Expected<core::Unit, std::string> CompressOptions::EncodeInto(uint8_t* buf,
                                                                       size_t len) const {
    if (len < 24) {
        return core::make_unexpected(std::string("compress options buffer too small"));
    }
    for (size_t i = 0; i < 24; ++i) buf[i] = 0;
    WriteU32Le(buf, 0, block_size);
    buf[4] = algo;
    buf[5] = level;
    buf[6] = use_dict;
    WriteU32Le(buf, 8, reserved);
    WriteU32Le(buf, 12, dict_size);
    buf[16] = verify;
    return core::Unit();
}

// ---------------------------------------------------------------------------
// Compressor
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> Compressor::CompressBatch(
    const uint8_t* src, size_t src_len, const std::vector<size_t>& src_chunk_len, uint8_t* dst,
    size_t dst_len, std::vector<size_t>* dst_chunk_len, size_t n) {
    if (n == 0) return core::Unit();
    if (src_chunk_len.size() < n || dst_chunk_len == NULL || dst_chunk_len->size() < n) {
        return core::make_unexpected(std::string("chunk length arrays are smaller than n"));
    }
    const size_t each_dst_capacity = dst_len / n;
    if (each_dst_capacity < MaxDstSize()) {
        std::ostringstream o;
        o << "destination chunk capacity (" << each_dst_capacity
          << ") smaller than max_dst_size (" << MaxDstSize() << ")";
        return core::make_unexpected(o.str());
    }

    size_t src_offset = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t chunk_len = src_chunk_len[i];
        if (chunk_len > SIZE_MAX - src_offset) {
            return core::make_unexpected(std::string("source offset overflow"));
        }
        const size_t end = src_offset + chunk_len;
        if (end > src_len) {
            return core::make_unexpected(std::string("source chunk exceeds source buffer"));
        }
        if (each_dst_capacity != 0 && i > SIZE_MAX / each_dst_capacity) {
            return core::make_unexpected(std::string("destination offset overflow"));
        }
        const size_t dst_begin = i * each_dst_capacity;
        const core::Expected<size_t, std::string> nout =
            Compress(src + src_offset, chunk_len, dst + dst_begin, each_dst_capacity);
        if (!nout.ok()) return core::make_unexpected(nout.error());
        (*dst_chunk_len)[i] = nout.value();
        src_offset = end;
    }
    return core::Unit();
}

core::Expected<std::shared_ptr<Compressor>, std::string> CreateCompressor(
    const CompressArgs& args) {
#ifdef AGENTENV_WITH_ZSTD
    const size_t src_blk = static_cast<size_t>(args.opt.block_size);
    if (src_blk == 0) {
        return core::make_unexpected(std::string("block_size must be > 0"));
    }

    if (args.opt.algo == CompressOptions::kLz4) {
        // LZ4 takes its sizes as `int`, so a block beyond INT_MAX cannot be
        // expressed to it at all.
        if (src_blk > static_cast<size_t>(0x7FFFFFFF)) {
            std::ostringstream o;
            o << "block_size " << src_blk << " exceeds LZ4 i32 limit";
            return core::make_unexpected(o.str());
        }
        const size_t bound = static_cast<size_t>(::LZ4_compressBound(static_cast<int>(src_blk)));
        return std::shared_ptr<Compressor>(new Lz4Compressor(bound, src_blk));
    }
    if (args.opt.algo == CompressOptions::kZstd) {
        return std::shared_ptr<Compressor>(
            new ZstdCompressor(::ZSTD_compressBound(src_blk), src_blk));
    }

    // Refused rather than defaulted: a wrong decoder produces plausible
    // garbage instead of an error.
    std::ostringstream o;
    o << "invalid compression algorithm: " << static_cast<int>(args.opt.algo);
    return core::make_unexpected(o.str());
#else
    (void)args;
    return core::make_unexpected(
        std::string("compression not compiled in (AGENTENV_WITH_ZSTD)"));
#endif
}

// ---------------------------------------------------------------------------
// HeaderTrailer
// ---------------------------------------------------------------------------

HeaderTrailer::HeaderTrailer()
    : magic0(Magic0()),
      size(static_cast<uint32_t>(kHeaderTrailerBodySize)),
      digest(0),
      flags(0),
      index_offset(0),
      index_size(0),
      original_file_size(0),
      index_crc(0),
      reserved_0(0) {
    for (size_t i = 0; i < 16; ++i) magic1[i] = kMagic1[i];
}

core::Expected<HeaderTrailer, std::string> HeaderTrailer::DecodeFrom512(const uint8_t* buf,
                                                                          size_t len) {
    if (len < kHeaderTrailerSpace) {
        return core::make_unexpected(std::string("header/trailer buffer too small"));
    }
    HeaderTrailer h;
    h.magic0 = ReadU64Le(buf, 0);
    for (size_t i = 0; i < 16; ++i) h.magic1[i] = buf[8 + i];
    h.size = ReadU32Le(buf, 24);
    h.digest = ReadU32Le(buf, 28);
    h.flags = ReadU64Le(buf, 32);
    h.index_offset = ReadU64Le(buf, 40);
    h.index_size = ReadU64Le(buf, 48);
    h.original_file_size = ReadU64Le(buf, 56);
    h.index_crc = ReadU32Le(buf, 64);
    h.reserved_0 = ReadU32Le(buf, 68);
    const core::Expected<CompressOptions, std::string> opt =
        CompressOptions::DecodeFrom(buf + 72, len - 72);
    if (!opt.ok()) return core::make_unexpected(opt.error());
    h.opt = opt.value();
    return h;
}

core::Expected<core::Unit, std::string> HeaderTrailer::Encode512(uint8_t* out,
                                                                   size_t cap) const {
    if (cap < kHeaderTrailerSpace) {
        return core::make_unexpected(std::string("header/trailer buffer too small"));
    }
    for (size_t i = 0; i < kHeaderTrailerSpace; ++i) out[i] = 0;
    WriteU64Le(out, 0, magic0);
    for (size_t i = 0; i < 16; ++i) out[8 + i] = magic1[i];
    WriteU32Le(out, 24, size);
    WriteU32Le(out, 28, digest);
    WriteU64Le(out, 32, flags);
    WriteU64Le(out, 40, index_offset);
    WriteU64Le(out, 48, index_size);
    WriteU64Le(out, 56, original_file_size);
    WriteU32Le(out, 64, index_crc);
    WriteU32Le(out, 68, reserved_0);
    return opt.EncodeInto(out + 72, cap - 72);
}

bool HeaderTrailer::VerifyMagic() const {
    if (magic0 != Magic0()) return false;
    for (size_t i = 0; i < 16; ++i) {
        if (magic1[i] != kMagic1[i]) return false;
    }
    return true;
}

bool HeaderTrailer::IsValid(const uint8_t* bytes, size_t len) const {
    // An absent digest is not a failed one.
    if (!IsDigestEnabled()) return true;
    if (len < kHeaderTrailerSpace) return false;

    // The digest covers the slot with its own field zeroed, which is the only
    // way the value can be checked against bytes that contain it.
    std::vector<uint8_t> tmp(bytes, bytes + kHeaderTrailerSpace);
    for (size_t i = 28; i < 32; ++i) tmp[i] = 0;
    return Crc32c(&tmp[0], tmp.size()) == digest;
}

// ---------------------------------------------------------------------------
// JumpTable
// ---------------------------------------------------------------------------

core::Expected<uint64_t, std::string> JumpTable::OffsetAt(size_t idx) const {
    if (group_size_ == 0) {
        return core::make_unexpected(std::string("jump table not initialized"));
    }
    if (idx >= deltas_.size()) {
        std::ostringstream o;
        o << "jump table index out of range: idx=" << idx << ", size=" << deltas_.size();
        return core::make_unexpected(o.str());
    }

    const size_t part_idx = idx / group_size_;
    const size_t inner_idx = idx % group_size_;
    if (part_idx >= partial_offset_.size()) {
        std::ostringstream o;
        o << "partial_offset index out of range: idx=" << part_idx
          << ", size=" << partial_offset_.size();
        return core::make_unexpected(o.str());
    }
    const uint64_t part_offset = partial_offset_[part_idx];

    // A group boundary stores its absolute offset, so the delta there is 0 by
    // construction and must not be added again.
    if (inner_idx == 0) return part_offset;
    return part_offset + static_cast<uint64_t>(deltas_[idx]);
}

core::Expected<core::Unit, std::string> JumpTable::Build(const std::vector<uint32_t>& ibuf,
                                                            uint64_t offset_begin,
                                                            uint32_t block_size,
                                                            bool enable_crc) {
    if (block_size == 0) {
        return core::make_unexpected(std::string("block_size cannot be zero"));
    }

    // How many blocks fit before the running delta could overflow a u16.
    const size_t bs = static_cast<size_t>(block_size);
    const size_t group_size = (static_cast<size_t>(0xFFFF) + 1) / bs;
    if (group_size == 0) {
        std::ostringstream o;
        o << "block_size too large for jump table grouping: " << bs;
        return core::make_unexpected(o.str());
    }

    group_size_ = group_size;
    partial_offset_.clear();
    deltas_.clear();
    partial_offset_.reserve(ibuf.size() / group_size + 1);
    deltas_.reserve(ibuf.size() + 1);

    uint64_t raw_offset = offset_begin;
    partial_offset_.push_back(raw_offset);
    deltas_.push_back(0);

    // With verification on every block carries a 4-byte CRC trailer, so a
    // block that small holds no payload at all.
    const size_t min_block_size = enable_crc ? sizeof(uint32_t) : 0;

    for (size_t i = 1; i <= ibuf.size(); ++i) {
        const size_t blk = static_cast<size_t>(ibuf[i - 1]);
        if (blk <= min_block_size) {
            std::ostringstream o;
            o << "unexpected block size: idx=" << (i - 1) << ", size=" << ibuf[i - 1];
            return core::make_unexpected(o.str());
        }

        if (raw_offset > ~static_cast<uint64_t>(0) - static_cast<uint64_t>(ibuf[i - 1])) {
            return core::make_unexpected(std::string("jump table offset overflow"));
        }
        raw_offset += static_cast<uint64_t>(ibuf[i - 1]);

        // Every `group_size` blocks the absolute offset is stored and the
        // delta restarts, which is what keeps the deltas inside 16 bits.
        if (i % group_size == 0) {
            partial_offset_.push_back(raw_offset);
            deltas_.push_back(0);
            continue;
        }

        const uint64_t prev = static_cast<uint64_t>(deltas_[i - 1]);
        const uint64_t next = prev + static_cast<uint64_t>(ibuf[i - 1]);
        if (next >= 0xFFFFULL) {
            std::ostringstream o;
            o << "build block length failed idx=" << (i - 1) << " delta=" << prev
              << " blk=" << ibuf[i - 1] << " exceeds " << 0xFFFF;
            return core::make_unexpected(o.str());
        }
        deltas_.push_back(static_cast<uint16_t>(next));
    }

    return core::Unit();
}

// ---------------------------------------------------------------------------
// load_jump_table
// ---------------------------------------------------------------------------

namespace {

core::Expected<core::Unit, std::string> LoadJumpTable(VirtualFile* file, HeaderTrailer* out_ht,
                                                         JumpTable* out_jt) {
    std::vector<uint8_t> buf(kHeaderTrailerSpace, 0);
    const core::Expected<core::Unit, std::string> head =
        ReadExactAt(file, &buf[0], buf.size(), 0);
    if (!head.ok()) return core::make_unexpected(head.error());

    const core::Expected<HeaderTrailer, std::string> header =
        HeaderTrailer::DecodeFrom512(&buf[0], buf.size());
    if (!header.ok()) return core::make_unexpected(header.error());

    if (!header.value().VerifyMagic() || !header.value().IsHeader()) {
        return core::make_unexpected(std::string("header magic/type mismatch"));
    }
    if (!header.value().IsValid(&buf[0], buf.size())) {
        return core::make_unexpected(std::string("header digest validation failed"));
    }

    const core::Expected<uint64_t, std::string> st_size = file->Size();
    if (!st_size.ok()) return core::make_unexpected(st_size.error());

    HeaderTrailer selected = header.value();
    size_t index_bytes = 0;

    if (!header.value().IsHeaderOverwrite()) {
        // The normal shape: the header is a stub and the trailer at EOF holds
        // the authoritative index location, because it is only known once the
        // data has been written.
        if (!header.value().IsDataFile()) {
            return core::make_unexpected(std::string("unsupported zfile type"));
        }
        if (st_size.value() < static_cast<uint64_t>(kHeaderTrailerSpace)) {
            return core::make_unexpected(std::string("file too small for trailer"));
        }

        const uint64_t trailer_offset =
            st_size.value() - static_cast<uint64_t>(kHeaderTrailerSpace);
        const core::Expected<core::Unit, std::string> tail =
            ReadExactAt(file, &buf[0], buf.size(), trailer_offset);
        if (!tail.ok()) return core::make_unexpected(tail.error());

        const core::Expected<HeaderTrailer, std::string> trailer =
            HeaderTrailer::DecodeFrom512(&buf[0], buf.size());
        if (!trailer.ok()) return core::make_unexpected(trailer.error());
        if (!trailer.value().VerifyMagic() || !trailer.value().IsTrailer() ||
            !trailer.value().IsDataFile() || !trailer.value().IsSealed()) {
            return core::make_unexpected(std::string("trailer magic/type/sealed mismatch"));
        }

        // `index_size` counts u32 entries, so the byte length is 4x it.
        if (trailer.value().index_size > (~static_cast<uint64_t>(0)) / 4) {
            return core::make_unexpected(std::string("index_size overflow"));
        }
        const uint64_t want = trailer.value().index_size * 4;
        if (trailer.value().index_offset > trailer_offset) {
            return core::make_unexpected(std::string("invalid index_offset beyond trailer"));
        }
        if (want > trailer_offset - trailer.value().index_offset) {
            return core::make_unexpected(std::string("invalid index bytes"));
        }
        if (want > static_cast<uint64_t>(~static_cast<size_t>(0))) {
            return core::make_unexpected(std::string("index_size does not fit size_t"));
        }
        index_bytes = static_cast<size_t>(want);
        selected = trailer.value();
    } else {
        // Header-overwrite files carry the index location up front; there is
        // no trailer to consult.
        if (header.value().index_size > (~static_cast<uint64_t>(0)) / 4) {
            return core::make_unexpected(std::string("index_size overflow"));
        }
        const uint64_t want = header.value().index_size * 4;
        if (want > static_cast<uint64_t>(~static_cast<size_t>(0))) {
            return core::make_unexpected(std::string("index_size does not fit size_t"));
        }
        index_bytes = static_cast<size_t>(want);
    }

    std::vector<uint8_t> index_raw(index_bytes, 0);
    if (index_bytes > 0) {
        const core::Expected<core::Unit, std::string> got =
            ReadExactAt(file, &index_raw[0], index_bytes, selected.index_offset);
        if (!got.ok()) return core::make_unexpected(got.error());
    }

    if (selected.IsDigestEnabled()) {
        const uint32_t crc =
            index_bytes > 0 ? Crc32c(&index_raw[0], index_bytes) : Crc32c(0, 0);
        if (crc != selected.index_crc) {
            std::ostringstream o;
            o << "jump table checksum mismatch: got " << crc << ", expected "
              << selected.index_crc;
            return core::make_unexpected(o.str());
        }
    }

    if (selected.index_size == 0) {
        return core::make_unexpected(std::string("empty index is not a valid zfile"));
    }
    if (index_raw.size() != static_cast<size_t>(selected.index_size) * sizeof(uint32_t)) {
        return core::make_unexpected(std::string("index byte length mismatch"));
    }

    std::vector<uint32_t> ibuf;
    ibuf.reserve(static_cast<size_t>(selected.index_size));
    for (size_t i = 0; i + 4 <= index_raw.size(); i += 4) {
        ibuf.push_back(ReadU32Le(&index_raw[0], i));
    }

    // The first block starts after the header slot plus any dictionary.
    const core::Expected<core::Unit, std::string> built = out_jt->Build(
        ibuf, static_cast<uint64_t>(kHeaderTrailerSpace) + selected.opt.dict_size,
        selected.opt.block_size, selected.opt.verify != 0);
    if (!built.ok()) return core::make_unexpected(built.error());

    *out_ht = selected;
    return core::Unit();
}

}  // namespace

// ---------------------------------------------------------------------------
// ZFileRO
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> ZFileRO::CompressedBlockExtent(uint64_t logical_offset,
                                                                         uint64_t* offset,
                                                                         uint64_t* len) const {
    const uint64_t block_size = static_cast<uint64_t>(Options().block_size);
    if (block_size == 0) {
        return core::make_unexpected(std::string("zfile block size is zero"));
    }
    const uint64_t idx64 = logical_offset / block_size;
    if (idx64 > static_cast<uint64_t>(~static_cast<size_t>(0))) {
        return core::make_unexpected(std::string("zfile block index overflow"));
    }
    const size_t idx = static_cast<size_t>(idx64);

    const core::Expected<uint64_t, std::string> begin = jump_table_.OffsetAt(idx);
    if (!begin.ok()) return core::make_unexpected(begin.error());
    const core::Expected<uint64_t, std::string> end = jump_table_.OffsetAt(idx + 1);
    if (!end.ok()) return core::make_unexpected(end.error());

    if (offset) *offset = begin.value();
    // Saturating, matching Rust: a non-monotonic table yields 0 rather than a
    // huge wrapped length.
    if (len) *len = end.value() > begin.value() ? end.value() - begin.value() : 0;
    return core::Unit();
}

core::Expected<size_t, std::string> ZFileRO::PlanCoalescedBatch(size_t begin_idx,
                                                                  size_t end_idx) const {
    const core::Expected<uint64_t, std::string> batch_begin = jump_table_.OffsetAt(begin_idx);
    if (!batch_begin.ok()) return core::make_unexpected(batch_begin.error());

    // Starts at begin_idx+1, so the first block is always included even when
    // it alone exceeds the budget -- otherwise a 64 KiB incompressible block
    // could never be read.
    size_t next = begin_idx + 1;
    while (next < end_idx) {
        const core::Expected<uint64_t, std::string> candidate_end =
            jump_table_.OffsetAt(next + 1);
        if (!candidate_end.ok()) return core::make_unexpected(candidate_end.error());
        if (candidate_end.value() < batch_begin.value()) {
            return core::make_unexpected(std::string("invalid block offsets"));
        }
        if (candidate_end.value() - batch_begin.value() >
            static_cast<uint64_t>(kMaxCoalescedReadSize)) {
            break;
        }
        ++next;
    }
    return next;
}

metrics::ZFileCodec ZFileRO::CodecLabel() const {
    // Only the two Rust names exist as labels. Anything else was already
    // refused at open (`zfile_open_ro` rejects an unknown algo rather than
    // defaulting), so this cannot mislabel a file that is actually readable.
    return ht_.opt.algo == CompressOptions::kZstd ? metrics::kZFileZstd : metrics::kZFileLz4;
}

core::Expected<size_t, std::string> ZFileRO::Pread(uint8_t* buf, size_t len,
                                                     uint64_t offset) const {
    // Rust `pread_generic`: the observation wrapper. The counters are
    // recorded exactly once per logical pread, on success and on every error
    // exit alike -- a pread that failed halfway still did the reads it did,
    // and dropping them would make the byte totals disagree with the backing
    // store under exactly the conditions worth investigating.
    const bool sampled = metrics::SampleZFilePread();
    metrics::ZFileReadStats stats;
    double decompress_seconds = 0.0;

    const double started = sampled ? MonotonicSecondsForMetrics() : 0.0;
    const core::Expected<size_t, std::string> result =
        PreadObserved(buf, len, offset, sampled, &stats, &decompress_seconds);
    metrics::RecordZFileRead(CodecLabel(), stats);

    if (sampled) {
        metrics::RecordZFileReadDurations(
            CodecLabel(), result.ok() ? metrics::kZFileReadOk : metrics::kZFileReadError,
            MonotonicSecondsForMetrics() - started, decompress_seconds);
    }
    return result;
}

core::Expected<size_t, std::string> ZFileRO::PreadObserved(uint8_t* buf, size_t len,
                                                             uint64_t offset, bool sampled,
                                                             metrics::ZFileReadStats* stats,
                                                             double* decompress_seconds) const {
    const size_t block_size = static_cast<size_t>(ht_.opt.block_size);
    if (block_size == 0) {
        return core::make_unexpected(std::string("invalid block_size"));
    }
    if (block_size > kMaxReadSize) {
        std::ostringstream o;
        o << "block_size " << block_size << " > MAX_READ_SIZE " << kMaxReadSize;
        return core::make_unexpected(o.str());
    }

    // Past EOF is an empty read, not an error: the logical size is whatever
    // the trailer declared.
    if (offset >= ht_.original_file_size) return static_cast<size_t>(0);

    const uint64_t avail = ht_.original_file_size - offset;
    const size_t clamped =
        static_cast<uint64_t>(len) < avail ? len : static_cast<size_t>(avail);
    if (clamped == 0) return static_cast<size_t>(0);

    const size_t begin_idx = static_cast<size_t>(offset / block_size);
    const uint64_t end = offset + clamped - 1;
    const size_t end_idx = static_cast<size_t>(end / block_size) + 1;

    // The request's span, counted once. Retries deliberately do not
    // re-increment it, so `blocks` stays a measure of how much was asked for
    // rather than of how much work it took.
    stats->blocks += static_cast<uint64_t>(end_idx - begin_idx);

    size_t written = 0;
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> retry_block;
    std::vector<uint8_t> raw;

    size_t batch_begin_idx = begin_idx;
    while (batch_begin_idx < end_idx) {
        const core::Expected<size_t, std::string> planned =
            PlanCoalescedBatch(batch_begin_idx, end_idx);
        if (!planned.ok()) return core::make_unexpected(planned.error());
        const size_t batch_end_idx = planned.value();

        const core::Expected<uint64_t, std::string> batch_begin =
            jump_table_.OffsetAt(batch_begin_idx);
        if (!batch_begin.ok()) return core::make_unexpected(batch_begin.error());
        const core::Expected<uint64_t, std::string> batch_end =
            jump_table_.OffsetAt(batch_end_idx);
        if (!batch_end.ok()) return core::make_unexpected(batch_end.error());
        if (batch_end.value() < batch_begin.value()) {
            return core::make_unexpected(std::string("invalid block offsets"));
        }
        const uint64_t batch_len64 = batch_end.value() - batch_begin.value();
        if (batch_len64 > static_cast<uint64_t>(~static_cast<size_t>(0))) {
            return core::make_unexpected(std::string("batch_len overflow"));
        }
        const size_t batch_len = static_cast<size_t>(batch_len64);

        // One merged read per batch; individual blocks are then sliced out of
        // it, so a run of small blocks costs one backing IO rather than N.
        compressed.assign(batch_len, 0);
        if (batch_len > 0) {
            // Counted before the read, not after: a failed read still
            // submitted the bytes, and only counting successes would hide
            // exactly the traffic worth looking at.
            ++stats->read_batches;
            stats->encoded_bytes += batch_len64;
            const core::Expected<core::Unit, std::string> got =
                ReadExactAt(file_.get(), &compressed[0], batch_len, batch_begin.value());
            if (!got.ok()) return core::make_unexpected(got.error());
        }

        for (size_t idx = batch_begin_idx; idx < batch_end_idx; ++idx) {
            const core::Expected<uint64_t, std::string> block_begin = jump_table_.OffsetAt(idx);
            if (!block_begin.ok()) return core::make_unexpected(block_begin.error());
            const core::Expected<uint64_t, std::string> block_end =
                jump_table_.OffsetAt(idx + 1);
            if (!block_end.ok()) return core::make_unexpected(block_end.error());
            if (block_end.value() < block_begin.value()) {
                return core::make_unexpected(std::string("invalid block offsets"));
            }
            const uint64_t encoded_len64 = block_end.value() - block_begin.value();
            const size_t encoded_len = static_cast<size_t>(encoded_len64);
            const size_t merged_begin =
                static_cast<size_t>(block_begin.value() - batch_begin.value());

            // With verification on, the last 4 bytes are the CRC and are not
            // part of the compressed payload.
            size_t compressed_size = encoded_len;
            if (data_verify_) {
                if (encoded_len < sizeof(uint32_t)) {
                    return core::make_unexpected(
                        std::string("encoded block smaller than crc32 trailer"));
                }
                compressed_size = encoded_len - sizeof(uint32_t);
            }

            // Only the first and last block of the request are partial.
            const size_t cp_begin =
                (idx == begin_idx) ? static_cast<size_t>(offset % block_size) : 0;
            size_t cp_len = (idx == end_idx - 1)
                                ? static_cast<size_t>(end % block_size) + 1
                                : block_size;
            if (cp_len < cp_begin) {
                return core::make_unexpected(std::string("invalid cp_len"));
            }
            cp_len -= cp_begin;

            int retry = 3;
            // The first attempt decodes straight out of the merged batch
            // buffer. Once a block fails it is re-read on its own, and every
            // later attempt for that block uses the private buffer -- blocks
            // already consumed from the batch are never re-read.
            bool from_retry_buffer = false;
            for (;;) {
                const uint8_t* block =
                    from_retry_buffer ? &retry_block[0] : &compressed[merged_begin];

                if (data_verify_) {
                    const uint32_t expected = ReadU32Le(block, compressed_size);
                    const uint32_t got = Crc32cSalt(block, compressed_size);
                    if (got != expected) {
                        if (retry > 0) {
                            --retry;
                            ++stats->checksum_retries;
                            // A retry is another exact read of the same
                            // block, so it is both a batch and more encoded
                            // bytes on the wire.
                            ++stats->read_batches;
                            stats->encoded_bytes += encoded_len64;
                            const core::Expected<core::Unit, std::string> ev =
                                file_->EvictRange(block_begin.value(), encoded_len64);
                            if (!ev.ok()) return core::make_unexpected(ev.error());
                            retry_block.assign(encoded_len, 0);
                            const core::Expected<core::Unit, std::string> re = ReadExactAt(
                                file_.get(), &retry_block[0], encoded_len, block_begin.value());
                            if (!re.ok()) return core::make_unexpected(re.error());
                            from_retry_buffer = true;
                            continue;
                        }
                        std::ostringstream o;
                        o << "checksum verification failed: idx=" << idx << ", got=" << got
                          << ", expected=" << expected;
                        return core::make_unexpected(o.str());
                    }
                }

                // CRC-only mode stops here: the point is to scan for
                // corruption without paying for decompression.
                if (valid_mode_ == kValidCrcOnly) break;

                std::string decomp_err;
                bool decomp_ok = false;
                // Clock read only when sampled: an unsampled pread must not
                // pay two `clock_gettime` calls per block. Accumulated across
                // every block *and every retry decode* of this one pread, so
                // the histogram observes a per-pread sum, never a per-block
                // value.
                const double decomp_started = sampled ? MonotonicSecondsForMetrics() : 0.0;
                if (cp_begin == 0 && cp_len == block_size) {
                    // A whole block decodes straight into the caller's
                    // buffer, with no intermediate copy.
                    const core::Expected<size_t, std::string> n = compressor_->Decompress(
                        block, compressed_size, buf + written, cp_len);
                    if (!n.ok()) {
                        decomp_err = n.error();
                    } else if (n.value() != cp_len) {
                        std::ostringstream o;
                        o << "unexpected decompressed size: got " << n.value() << ", expected "
                          << cp_len;
                        decomp_err = o.str();
                    } else {
                        decomp_ok = true;
                    }
                } else {
                    // Partial block: decode in full to scratch, then copy the
                    // requested window out of it.
                    if (raw.size() < block_size) raw.assign(block_size, 0);
                    const core::Expected<size_t, std::string> n =
                        compressor_->Decompress(block, compressed_size, &raw[0], raw.size());
                    if (!n.ok()) {
                        decomp_err = n.error();
                    } else if (cp_begin + cp_len > n.value()) {
                        std::ostringstream o;
                        o << "decompressed range out of bounds: n=" << n.value()
                          << ", begin=" << cp_begin << ", len=" << cp_len;
                        decomp_err = o.str();
                    } else {
                        std::memcpy(buf + written, &raw[cp_begin], cp_len);
                        decomp_ok = true;
                    }
                }

                if (sampled) {
                    *decompress_seconds += MonotonicSecondsForMetrics() - decomp_started;
                }

                if (decomp_ok) break;

                if (retry > 0) {
                    --retry;
                    ++stats->decompress_retries;
                    ++stats->read_batches;
                    stats->encoded_bytes += encoded_len64;
                    const core::Expected<core::Unit, std::string> ev =
                        file_->EvictRange(block_begin.value(), encoded_len64);
                    if (!ev.ok()) return core::make_unexpected(ev.error());
                    retry_block.assign(encoded_len, 0);
                    const core::Expected<core::Unit, std::string> re = ReadExactAt(
                        file_.get(), &retry_block[0], encoded_len, block_begin.value());
                    if (!re.ok()) return core::make_unexpected(re.error());
                    from_retry_buffer = true;
                    continue;
                }
                return core::make_unexpected(decomp_err);
            }

            written += cp_len;
        }

        batch_begin_idx = batch_end_idx;
    }

    // Only on the success path, as Rust arranges: a pread that failed
    // returned nothing to the caller, and counting the bytes it had staged
    // would overstate what the layer actually served.
    stats->output_bytes += static_cast<uint64_t>(written);
    return written;
}

core::Expected<ByteBuffer, std::string> ZFileRO::ReadAt(uint64_t offset, size_t len) {
    ByteBuffer out(len, 0);
    if (len == 0) return out;
    const core::Expected<size_t, std::string> n = Pread(&out[0], len, offset);
    if (!n.ok()) return core::make_unexpected(n.error());
    out.resize(n.value());
    return out;
}

core::Expected<size_t, std::string> ZFileRO::ReadAtInto(uint64_t offset, uint8_t* dst,
                                                          size_t len) {
    return Pread(dst, len, offset);
}

core::Expected<size_t, std::string> ZFileRO::WriteAt(uint64_t, const uint8_t*, size_t) {
    // A zfile's layout is fixed by its trailer; a write would invalidate the
    // jump table it was read through.
    return core::make_unexpected(std::string("zfile is read-only"));
}

core::Expected<uint64_t, std::string> ZFileRO::Size() {
    // The decompressed size. A caller asking a zfile for its size wants the
    // content length, not the encoded footprint.
    return ht_.original_file_size;
}

// ---------------------------------------------------------------------------
// open / probe / bulk operations
// ---------------------------------------------------------------------------

core::Expected<std::shared_ptr<ZFileRO>, std::string> ZFileOpenRo(
    std::shared_ptr<VirtualFile> file, bool verify) {
    if (!file) return core::make_unexpected(std::string("zfile open: null file"));

    HeaderTrailer ht;
    JumpTable jt;
    // Bounded retry: a torn cached page is recoverable by evicting and
    // re-reading, so `verify` trades two extra reads for not failing the open.
    int retry = 2;
    for (;;) {
        const core::Expected<core::Unit, std::string> loaded =
            LoadJumpTable(file.get(), &ht, &jt);
        if (loaded.ok()) break;
        if (verify && retry > 0) {
            --retry;
            const core::Expected<core::Unit, std::string> ev = file->EvictAll();
            if (!ev.ok()) return core::make_unexpected(ev.error());
            continue;
        }
        return core::make_unexpected(loaded.error());
    }

    const CompressArgs args(ht.opt);
    const core::Expected<std::shared_ptr<Compressor>, std::string> compressor =
        CreateCompressor(args);
    if (!compressor.ok()) return core::make_unexpected(compressor.error());

    std::shared_ptr<ZFileRO> ro(new ZFileRO());
    ro->file_ = file;
    ro->ht_ = ht;
    ro->jump_table_ = jt;
    ro->compressor_ = compressor.value();
    ro->valid_mode_ = kValidNormal;
    ro->data_verify_ = ht.opt.verify != 0;
    return ro;
}

core::Expected<std::shared_ptr<VirtualFile>, std::string> ZFileOpenRoVfile(
    std::shared_ptr<VirtualFile> file, bool verify) {
    const core::Expected<std::shared_ptr<ZFileRO>, std::string> ro = ZFileOpenRo(file, verify);
    if (!ro.ok()) return core::make_unexpected(ro.error());
    return std::shared_ptr<VirtualFile>(ro.value());
}

core::Expected<int, std::string> IsZFile(VirtualFile* file) {
    if (file == 0) return core::make_unexpected(std::string("is_zfile: null file"));

    std::vector<uint8_t> buf(kHeaderTrailerSpace, 0);
    const core::Expected<core::Unit, std::string> got =
        ReadExactAt(file, &buf[0], buf.size(), 0);
    if (!got.ok()) return core::make_unexpected(got.error());

    const core::Expected<HeaderTrailer, std::string> ht =
        HeaderTrailer::DecodeFrom512(&buf[0], buf.size());
    if (!ht.ok()) return core::make_unexpected(ht.error());

    // Not a zfile at all -- a plain answer, not a failure.
    if (!ht.value().VerifyMagic() || !ht.value().IsHeader()) return 0;

    // But a file that *claims* to be a zfile and fails its own digest is an
    // error: falling back to treating it as plain data would be the wrong
    // recovery for what is almost certainly corruption.
    if (!ht.value().IsValid(&buf[0], buf.size())) {
        return core::make_unexpected(std::string("file is zfile but header digest is invalid"));
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Encode side
//
// Rust `write_header_trailer` .. `zfile_compress`. These live in one named
// namespace rather than the file's anonymous one because `ZFileBuilder::Impl`
// has members of these types and is itself externally linked; an
// internal-linkage member type there is exactly what -Wsubobject-linkage
// warns about.
// ---------------------------------------------------------------------------

namespace detail {

/// Rust `BUF_SIZE` — the slack the per-block output buffer carries on top of
/// one logical block, covering codec expansion on incompressible input plus
/// the optional CRC trailer. Numerically equal to `kHeaderTrailerSpace`, and
/// unrelated to it: upstream keeps them as two constants for that reason.
const size_t kBufSize = 512;

/// Rust `ZFILE_PHYSICAL_WRITE_CHUNK_SIZE`.
const size_t kZFilePhysicalWriteChunkSize = 4096;

/// Rust `write_all_at` — a short write is an error.
///
/// `VirtualFile::WriteAt` is permitted to be short; at this level it is not,
/// because every caller is laying down a block, an index or a trailer whose
/// length the format depends on.
core::Expected<core::Unit, std::string> WriteAllAtFile(VirtualFile* file, const uint8_t* buf,
                                                          size_t len, uint64_t offset) {
    if (len == 0) return core::Unit();
    if (offset > UINT64_MAX - static_cast<uint64_t>(len)) {
        return core::make_unexpected(std::string("offset overflow"));
    }
    size_t done = 0;
    while (done < len) {
        const core::Expected<size_t, std::string> n =
            file->WriteAt(offset + done, buf + done, len - done);
        if (!n.ok()) return core::make_unexpected(n.error());
        if (n.value() == 0) return core::make_unexpected(std::string("short write"));
        done += n.value();
    }
    return core::Unit();
}

/// Rust `write_header_trailer`.
///
/// The flag pairs are written both ways round (`SetHeader`/`SetTrailer` and
/// so on) because one `HeaderTrailer` is reused for both records: the trailer
/// `Finish` writes is the same struct the header came from, so leaving the
/// header bit set would produce a file with two headers and no trailer.
///
/// The digest is computed over the encoded slot with its own four bytes
/// blanked, which is the only order that works — the digest cannot cover
/// itself — and is the same convention `IsValid` checks.
core::Expected<core::Unit, std::string> WriteHeaderTrailer(VirtualFile* file, bool is_header,
                                                              bool is_sealed, bool is_data_file,
                                                              bool overwrite_header,
                                                              HeaderTrailer* ht,
                                                              uint64_t offset) {
    if (is_header) {
        ht->SetHeader();
    } else {
        ht->SetTrailer();
    }
    if (is_sealed) {
        ht->SetSealed();
    } else {
        ht->ClrSealed();
    }
    if (is_data_file) {
        ht->SetDataFile();
    } else {
        ht->SetIndexFile();
    }
    if (overwrite_header) ht->SetHeaderOverwrite();

    ht->SetDigestEnable();
    ht->digest = 0;
    uint8_t encoded[kHeaderTrailerSpace];
    const core::Expected<core::Unit, std::string> enc = ht->Encode512(encoded, sizeof(encoded));
    if (!enc.ok()) return core::make_unexpected(enc.error());
    for (size_t i = 28; i < 32; ++i) encoded[i] = 0;
    ht->digest = Crc32c(encoded, kHeaderTrailerSpace);
    WriteU32Le(encoded, 28, ht->digest);

    return WriteAllAtFile(file, encoded, kHeaderTrailerSpace, offset);
}

/// Rust `compress_data` — encode one block, appending the CRC when asked.
///
/// The CRC is salted and taken over the *compressed* bytes, and it is counted
/// into the returned length: the jump table measures encoded blocks including
/// their trailer, so a length excluding it would make every later block
/// unreadable.
core::Expected<size_t, std::string> CompressData(Compressor* compressor, const uint8_t* src,
                                                   size_t src_len, uint8_t* dst, size_t dst_cap,
                                                   bool gen_crc) {
    const core::Expected<size_t, std::string> encoded =
        compressor->Compress(src, src_len, dst, dst_cap);
    if (!encoded.ok()) return core::make_unexpected(encoded.error());
    size_t compressed_len = encoded.value();
    if (compressed_len == 0) return core::make_unexpected(std::string("compress returned zero"));

    if (gen_crc) {
        if (compressed_len > SIZE_MAX - sizeof(uint32_t)) {
            return core::make_unexpected(std::string("crc length overflow"));
        }
        const size_t end = compressed_len + sizeof(uint32_t);
        if (end > dst_cap) {
            return core::make_unexpected(
                std::string("destination buffer cannot hold crc32 trailer"));
        }
        const uint32_t crc = Crc32cSalt(dst, compressed_len);
        WriteU32Le(dst, compressed_len, crc);
        compressed_len = end;
    }
    return compressed_len;
}

/// Rust `CompressedSegment` — a run of encoded blocks plus their lengths.
struct CompressedSegment {
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> block_lengths;
};

/// Rust `max_compressed_block_size`.
core::Expected<size_t, std::string> MaxCompressedBlockSize(const Compressor* compressor,
                                                              bool gen_crc) {
    const size_t base = compressor->MaxDstSize();
    const size_t extra = gen_crc ? sizeof(uint32_t) : 0;
    if (base > SIZE_MAX - extra) {
        return core::make_unexpected(std::string("buffer size overflow"));
    }
    return base + extra;
}

/// Rust `compress_segment`.
///
/// Encodes into worst-case slots and then truncates: the blocks are written
/// back to back in the output, so each one has to be compressed into the
/// position the previous one actually ended at, not into a fixed stride.
core::Expected<CompressedSegment, std::string> CompressSegment(
    Compressor* compressor, const uint8_t* source, size_t source_len, size_t block_size,
    size_t max_compressed_block_size, bool gen_crc) {
    if (block_size == 0) return core::make_unexpected(std::string("block_size must be > 0"));
    if (source_len % block_size != 0) {
        return core::make_unexpected(std::string("source must contain a whole number of blocks"));
    }

    const size_t block_count = source_len / block_size;
    CompressedSegment out;
    if (block_count == 0) return std::move(out);
    if (block_count > SIZE_MAX / max_compressed_block_size) {
        return core::make_unexpected(std::string("compressed segment size overflow"));
    }
    out.bytes.assign(block_count * max_compressed_block_size, 0);
    out.block_lengths.reserve(block_count);

    size_t output_offset = 0;
    for (size_t i = 0; i < block_count; ++i) {
        if (output_offset > out.bytes.size() - max_compressed_block_size) {
            return core::make_unexpected(std::string("compressed segment offset overflow"));
        }
        const core::Expected<size_t, std::string> compressed_len =
            CompressData(compressor, source + i * block_size, block_size,
                         &out.bytes[output_offset], max_compressed_block_size, gen_crc);
        if (!compressed_len.ok()) return core::make_unexpected(compressed_len.error());
        if (compressed_len.value() > 0xFFFFFFFFu) {
            return core::make_unexpected(
                std::string("compressed_len cannot fit in u32 jump table entry"));
        }
        out.block_lengths.push_back(static_cast<uint32_t>(compressed_len.value()));
        output_offset += compressed_len.value();
    }
    out.bytes.resize(output_offset);
    return std::move(out);
}

/// Rust's `crossbeam_channel::bounded` pair, reduced to what the pool needs.
///
/// Bounded so submission backpressures rather than queueing unbounded segment
/// buffers. `Close` stands in for crossbeam's "all senders dropped": after it,
/// `Send` fails and `Recv` fails *once drained*, which is what lets a worker
/// loop finish its queue and then exit instead of being cut off mid-batch.
template <typename T>
class BoundedChannel {
 public:
    explicit BoundedChannel(size_t capacity)
        : capacity_(capacity == 0 ? 1 : capacity), closed_(false) {}

    bool Send(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!closed_ && queue_.size() >= capacity_) not_full_.wait(lock);
        if (closed_) return false;
        queue_.push_back(std::move(item));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    bool Recv(T* out) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.empty() && !closed_) not_empty_.wait(lock);
        if (queue_.empty()) return false;
        *out = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

 private:
    BoundedChannel(const BoundedChannel&);
    BoundedChannel& operator=(const BoundedChannel&);

    std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<T> queue_;
    size_t capacity_;
    bool closed_;
};

/// Rust `WorkItem` — one segment of full blocks, tagged with its submission
/// order so out-of-order completions can be put back in source order.
struct WorkItem {
    size_t seq;
    std::vector<uint8_t> source;
    WorkItem() : seq(0) {}
};

/// Rust `CompressedBatch`.
struct CompressedBatch {
    size_t seq;
    bool ok;
    CompressedSegment segment;
    std::string error;
    CompressedBatch() : seq(0), ok(false) {}
};

/// Rust `WorkerPool` — persistent compression workers.
///
/// Each worker owns one compressor, because a zstd context costs more to
/// create than a small block costs to compress, and loops on a shared bounded
/// queue returning exactly one `CompressedBatch` per `WorkItem`. That
/// one-for-one guarantee is the pool's contract: the consumer drains by
/// count, so a work item that went unanswered would leave it waiting forever.
class WorkerPool {
 public:
    /// Rust `MAX_WORKERS`. Bounds both the thread count and the channel
    /// capacity arithmetic below it.
    static const size_t kMaxWorkers = 256;

    WorkerPool(size_t workers, const CompressOptions& opt, bool gen_crc, size_t block_size,
               size_t max_compressed_block_size)
        : work_(Clamp(workers) * 2), results_(Clamp(workers) * 2), fail_work_(false) {
        const size_t n = Clamp(workers);
        threads_.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            threads_.push_back(std::thread(&WorkerPool::WorkerMain, this, opt, gen_crc,
                                           block_size, max_compressed_block_size));
        }
    }

    ~WorkerPool() {
        // Rust drops its real sender in favour of a disconnected one so every
        // worker's `recv()` fails and the joins below cannot hang. Closing the
        // queue says the same thing directly.
        work_.Close();
        for (size_t i = 0; i < threads_.size(); ++i) threads_[i].join();
        results_.Close();
    }

    size_t WorkerCount() const { return threads_.size(); }
    bool Submit(WorkItem item) { return work_.Send(std::move(item)); }
    bool Recv(CompressedBatch* out) { return results_.Recv(out); }
    void InjectFailure() { fail_work_.store(true); }

 private:
    WorkerPool(const WorkerPool&);
    WorkerPool& operator=(const WorkerPool&);

    static size_t Clamp(size_t workers) {
        if (workers < 1) return 1;
        if (workers > kMaxWorkers) return kMaxWorkers;
        return workers;
    }

    void WorkerMain(CompressOptions opt, bool gen_crc, size_t block_size,
                    size_t max_compressed_block_size) {
        const core::Expected<std::shared_ptr<Compressor>, std::string> compressor =
            CreateCompressor(CompressArgs(opt));
        if (!compressor.ok()) {
            // Construction is config-driven and `ZFileBuilder::Create` has
            // already validated it; if it still fails, answer *every* item
            // with that failure so the one-result-per-item protocol holds
            // until the pool shuts down.
            const std::string message =
                "zfile worker compressor init failed: " + compressor.error();
            WorkItem item;
            while (work_.Recv(&item)) {
                CompressedBatch batch;
                batch.seq = item.seq;
                batch.ok = false;
                batch.error = message;
                if (!results_.Send(std::move(batch))) break;
            }
            return;
        }

        WorkItem item;
        while (work_.Recv(&item)) {
            CompressedBatch batch;
            batch.seq = item.seq;
            // Rust turns a panicking compression into this seq's Err for one
            // reason, and the same reason applies to an exception here: the
            // consumer drains by count, so an item that never produced a
            // result would hang it. Letting the exception escape a thread
            // would additionally terminate the process.
            try {
                if (fail_work_.load()) {
                    batch.ok = false;
                    batch.error = "injected zfile worker failure";
                } else {
                    core::Expected<CompressedSegment, std::string> segment = CompressSegment(
                        compressor.value().get(), item.source.empty() ? NULL : &item.source[0],
                        item.source.size(), block_size, max_compressed_block_size, gen_crc);
                    if (segment.ok()) {
                        batch.ok = true;
                        batch.segment = segment.take_value();
                    } else {
                        batch.ok = false;
                        batch.error = segment.error();
                    }
                }
            } catch (const std::exception& e) {
                batch.ok = false;
                batch.error = std::string("zfile worker threw during compression: ") + e.what();
            } catch (...) {
                batch.ok = false;
                batch.error = "zfile worker threw during compression";
            }
            if (!results_.Send(std::move(batch))) break;
        }
    }

    BoundedChannel<WorkItem> work_;
    BoundedChannel<CompressedBatch> results_;
    std::vector<std::thread> threads_;
    std::atomic<bool> fail_work_;
};

/// Rust `write_batch_size` — how much of one `Write` call is handed to the
/// block path at a time, rounded down to whole blocks and never zero.
core::Expected<size_t, std::string> WriteBatchSize(size_t block_size) {
    if (block_size == 0) return core::make_unexpected(std::string("block_size must be > 0"));
    size_t blocks = kZFileCompactWriterBufferSize / block_size;
    if (blocks < 1) blocks = 1;
    if (blocks > SIZE_MAX / block_size) {
        return core::make_unexpected(std::string("write batch size overflow"));
    }
    return blocks * block_size;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// ZFileBuilder
// ---------------------------------------------------------------------------

class ZFileBuilder::Impl {
 public:
    Impl() : reserved_size(0), raw_data_size(0), moffset(0), finished(false) {}

    core::Expected<core::Unit, std::string> WriteBuffer(const uint8_t* src, size_t len);
    core::Expected<core::Unit, std::string> CommitCompressedBatch(
        detail::CompressedSegment batch);
    core::Expected<core::Unit, std::string> CompressAndCommit(const uint8_t* source, size_t len,
                                                                 size_t block_size);
    core::Expected<core::Unit, std::string> WriteFullBlocks(const uint8_t* source, size_t len);
    core::Expected<size_t, std::string> Write(const uint8_t* buf, size_t len);
    core::Expected<size_t, std::string> WriteWholeBlocks(const uint8_t* buf, size_t len);
    core::Expected<core::Unit, std::string> Finish();

    std::shared_ptr<VirtualFile> dest;
    CompressArgs args;
    CompressOptions opt;
    std::shared_ptr<Compressor> compressor;
    /// Rust's `Option<WorkerPool>`: absent means multi-block writes fall back
    /// to the in-line compressor. Created only when `workers > 1`, and
    /// released on a pool failure.
    std::unique_ptr<detail::WorkerPool> pool;
    std::vector<uint32_t> block_len;
    std::vector<uint8_t> compressed_data;
    std::vector<uint8_t> reserved_buf;
    size_t reserved_size;
    uint64_t raw_data_size;
    uint64_t moffset;
    HeaderTrailer ht;
    bool finished;

 private:
    Impl(const Impl&);
    Impl& operator=(const Impl&);
};

core::Expected<core::Unit, std::string> ZFileBuilder::Impl::WriteBuffer(const uint8_t* src,
                                                                           size_t len) {
    const core::Expected<size_t, std::string> compressed_len =
        detail::CompressData(compressor.get(), src, len, &compressed_data[0],
                             compressed_data.size(), opt.verify != 0);
    if (!compressed_len.ok()) return core::make_unexpected(compressed_len.error());

    const core::Expected<core::Unit, std::string> w = detail::WriteAllAtFile(
        dest.get(), &compressed_data[0], compressed_len.value(), moffset);
    if (!w.ok()) return core::make_unexpected(w.error());

    if (compressed_len.value() > 0xFFFFFFFFu) {
        return core::make_unexpected(
            std::string("compressed_len cannot fit in u32 jump table entry"));
    }
    block_len.push_back(static_cast<uint32_t>(compressed_len.value()));
    if (static_cast<uint64_t>(compressed_len.value()) > UINT64_MAX - moffset) {
        return core::make_unexpected(std::string("moffset overflow"));
    }
    moffset += compressed_len.value();
    return core::Unit();
}

core::Expected<core::Unit, std::string> ZFileBuilder::Impl::CommitCompressedBatch(
    detail::CompressedSegment batch) {
    // The declared lengths must account for every byte in the batch. A
    // mismatch here would be written out as valid data with a jump table that
    // disagrees with it, which no reader can detect.
    size_t encoded_size = 0;
    for (size_t i = 0; i < batch.block_lengths.size(); ++i) {
        const uint32_t block_length = batch.block_lengths[i];
        if (block_length == 0) {
            return core::make_unexpected(std::string("compressed block length cannot be zero"));
        }
        if (static_cast<size_t>(block_length) > SIZE_MAX - encoded_size) {
            return core::make_unexpected(std::string("compressed batch length overflow"));
        }
        encoded_size += block_length;
    }
    if (encoded_size != batch.bytes.size()) {
        return core::make_unexpected(
            std::string("compressed block lengths do not match batch size"));
    }

    if (static_cast<uint64_t>(batch.bytes.size()) > UINT64_MAX - moffset) {
        return core::make_unexpected(std::string("moffset overflow"));
    }
    const uint64_t next_offset = moffset + batch.bytes.size();

    // Rust takes one synchronous pwrite for large batches bound for a local
    // file, bypassing io_uring's per-4 KiB submit/completion round trips. The
    // branch is kept because the condition it tests still holds: the builder
    // owns the destination exclusively and the bytes land in local page
    // cache, so a single blocked syscall is the cheaper path. Non-local
    // destinations take the ordinary write path.
    LocalFile* local = NULL;
    if (batch.bytes.size() > detail::kZFilePhysicalWriteChunkSize) {
        local = dynamic_cast<LocalFile*>(dest.get());
    }
    if (local != NULL) {
        const core::Expected<size_t, std::string> written =
            local->WriteAtSyncPwrite(moffset, &batch.bytes[0], batch.bytes.size());
        if (!written.ok()) return core::make_unexpected(written.error());
        if (written.value() != batch.bytes.size()) {
            return core::make_unexpected(std::string("short sync pwrite"));
        }
    } else if (!batch.bytes.empty()) {
        const core::Expected<core::Unit, std::string> w = detail::WriteAllAtFile(
            dest.get(), &batch.bytes[0], batch.bytes.size(), moffset);
        if (!w.ok()) return core::make_unexpected(w.error());
    }

    block_len.insert(block_len.end(), batch.block_lengths.begin(), batch.block_lengths.end());
    moffset = next_offset;
    return core::Unit();
}

core::Expected<core::Unit, std::string> ZFileBuilder::Impl::CompressAndCommit(
    const uint8_t* source, size_t len, size_t block_size) {
    const core::Expected<size_t, std::string> max_block_size =
        detail::MaxCompressedBlockSize(compressor.get(), opt.verify != 0);
    if (!max_block_size.ok()) return core::make_unexpected(max_block_size.error());

    core::Expected<detail::CompressedSegment, std::string> batch =
        detail::CompressSegment(compressor.get(), source, len, block_size,
                                max_block_size.value(), opt.verify != 0);
    if (!batch.ok()) return core::make_unexpected(batch.error());
    return CommitCompressedBatch(batch.take_value());
}

core::Expected<core::Unit, std::string> ZFileBuilder::Impl::WriteFullBlocks(
    const uint8_t* source, size_t len) {
    // Rust splits this into `write_full_blocks_borrowed` and
    // `write_full_blocks_owned`; the owned one only ever *borrows* `source`
    // (its fan-out copies each segment separately), so the sole difference is
    // the `to_vec()` the borrowed one performs before delegating. A pointer
    // and a length express "borrowed" here, which leaves the two bodies
    // identical — so there is one, rather than a second copy that is
    // permanently equivalent to the first.
    const size_t block_size = static_cast<size_t>(opt.block_size);
    if (block_size == 0) return core::make_unexpected(std::string("block_size must be > 0"));
    if (len % block_size != 0) {
        return core::make_unexpected(std::string("source must contain a whole number of blocks"));
    }
    if (len == 0) return core::Unit();

    const size_t block_count = len / block_size;
    if (args.workers <= 1 || block_count == 1 || !pool) {
        return CompressAndCommit(source, len, block_size);
    }

    // Persistent-pool fan-out: split into per-worker segments, submit them
    // all, then collect every result before committing in `seq` order.
    const size_t worker_count = pool->WorkerCount();
    const size_t workers = worker_count < block_count ? worker_count : block_count;
    const size_t segment_blocks = (block_count + workers - 1) / workers;

    std::map<size_t, detail::CompressedSegment> segments;
    size_t pending = 0;
    bool have_error = false;
    std::string first_error;

    for (size_t begin_block = 0; begin_block < block_count; begin_block += segment_blocks) {
        size_t end_block = begin_block + segment_blocks;
        if (end_block > block_count) end_block = block_count;
        detail::WorkItem item;
        item.seq = pending;
        item.source.assign(source + begin_block * block_size, source + end_block * block_size);
        if (!pool->Submit(std::move(item))) {
            have_error = true;
            first_error = "zfile worker pool is shut down";
            break;
        }
        ++pending;
    }

    // Drain exactly the submitted results, even after an error, so the pool
    // stays clean for the next round. A failed `Recv` means every worker is
    // gone, in which case no further result can arrive and the drain stops.
    while (pending > 0) {
        --pending;
        detail::CompressedBatch batch;
        if (!pool->Recv(&batch)) {
            if (!have_error) {
                have_error = true;
                first_error = "zfile worker pool result channel closed";
            }
            break;
        }
        if (batch.ok) {
            segments[batch.seq] = std::move(batch.segment);
        } else if (!have_error) {
            have_error = true;
            first_error = batch.error;
        }
    }

    if (have_error) {
        // Nothing was committed. Drop the pool so a later write falls back to
        // the in-line compressor rather than risking stale results from a
        // half-drained queue.
        pool.reset();
        return core::make_unexpected(first_error);
    }

    for (std::map<size_t, detail::CompressedSegment>::iterator it = segments.begin();
         it != segments.end(); ++it) {
        const core::Expected<core::Unit, std::string> c =
            CommitCompressedBatch(std::move(it->second));
        if (!c.ok()) return core::make_unexpected(c.error());
    }
    return core::Unit();
}

core::Expected<size_t, std::string> ZFileBuilder::Impl::Write(const uint8_t* buf, size_t len) {
    if (finished) return core::make_unexpected(std::string("builder already closed"));
    if (static_cast<uint64_t>(len) > UINT64_MAX - raw_data_size) {
        return core::make_unexpected(std::string("raw_data_size overflow"));
    }
    const uint64_t next_raw_data_size = raw_data_size + len;

    const size_t expected = len;
    const size_t block_size = static_cast<size_t>(opt.block_size);
    if (block_size == 0) return core::make_unexpected(std::string("block_size must be > 0"));
    const core::Expected<size_t, std::string> batch_size = detail::WriteBatchSize(block_size);
    if (!batch_size.ok()) return core::make_unexpected(batch_size.error());

    if (reserved_size != 0) {
        const size_t needed = block_size - reserved_size;
        if (len < needed) {
            // Still short of a block: hold it and report the bytes as taken.
            // `Finish` is what eventually flushes a tail this short.
            if (len != 0) std::memcpy(&reserved_buf[reserved_size], buf, len);
            reserved_size += len;
            raw_data_size = next_raw_data_size;
            return expected;
        }

        std::memcpy(&reserved_buf[reserved_size], buf, needed);
        // Copied out before the block path runs, as upstream does: that path
        // can hand the buffer to the worker pool, and `reserved_buf` is the
        // builder's own scratch, reused by the next call.
        std::vector<uint8_t> completed_block(reserved_buf.begin(),
                                             reserved_buf.begin() + block_size);
        const core::Expected<core::Unit, std::string> w =
            WriteFullBlocks(&completed_block[0], block_size);
        if (!w.ok()) return core::make_unexpected(w.error());
        reserved_size = 0;
        buf += needed;
        len -= needed;
    }

    while (len >= block_size) {
        const size_t full_bytes = (len / block_size) * block_size;
        const size_t batch_bytes =
            full_bytes < batch_size.value() ? full_bytes : batch_size.value();
        const core::Expected<core::Unit, std::string> w = WriteFullBlocks(buf, batch_bytes);
        if (!w.ok()) return core::make_unexpected(w.error());
        buf += batch_bytes;
        len -= batch_bytes;
    }

    if (len != 0) {
        std::memcpy(&reserved_buf[0], buf, len);
        reserved_size = len;
    }

    raw_data_size = next_raw_data_size;
    return expected;
}

core::Expected<size_t, std::string> ZFileBuilder::Impl::WriteWholeBlocks(const uint8_t* buf,
                                                                           size_t len) {
    if (finished) return core::make_unexpected(std::string("builder already closed"));
    const size_t block_size = static_cast<size_t>(opt.block_size);
    if (block_size == 0) return core::make_unexpected(std::string("block_size must be > 0"));

    // A pending tail or a partial block cannot take the whole-block path:
    // the tail has to be completed in order first.
    if (reserved_size != 0 || len % block_size != 0) return Write(buf, len);

    if (static_cast<uint64_t>(len) > UINT64_MAX - raw_data_size) {
        return core::make_unexpected(std::string("raw_data_size overflow"));
    }
    const uint64_t next_raw_data_size = raw_data_size + len;

    const core::Expected<core::Unit, std::string> w = WriteFullBlocks(buf, len);
    if (!w.ok()) return core::make_unexpected(w.error());

    raw_data_size = next_raw_data_size;
    return len;
}

core::Expected<core::Unit, std::string> ZFileBuilder::Impl::Finish() {
    if (finished) return core::Unit();

    if (reserved_size != 0) {
        // The final block is short, and goes through the single-block path:
        // `WriteFullBlocks` would reject it for not being a whole block.
        std::vector<uint8_t> chunk(reserved_buf.begin(), reserved_buf.begin() + reserved_size);
        const core::Expected<core::Unit, std::string> w = WriteBuffer(&chunk[0], chunk.size());
        if (!w.ok()) return core::make_unexpected(w.error());
        reserved_size = 0;
    }

    const uint64_t index_offset = moffset;
    const uint64_t index_size = static_cast<uint64_t>(block_len.size());
    std::vector<uint8_t> index_raw(block_len.size() * sizeof(uint32_t), 0);
    for (size_t i = 0; i < block_len.size(); ++i) {
        WriteU32Le(index_raw.data(), i * sizeof(uint32_t), block_len[i]);
    }

    const core::Expected<core::Unit, std::string> iw =
        detail::WriteAllAtFile(dest.get(), index_raw.data(), index_raw.size(), index_offset);
    if (!iw.ok()) return core::make_unexpected(iw.error());

    ht.index_crc = Crc32c(index_raw.data(), index_raw.size());
    ht.index_offset = index_offset;
    ht.index_size = index_size;
    ht.original_file_size = raw_data_size;

    if (static_cast<uint64_t>(index_raw.size()) > UINT64_MAX - index_offset) {
        return core::make_unexpected(std::string("trailer offset overflow"));
    }
    const uint64_t trailer_offset = index_offset + index_raw.size();
    const core::Expected<core::Unit, std::string> tw = detail::WriteHeaderTrailer(
        dest.get(), false, true, true, false, &ht, trailer_offset);
    if (!tw.ok()) return core::make_unexpected(tw.error());

    if (args.overwrite_header) {
        // Rewrites the header now that the index is known, flagged so a
        // reader can tell it apart from the placeholder written at Create.
        const core::Expected<core::Unit, std::string> hw =
            detail::WriteHeaderTrailer(dest.get(), true, false, true, true, &ht, 0);
        if (!hw.ok()) return core::make_unexpected(hw.error());
    }

    finished = true;
    return core::Unit();
}

core::Expected<ZFileBuilder, std::string> ZFileBuilder::Create(std::shared_ptr<VirtualFile> file,
                                                                  const CompressArgs& args) {
    if (!file) return core::make_unexpected(std::string("zfile builder: null destination"));

    std::unique_ptr<Impl> impl(new Impl);
    impl->dest = file;
    impl->args = args;
    impl->opt = args.opt;
    impl->ht.SetCompressOption(args.opt);

    // The header goes down before anything is validated, as upstream does it,
    // so the destination is an (unindexed) zfile from here on: a crash leaves
    // a file the reader refuses rather than one it mistakes for raw data.
    const core::Expected<core::Unit, std::string> header =
        detail::WriteHeaderTrailer(file.get(), true, false, true, false, &impl->ht, 0);
    if (!header.ok()) return core::make_unexpected(header.error());

    const size_t block_size = static_cast<size_t>(args.opt.block_size);
    if (block_size == 0) return core::make_unexpected(std::string("block_size must be > 0"));
    if (block_size > SIZE_MAX - detail::kBufSize) {
        return core::make_unexpected(std::string("buffer size overflow"));
    }
    const size_t buf_size = block_size + detail::kBufSize;

    const core::Expected<std::shared_ptr<Compressor>, std::string> compressor =
        CreateCompressor(args);
    if (!compressor.ok()) return core::make_unexpected(compressor.error());
    impl->compressor = compressor.value();

    if (args.workers > 1) {
        const bool gen_crc = args.opt.verify != 0;
        const core::Expected<size_t, std::string> max_cbs =
            detail::MaxCompressedBlockSize(impl->compressor.get(), gen_crc);
        if (!max_cbs.ok()) return core::make_unexpected(max_cbs.error());
        impl->pool.reset(new detail::WorkerPool(args.workers, args.opt, gen_crc, block_size,
                                                max_cbs.value()));
    }

    impl->compressed_data.assign(buf_size, 0);
    impl->reserved_buf.assign(buf_size, 0);
    impl->moffset =
        static_cast<uint64_t>(kHeaderTrailerSpace) + static_cast<uint64_t>(args.opt.dict_size);

    return ZFileBuilder(impl.release());
}

ZFileBuilder& ZFileBuilder::operator=(ZFileBuilder&& other) {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = NULL;
    }
    return *this;
}

ZFileBuilder::~ZFileBuilder() { delete impl_; }

namespace {

const char* kMovedFromBuilder = "zfile builder was moved from";

}  // namespace

core::Expected<size_t, std::string> ZFileBuilder::Write(const uint8_t* buf, size_t len) {
    if (impl_ == NULL) return core::make_unexpected(std::string(kMovedFromBuilder));
    return impl_->Write(buf, len);
}

core::Expected<size_t, std::string> ZFileBuilder::WriteWholeBlocks(const uint8_t* buf,
                                                                      size_t len) {
    if (impl_ == NULL) return core::make_unexpected(std::string(kMovedFromBuilder));
    return impl_->WriteWholeBlocks(buf, len);
}

core::Expected<core::Unit, std::string> ZFileBuilder::Finish() {
    if (impl_ == NULL) return core::make_unexpected(std::string(kMovedFromBuilder));
    return impl_->Finish();
}

core::Expected<std::shared_ptr<VirtualFile>, std::string> ZFileBuilder::Close() {
    if (impl_ == NULL) return core::make_unexpected(std::string(kMovedFromBuilder));
    const core::Expected<core::Unit, std::string> f = impl_->Finish();
    if (!f.ok()) return core::make_unexpected(f.error());
    // Rust's `close(mut self)` consumes the builder; this one cannot, since
    // the caller holds it by value. `Finish` is idempotent, so a `Close`
    // followed by the destructor is well defined either way.
    return impl_->dest;
}

void ZFileBuilder::InjectWorkerFailureForTest() {
    if (impl_ != NULL && impl_->pool) impl_->pool->InjectFailure();
}

core::Expected<ZFileBuilder, std::string> NewZFileBuilder(std::shared_ptr<VirtualFile> file,
                                                             const CompressArgs& args) {
    return ZFileBuilder::Create(file, args);
}

// ---------------------------------------------------------------------------
// ZFileCompactWriter
// ---------------------------------------------------------------------------

namespace {

const char* kWriterFinalized = "zfile compact writer already finalized";
const char* kWriterPoisoned = "zfile compact writer failed after previous write error";
const char* kWriterOffsetOverflow = "zfile compact writer logical offset overflow";

std::string WrongOffset(uint64_t got, uint64_t want) {
    std::ostringstream o;
    o << "zfile compact writer received offset " << got << ", expected " << want;
    return o.str();
}

}  // namespace

core::Expected<std::shared_ptr<ZFileCompactWriter>, std::string> ZFileCompactWriter::Create(
    std::shared_ptr<VirtualFile> destination, const CompressArgs& args) {
    core::Expected<ZFileBuilder, std::string> builder =
        ZFileBuilder::Create(destination, args);
    if (!builder.ok()) return core::make_unexpected(builder.error());
    return std::shared_ptr<ZFileCompactWriter>(
        new ZFileCompactWriter(builder.take_value()));
}

core::Expected<util::CompactBuffer, std::string> ZFileCompactWriter::AllocBuffer() {
    return util::CompactBuffer(kZFileCompactWriterBufferSize, 0);
}

core::Expected<core::Unit, std::string> ZFileCompactWriter::WriteSliceAt(const uint8_t* data,
                                                                           size_t len,
                                                                           uint64_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finalized_) return core::make_unexpected(std::string(kWriterFinalized));
    if (failed_) return core::make_unexpected(std::string(kWriterPoisoned));
    if (offset != next_offset_) {
        return core::make_unexpected(WrongOffset(offset, next_offset_));
    }
    if (static_cast<uint64_t>(len) > UINT64_MAX - offset) {
        return core::make_unexpected(std::string(kWriterOffsetOverflow));
    }
    const uint64_t next_offset = offset + len;

    // Poisoned before the call and cleared only after success, so a failure
    // part-way through a block cannot leave the writer looking usable.
    failed_ = true;
    const core::Expected<size_t, std::string> written = builder_.Write(data, len);
    if (!written.ok()) return core::make_unexpected(written.error());
    if (written.value() != len) {
        return core::make_unexpected(std::string("zfile builder performed a short write"));
    }
    next_offset_ = next_offset;
    failed_ = false;
    return core::Unit();
}

core::Expected<core::Unit, std::string> ZFileCompactWriter::Write(const util::CompactBuffer& buf,
                                                                     uint64_t offset,
                                                                     size_t len) {
    // Rust's `ensure!(len <= buffer.len())` lives in `write_owned`; it has to
    // be here instead, because the whole-block entry point takes a pointer
    // and cannot see the buffer's length.
    if (len > buf.size()) return core::make_unexpected(std::string("write length exceeds buffer"));

    std::lock_guard<std::mutex> lock(mutex_);
    if (finalized_) return core::make_unexpected(std::string(kWriterFinalized));
    if (failed_) return core::make_unexpected(std::string(kWriterPoisoned));
    if (offset != next_offset_) {
        return core::make_unexpected(WrongOffset(offset, next_offset_));
    }
    if (static_cast<uint64_t>(len) > UINT64_MAX - offset) {
        return core::make_unexpected(std::string(kWriterOffsetOverflow));
    }
    const uint64_t next_offset = offset + len;

    failed_ = true;
    const core::Expected<size_t, std::string> written =
        builder_.WriteWholeBlocks(len == 0 ? NULL : &buf[0], len);
    if (!written.ok()) return core::make_unexpected(written.error());
    if (written.value() != len) {
        return core::make_unexpected(std::string("zfile builder performed a short write"));
    }
    next_offset_ = next_offset;
    failed_ = false;
    return core::Unit();
}

core::Expected<core::Unit, std::string> ZFileCompactWriter::WriteAllAt(const uint8_t* data,
                                                                         size_t len,
                                                                         uint64_t offset) {
    // An empty write still goes through the checked path rather than
    // returning early: the offset assertion is the point, so a caller that
    // skips ahead with a zero-length write is told, not accommodated.
    if (len == 0) return WriteSliceAt(data, 0, offset);

    uint64_t at = offset;
    size_t done = 0;
    while (done < len) {
        const size_t remain = len - done;
        const size_t chunk =
            remain < kZFileCompactWriterBufferSize ? remain : kZFileCompactWriterBufferSize;
        const core::Expected<core::Unit, std::string> w = WriteSliceAt(data + done, chunk, at);
        if (!w.ok()) return core::make_unexpected(w.error());
        if (static_cast<uint64_t>(chunk) > UINT64_MAX - at) {
            return core::make_unexpected(std::string(kWriterOffsetOverflow));
        }
        at += chunk;
        done += chunk;
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> ZFileCompactWriter::Finalize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finalized_) return core::Unit();
    if (failed_) return core::make_unexpected(std::string(kWriterPoisoned));
    failed_ = true;
    const core::Expected<core::Unit, std::string> f = builder_.Finish();
    if (!f.ok()) return core::make_unexpected(f.error());
    finalized_ = true;
    failed_ = false;
    return core::Unit();
}

// ---------------------------------------------------------------------------
// ZFileCompress
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> ZFileCompress(std::shared_ptr<VirtualFile> src,
                                                         std::shared_ptr<VirtualFile> dst,
                                                         const CompressArgs& args) {
    if (!src || !dst) return core::make_unexpected(std::string("zfile compress: null file"));

    const CompressOptions opt = args.opt;
    const core::Expected<std::shared_ptr<Compressor>, std::string> compressor_handle =
        CreateCompressor(args);
    if (!compressor_handle.ok()) return core::make_unexpected(compressor_handle.error());
    Compressor* compressor = compressor_handle.value().get();

    HeaderTrailer ht;
    ht.SetCompressOption(opt);
    const core::Expected<core::Unit, std::string> header =
        detail::WriteHeaderTrailer(dst.get(), true, false, true, false, &ht, 0);
    if (!header.ok()) return core::make_unexpected(header.error());

    const size_t block_size = static_cast<size_t>(opt.block_size);
    if (block_size == 0) return core::make_unexpected(std::string("block_size must be > 0"));
    if (block_size > SIZE_MAX - detail::kBufSize) {
        return core::make_unexpected(std::string("buffer size overflow"));
    }
    const size_t buf_size = block_size + detail::kBufSize;

    size_t nbatch = compressor->Nbatch();
    if (nbatch < 1) nbatch = 1;

    std::vector<uint32_t> block_len;
    uint64_t moffset =
        static_cast<uint64_t>(kHeaderTrailerSpace) + static_cast<uint64_t>(opt.dict_size);
    uint64_t infile_size = 0;

    const core::Expected<uint64_t, std::string> src_size = src->Size();
    if (!src_size.ok()) return core::make_unexpected(src_size.error());
    uint64_t src_offset = 0;

    std::vector<uint8_t> raw_data(block_size * nbatch, 0);
    std::vector<uint8_t> compressed_data(buf_size * nbatch, 0);

    while (src_offset < src_size.value()) {
        const uint64_t remain64 = src_size.value() - src_offset;
        const size_t to_read = remain64 < static_cast<uint64_t>(raw_data.size())
                                   ? static_cast<size_t>(remain64)
                                   : raw_data.size();
        size_t readn = 0;
        while (readn < to_read) {
            const core::Expected<ByteBuffer, std::string> chunk =
                src->ReadAt(src_offset + readn, to_read - readn);
            if (!chunk.ok()) return core::make_unexpected(chunk.error());
            if (chunk.value().empty()) break;
            std::memcpy(&raw_data[readn], &chunk.value()[0], chunk.value().size());
            readn += chunk.value().size();
        }
        // A source that reports a size but stops delivering is not an error
        // here: the trailer records what was actually read.
        if (readn == 0) break;
        src_offset += readn;
        infile_size += readn;

        std::vector<size_t> raw_chunk_len;
        size_t remain = readn;
        while (remain > 0) {
            const size_t chunk = remain < block_size ? remain : block_size;
            raw_chunk_len.push_back(chunk);
            remain -= chunk;
        }
        const size_t n = raw_chunk_len.size();

        std::vector<size_t> compressed_len(n, 0);
        const core::Expected<core::Unit, std::string> batched =
            compressor->CompressBatch(&raw_data[0], readn, raw_chunk_len, &compressed_data[0],
                                      n * buf_size, &compressed_len, n);
        if (!batched.ok()) return core::make_unexpected(batched.error());

        for (size_t j = 0; j < n; ++j) {
            const size_t compressed = compressed_len[j];
            const size_t begin = j * buf_size;
            const core::Expected<core::Unit, std::string> w = detail::WriteAllAtFile(
                dst.get(), &compressed_data[begin], compressed, moffset);
            if (!w.ok()) return core::make_unexpected(w.error());
            size_t written = compressed;

            if (opt.verify != 0) {
                // Written as a separate call at the block's end rather than
                // into the batch buffer: the slots are strided, so the CRC
                // would land in the next block's slot, not after this one.
                const uint32_t crc = Crc32cSalt(&compressed_data[begin], compressed);
                uint8_t crc_bytes[4];
                WriteU32Le(crc_bytes, 0, crc);
                if (static_cast<uint64_t>(written) > UINT64_MAX - moffset) {
                    return core::make_unexpected(std::string("crc offset overflow"));
                }
                const core::Expected<core::Unit, std::string> cw = detail::WriteAllAtFile(
                    dst.get(), crc_bytes, sizeof(crc_bytes), moffset + written);
                if (!cw.ok()) return core::make_unexpected(cw.error());
                written += sizeof(uint32_t);
            }

            if (written > 0xFFFFFFFFu) {
                return core::make_unexpected(
                    std::string("compressed length cannot fit in u32"));
            }
            block_len.push_back(static_cast<uint32_t>(written));
            if (static_cast<uint64_t>(written) > UINT64_MAX - moffset) {
                return core::make_unexpected(std::string("moffset overflow"));
            }
            moffset += written;
        }
    }

    const uint64_t index_offset = moffset;
    const uint64_t index_size = static_cast<uint64_t>(block_len.size());
    std::vector<uint8_t> index_raw(block_len.size() * sizeof(uint32_t), 0);
    for (size_t i = 0; i < block_len.size(); ++i) {
        WriteU32Le(index_raw.data(), i * sizeof(uint32_t), block_len[i]);
    }

    const core::Expected<core::Unit, std::string> iw =
        detail::WriteAllAtFile(dst.get(), index_raw.data(), index_raw.size(), index_offset);
    if (!iw.ok()) return core::make_unexpected(iw.error());

    ht.index_crc = Crc32c(index_raw.data(), index_raw.size());
    ht.index_offset = index_offset;
    ht.index_size = index_size;
    ht.original_file_size = infile_size;

    if (static_cast<uint64_t>(index_raw.size()) > UINT64_MAX - index_offset) {
        return core::make_unexpected(std::string("trailer offset overflow"));
    }
    const uint64_t trailer_offset = index_offset + index_raw.size();
    const core::Expected<core::Unit, std::string> tw =
        detail::WriteHeaderTrailer(dst.get(), false, true, true, false, &ht, trailer_offset);
    if (!tw.ok()) return core::make_unexpected(tw.error());

    if (args.overwrite_header) {
        const core::Expected<core::Unit, std::string> hw =
            detail::WriteHeaderTrailer(dst.get(), true, false, true, true, &ht, 0);
        if (!hw.ok()) return core::make_unexpected(hw.error());
    }

    return core::Unit();
}

core::Expected<core::Unit, std::string> ZFileDecompress(std::shared_ptr<VirtualFile> src,
                                                           std::shared_ptr<VirtualFile> dst) {
    if (!dst) return core::make_unexpected(std::string("zfile decompress: null destination"));

    const core::Expected<std::shared_ptr<ZFileRO>, std::string> ro = ZFileOpenRo(src, false);
    if (!ro.ok()) return core::make_unexpected(ro.error());

    const uint64_t total = ro.value()->OriginalSize();
    // One logical block size per iteration rather than the whole file: the
    // output can be far larger than memory.
    const size_t step = static_cast<size_t>(kMaxReadSize);
    std::vector<uint8_t> buf(step, 0);

    uint64_t done = 0;
    while (done < total) {
        const uint64_t remain = total - done;
        const size_t want = remain < static_cast<uint64_t>(step)
                                ? static_cast<size_t>(remain)
                                : step;
        const core::Expected<size_t, std::string> n = ro.value()->Pread(&buf[0], want, done);
        if (!n.ok()) return core::make_unexpected(n.error());
        if (n.value() == 0) {
            return core::make_unexpected(std::string("zfile decompress: unexpected short read"));
        }

        size_t put = 0;
        while (put < n.value()) {
            const core::Expected<size_t, std::string> w =
                dst->WriteAt(done + put, &buf[put], n.value() - put);
            if (!w.ok()) return core::make_unexpected(w.error());
            if (w.value() == 0) {
                return core::make_unexpected(std::string("zfile decompress: short write"));
            }
            put += w.value();
        }
        done += n.value();
    }

    return core::Unit();
}

core::Expected<core::Unit, std::string> ZFileValidationCheck(std::shared_ptr<VirtualFile> src) {
    const core::Expected<std::shared_ptr<ZFileRO>, std::string> ro = ZFileOpenRo(src, false);
    if (!ro.ok()) return core::make_unexpected(ro.error());

    // CRC-only: this walks the whole file, and decoding it as well would
    // double the cost for no extra coverage of what is being checked.
    ro.value()->SetCrcCheckOnly();

    const uint64_t total = ro.value()->OriginalSize();
    const size_t step = static_cast<size_t>(kMaxReadSize);
    std::vector<uint8_t> buf(step, 0);

    uint64_t done = 0;
    while (done < total) {
        const uint64_t remain = total - done;
        const size_t want = remain < static_cast<uint64_t>(step)
                                ? static_cast<size_t>(remain)
                                : step;
        const core::Expected<size_t, std::string> n = ro.value()->Pread(&buf[0], want, done);
        if (!n.ok()) return core::make_unexpected(n.error());
        // In CRC-only mode nothing is copied out, so `Pread` reports the span
        // it covered rather than bytes written; a zero would mean no progress.
        if (n.value() == 0) {
            return core::make_unexpected(std::string("zfile validation: no progress"));
        }
        done += n.value();
    }

    return core::Unit();
}

}  // namespace zfile

// ---------------------------------------------------------------------------
// Pre-existing minimal surface
// ---------------------------------------------------------------------------

core::Expected<int64_t, std::string>
DecodeBlock(CompressionAlgo algo, const uint8_t* src, int64_t src_len,
   uint8_t* dst, int64_t dst_cap) {
    if (algo == CompressionAlgo::None) {
     if (src_len > dst_cap) {
            return core::make_unexpected(std::string("dst too small"));
        }
    std::memcpy(dst, src, static_cast<size_t>(src_len));
        return src_len;
  }
#ifdef AGENTENV_WITH_ZSTD
    if (algo == CompressionAlgo::Lz4) {
        const int n = ::LZ4_decompress_safe(reinterpret_cast<const char*>(src),
                                            reinterpret_cast<char*>(dst),
                                            static_cast<int>(src_len),
                                            static_cast<int>(dst_cap));
        if (n <= 0) return core::make_unexpected(std::string("lz4 decompress failed"));
        return static_cast<int64_t>(n);
    }
    const size_t n = ::ZSTD_decompress(dst, static_cast<size_t>(dst_cap), src,
                                       static_cast<size_t>(src_len));
    if (::ZSTD_isError(n)) {
        return core::make_unexpected(std::string("zstd decompress failed: ") +
                                     ::ZSTD_getErrorName(n));
    }
    return static_cast<int64_t>(n);
#else
    return core::make_unexpected(std::string("compression not compiled in (AGENTENV_WITH_ZSTD)"));
#endif
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
