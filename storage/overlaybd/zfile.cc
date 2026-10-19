// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/compression/zfile.rs
#include "agentenv/storage/overlaybd/zfile.h"

#include <cstring>
#include <sstream>

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

class Lz4Compressor : public Compressor {
 public:
    Lz4Compressor(size_t max_dst, size_t src_blk) : max_dst_(max_dst), src_blk_(src_blk) {}

    size_t MaxDstSize() const { return max_dst_; }
    size_t SrcBlockSize() const { return src_blk_; }

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

class ZstdCompressor : public Compressor {
 public:
    ZstdCompressor(size_t max_dst, size_t src_blk) : max_dst_(max_dst), src_blk_(src_blk) {}

    size_t MaxDstSize() const { return max_dst_; }
    size_t SrcBlockSize() const { return src_blk_; }

    core::Expected<size_t, std::string> Decompress(const uint8_t* src, size_t src_len,
                                                      uint8_t* dst, size_t dst_cap) const {
        if (dst_cap < src_blk_) {
            std::ostringstream o;
            o << "destination buffer too small (" << dst_cap << "), expected at least "
              << src_blk_;
            return core::make_unexpected(o.str());
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

core::Expected<size_t, std::string> ZFileRO::Pread(uint8_t* buf, size_t len,
                                                     uint64_t offset) const {
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

                if (decomp_ok) break;

                if (retry > 0) {
                    --retry;
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
