// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/service.rs
#include "agentenv/snapshot/image_export/service.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/snapshot/acr_manifest.h"
#include "agentenv/snapshot/image_export/acr.h"
#include "agentenv/snapshot/repository/blobs.h"

namespace agentenv {
namespace snapshot {
namespace image_export {

using repository::BlobKey;
using repository::LayerRole;
using repository::RepositoryError;
using repository::RepositoryResult;

const char* const kManagedLayerUrlScheme = "s3://";

namespace {

std::string ToDecimal(uint64_t value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

BlobKey RootfsLayerKey(const std::string& digest) {
    return BlobKey::Layer(digest, LayerRole::Rootfs);
}

/// Writes one `regctl` payload (the config or the manifest) into `scratch_dir`.
///
/// A file rather than a buffer because `regctl` reads its payload from stdin
/// as a descriptor; see `Regctl::BlobPut`.
RepositoryResult<std::string> WritePayload(const std::string& scratch_dir,
                                           const std::string& name,
                                           const std::string& bytes) {
    const std::string path = scratch_dir + "/" + name;
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        return core::make_unexpected(
            RepositoryError::Backend("write " + name + " payload", std::strerror(errno)));
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        return core::make_unexpected(
            RepositoryError::Backend("write " + name + " payload", "short write"));
    }
    return path;
}

std::string JoinRepository(const std::string& registry, const std::string& repository) {
    return registry + "/" + repository;
}

/// Rust's `tempfile::tempdir()` guard, which upstream creates inside
/// `publish_committed_rootfs`.
///
/// Needed as a type because `PublishCommittedRootfs` returns early in a dozen
/// places and every one of them must still remove the directory. Downloaded
/// managed layers land here, so leaking it leaks gigabytes, and the scratch
/// space is useless once the export has finished either way.
class ScratchDir {
 public:
    ScratchDir() {}

    ~ScratchDir() {
        if (path_.empty()) return;
        const core::Expected<core::Unit, std::string> removed = core::fs::RemoveDirAll(path_);
        if (!removed.ok()) {
            // Warned, not thrown: the export's own outcome is already decided
            // and must not be overwritten by a cleanup failure.
            AGENTENV_WARN("failed to remove snapshot image export scratch directory "
                          << path_ << ": " << removed.error());
        }
    }

    core::Expected<core::Unit, std::string> Create() {
        const core::Expected<std::string, std::string> created =
            core::fs::CreateTempDir("agentenv-snapshot-image-export-");
        if (!created.ok()) return core::make_unexpected(created.error());
        path_ = created.value();
        return core::Unit();
    }

    const std::string& path() const { return path_; }

 private:
    ScratchDir(const ScratchDir&);
    ScratchDir& operator=(const ScratchDir&);

    std::string path_;
};

}  // namespace

ManagedLayerLocator PosixManagedLayerLocator(const std::string& root) {
    return [root](const ManagedLayer& layer,
                  const std::string&) -> RepositoryResult<std::string> {
        const std::string path = root + "/" + RootfsLayerKey(layer.digest).PosixPath();
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) {
            // A missing layer is reported as a *managed layer* that is gone,
            // not as a backend fault: the record names it, so the store has
            // lost something it claimed to hold, and the caller cannot fix
            // that by retrying.
            if (errno == ENOENT) {
                return core::make_unexpected(RepositoryError::ManagedLayerNotFound(layer.digest));
            }
            return core::make_unexpected(RepositoryError::Backend(
                "stat managed layer '" + layer.digest + "' at " + path, std::strerror(errno)));
        }
        const uint64_t actual = static_cast<uint64_t>(st.st_size);
        if (actual != layer.size) {
            // Checked before the upload, because the registry would verify the
            // digest and reject it anyway -- but far from the cause, after the
            // bytes had been sent.
            return core::make_unexpected(RepositoryError::IntegrityMismatch(
                "managed layer '" + layer.digest + "' at " + path, ToDecimal(layer.size),
                ToDecimal(actual)));
        }
        // Returned in place, never copied: a rootfs layer is routinely
        // gigabytes, and `regctl` only needs something to read.
        return path;
    };
}

RepositoryResult<SnapshotImageResult> SnapshotImageService::ExportRootfsImage(
    const std::string& id_or_alias, const core::Optional<std::string>& target_repository,
    const core::Optional<std::string>& tag) {
    const RepositoryResult<core::Optional<SnapshotRecord> > record =
        repository_->Get(id_or_alias);
    if (!record.ok()) return core::make_unexpected(record.error());
    if (!record.value().has_value()) {
        return core::make_unexpected(RepositoryError::SnapshotNotFound(id_or_alias));
    }
    if (!record.value()->committed.has_value()) {
        // Not a 404: the snapshot is there, its build has not finished. The
        // caller can retry; being told it does not exist would send them
        // looking for a different snapshot.
        return core::make_unexpected(RepositoryError::InvalidRequest(
            "snapshot '" + id_or_alias + "' has no committed artifacts to export yet"));
    }
    // Owned here, so it is removed however the publish below ends. Upstream
    // creates it one level deeper; it is hoisted because the guard has to
    // outlive every early return inside the publish.
    ScratchDir scratch;
    const core::Expected<core::Unit, std::string> created = scratch.Create();
    if (!created.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("create snapshot image export tempdir", created.error()));
    }

