// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/artifacts.rs
#include "agentenv/snapshot/repository/posixfs/artifacts.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/snapshot/repository/posixfs/layout.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {

namespace {

std::string ToStr(uint64_t v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

/// Rust `sync_dir`.
RepositoryResult<core::Unit> SyncDir(const std::string& path) {
    core::Expected<core::Unit, std::string> r = core::fs::SyncDirectory(path);
    if (!r.ok()) {
        return core::make_unexpected(RepositoryError::Backend("sync '" + path + "'", r.error()));
    }
    return core::Unit();
}

/// Rust `finalize_hard_linked_file` — a hard link shares the inode with the
/// source, so we make it read-only and fsync it before anyone can observe it.
RepositoryResult<core::Unit> FinalizeHardLinkedFile(const std::string& path) {
    RepositoryResult<core::Unit> chmod = ChmodReadOnly(path);
    if (!chmod.ok()) return chmod;
    core::Expected<core::fs::FileDescriptor, std::string> fd = core::fs::OpenReadFollow(path);
    if (!fd.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("open hard-linked file '" + path + "'", fd.error()));
    }
    core::Expected<core::Unit, std::string> sync = core::fs::SyncFd(fd.value().get());
    if (!sync.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("sync hard-linked file '" + path + "'", sync.error()));
    }
    return core::Unit();
}

}  // namespace

RepositoryResult<bool> SameFile(const std::string& source, const std::string& destination) {
    struct stat src;
    if (::stat(source.c_str(), &src) != 0) {
        return core::make_unexpected(RepositoryError::Backend(
            "read source artifact metadata '" + source + "'", std::strerror(errno)));
    }
    struct stat dst;
    if (::stat(destination.c_str(), &dst) != 0) {
        // Rust: a NotFound destination is simply "not the same file".
        if (errno == ENOENT) return false;
        return core::make_unexpected(RepositoryError::Backend(
            "read destination artifact metadata '" + destination + "'", std::strerror(errno)));
    }
    return src.st_dev == dst.st_dev && src.st_ino == dst.st_ino;
}

RepositoryResult<core::Unit> ChmodReadOnly(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return core::make_unexpected(RepositoryError::Backend(
            "read metadata of '" + path + "'", std::strerror(errno)));
    }
    // Rust: permissions.set_mode(mode & !0o222)
    const uint32_t mode = static_cast<uint32_t>(st.st_mode) & ~static_cast<uint32_t>(0222);
    core::Expected<core::Unit, std::string> r = core::fs::SetPermissions(path, mode);
    if (!r.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("chmod '" + path + "' to read-only", r.error()));
    }
    return core::Unit();
}

RepositoryResult<core::Unit>
CopyFileWithSha256(const std::string& source, const std::string& destination) {
    core::Expected<core::FileDigest, std::string> source_digest = core::DescribeFile(source);
    if (!source_digest.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "open source artifact '" + source + "'", source_digest.error()));
    }

    core::Expected<core::Unit, std::string> copied = core::fs::Copy(source, destination);
    if (!copied.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "copy source artifact '" + source + "' to destination artifact '" + destination + "'",
            copied.error()));
    }

    {
        core::Expected<core::fs::FileDescriptor, std::string> fd =
            core::fs::OpenReadFollow(destination);
        if (!fd.ok()) {
            return core::make_unexpected(RepositoryError::Backend(
                "sync destination artifact '" + destination + "'", fd.error()));
        }
        core::Expected<core::Unit, std::string> sync = core::fs::SyncFd(fd.value().get());
        if (!sync.ok()) {
            return core::make_unexpected(RepositoryError::Backend(
                "sync destination artifact '" + destination + "'", sync.error()));
        }
    }

    // Rust re-reads the destination rather than trusting the copy loop.
    core::Expected<core::FileDigest, std::string> dest_digest = core::DescribeFile(destination);
    if (!dest_digest.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "describe destination artifact '" + destination + "'", dest_digest.error()));
    }
    if (dest_digest.value() != source_digest.value()) {
        return core::make_unexpected(RepositoryError::Backend(
            "copied artifact digest mismatch for '" + source + "' -> '" + destination +
            "': expected " + source_digest.value().sha256 + " (" +
            ToStr(source_digest.value().size) + " bytes), found " +
            dest_digest.value().sha256 + " (" + ToStr(dest_digest.value().size) + " bytes)"));
    }
    return core::Unit();
}

