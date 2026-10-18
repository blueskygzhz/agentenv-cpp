// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/tar.rs — the detection half.
#include "agentenv/storage/overlaybd/tar.h"

#include <cstdlib>
#include <cstring>
#include <sstream>

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

// Rust `TMAGIC` / `TVERSION`.
const char* const kTarMagic = "ustar";
const char* const kTarVersion = "00";

const size_t kBlock = static_cast<size_t>(kTarBlockSize);
// Rust's `148..156` — the checksum field.
const size_t kCrcOffset = 148;
const size_t kCrcLength = 8;

}  // namespace

core::Optional<uint64_t> OctToU64(const uint8_t* field, size_t len) {
    if (field == 0) return core::Optional<uint64_t>();

    // Rust `take_while(|b| *b != 0)`: the field is NUL-terminated within its
    // fixed width, and anything after the first NUL is padding.
    size_t end = 0;
    while (end < len && field[end] != 0) ++end;

    // Then trimmed, because tar pads numeric fields with spaces as well.
    size_t start = 0;
    while (start < end && (field[start] == ' ' || field[start] == '\t')) ++start;
    while (end > start && (field[end - 1] == ' ' || field[end - 1] == '\t')) --end;

    // An empty field is 0, which is what tar writes for an unused numeric
    // field -- not a parse failure.
    if (start == end) return core::Optional<uint64_t>(static_cast<uint64_t>(0));

    uint64_t value = 0;
    for (size_t i = start; i < end; ++i) {
        const uint8_t c = field[i];
        if (c < '0' || c > '7') {
            // No value rather than a partial parse. A corrupt size field must
            // not read back as whatever prefix happened to be octal, because
            // the caller would then skip the wrong number of bytes.
            return core::Optional<uint64_t>();
        }
        if (value > (~static_cast<uint64_t>(0)) >> 3) return core::Optional<uint64_t>();
        value = (value << 3) | static_cast<uint64_t>(c - '0');
    }
    return core::Optional<uint64_t>(value);
}

core::Expected<TarHeader, std::string> TarHeader::FromBlock(const uint8_t* block, size_t len) {
    if (block == 0 || len != kBlock) {
        std::ostringstream oss;
        oss << "short tar header: got " << len << ", expected " << kBlock;
        return core::make_unexpected(oss.str());
    }
    TarHeader header;
    std::memcpy(header.raw_, block, kBlock);
    return header;
}

bool TarHeader::MagicMatches() const {
    return std::memcmp(raw_ + 257, kTarMagic, 5) == 0;
}

bool TarHeader::VersionMatches() const {
    return std::memcmp(raw_ + 263, kTarVersion, 2) == 0;
}

uint64_t TarHeader::Size() const {
    const core::Optional<uint64_t> v = OctToU64(raw_ + 124, 12);
    // Rust `unwrap_or(0)`.
    return v.has_value() ? *v : 0;
}

uint64_t TarHeader::Crc() const {
    const core::Optional<uint64_t> v = OctToU64(raw_ + kCrcOffset, kCrcLength);
    return v.has_value() ? *v : 0;
}

uint64_t TarHeader::CrcCalcUnsigned() const {
    uint64_t sum = 0;
    for (size_t i = 0; i < kBlock; ++i) sum += static_cast<uint64_t>(raw_[i]);
    // The checksum field is treated as spaces: it cannot contribute to the
    // sum it stores, so each of its bytes is swapped for ' '.
    for (size_t i = kCrcOffset; i < kCrcOffset + kCrcLength; ++i) {
        sum += static_cast<uint64_t>(' ');
        sum -= static_cast<uint64_t>(raw_[i]);
    }
    return sum;
}

int64_t TarHeader::CrcCalcSigned() const {
    int64_t sum = 0;
    for (size_t i = 0; i < kBlock; ++i) {
        sum += static_cast<int64_t>(static_cast<int8_t>(raw_[i]));
    }
    for (size_t i = kCrcOffset; i < kCrcOffset + kCrcLength; ++i) {
        sum += static_cast<int64_t>(static_cast<int8_t>(' '));
        sum -= static_cast<int64_t>(static_cast<int8_t>(raw_[i]));
    }
    return sum;
}

bool TarHeader::CrcOk() const {
    const uint64_t expect = Crc();
    if (expect == CrcCalcUnsigned()) return true;
    // Historic implementations disagreed on whether the header bytes were
    // signed. Accepting both is what lets this read archives either produced.
    const int64_t signed_sum = CrcCalcSigned();
    return signed_sum >= 0 && expect == static_cast<uint64_t>(signed_sum);
}

core::Expected<bool, std::string> IsTarFile(VirtualFile* file) {
    if (file == 0) return core::make_unexpected(std::string("is_tar_file: null file"));

    const core::Expected<ByteBuffer, std::string> raw = file->ReadAt(0, kBlock);
    if (!raw.ok()) return core::make_unexpected(raw.error());

    // A short read means the file is smaller than one header block, so it
    // cannot be a tar. Reported as `false` rather than an error: a short file
    // is a legitimate non-tar input.
    if (raw.value().size() != kBlock) return false;

    const core::Expected<TarHeader, std::string> header =
        TarHeader::FromBlock(&raw.value()[0], raw.value().size());
    if (!header.ok()) return core::make_unexpected(header.error());

    // All three, because each alone is weak: the magic can appear in arbitrary
    // data, and the checksum of a block of zeros is itself consistent.
    return header.value().MagicMatches() && header.value().VersionMatches() &&
           header.value().CrcOk();
}

core::Expected<std::shared_ptr<VirtualFile>, std::string> NewTarFileAdaptor(
    std::shared_ptr<VirtualFile> file) {
    if (!file) {
        return core::make_unexpected(std::string("new_tar_file_adaptor: null file"));
    }
    const core::Expected<bool, std::string> is_tar = IsTarFile(file.get());
    if (!is_tar.ok()) return core::make_unexpected(is_tar.error());

    if (!is_tar.value()) return file;

    // Refused, not passed through. The caller is about to read this as LSMT
    // data, and an unwrapped tar would hand it the archive's 512-byte header
    // as the layer's first block -- which parses as a corrupt LSMT header at
    // best, and as a plausible-but-wrong one at worst.
    return core::make_unexpected(std::string(
        "tar-wrapped overlaybd layer is not available in this build "
        "(Rust: backend::tar::TarFile)"));
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