    return PublishCommittedRootfs(record.value()->id, *record.value()->committed,
                                  target_repository, tag, scratch.path());
}

RepositoryResult<SnapshotImageResult> SnapshotImageService::PublishCommittedRootfs(
    const core::SnapshotId& snapshot_id, const CommittedSnapshot& committed,
    const core::Optional<std::string>& target_repository,
    const core::Optional<std::string>& tag, const std::string& scratch_dir) {
    const core::Expected<SnapshotImageTarget, SnapshotImageTargetError> target =
        ResolveSnapshotImageTarget(snapshot_id, committed, target_repository, tag);
    if (!target.ok()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(target.error().Message()));
    }
    if (committed.rootfs_layers.empty()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("committed snapshot has no rootfs layers to export"));
    }

    // Deterministic per snapshot and tag, which is what makes the digest a
    // usable identity: the same snapshot exported twice produces byte-identical
    // config and manifest, so the comparison below is meaningful.
    const OciConfigBlob config = SnapshotOciConfigBlob(
        HostArchitectureForOci(), committed.context, committed.image_configs.RootfsConfig());

    std::vector<OciDescriptor> descriptors;
    for (std::size_t i = 0; i < committed.rootfs_layers.size(); ++i) {
        descriptors.push_back(OciDescriptor::OverlaybdLayer(
            committed.rootfs_layers[i].digest(), committed.rootfs_layers[i].size()));
    }
    const std::string manifest = BuildOciImageManifest(
        OciDescriptor::Config(config.digest, config.size), descriptors, target.value().tag);
    const std::string manifest_digest = core::Sha256Digest(manifest);

    const std::string image_ref = target.value().ImageRef();
    const core::Optional<PersistedDiskImagePublication> managed =
        SnapshotManagedPublicationForTarget(committed, target.value());
    if (managed.has_value()) {
        // Warned, not refused: the export is legitimate, but this ref is one
        // the snapshot's own delete would roll back. Someone pulling it would
        // find it gone for a reason unrelated to anything they did.
        AGENTENV_WARN("target image reference is recorded as a snapshot-managed publication "
                      "and may be deleted when the snapshot is deleted: snapshot "
                      << snapshot_id.ToString() << " image_ref " << image_ref
                      << " manifest_digest " << managed->manifest_digest);
    }

    const std::string target_repo =
        JoinRepository(target.value().registry, target.value().repository);

    const core::Expected<core::Optional<std::string>, RepositoryError> existing =
        regctl_.ManifestHead(image_ref);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (existing.value().has_value()) {
        if (*existing.value() == manifest_digest) {
            // Byte-identical, so there is nothing to do. Re-uploading would be
            // correct but wasteful; reporting it as a conflict would be wrong.
            SnapshotImageResult result;
            result.image_ref = image_ref;
            result.manifest_digest = manifest_digest;
            result.reused = true;
            return result;
        }
        // Refused rather than overwritten: repointing the tag would orphan the
        // manifest already there, making the layers it names collectable, and
        // anyone pulling that tag would silently get different bytes.
        return core::make_unexpected(RepositoryError::InvalidRequest(
            "target image ref '" + image_ref + "' already points to manifest " +
            *existing.value() + ", refusing to overwrite it with " + manifest_digest +
            "; choose another tag"));
    }

    const RepositoryResult<std::string> config_path =
        WritePayload(scratch_dir, "config.json", config.bytes);
    if (!config_path.ok()) return core::make_unexpected(config_path.error());
    const RepositoryResult<std::string> manifest_path =
        WritePayload(scratch_dir, "manifest.json", manifest);
    if (!manifest_path.ok()) return core::make_unexpected(manifest_path.error());

    for (std::size_t i = 0; i < committed.rootfs_layers.size(); ++i) {
        const RepositoryResult<core::Unit> ensured = EnsureLayerBlob(
            target.value(), target_repo, committed.rootfs_layers[i], scratch_dir);
        if (!ensured.ok()) return core::make_unexpected(ensured.error());
    }

    const core::Expected<bool, RepositoryError> has_config =
        regctl_.BlobHead(target_repo, config.digest);
    if (!has_config.ok()) return core::make_unexpected(has_config.error());
    if (!has_config.value()) {
        const RepositoryResult<core::Unit> put =
            regctl_.BlobPut(target_repo, config.digest, config_path.value());
        if (!put.ok()) return core::make_unexpected(put.error());
    }

    // Last, after every blob it names is in place. A manifest published first
    // would describe an image that cannot be pulled, and a puller cannot tell
    // that from a corrupt one.
    const RepositoryResult<core::Unit> put_manifest =
        regctl_.ManifestPut(image_ref, manifest_path.value());
    if (!put_manifest.ok()) return core::make_unexpected(put_manifest.error());

    SnapshotImageResult result;
    result.image_ref = image_ref;
    result.manifest_digest = manifest_digest;
    result.reused = false;
    return result;
}

