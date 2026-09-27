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

}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_DIGEST_H_