RepositoryResult<core::Unit>
HardLinkOrCopyFileWithSha256(const std::string& source, const std::string& destination) {
    // Rust tries the link first; *any* failure falls through to a copy, and an
    // AlreadyExists is explicitly not treated as success here (unlike the
    // managed-layer path, this destination is not content-addressed).
    core::Expected<core::Unit, std::string> linked = core::fs::HardLink(source, destination);
    if (linked.ok()) return FinalizeHardLinkedFile(destination);
    return CopyFileWithSha256(source, destination);
}

RepositoryResult<core::Unit>
HardLinkOrCopyManagedLayer(const std::string& source, const std::string& destination,
                           const std::string& destination_parent) {
    core::Expected<core::Unit, std::string> linked = core::fs::HardLink(source, destination);
    if (linked.ok()) return FinalizeHardLinkedFile(destination);
    // Rust: AlreadyExists is success — the layer is content-addressed, so a
    // file already sitting at this digest is by definition the right bytes.
    if (core::fs::Exists(destination)) return core::Unit();

    core::Expected<core::fs::TempFile, std::string> temp =
        core::fs::CreateTempFileIn(destination_parent, ".managed-layer-");
    if (!temp.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "create temp managed layer next to '" + destination + "'", temp.error()));
    }
    const std::string temp_path = temp.value().path;

    core::Expected<core::Unit, std::string> copied = core::fs::Copy(source, temp_path);
    if (!copied.ok()) {
        core::fs::RemoveFile(temp_path);
        return core::make_unexpected(RepositoryError::Backend(
            "copy managed layer from '" + source + "' to temp '" + temp_path + "'",
            copied.error()));
    }
    {
        core::Expected<core::fs::FileDescriptor, std::string> fd =
            core::fs::OpenReadFollow(temp_path);
        if (fd.ok()) {
            core::Expected<core::Unit, std::string> sync = core::fs::SyncFd(fd.value().get());
            if (!sync.ok()) {
                core::fs::RemoveFile(temp_path);
                return core::make_unexpected(RepositoryError::Backend(
                    "sync temp managed layer '" + temp_path + "'", sync.error()));
            }
        }
    }

    core::Expected<bool, std::string> persisted =
        core::fs::PersistNoClobber(temp_path, destination);
    if (!persisted.ok()) {
        core::fs::RemoveFile(temp_path);
        return core::make_unexpected(RepositoryError::Backend(
            "persist managed layer temp '" + temp_path + "' -> '" + destination + "'",
            persisted.error()));
    }
    // persisted == false means someone else won the race; Rust maps that
    // AlreadyExists to Ok(()).
    core::fs::RemoveFile(temp_path);
    return core::Unit();
}

std::string PosixFsArtifactStore::ManagedLayerPath(const std::string& digest) const {
    return PosixFsSnapshotArtifactLayout::ManagedLayerPath(root_, digest);
}

