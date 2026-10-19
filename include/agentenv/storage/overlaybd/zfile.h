// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/compression/zfile.rs — ZFile compressed blocks.
#ifndef AGENTENV_STORAGE_OVERLAYBD_ZFILE_H_
#define AGENTENV_STORAGE_OVERLAYBD_ZFILE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace zfile {

/// Rust `MAX_READ_SIZE` — the largest *logical* block a zfile may declare.
const size_t kMaxReadSize = 65536;

/// Rust `HEADER_TRAILER_SPACE` — the slot each of the header and trailer
/// occupies. The struct itself is smaller (`kHeaderTrailerBodySize`); the rest
/// is padding so both land on a 512-byte boundary.
const size_t kHeaderTrailerSpace = 512;
const size_t kHeaderTrailerBodySize = 96;

/// Rust `NOI_WELL_KNOWN_PRIME` — the seed the per-block CRC is salted with.
///
/// Upstream OverlayBD seeds the running CRC state with this prime directly
/// rather than feeding it as data, which is why block CRCs do not match a
/// plain crc32c of the same bytes.
const uint32_t kNoiWellKnownPrime = 100007;

/// Rust `crc32c` — Castagnoli CRC-32, standard initial/final complement.
uint32_t Crc32c(const uint8_t* data, size_t len);

/// Rust `crc32c_salt` — the same polynomial chained from the salted raw state.
uint32_t Crc32cSalt(const uint8_t* data, size_t len);

/// Rust `crc32c_append` — continue a CRC-32C from a previous result.
///
/// `Crc32cAppend(0, d, n) == Crc32c(d, n)`, and chaining chunk by chunk gives
/// the same value as one call over the concatenation. Exposed because the
/// prefetch trace accumulates its checksum record by record while encoding
/// them, rather than making a second pass over the assembled payload.
uint32_t Crc32cAppend(uint32_t crc, const uint8_t* data, size_t len);

/// Rust `CompressOptions`.
struct CompressOptions {
    static const uint8_t kLz4 = 1;
    static const uint8_t kZstd = 2;
    static const uint32_t kDefaultBlockSize = 4096;

    uint32_t block_size;
    uint8_t algo;
    uint8_t level;
    uint8_t use_dict;
    uint32_t reserved;
    uint32_t dict_size;
    uint8_t verify;

    CompressOptions()
        : block_size(kDefaultBlockSize),
          algo(kLz4),
          level(0),
          use_dict(0),
          reserved(0),
          dict_size(0),
          verify(0) {}

    /// Rust `decode_from` — reads the 24-byte options block.
    ///
    /// The layout is sparse on purpose: `algo`/`level`/`use_dict` are single
    /// bytes at 4/5/6, byte 7 is a hole, and `verify` sits at 16. Fields are
    /// read at their fixed offsets rather than packed sequentially.
    static core::Expected<CompressOptions, std::string> DecodeFrom(const uint8_t* buf,
                                                                      size_t len);
    /// Rust `encode_into`.
    core::Expected<core::Unit, std::string> EncodeInto(uint8_t* buf, size_t len) const;
};

/// Rust `CompressArgs`.
struct CompressArgs {
    CompressOptions opt;
    bool overwrite_header;
    size_t workers;

    CompressArgs() : overwrite_header(false), workers(1) {}
    explicit CompressArgs(const CompressOptions& o)
        : opt(o), overwrite_header(false), workers(1) {}
};

/// Rust `trait Compressor`, decode half only.
///
/// The encode half (`compress` / `compress_batch`) is not ported: nothing in
/// this codebase writes zfiles, and the write path additionally needs the
/// worker pool and builder that go with it.
///
/// `Decompress` is const because one instance decodes concurrently; keep
/// implementations stateless, with scratch on the stack or caller-provided.
class Compressor {
 public:
    virtual ~Compressor() {}
    virtual size_t MaxDstSize() const = 0;
    virtual size_t SrcBlockSize() const = 0;
    virtual core::Expected<size_t, std::string> Decompress(const uint8_t* src, size_t src_len,
                                                              uint8_t* dst,
                                                              size_t dst_cap) const = 0;
};