RepositoryResult<core::Unit> SnapshotImageService::EnsureLayerBlob(
    const SnapshotImageTarget& target, const std::string& target_repository,
    const OverlaybdLayerRef& layer, const std::string& scratch_dir) {
    const std::string& layer_digest = layer.digest();

    // First, so a layer the target already has is neither materialised nor
    // transferred. Most of a rootfs stack is shared between snapshots, and for
    // the managed arm materialising can mean downloading gigabytes.
    const core::Expected<bool, RepositoryError> present =
        regctl_.BlobHead(target_repository, layer_digest);
    if (!present.ok()) return core::make_unexpected(present.error());
    if (present.value()) return core::Unit();

    if (layer.is_managed()) {
        const RepositoryResult<std::string> path = locator_(layer.managed, scratch_dir);
        if (!path.ok()) return core::make_unexpected(path.error());
        return regctl_.BlobPut(target_repository, layer_digest, path.value());
    }

    // An OSS-managed layer is recorded as *external* with a synthetic `s3://`
    // URL, so the bytes are in our own managed-layer store rather than in any
    // registry. Handing this URL to `regctl` would point it at a host that
    // serves no registry API.
    const std::string& url = layer.external.repo_blob_url;
    if (url.compare(0, std::strlen(kManagedLayerUrlScheme), kManagedLayerUrlScheme) == 0) {
        ManagedLayer managed;
        managed.digest = layer.external.digest;
        managed.size = layer.external.size;
        const RepositoryResult<std::string> path = locator_(managed, scratch_dir);
        if (!path.ok()) return core::make_unexpected(path.error());
        return regctl_.BlobPut(target_repository, layer_digest, path.value());
    }

    const RepositoryResult<SourceRegistryRepository> source =
        SourceRegistryRepository::Parse(url);
    if (!source.ok()) return core::make_unexpected(source.error());
    const std::string source_repository =
        JoinRepository(source.value().registry, source.value().repository);

    // Three shapes, cheapest first. Choosing wrongly is not a correctness
    // failure but a large cost one: a cross-repo mount within a registry moves
    // no bytes at all, while a pipe moves every one of them over the network
    // twice.
    if (source.value().registry != target.registry) {
        return regctl_.BlobPipe(source_repository, target_repository, layer_digest);
    }
    if (source.value().repository != target.repository) {
        return regctl_.BlobCopy(source_repository, target_repository, layer_digest);
    }
    // Same registry *and* repository, yet the head probe just proved the blob
    // absent. There is nowhere to transfer it from, so the record points at
    // something the registry no longer has.
    return core::make_unexpected(RepositoryError::ArtifactNotFound(
        "external rootfs layer blob " + layer_digest + " in source repository " +
        source_repository));
}

}  // namespace image_export
}  // namespace snapshot
}  // namespace agentenv