RepositoryResult<core::Unit>
PosixFsArtifactStore::CopyLocalArtifact(const std::string& destination,
                                        const std::string& source) {
    core::Expected<core::fs::FileStat, std::string> source_stat = core::fs::Stat(source);
    if (!source_stat.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "read source artifact metadata '" + source + "'", source_stat.error()));
    }
    const uint64_t source_size = source_stat.value().size;

    core::Optional<std::string> parent = core::fs::Parent(destination);
    if (!parent.has_value()) {
        return core::make_unexpected(RepositoryError::Backend(
            "resolve parent dir for artifact '" + destination + "'"));
    }
    core::Expected<core::Unit, std::string> mk = core::fs::CreateDirAll(*parent);
    if (!mk.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("create artifact dir '" + *parent + "'", mk.error()));
    }

    // Skip the work entirely when the destination already *is* the source
    // inode (a previous run hard-linked it).
    RepositoryResult<bool> same = SameFile(source, destination);
    if (!same.ok()) return core::make_unexpected(same.error());
    if (!same.value()) {
        RepositoryResult<core::Unit> put = HardLinkOrCopyFileWithSha256(source, destination);
        if (!put.ok()) return put;
        RepositoryResult<core::Unit> sync = SyncDir(*parent);
        if (!sync.ok()) return sync;
    }

    core::Expected<core::fs::FileStat, std::string> dest_stat = core::fs::Stat(destination);
    if (!dest_stat.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "read artifact metadata '" + destination + "'", dest_stat.error()));
    }
    if (dest_stat.value().size != source_size) {
        return core::make_unexpected(RepositoryError::Backend(
            "copied artifact size mismatch for '" + source + "' -> '" + destination +
            "': expected " + ToStr(source_size) + " bytes, found " +
            ToStr(dest_stat.value().size) + " bytes"));
    }
    return core::Unit();
}

RepositoryResult<ManagedLayer>
PosixFsArtifactStore::StoreManagedLayer(const std::string& source, const std::string& digest,
                                        uint64_t size,
                                        const core::Optional<std::string>& uuid) {
    const std::string destination =
        PosixFsSnapshotArtifactLayout::ManagedLayerPath(root_, digest);

    if (core::fs::Exists(destination)) {
        core::Expected<uint64_t, std::string> existing = core::fs::FileSize(destination);
        if (!existing.ok()) {
            return core::make_unexpected(RepositoryError::Backend(
                "read managed layer metadata '" + destination + "'", existing.error()));
        }
        if (existing.value() != size) {
            // Same digest, different bytes: refuse rather than overwrite, so a
            // corrupted or mislabelled layer cannot silently replace a good one.
            return core::make_unexpected(RepositoryError::Backend(
                "managed layer '" + digest + "' size mismatch: descriptor says " + ToStr(size) +
                ", existing file has " + ToStr(existing.value())));
        }
        ManagedLayer layer;
        layer.digest = digest;
        layer.size = size;
        layer.uuid = uuid;
        return layer;
    }

    core::Optional<std::string> parent = core::fs::Parent(destination);
    if (!parent.has_value()) {
        return core::make_unexpected(RepositoryError::Backend(
            "resolve parent dir for managed layer '" + destination + "'"));
    }
    core::Expected<core::Unit, std::string> mk = core::fs::CreateDirAll(*parent);
    if (!mk.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("create managed layer dir '" + *parent + "'", mk.error()));
    }

    RepositoryResult<core::Unit> put =
        HardLinkOrCopyManagedLayer(source, destination, *parent);
    if (!put.ok()) return core::make_unexpected(put.error());
    RepositoryResult<core::Unit> sync = SyncDir(*parent);
    if (!sync.ok()) return core::make_unexpected(sync.error());

    ManagedLayer layer;
    layer.digest = digest;
    layer.size = size;
    layer.uuid = uuid;
    return layer;
}

RepositoryResult<ManagedLayer>
PosixFsArtifactStore::ImportManagedLayerWithDescriptor(const std::string& source,
                                                       const std::string& digest,
                                                       uint64_t size) {
    core::Expected<uint64_t, std::string> source_size = core::fs::FileSize(source);
    if (!source_size.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "read managed layer source metadata '" + source + "'", source_size.error()));
    }
    if (source_size.value() != size) {
        return core::make_unexpected(RepositoryError::Backend(
            "managed layer descriptor size mismatch for '" + source + "': descriptor says " +
            ToStr(size) + ", file has " + ToStr(source_size.value())));
    }
    // Descriptor-backed imports trust internally generated content digests and
    // only validate the cheap size invariant.
    return StoreManagedLayer(source, digest, size, core::Optional<std::string>());
}

