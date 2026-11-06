// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/service.rs — committed-snapshot rootfs
// image export.
//
// Loads a record, resolves where the image should go, and publishes the rootfs
// layer stack plus a generated OCI config as a plain OCI image through
// `regctl`. Attached drives and memory snapshots never take part: the point is
// an image another runtime can pull, and neither of those means anything
// outside this system.
#ifndef AGENTENV_SNAPSHOT_IMAGE_EXPORT_SERVICE_H_
#define AGENTENV_SNAPSHOT_IMAGE_EXPORT_SERVICE_H_

#include <functional>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/image_export/regctl.h"
#include "agentenv/snapshot/image_export_target.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/repository/interfaces.h"

namespace agentenv {
namespace snapshot {
namespace image_export {

/// Rust `struct SnapshotImageResult`.
struct SnapshotImageResult {
    std::string image_ref;
    std::string manifest_digest;
    /// True when the target already carried the identical image and nothing
    /// was transferred.
    bool reused = false;

    bool operator==(const SnapshotImageResult& o) const {
        return image_ref == o.image_ref && manifest_digest == o.manifest_digest &&
               reused == o.reused;
    }
    bool operator!=(const SnapshotImageResult& o) const { return !(*this == o); }
};

/// Puts one managed rootfs layer somewhere `regctl` can read it, returning the
/// path, and verifies its size against the record.
///
/// Injected rather than branching on a backend enum inside the service, as
/// Rust's `ManagedLayerLocator` does. The two upstream arms are genuinely
/// different operations — a POSIX layer is *validated in place* while an OSS
/// object is *downloaded* — and expressing that as a functor keeps the OSS
/// client out of this header and lets the export be tested without one.
///
/// The size check is not optional in either arm: the digest is what the
/// registry will verify the upload against, and a layer that has been
/// truncated locally would otherwise be pushed and rejected far from the
/// cause.
typedef std::function<repository::RepositoryResult<std::string>(
    const ManagedLayer& layer, const std::string& scratch_dir)>
    ManagedLayerLocator;

/// Rust `SnapshotImageService`.
class SnapshotImageService {
 public:
    SnapshotImageService(repository::SnapshotRepository& repository,
                         const ManagedLayerLocator& locator, const Regctl& regctl)
        : repository_(&repository), locator_(locator), regctl_(regctl) {}

    /// Rust `export_rootfs_image` — by snapshot id or alias.
    ///
    /// A record with no committed artifacts is refused as an invalid request
    /// rather than reported as missing: the snapshot exists, its build simply
    /// has not finished, and the caller can retry later. Collapsing the two
    /// would answer 404 for a template that is still building.
    repository::RepositoryResult<SnapshotImageResult> ExportRootfsImage(
        const std::string& id_or_alias, const core::Optional<std::string>& target_repository,
        const core::Optional<std::string>& tag);

    /// Rust `publish_committed_rootfs`.
    ///
    /// The deterministic manifest digest decides what happens: the identical
    /// digest already at the ref means `reused` and nothing moves, a different
    /// digest is refused rather than overwritten, and an absent ref uploads
    /// only the blobs the target lacks before putting the manifest.
    ///
    /// Refusing the conflict is the consequential half. The alternative —
    /// overwriting — would silently repoint a tag someone else is pulling, and
    /// because the old manifest is then unreferenced, the layers it named
    /// become garbage-collectable. A tag is cheap to choose; an overwritten
    /// one is not recoverable.
    repository::RepositoryResult<SnapshotImageResult> PublishCommittedRootfs(
        const core::SnapshotId& snapshot_id, const CommittedSnapshot& committed,
        const core::Optional<std::string>& target_repository,
        const core::Optional<std::string>& tag, const std::string& scratch_dir);

 private:
    /// Rust `ensure_layer_blob` — one rootfs layer into the target repository.
    ///
    /// The `blob head` probe runs first, so a layer the target already has is
    /// neither materialised nor transferred. That matters most for the
    /// managed arm, where materialising can mean downloading gigabytes.
    repository::RepositoryResult<core::Unit> EnsureLayerBlob(
        const SnapshotImageTarget& target, const std::string& target_repository,
        const OverlaybdLayerRef& layer, const std::string& scratch_dir);

    repository::SnapshotRepository* repository_;
    ManagedLayerLocator locator_;
    Regctl regctl_;
};

/// The POSIX arm of Rust's `ManagedLayerLocator`: the layer is validated where
/// it already lives under `root`, never copied.
ManagedLayerLocator PosixManagedLayerLocator(const std::string& root);

/// Rust's `s3://` marker on an external layer's `repo_blob_url`.
///
/// A committed snapshot records OSS-managed layers as *external* refs
/// carrying this synthetic URL, so the marker is what distinguishes "the bytes
/// are in our own managed-layer store" from "the bytes are in a registry".
/// Treating it as a registry would send `regctl` at a URL no registry serves.
extern const char* const kManagedLayerUrlScheme;

}  // namespace image_export
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_IMAGE_EXPORT_SERVICE_H_
