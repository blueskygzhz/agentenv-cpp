// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/tar.rs — the detection half.
//
// An overlaybd layer may be stored inside a tar so the same blob is a valid
// OCI layer. Anything opening a layer therefore has to ask whether what it got
// is a tar before reading it as LSMT data: the first 512 bytes would otherwise
// be parsed as an LSMT header and reported as corrupt.
#ifndef AGENTENV_STORAGE_OVERLAYBD_TAR_H_
#define AGENTENV_STORAGE_OVERLAYBD_TAR_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `T_BLOCKSIZE` — one tar block, and the size of its header.
const uint64_t kTarBlockSize = 512;

/// Rust `TarHeader` — the 512-byte header block, read-only.
///
/// Only the fields the detection needs are exposed. The rest of the header
/// (name, mode, mtime, link target) is for the full reader.
class TarHeader {
 public:
    /// Rust `from_block` — refuses anything that is not exactly one block,
    /// because every offset below is a fixed position inside one.
    static core::Expected<TarHeader, std::string> FromBlock(const uint8_t* block, size_t len);

    /// Rust `magic_matches` — `ustar` at offset 257.
    bool MagicMatches() const;
    /// Rust `version_matches` — `00` at offset 263.
    bool VersionMatches() const;

    /// Rust `get_size` / `get_crc` — octal ASCII fields.
    uint64_t Size() const;
    uint64_t Crc() const;
    uint8_t TypeFlag() const { return raw_[156]; }

    /// Rust `crc_ok`.
    ///
    /// Accepts either the unsigned or the signed checksum. Historic tar
    /// implementations disagreed on whether the header bytes were signed, so a
    /// reader that insisted on one would reject archives the other wrote.
    bool CrcOk() const;

 private:
    TarHeader() {}
    /// Rust `crc_calc` / `signed_crc_calc`.
    ///
    /// Both sum every byte of the block with the checksum field itself
    /// replaced by spaces -- the field cannot contribute to the sum it stores.
    uint64_t CrcCalcUnsigned() const;
    int64_t CrcCalcSigned() const;

    uint8_t raw_[512];
};

/// Rust `oct_to_u64` — parse a NUL-or-space padded octal ASCII field.
///
/// An empty field reads as 0, which is what tar writes for an unused numeric
/// field. A field that is not valid octal yields no value rather than a
/// partial parse, so a corrupt size is never silently treated as a small one.
core::Optional<uint64_t> OctToU64(const uint8_t* field, size_t len);

/// Rust `is_tar_file`.
///
/// A file shorter than one block is not a tar. That is reported as `false`
/// rather than an error: a short file is a legitimate non-tar input, and the
/// caller's next step is to read it as whatever it actually is.
core::Expected<bool, std::string> IsTarFile(VirtualFile* file);

/// Rust `new_tar_file_adaptor` / `open_tar_file` — hand back a file that reads
/// through the tar, or the original when it is not one.
///
/// Deviation: the wrapping half (`TarFile`, which maps reads through the
/// archive's entry table) is not ported. A plain file passes straight through,
/// which is the common case for a local commit layer; an actual tar is
/// refused rather than returned unwrapped, because reading a tar as if it were
/// raw LSMT data would hand back the archive's own header bytes as layer
/// content.
core::Expected<std::shared_ptr<VirtualFile>, std::string> NewTarFileAdaptor(
    std::shared_ptr<VirtualFile> file);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_TAR_H_
