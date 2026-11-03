// SPDX-License-Identifier: MIT
// Rust: src/snapshot/store.rs — `store_volume_layers` and the parts of
// `LayerStore` it reaches (`store_disk_layers`, `store_descriptorless_layer`,
// `store_layer`, `put_prepared`, `canonicalize`, `overlaybd_layer_uuid`).
#include "agentenv/snapshot/store.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/identity.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/dense_export.h"
#include "agentenv/storage/overlaybd/layer_metadata.h"
#include "agentenv/storage/overlaybd/local_file.h"

namespace agentenv {
namespace snapshot {
namespace {

using repository::BlobBody;
using repository::BlobKey;
using repository::LayerRole;
using repository::RepositoryError;
using repository::RepositoryResult;
using repository::SnapshotRepository;
namespace obd = storage::overlaybd;

std::string ToStr(uint64_t value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

/// Rust `canonicalize`.
RepositoryResult<std::string> Canonicalize(const std::string& source) {
    char resolved[PATH_MAX];
    if (::realpath(source.c_str(), resolved) == NULL) {
        return core::make_unexpected(RepositoryError::Backend(
            "canonicalize managed layer '" + source + "'", std::strerror(errno)));
    }
    return std::string(resolved);
}

/// Rust `overlaybd_layer_uuid` — unset when unreadable or nil.
core::Optional<std::string> OverlaybdLayerUuid(const std::string& source) {
    const core::Expected<std::string, std::string> uuid = obd::ReadOverlaybdLayerUuid(source);
    if (!uuid.ok()) return core::Optional<std::string>();
    core::Uuid parsed;
    if (!core::Uuid::Parse(uuid.value(), &parsed) || parsed.is_nil()) {
        return core::Optional<std::string>();
    }
    return core::Optional<std::string>(parsed.ToString());
}

/// Rust `PreparedLayerUpload` as `passthrough` builds it — the only shape the
/// port produces (see the header's note on compression).
struct PreparedLayerUpload {
    std::string path;
    std::string digest;
    uint64_t size;
};

/// Rust `passthrough`.
RepositoryResult<PreparedLayerUpload> Passthrough(const std::string& source,
                                                  const core::Optional<std::string>& digest,
                                                  uint64_t size) {
    PreparedLayerUpload upload;
    upload.path = source;
    if (digest.has_value()) {
        upload.digest = *digest;
        upload.size = size;
        return upload;
    }
    const core::Expected<core::FileDigest, std::string> described = core::DescribeFile(source);
    if (!described.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "describe managed layer '" + source + "'", described.error()));
    }
    upload.digest = described.value().sha256;
    upload.size = described.value().size;
    return upload;
}

/// Rust `LayerStore::put_prepared`.
RepositoryResult<ManagedLayer> PutPrepared(SnapshotRepository& repository,
                                           const PreparedLayerUpload& upload,
                                           const core::Optional<std::string>& uuid,
                                           LayerRole role) {
    ManagedLayer layer;
    layer.digest = upload.digest;
    layer.size = upload.size;
    layer.uuid = uuid;
    const RepositoryResult<core::Unit> put =
        repository.PutBlob(BlobKey::Layer(layer.digest, role), BlobBody::File(upload.path));
    if (!put.ok()) return core::make_unexpected(put.error());
    return layer;
}

/// Rust `LayerStore::store_layer` — a descriptor from the capture side is
/// trusted after a cheap size check; otherwise the file is hashed.
RepositoryResult<ManagedLayer> StoreLayer(SnapshotRepository& repository,
                                          const std::string& source,
                                          const core::Optional<std::string>& digest,
                                          uint64_t size, LayerRole role) {
    const RepositoryResult<std::string> canonical = Canonicalize(source);
    if (!canonical.ok()) return core::make_unexpected(canonical.error());
    if (digest.has_value()) {
        const core::Expected<uint64_t, std::string> source_size =
            core::fs::FileSize(canonical.value());
        if (!source_size.ok()) {
            return core::make_unexpected(RepositoryError::Backend(
                "read managed layer metadata '" + canonical.value() + "'", source_size.error()));
        }
        if (source_size.value() != size) {
            return core::make_unexpected(RepositoryError::Backend(
                "managed layer descriptor size mismatch for '" + canonical.value() +
                "': descriptor says " + ToStr(size) + ", file has " +
                ToStr(source_size.value())));
        }
    }
    const RepositoryResult<PreparedLayerUpload> upload =
        Passthrough(canonical.value(), digest, size);
    if (!upload.ok()) return core::make_unexpected(upload.error());
    return PutPrepared(repository, upload.value(), OverlaybdLayerUuid(upload.value().path), role);
}

/// Rust `LayerStore::store_descriptorless_layer`.
RepositoryResult<ManagedLayer> StoreDescriptorlessLayer(SnapshotRepository& repository,
                                                        const std::string& source,
                                                        LayerRole role) {
    const RepositoryResult<std::string> canonical = Canonicalize(source);
    if (!canonical.ok()) return core::make_unexpected(canonical.error());
    if (!obd::ShouldDenseExportLayer(canonical.value())) {
        return StoreLayer(repository, canonical.value(), core::Optional<std::string>(), 0, role);
    }

    // Rust stages this in `repository.staging_file()`, which the C++
    // repository does not have; `put_blob` copies a file body, so a temporary
    // beside the source is equivalent and is removed once stored.
    const core::Optional<std::string> directory = core::fs::Parent(canonical.value());
    const core::Expected<core::fs::TempFile, std::string> dense_temp =
        core::fs::CreateTempFileIn(directory.has_value() ? *directory : std::string("."),
                                   ".dense-layer-");
    if (!dense_temp.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("create dense layer staging file", dense_temp.error()));
    }
    const std::string dense_path = dense_temp.value().path;

