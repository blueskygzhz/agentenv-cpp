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
#include <memory>
#include <string>

#include "agentenv/cfg.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/image_export/regctl.h"
#include "agentenv/snapshot/image_export_target.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/repository/interfaces.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace oss {
// Forward declared, not included: see `OssManagedLayerLocator`.
class OssClient;
}  // namespace oss
}  // namespace repository

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

/// The scratch path one managed layer downloads to.
///
/// Rust `layer.digest.replace([':', '/'], "_")`: a digest is not a filename,
/// and leaving the `:` of `sha256:` in would produce a name some tools read as
/// a drive-letter-shaped path.
std::string ManagedLayerDownloadPath(const std::string& scratch_dir, const std::string& digest);

/// The two checks the OSS arm makes once a download has finished.
///
/// Split out of `OssManagedLayerLocator` because `OssClient` is a concrete
/// class with no virtual interface, so the arm as a whole cannot be driven
/// without a bucket. These are the only *decisions* in it — the rest is one
/// `GetToFile` call — and this way they are covered without one.
repository::RepositoryResult<std::string> FinishManagedLayerDownload(
    const ManagedLayer& layer, const std::string& destination,
    const repository::RepositoryResult<uint64_t>& downloaded);

/// The OSS arm of Rust's `ManagedLayerLocator`: the object is **downloaded**
/// into the scratch directory, because `regctl` needs a path and an object
/// store has none.
///
/// Held as a `shared_ptr` to an incomplete type on purpose — the whole reason
/// the locator is a functor is to keep `oss/client.h` out of this header, and
/// a type-erased deleter means a forward declaration is enough.
ManagedLayerLocator OssManagedLayerLocator(
    const std::shared_ptr<repository::oss::OssClient>& client);

/// The config-driven wiring of one export, owning the parts the service only
/// borrows.
///
/// This is what was missing for a rootfs export to have a call site at all:
/// `SnapshotImageService` borrows its repository, so something has to own one,
/// and choosing *which* backend to own is exactly the decision
/// `snapshot.repository_backend` exists to make. Without it the locator had to
/// be hand-wired, which meant nothing in this port could export from an OSS
/// node.
///
/// Kept separate from `SnapshotImageService` rather than folded into it so the
/// service keeps its injectable shape: the thirteen tests that drive it with a
/// hand-made locator and no config are the reason that shape is worth
/// preserving.
class ConfiguredSnapshotImageService {
 public:
    /// Rust `SnapshotImageService::from_global_config`.
    ///
    /// Takes the config explicitly where Rust reads
    /// `ConfigManager::global_config()`: this port has no such singleton, and
    /// an argument also makes the backend selection testable without touching
    /// process-wide state.
    ///
    /// **Selected by the declared `snapshot.repository_backend`, never by
    /// which `[backend.*]` section happens to be present** — see
    /// `MakeRepositoryFromConfig`, where that distinction is spelled out as
    /// the bug it fixes. A missing section for the declared backend is an
    /// error, matching Rust's `.context(..)`, because degrading to the other
    /// one is that same bug.
    ///
    /// Deliberately skips the node-runtime resolver, artifact cache, layer
    /// store and P2P machinery, as upstream does: an export reads committed
    /// records and pushes blobs, and none of those take part.
    static repository::RepositoryResult<ConfiguredSnapshotImageService> FromConfig(
        const cfg::AppConfig& config, const std::string& regctl_binary);

    /// The service, borrowing this object's repository. It must not outlive
    /// the `ConfiguredSnapshotImageService` it came from.
    SnapshotImageService Service() {
        return SnapshotImageService(*repository_, locator_, regctl_);
    }

    const std::shared_ptr<repository::SnapshotRepository>& repository() const {
        return repository_;
    }
    /// Null on the posix-fs backend.
    const std::shared_ptr<repository::oss::OssClient>& oss_client() const { return oss_client_; }

 private:
    ConfiguredSnapshotImageService(
        const std::shared_ptr<repository::SnapshotRepository>& repository,
        const std::shared_ptr<repository::oss::OssClient>& oss_client,
        const ManagedLayerLocator& locator, const Regctl& regctl)
        : repository_(repository),
          oss_client_(oss_client),
          locator_(locator),
          regctl_(regctl) {}

    std::shared_ptr<repository::SnapshotRepository> repository_;
    std::shared_ptr<repository::oss::OssClient> oss_client_;
    ManagedLayerLocator locator_;
    Regctl regctl_;
};

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
