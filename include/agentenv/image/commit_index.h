// SPDX-License-Identifier: MIT
// Rust: src/image/commit_index.rs
//
// The overlaybd commit cache index: a small JSON sidecar recording how a cached
// overlaybd commit blob was produced, plus the content-addressed on-disk layout
// helpers (digest slug -> commit dir/file) and the seed helpers that copy or
// hard-link a source commit into the cache while verifying size (and, for the
// untrusted path, the SHA-256 digest).
//
// This is pure logic (filesystem + SHA-256 + JSON), so it is a full port. The
// async `read`/`write` in Rust become synchronous here; every fallible call
// returns `core::Expected<T, std::string>` mirroring `anyhow::Result`.
#ifndef AGENTENV_IMAGE_COMMIT_INDEX_H_
#define AGENTENV_IMAGE_COMMIT_INDEX_H_

#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace image {

/// Rust `OVERLAYBD_COMMIT_FILE`.
extern const char* const kOverlaybdCommitFile;

/// Rust `CommitIndex` — the sidecar describing a cached overlaybd commit.
struct CommitIndex {
    uint32_t     schema = 0;
    std::string  source_kind;
  std::string  source_digest;
    std::string  commit_digest;
    uint64_t     size = 0;
    std::string  converter;
    uint64_t     virtual_size_gib = 0;
    bool         mkfs = false;
    core::Optional<std::string> parent_commit_digest;

    /// Rust `CommitIndex::oci_layer`.
    static CommitIndex OciLayer(std::string source_digest,
        std::string commit_digest,
      uint64_t size,
    std::string converter,
 uint64_t virtual_size_gib,
         bool mkfs,
            core::Optional<std::string> parent_commit_digest);

  /// Rust `CommitIndex::matches_oci_context`.
    bool MatchesOciContext(const std::string& source_digest,
      const std::string& converter,
           uint64_t virtual_size_gib,
      bool mkfs,
              const core::Optional<std::string>& parent_commit_digest) const;

    /// Rust `CommitIndex::read` — returns None when the file is absent, an error
  /// when it exists but cannot be read/parsed.
    static core::Expected<core::Optional<CommitIndex>, std::string> Read(
        const std::string& path);

 /// Rust `CommitIndex::write` — atomically writes pretty JSON via a temp file.
    core::Expected<core::Unit, std::string> Write(const std::string& path) const;

    /// Serialize to the camelCase pretty JSON that `write` persists.
    std::string ToJsonPretty() const;

    /// Parse from the JSON that `read` accepts.
    static core::Expected<CommitIndex, std::string> FromJson(const std::string& json);

    bool operator==(const CommitIndex& o) const;
    bool operator!=(const CommitIndex& o) const { return !(*this == o); }
};

/// Rust `digest_slug` — "<algo>-<hex>" sanitized and length-bounded.
std::string DigestSlug(const std::string& digest);

/// Rust `commit_dir` — commit_store/<slug>.
std::string CommitDir(const std::string& commit_store, const std::string& digest);

/// Rust `commit_file` — commit_store/<slug>/overlaybd.commit.
std::string CommitFile(const std::string& commit_store, const std::string& digest);

/// Rust `cached_commit_file_if_usable` — Some(path) iff a same-size regular file
/// already exists at the commit-cache location.
core::Optional<std::string> CachedCommitFileIfUsable(
    const std::string& commit_store, const std::string& digest, uint64_t size);

/// Rust `accept_existing_commit_file` — validate an already-present cache file.
core::Expected<std::string, std::string> AcceptExistingCommitFile(
    const std::string& destination, const std::string& digest, uint64_t size);

/// Rust `seed_commit_file` — copy `source` into the cache, verifying size and
/// SHA-256 digest against `digest`.
core::Expected<std::string, std::string> SeedCommitFile(
    const std::string& commit_store, const std::string& source,
    const std::string& digest, uint64_t size);

/// Rust `seed_commit_file_trusted_descriptor` — hard-link when possible, else
/// copy, verifying only size (descriptor is trusted).
core::Expected<std::string, std::string> SeedCommitFileTrustedDescriptor(
    const std::string& commit_store, const std::string& source,
    const std::string& digest, uint64_t size);

/// Rust `index_path` — index_dir/<source_slug>/sha256-<ctx_hex>.json.
std::string IndexPath(const std::string& index_dir,
       const std::string& source_digest,
        const std::string& converter,
    uint64_t virtual_size_gib,
        bool mkfs,
   const core::Optional<std::string>& parent_commit_digest);

/// Rust `sanitize_filename_component` — keep [A-Za-z0-9-_.], map the rest to '-',
/// truncate to `max_len`.
std::string SanitizeFilenameComponent(const std::string& value, std::size_t max_len);

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_COMMIT_INDEX_H_
