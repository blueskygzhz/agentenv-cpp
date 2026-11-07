// SPDX-License-Identifier: MIT
// Rust: src/snapshot/recontainerize.rs
//
// Publish-time recontainerization of local overlaybd layers.
//
// Local layers always stay raw so a local resume pays no decompression cost.
// When `[snapshot.publish_compression]` is enabled, the upload paths
// recontainerize each raw local layer as zfile exactly once before upload: the
// compressed bytes are what gets uploaded, so **every content reference
// derived here describes the uploaded bytes** — the managed-layer digest and
// size, the OSS object key, the ACR blob digest, and the overlaybd blob
// annotations in the manifest. A descriptor that described the pre-compression
// bytes instead would produce a manifest the registry accepts and no puller
// can verify.
#ifndef AGENTENV_SNAPSHOT_RECONTAINERIZE_H_
#define AGENTENV_SNAPSHOT_RECONTAINERIZE_H_

#include <cstdint>
#include <memory>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/overlaybd/compact.h"
#include "agentenv/snapshot/repository/errors.h"

namespace agentenv {
namespace snapshot {

/// Rust `tempfile::NamedTempFile`, reduced to what this module needs: a path
/// that is unlinked when the last reference to it dies.
///
/// Shared rather than unique ownership, for the same reason `core::fs::
/// FileDescriptor` is: `Expected<T, E>` here is a value-semantic container
/// that copies its payload, so a move-only handle could not be returned
/// through it. The file is unlinked exactly once, when the last copy dies.
///
/// Distinct from `core::fs::TempFile`, which hands back a descriptor and a
/// path but does *not* remove the file — fine for a file that gets renamed
/// into place, wrong for one whose only job is to be deleted afterwards.
class OwnedTempPath {
 public:
    OwnedTempPath() {}
    /// Takes ownership of `path`; it will be unlinked when the last copy of
    /// this handle is destroyed. An empty path yields an empty handle.
    explicit OwnedTempPath(const std::string& path);

    bool valid() const { return holder_ && !holder_->empty(); }
    const std::string& path() const;

    /// Rust `tempfile::NamedTempFile::new` — `{TMPDIR}/{prefix}XXXXXX`,
    /// created and closed, with only the name retained.
    static core::Expected<OwnedTempPath, std::string> Create(const std::string& prefix);

 private:
    std::shared_ptr<std::string> holder_;
};

/// Rust `struct PreparedLayerUpload`.
///
/// A local layer file prepared for upload, plus the content descriptor of
/// exactly the bytes that will be uploaded. Owns the temporary
/// recontainerized copy, when one was produced, so it is removed once the
/// upload and any post-commit P2P publication have completed.
class PreparedLayerUpload {
 public:
    PreparedLayerUpload() : size_(0) {}

    /// Rust `path` — the local file holding the exact bytes to upload.
    /// Differs from the source path exactly when the layer was
    /// recontainerized.
    const std::string& Path() const { return path_; }

    /// Rust `digest` — `sha256:<hex>` of the bytes at `Path()`.
    const std::string& Digest() const { return digest_; }

    /// Rust `size` — byte size of the file at `Path()`.
    uint64_t Size() const { return size_; }

    /// Rust `retain_source`.
    ///
    /// Keeps a caller-owned temporary alive when preparation passed it through
    /// unchanged — the dense-export case, where the "source" is itself a
    /// temporary nobody else holds. **A compressed output already owns its own
    /// temporary, so an existing one is not replaced**: overwriting it would
    /// drop the last reference to the file at `Path()` and unlink the very
    /// bytes that are about to be uploaded.
    void RetainSource(const OwnedTempPath& source);

    /// The module-internal constructor. Rust builds the struct literally,
    /// which its privacy allows; C++11 has no equivalent for a type with
    /// private members and no friend, so this is public and named for what it
    /// does. `PrepareLayerUpload` is the only caller.
    static PreparedLayerUpload Make(const std::string& path, const std::string& digest,
                                    uint64_t size, const OwnedTempPath& temp);

 private:
    std::string path_;
    std::string digest_;
    uint64_t size_;
    OwnedTempPath temp_;
};

/// Rust `prepare_layer_upload`.
///
/// `trusted` carries a capture-side descriptor of `source`'s current bytes
/// when the image config provides one. **It is honoured only when the file is
/// uploaded unchanged.** A recontainerized layer no longer matches it, so the
/// result is re-hashed instead — trusting it there would publish a digest for
/// bytes that no longer exist.
///
/// All four input shapes are handled:
///
///   * `Raw` mode — passthrough, whatever the input is.
///   * `ZFile` mode, input already zfile — passthrough, so the operation is
///     idempotent and a re-publish does not recompress.
///   * `ZFile` mode, input is not a layer at all — the probe fails, and that
///     failure is returned rather than being treated as "not compressed". A
///     file claiming to be a zfile whose header digest does not check out is
///     an error for the same reason: uploading it as plain bytes would
///     publish a corrupt layer.
///   * `ZFile` mode, raw layer input — compacted into a temporary zfile via
///     `overlaybd::CompactLayers`, and the *output* is what gets described.
///
/// In that last case the trusted descriptor is deliberately **not** carried
/// across: it describes the raw source bytes, and these are the compressed
/// ones. A manifest built from it would name a blob whose content does not
/// hash to it — which no registry accepts and no puller can verify. The
/// temporary is handed to the returned `PreparedLayerUpload`, so it outlives
/// this call and is unlinked once the upload drops the last copy.
repository::RepositoryResult<PreparedLayerUpload> PrepareLayerUpload(
    const std::string& source, const overlaybd::OverlaybdCompactOutput& mode,
    const core::Optional<std::string>& trusted_digest = core::Optional<std::string>(),
    uint64_t trusted_size = 0);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_RECONTAINERIZE_H_
