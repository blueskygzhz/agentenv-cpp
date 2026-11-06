// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/regctl.rs — the `regctl` operations one
// snapshot rootfs export needs.
//
// Thin on purpose: every call is one `regctl` invocation, and the retry,
// GOMAXPROCS cap and 404 classification already live in `image::RunRegctl`.
// What this adds is the argv of each operation and, for the two that move
// bytes, which of the three transfer shapes applies.
#ifndef AGENTENV_SNAPSHOT_IMAGE_EXPORT_REGCTL_H_
#define AGENTENV_SNAPSHOT_IMAGE_EXPORT_REGCTL_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/repository/errors.h"

namespace agentenv {
namespace snapshot {
namespace image_export {

/// Rust `struct Regctl`.
class Regctl {
 public:
    explicit Regctl(const std::string& binary) : binary_(binary) {}

    const std::string& binary() const { return binary_; }

    /// Rust `manifest_head` — the digest at `image_ref`, or absent when the
    /// ref does not exist.
    ///
    /// Absent is **not** an error: "no image there yet" is the ordinary case
    /// for a first export, and it is what tells the caller to upload rather
    /// than to compare. `image::RunRegctl` already separates a 404 from a
    /// transient failure, so a registry being briefly unreachable does not
    /// get mistaken for an empty ref — which would overwrite a live image.
    core::Expected<core::Optional<std::string>, repository::RepositoryError> ManifestHead(
        const std::string& image_ref) const;

    /// Rust `blob_head` — whether `digest` already exists in `repository`.
    ///
    /// Run before every transfer. A rootfs layer stack is mostly shared
    /// between snapshots, so this is what keeps a re-export from pushing
    /// gigabytes the target already has.
    core::Expected<bool, repository::RepositoryError> BlobHead(
        const std::string& repository, const std::string& digest) const;

    /// Rust `blob_put` — uploads `path` as `digest`.
    core::Expected<core::Unit, repository::RepositoryError> BlobPut(
        const std::string& repository, const std::string& digest,
        const std::string& path) const;

    /// Rust `blob_copy` — a server-side copy **within one registry**.
    ///
    /// Cheap because the bytes never leave the registry: it is a cross-repo
    /// blob mount. Only valid when source and target registries are the same
    /// host; across registries there is nothing to mount from, which is what
    /// `BlobPipe` is for.
    core::Expected<core::Unit, repository::RepositoryError> BlobCopy(
        const std::string& source_repository, const std::string& target_repository,
        const std::string& digest) const;

    /// Rust `blob_pipe` — `blob get | blob put` across two registries.
    ///
    /// The two children are joined by an OS pipe, so the blob streams from
    /// one registry to the other **without passing through this process's
    /// memory**. A layer is routinely gigabytes; buffering one to hand it
    /// straight on would be the difference between a flat and a proportional
    /// memory cost.
    core::Expected<core::Unit, repository::RepositoryError> BlobPipe(
        const std::string& source_repository, const std::string& target_repository,
        const std::string& digest) const;

    /// Rust `manifest_put` — publishes `path` at `image_ref`.
    ///
    /// Last, always: a manifest naming blobs the registry does not have is a
    /// broken image, and a puller cannot tell that from a corrupt one. Doing
    /// this first would publish exactly that for the length of the upload.
    core::Expected<core::Unit, repository::RepositoryError> ManifestPut(
        const std::string& image_ref, const std::string& path) const;

 private:
    std::string binary_;
};

/// Rust `REGCTL_OCI_MANIFEST_CONTENT_TYPE` — passed explicitly to
/// `manifest put`.
///
/// regctl otherwise guesses from the body, and a guess of the Docker v2 media
/// type would change the bytes and therefore the digest the caller just
/// computed and compared.
extern const char* const kRegctlOciManifestContentType;

}  // namespace image_export
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_IMAGE_EXPORT_REGCTL_H_
