// SPDX-License-Identifier: MIT
// Rust: `sha2` + `digest` — a content-addressed digest wrapper.
// C++11: a self-contained SHA-256 (FIPS 180-4) backs both `Digest::Sha256` and
// the free functions below, so the port needs no crypto dependency.
#ifndef AGENTENV_CORE_DIGEST_H_
#define AGENTENV_CORE_DIGEST_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace core {

/// A SHA256 digest, kept as 32 raw bytes. Rendered as "sha256:<hex>".
class Digest {
 public:
    Digest() : bytes_{} {}
    explicit Digest(const uint8_t bytes[32]);

    /// Parse "sha256:<64 lowercase hex>".
    static bool Parse(const std::string& s, Digest* out);

    /// Format as "sha256:<64 lowercase hex>".
    std::string ToString() const;

    /// Compute over a byte buffer.
    static Digest Sha256(const void* data, std::size_t len);

    bool operator==(const Digest& o) const;
    bool operator!=(const Digest& o) const { return !(*this == o); }
    bool operator<(const Digest& o) const;

    const uint8_t* bytes() const { return bytes_.data(); }

 private:
    std::array<uint8_t, 32> bytes_;
};

/// Rust struct `FileDigest` — content identity for a local file.
///
/// `size` is counted from the *same stream* used for hashing, not from a
/// separate `stat`: a file that changes between the two calls would otherwise
/// yield a size and digest that never coexisted.
struct FileDigest {
    uint64_t size = 0;
    /// `sha256:<hex>`.
    std::string sha256;

    bool operator==(const FileDigest& o) const {
        return size == o.size && sha256 == o.sha256;
    }
    bool operator!=(const FileDigest& o) const { return !(*this == o); }
};

/// Rust `FileDigest::describe_blocking` — streams the file, so memory use is
/// bounded regardless of file size.
Expected<FileDigest, std::string> DescribeFile(const std::string& path);

// ---- Free functions mirroring Rust `src/digest.rs` ----

/// Rust `sha256_bytes` — raw 32-byte SHA-256 of in-memory bytes.
std::array<uint8_t, 32> Sha256Bytes(const void* data, std::size_t len);

/// Rust `sha256_hex` — lowercase hex SHA-256 of in-memory bytes.
std::string Sha256Hex(const void* data, std::size_t len);
std::string Sha256Hex(const std::string& data);

/// Rust `sha256_digest` — OCI-style "sha256:<hex>" of in-memory bytes.
std::string Sha256Digest(const void* data, std::size_t len);
std::string Sha256Digest(const std::string& data);

// ---- HMAC-SHA256 (Rust: the `hmac` crate over `sha2::Sha256`) -------------

/// Rust `Hmac::<Sha256>::new_from_slice(key).chain_update(data).finalize()`.
///
/// The key is accepted at any length, matching the Rust call sites that rely
/// on `new_from_slice` never failing (RFC 2104 hashes over-long keys and
/// zero-pads short ones).
std::array<uint8_t, 32> HmacSha256(const void* key, std::size_t key_len, const void* data,
                                   std::size_t data_len);

/// Lowercase hex form of `HmacSha256`.
std::string HmacSha256Hex(const std::string& key, const std::string& data);

/// Rust `Mac::verify_slice` — compares a 32-byte tag in constant time.
///
/// A byte-wise early return would leak the length of the matching prefix,
/// which is enough to recover a tag one byte at a time.
bool ConstantTimeEquals(const void* a, const void* b, std::size_t len);

/// Rust `hex::decode_to_slice` — decodes exactly `out_len` bytes, so a short,
/// long, or non-hex input is rejected rather than silently truncated.
bool HexDecodeExact(const std::string& hex, uint8_t* out, std::size_t out_len);

/// Rust `hex::encode`.
std::string HexEncode(const void* data, std::size_t len);

/// Rust `rand::rngs::SysRng::try_fill_bytes` — reads from the OS CSPRNG
/// (`getrandom(2)`, falling back to `/dev/urandom`). Never a userspace PRNG:
/// these bytes become long-lived secrets.
Expected<Unit, std::string> FillSecureRandom(uint8_t* out, std::size_t len);

}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_DIGEST_H_