    RepositoryResult<ManagedLayer> stored = core::make_unexpected(RepositoryError::Backend(""));
    do {
        const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> dest =
            obd::LocalFile::OpenRw(dense_path, false);
        if (!dest.ok()) {
            stored = core::make_unexpected(RepositoryError::Backend(
                "dense-export sparse overlaybd layer '" + canonical.value() + "'", dest.error()));
            break;
        }
        const core::Expected<core::Unit, std::string> written =
            obd::WriteDenseLayerToFile(canonical.value(), dest.value());
        if (!written.ok()) {
            stored = core::make_unexpected(RepositoryError::Backend(
                "dense-export sparse overlaybd layer '" + canonical.value() + "'",
                written.error()));
            break;
        }
        // Rust takes the descriptor `write_dense_overlaybd_layer_to_file`
        // returns; it is the digest and size of exactly these bytes.
        const core::Expected<core::FileDigest, std::string> descriptor =
            core::DescribeFile(dense_path);
        if (!descriptor.ok()) {
            stored = core::make_unexpected(RepositoryError::Backend(
                "dense-export sparse overlaybd layer '" + canonical.value() + "'",
                descriptor.error()));
            break;
        }
        const RepositoryResult<PreparedLayerUpload> upload =
            Passthrough(dense_path, core::Optional<std::string>(descriptor.value().sha256),
                        descriptor.value().size);
        if (!upload.ok()) {
            stored = core::make_unexpected(upload.error());
            break;
        }
        // Dense-exported layers never carry a layer uuid, recontainerized or not.
        stored = PutPrepared(repository, upload.value(), core::Optional<std::string>(), role);
    } while (false);
    core::fs::RemoveFile(dense_path);
    return stored;
}

/// Rust `LayerStore::store_disk_layers`.
RepositoryResult<std::vector<OverlaybdLayerRef> > StoreDiskLayers(
    SnapshotRepository& repository, const std::string& image_config_path, LayerRole role,
    bool allow_descriptorless) {
    const core::Expected<obd::ImageConfig, std::string> image_config =
        obd::LoadImageConfig(image_config_path);
    if (!image_config.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "load overlaybd image config '" + image_config_path + "'", image_config.error()));
    }
    std::vector<OverlaybdLayerRef> layers;
    layers.reserve(image_config.value().lowers.size());
    for (std::size_t index = 0; index < image_config.value().lowers.size(); ++index) {
        const obd::LayerConfig& layer = image_config.value().lowers[index];
        if (!layer.file.empty()) {
            RepositoryResult<ManagedLayer> managed =
                core::make_unexpected(RepositoryError::Backend(""));
            if (!layer.digest.empty() && layer.size > 0) {
                managed = StoreLayer(repository, layer.file,
                                     core::Optional<std::string>(layer.digest), layer.size, role);
            } else if (allow_descriptorless) {
                // Rust also admits `rootfs_layer_is_runtime_generated_delta`;
                // volumes always pass `allow_descriptorless`, so that branch
                // cannot be reached from here.
                managed = StoreDescriptorlessLayer(repository, layer.file, role);
            } else {
                return core::make_unexpected(RepositoryError::Unsupported(
                    "local overlaybd lower layer " + ToStr(index) + " '" + layer.file +
                    "' missing digest/size"));
            }
            if (!managed.ok()) return core::make_unexpected(managed.error());
            layers.push_back(OverlaybdLayerRef::Managed(managed.value()));
            continue;
        }
        const std::string repo_blob_url =
            layer.EffectiveRepoBlobUrl(image_config.value().repo_blob_url);
        if (repo_blob_url.empty()) {
            return core::make_unexpected(RepositoryError::Unsupported(
                "overlaybd lower layer " + ToStr(index) + " without local file or repoBlobUrl"));
        }
        ExternalLayer external;
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
    }
    return layers;
}

}  // namespace

RepositoryResult<std::vector<OverlaybdLayerRef> > StoreVolumeLayers(
    SnapshotRepository& repository, const std::string& image_config_path) {
    return StoreDiskLayers(repository, image_config_path, LayerRole::Rootfs, true);
}

}  // namespace snapshot
}  // namespace agentenv