/// Rust `create_compressor` — LZ4 or ZSTD, nothing else.
///
/// An unknown algorithm is refused rather than defaulted: guessing would
/// decode the file into plausible-looking garbage.
core::Expected<std::shared_ptr<Compressor>, std::string> CreateCompressor(
    const CompressArgs& args);

/// Rust `HeaderTrailer` — the 96-byte record at both ends of a zfile.
struct HeaderTrailer {
    static const uint32_t kFlagShiftHeader = 0;
    static const uint32_t kFlagShiftType = 1;
    static const uint32_t kFlagShiftSealed = 2;
    static const uint32_t kFlagShiftHeaderOverwrite = 3;
    static const uint32_t kFlagShiftCalcDigest = 4;

    uint64_t magic0;
    uint8_t magic1[16];
    uint32_t size;
    uint32_t digest;
    uint64_t flags;
    uint64_t index_offset;
    uint64_t index_size;
    uint64_t original_file_size;
    uint32_t index_crc;
    uint32_t reserved_0;
    CompressOptions opt;

    HeaderTrailer();

    static core::Expected<HeaderTrailer, std::string> DecodeFrom512(const uint8_t* buf,
                                                                       size_t len);
    core::Expected<core::Unit, std::string> Encode512(uint8_t* out, size_t cap) const;

    bool VerifyMagic() const;
    bool GetFlagBit(uint32_t shift) const { return (flags & (1ULL << shift)) != 0; }
    void SetFlagBit(uint32_t shift) { flags |= 1ULL << shift; }
    void ClrFlagBit(uint32_t shift) { flags &= ~(1ULL << shift); }

    bool IsHeader() const { return GetFlagBit(kFlagShiftHeader); }
    bool IsTrailer() const { return !IsHeader(); }
    bool IsHeaderOverwrite() const { return GetFlagBit(kFlagShiftHeaderOverwrite); }
    bool IsDataFile() const { return GetFlagBit(kFlagShiftType); }
    bool IsSealed() const { return GetFlagBit(kFlagShiftSealed); }
    bool IsDigestEnabled() const { return GetFlagBit(kFlagShiftCalcDigest); }

    /// Rust `is_valid` — recompute the digest over the 512-byte slot with the
    /// digest field itself zeroed.
    ///
    /// Trivially true when digests are disabled: an absent digest is not a
    /// failed one.
    bool IsValid(const uint8_t* bytes, size_t len) const;
};

/// Rust `JumpTable` — block index to encoded file offset.
///
/// Stored as a sparse base array plus per-block `u16` deltas rather than one
/// `u64` per block: a group covers at most 64 KiB of encoded output, so the
/// delta always fits 16 bits and the table costs a quarter of the space.
class JumpTable {
 public:
    JumpTable() : group_size_(0) {}

    /// Rust `offset_at`. Valid for `idx` in `[0, Deltas().size())`, which is
    /// one past the last block — that entry is the end offset callers need to
    /// size the final block.
    core::Expected<uint64_t, std::string> OffsetAt(size_t idx) const;

    /// Rust `build`.
    ///
    /// `enable_crc` raises the minimum legal encoded block size to 4: with
    /// verification on, every block carries a CRC trailer, so a block that
    /// small holds no payload at all.
    core::Expected<core::Unit, std::string> Build(const std::vector<uint32_t>& ibuf,
                                                     uint64_t offset_begin, uint32_t block_size,
                                                     bool enable_crc);

    size_t GroupSize() const { return group_size_; }
    const std::vector<uint16_t>& Deltas() const { return deltas_; }

 private:
    size_t group_size_;
    std::vector<uint64_t> partial_offset_;
    std::vector<uint16_t> deltas_;
};

/// Rust `ValidMode`.
enum ValidMode {
    /// Verify the CRC and decode.
    kValidNormal,
    /// Verify the CRC only — used to scan a file for corruption without
    /// paying for decompression.
    kValidCrcOnly
};

/// Rust `ZFileRO` — a read-only view of a compressed file.
///
/// Presents the *decompressed* bytes: offsets and sizes a caller passes are
/// logical, and the block lookup, backing read and decode happen underneath.
class ZFileRO : public VirtualFile {
 public:
    /// Rust `original_size`.
    uint64_t OriginalSize() const { return ht_.original_file_size; }
    /// Rust `options`.
    CompressOptions Options() const { return ht_.opt; }

