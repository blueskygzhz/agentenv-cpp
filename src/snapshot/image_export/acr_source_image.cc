// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/acr/source_image.rs
#include "agentenv/snapshot/image_export/acr_source_image.h"

#include <cstdio>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/image/local_layer.h"
#include "agentenv/storage/overlaybd/dense_export.h"

namespace agentenv {
namespace snapshot {
namespace image_export {

using repository::RepositoryError;
using repository::RepositoryResult;

namespace {

std::string FormatIndex(std::size_t index) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(index));
    return std::string(buf);
}

}  // namespace

RepositoryResult<core::Unit> RejectUnsupportedShape(
    std::size_t index, const storage::overlaybd::LayerConfig& layer) {
    const std::string at = FormatIndex(index);

    // `targetFile` / `targetDigest` / `gzipIndex` describe a converted or
    // gzip-indexed layer, which the publisher has no code to upload. Checked
    // before the local/remote split because they are unsupported either way.
    if (!layer.target_file.empty() || !layer.target_digest.empty() ||
        !layer.gzip_index.empty()) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "ACR disk image export does not support complex overlaybd lower " + at));
    }

    if (!layer.file.empty()) {
        // A local lower with a `dir` cache hint is ambiguous: `dir` names a
        // downloaded copy of a *remote* blob, so the two together do not say
        // which file is authoritative.
        if (!layer.dir.empty()) {
            return core::make_unexpected(RepositoryError::Unsupported(
                "ACR disk image export does not support local lower " + at +
                " with dir cache hint"));
        }
        return core::Unit();
    }

    // From here the lower is remote, and the manifest will name it by digest
    // and size alone — so both must already be recorded. There is no file to
    // measure.
    if (layer.digest.empty()) {
        return core::make_unexpected(
            RepositoryError::Unsupported("remote ACR lower " + at + " is missing digest"));
    }
    if (!layer.repo_blob_url.empty()) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "ACR disk image export does not support per-layer repoBlobUrl on remote lower " +
            at));
    }
    if (layer.size == 0) {
        return core::make_unexpected(
            RepositoryError::Unsupported("remote ACR lower " + at + " is missing size"));
    }

    return core::Unit();
}

RepositoryResult<SourceRegistryImage> LoadSourceRegistryImage(
    const std::string& image_config_path, bool allow_loopback_http) {
    const core::Expected<storage::overlaybd::ImageConfig, std::string> image_config =
        storage::overlaybd::LoadImageConfig(image_config_path);
    if (!image_config.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "load overlaybd image config '" + image_config_path + "'", image_config.error()));
    }

    // One image-level source is required rather than inferred from the lowers:
    // the publisher uploads every new blob to exactly one repository, and a
    // per-layer URL would mean there is no single such place.
    if (image_config.value().repo_blob_url.empty()) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "ACR disk image export requires a single image-level source registry "
            "repoBlobUrl"));
    }
    const RepositoryResult<SourceRegistryRepository> repository_ref =
        SourceRegistryRepository::Parse(image_config.value().repo_blob_url,
                                        allow_loopback_http);
    if (!repository_ref.ok()) return core::make_unexpected(repository_ref.error());

    const std::vector<storage::overlaybd::LayerConfig>& lowers = image_config.value().lowers;
    std::vector<SourceRegistryLayer> layers;
    layers.reserve(lowers.size());
    bool saw_remote = false;
    bool saw_local = false;

    for (std::size_t index = 0; index < lowers.size(); ++index) {
        const storage::overlaybd::LayerConfig& layer = lowers[index];
        const RepositoryResult<core::Unit> shape = RejectUnsupportedShape(index, layer);
        if (!shape.ok()) return core::make_unexpected(shape.error());

        if (!layer.file.empty()) {
            saw_local = true;
            // Canonicalize before the predicate and before recording the
            // path: the delta must exist now, and the publisher must later
            // open the same file this check approved.
            const core::Expected<std::string, std::string> path =
                core::fs::Canonicalize(layer.file);
            if (!path.ok()) {
                return core::make_unexpected(RepositoryError::Backend(
                    "canonicalize local ACR delta '" + layer.file + "'", path.error()));
            }
            // Only a sandbox's own runtime-generated rootfs delta may be
            // uploaded. Any other local file is something this host happens
            // to have — a cached remote blob, a hand-placed layer — and
            // pushing it would publish bytes whose provenance is unknown.
            if (!image::RootfsLayerIsRuntimeGeneratedDelta(path.value())) {
                return core::make_unexpected(RepositoryError::Unsupported(
                    "ACR disk image export only supports local runtime-generated delta "
                    "lowers, got '" + path.value() + "'"));
            }

            LocalSnapshotDeltaDescriptor descriptor;
            if (!layer.digest.empty() && layer.size > 0) {
                // Already described by whoever wrote the config; re-hashing
                // would only risk disagreeing with it.
                descriptor = LocalSnapshotDeltaDescriptor::Raw(layer.digest, layer.size);
            } else if (storage::overlaybd::ShouldDenseExportLayer(path.value())) {
                // Sparse: the uploaded bytes differ from the bytes on disk, so
                // there is nothing truthful to record until the upload runs.
                descriptor = LocalSnapshotDeltaDescriptor::DenseOverlaybd();
            } else {
                const core::Expected<core::FileDigest, std::string> described =
                    core::DescribeFile(path.value());
                if (!described.ok()) {
                    return core::make_unexpected(RepositoryError::Backend(
                        "describe local ACR delta '" + path.value() + "'",
                        described.error()));
                }
                descriptor = LocalSnapshotDeltaDescriptor::Raw(described.value().sha256,
                                                               described.value().size);
            }

            LocalSnapshotDelta delta;
            delta.path = path.value();
            delta.descriptor = descriptor;
            layers.push_back(SourceRegistryLayer::LocalDelta(delta));
            continue;
        }

        if (saw_local) {
            return core::make_unexpected(RepositoryError::Unsupported(
                "ACR disk image export requires all remote lowers before local deltas"));
        }
        saw_remote = true;
        layers.push_back(SourceRegistryLayer::Remote(layer.digest, layer.size));
    }

    // Without a remote base there is nothing in the target registry to build
    // on, so the "export" would be a full image push this planner does not
    // model — and would leave a manifest referencing blobs that were never
    // uploaded.
    if (!saw_remote) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "ACR disk image export requires at least one remote source-registry lower"));
    }

    SourceRegistryImage out;
    out.target = repository_ref.value();
    out.layers = layers;
    return out;
}

}  // namespace image_export
}  // namespace snapshot
}  // namespace agentenv