RepositoryResult<ManagedLayer>
PosixFsArtifactStore::ImportManagedLayerByHash(const std::string& source) {
    core::Expected<core::FileDigest, std::string> descriptor = core::DescribeFile(source);
    if (!descriptor.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "describe managed layer source '" + source + "'", descriptor.error()));
    }
    return StoreManagedLayer(source, descriptor.value().sha256, descriptor.value().size,
                             core::Optional<std::string>());
}

RepositoryResult<std::vector<OverlaybdLayerRef> >
PosixFsArtifactStore::DeriveRootfsLayers(const std::string& image_config_path,
                                         bool allow_descriptorless) {
    core::Expected<storage::overlaybd::ImageConfig, std::string> image_config =
        storage::overlaybd::LoadImageConfig(image_config_path);
    if (!image_config.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "load overlaybd image config for rootfs layout '" + image_config_path + "'",
            image_config.error()));
    }

    std::vector<OverlaybdLayerRef> layers;
    for (size_t index = 0; index < image_config.value().lowers.size(); ++index) {
        const storage::overlaybd::LayerConfig& layer = image_config.value().lowers[index];

        if (!layer.file.empty()) {
            if (!layer.digest.empty() && layer.size > 0) {
                RepositoryResult<ManagedLayer> managed =
                    ImportManagedLayerWithDescriptor(layer.file, layer.digest, layer.size);
                if (!managed.ok()) return core::make_unexpected(managed.error());
                layers.push_back(OverlaybdLayerRef::Managed(managed.value()));
                continue;
            }
            // A descriptorless lower is only acceptable for volume publishing
            // or for a delta this node generated itself; otherwise we cannot
            // vouch for its identity.
            if (allow_descriptorless) {
                RepositoryResult<ManagedLayer> managed = ImportManagedLayerByHash(layer.file);
                if (!managed.ok()) return core::make_unexpected(managed.error());
                layers.push_back(OverlaybdLayerRef::Managed(managed.value()));
                continue;
            }
            return core::make_unexpected(RepositoryError::Unsupported(
                "local overlaybd lower layer " + ToStr(index) + " '" + layer.file +
                "' missing digest/size"));
        }

        const std::string repo_blob_url =
            layer.EffectiveRepoBlobUrl(image_config.value().repo_blob_url);
        if (!repo_blob_url.empty()) {
            ExternalLayer external;
            // Rust's digest fallback chain: digest -> target_digest ->
            // a synthetic "external:{index}" so the ref is never unnamed.
            if (!layer.digest.empty()) {
                external.digest = layer.digest;
            } else if (!layer.target_digest.empty()) {
                external.digest = layer.target_digest;
            } else {
                external.digest = "external:" + ToStr(index);
            }
            external.repo_blob_url = repo_blob_url;
            external.size = layer.size;
            layers.push_back(OverlaybdLayerRef::External(external));
            continue;
        }

        return core::make_unexpected(RepositoryError::Unsupported(
            "overlaybd lower layer " + ToStr(index) + " without local file or repoBlobUrl"));
    }
    return layers;
}

RepositoryResult<std::vector<ManagedLayer> >
PosixFsArtifactStore::DeriveMemoryLayers(const std::string& mem_image_config_path) {
    core::Expected<storage::overlaybd::ImageConfig, std::string> image_config =
        storage::overlaybd::LoadImageConfig(mem_image_config_path);
    if (!image_config.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "load mem image config for memory layers '" + mem_image_config_path + "'",
            image_config.error()));
    }

    std::vector<ManagedLayer> layers;
    for (size_t index = 0; index < image_config.value().lowers.size(); ++index) {
        const storage::overlaybd::LayerConfig& layer = image_config.value().lowers[index];
        // Unlike rootfs, a memory layer can never be external: the bytes are
        // produced by this node's capture and have nowhere else to live.
        if (layer.file.empty()) {
            return core::make_unexpected(RepositoryError::Unsupported(
                "memory layer " + ToStr(index) + " without local file path"));
        }
        RepositoryResult<ManagedLayer> managed =
            (!layer.digest.empty() && layer.size > 0)
                ? ImportManagedLayerWithDescriptor(layer.file, layer.digest, layer.size)
                : ImportManagedLayerByHash(layer.file);
        if (!managed.ok()) return core::make_unexpected(managed.error());
        layers.push_back(managed.value());
    }
    return layers;
}