    /// Rust `compressed_block_extent` — where the block covering
    /// `logical_offset` lives in the backing file.
    ///
    /// Metadata only: reads and decodes nothing. Used by the publish path to
    /// plan transfers without materialising the content.
    core::Expected<core::Unit, std::string> CompressedBlockExtent(uint64_t logical_offset,
                                                                     uint64_t* offset,
                                                                     uint64_t* len) const;

    /// Rust `set_crc_check_only`.
    void SetCrcCheckOnly() { valid_mode_ = kValidCrcOnly; }

    /// Rust `pread` — read decompressed bytes into `buf`.
    ///
    /// Returns a short count at EOF rather than erroring: the logical size is
    /// whatever the trailer declared, and a read crossing it is clamped.
    core::Expected<size_t, std::string> Pread(uint8_t* buf, size_t len, uint64_t offset) const;

    // -- VirtualFile -------------------------------------------------------

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override;
    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst,
                                                     size_t len) override;
    /// Always refused — a zfile is sealed.
    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                  size_t len) override;
    /// The *decompressed* size, not the size of the backing file.
    core::Expected<uint64_t, std::string> Size() override;

 private:
    friend core::Expected<std::shared_ptr<ZFileRO>, std::string> ZFileOpenRo(
        std::shared_ptr<VirtualFile>, bool);

    ZFileRO() : valid_mode_(kValidNormal), data_verify_(false) {}
    ZFileRO(const ZFileRO&);
    ZFileRO& operator=(const ZFileRO&);

    /// Rust `plan_coalesced_batch` — how many consecutive blocks to fetch in
    /// one backing read.
    core::Expected<size_t, std::string> PlanCoalescedBatch(size_t begin_idx,
                                                              size_t end_idx) const;

    std::shared_ptr<VirtualFile> file_;
    HeaderTrailer ht_;
    JumpTable jump_table_;
    std::shared_ptr<Compressor> compressor_;
    ValidMode valid_mode_;
    bool data_verify_;
};

/// Rust `zfile_open_ro`.
///
/// `verify` enables a bounded retry around the metadata load: on failure the
/// backing file's cache is evicted and the header/trailer re-read, which
/// recovers from a torn cached page without failing the open.
core::Expected<std::shared_ptr<ZFileRO>, std::string> ZFileOpenRo(
    std::shared_ptr<VirtualFile> file, bool verify);

/// Rust `zfile_open_ro_vfile` — the same reader as a plain `VirtualFile`.
core::Expected<std::shared_ptr<VirtualFile>, std::string> ZFileOpenRoVfile(
    std::shared_ptr<VirtualFile> file, bool verify);

/// Rust `is_zfile` — 1 if the file carries a zfile header, 0 if not.
///
/// A readable header with a bad digest is an *error*, not a 0: the file claims
/// to be a zfile, so treating it as a plain file would be the wrong recovery.
core::Expected<int, std::string> IsZFile(VirtualFile* file);

/// Rust `zfile_decompress` — write `src`'s decompressed contents to `dst`.
core::Expected<core::Unit, std::string> ZFileDecompress(std::shared_ptr<VirtualFile> src,
                                                           std::shared_ptr<VirtualFile> dst);

/// Rust `zfile_validation_check` — verify every block's CRC without decoding.
core::Expected<core::Unit, std::string> ZFileValidationCheck(std::shared_ptr<VirtualFile> src);

}  // namespace zfile

// ---------------------------------------------------------------------------
// Pre-existing minimal surface, kept for callers that only decode one block.
// ---------------------------------------------------------------------------

/// Rust enum: the compression algorithm of a ZFile block.
enum class CompressionAlgo {
    None,
    Lz4,
    Zstd,
};

/// Rust struct `ZFileHeader`.
struct ZFileHeader {
    uint64_t    magic = 0;
    uint32_t  block_size = 0;
    CompressionAlgo algo = CompressionAlgo::None;
    uint64_t        raw_size = 0;
};

/// Rust: decode one compressed block into `dst`.
core::Expected<int64_t, std::string>
  DecodeBlock(CompressionAlgo algo,
      const uint8_t* src, int64_t src_len,
         uint8_t* dst, int64_t dst_cap);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_ZFILE_H_