RepositoryResult<std::vector<OverlaybdLayerRef> >
PosixFsArtifactStore::PublishVolumeBacking(const std::string& image_config_path) {
    return DeriveRootfsLayers(image_config_path, true);
}

RepositoryResult<CollectedBuiltArtifacts>
PosixFsArtifactStore::ImportBuiltArtifacts(const core::SnapshotId& snapshot_id,
                                           const sandbox::SandboxSnapshotManifest& manifest) {
    PosixFsSnapshotArtifactLayout committed(root_, snapshot_id);

    RepositoryResult<core::Unit> vm_state = CopyLocalArtifact(
        committed.Path(kSnapshotArtifactLayout.vm_state), manifest.vm_state.path);
    if (!vm_state.ok()) return core::make_unexpected(vm_state.error());

    // Rust `persist_firecracker_manifest`.
    {
        const std::string destination =
            committed.Path(kSnapshotArtifactLayout.firecracker_manifest);
        core::Optional<std::string> parent = core::fs::Parent(destination);
        if (parent.has_value()) {
            core::Expected<core::Unit, std::string> mk = core::fs::CreateDirAll(*parent);
            if (!mk.ok()) {
                return core::make_unexpected(RepositoryError::Backend(
                    "create firecracker manifest dir '" + *parent + "'", mk.error()));
            }
        }
        // Rust `serde_json::to_vec_pretty`.
        const std::string bytes = manifest.ToJson().ToString();
        core::Expected<core::Unit, std::string> w = core::fs::Write(destination, bytes);
        if (!w.ok()) {
            return core::make_unexpected(RepositoryError::Backend(
                "write firecracker manifest '" + destination + "'", w.error()));
        }
    }

    CollectedBuiltArtifacts out;

    RepositoryResult<std::vector<ManagedLayer> > memory =
        DeriveMemoryLayers(manifest.memory.image_config_path);
    if (!memory.ok()) return core::make_unexpected(memory.error());
    out.memory_layers = memory.value();

    for (size_t i = 0; i < manifest.attached_drives.size(); ++i) {
        const sandbox::SnapshotAttachedDriveArtifacts& drive = manifest.attached_drives[i];
        // The build-time image config is only an input for deriving layers; it
        // is deliberately not copied into the committed repository.
        RepositoryResult<std::vector<OverlaybdLayerRef> > drive_layers =
            DeriveRootfsLayers(drive.image_config_path, false);
        if (!drive_layers.ok()) return core::make_unexpected(drive_layers.error());

        CommittedAttachedDrive committed_drive;
        committed_drive.kind = CommittedAttachedDrive::Kind::Overlaybd;
        committed_drive.drive_id = drive.drive_id;
        committed_drive.layers = drive_layers.value();
        committed_drive.read_only = drive.read_only;
        committed_drive.virtual_size = drive.virtual_size;
        // Rust: normalize, and fall back to the default on rejection rather
        // than failing the whole import over a cosmetic path.
        core::Expected<std::string, std::string> normalized =
            sandbox::NormalizeMountPathForDrive(drive.drive_id, drive.mount_path);
        committed_drive.mount_path =
            normalized.ok() ? normalized.value()
                            : sandbox::ExtraDrive::DefaultMountPath(drive.drive_id);
        committed_drive.sub_path = drive.sub_path;
        out.attached_drives.push_back(committed_drive);
    }

    RepositoryResult<std::vector<OverlaybdLayerRef> > rootfs =
        DeriveRootfsLayers(manifest.rootfs.image_config_path, false);
    if (!rootfs.ok()) return core::make_unexpected(rootfs.error());
    out.rootfs_layers = rootfs.value();

    return out;
}

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
