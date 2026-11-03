// SPDX-License-Identifier: MIT
// Rust: src/image/test_support.rs — a built overlaybd image on disk and the
// `store_layers` callback `ImageResolver::publish_image` takes.
#ifndef AGENTENV_TESTS_IMAGE_TEST_SUPPORT_H_
#define AGENTENV_TESTS_IMAGE_TEST_SUPPORT_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/snapshot/layers.h"
#include "agentenv/snapshot/repository/interfaces.h"
#include "agentenv/snapshot/repository/posixfs/artifacts.h"
#include "agentenv/snapshot/repository/posixfs/backend.h"
#include "agentenv/snapshot/repository/posixfs/catalog.h"
#include "agentenv/storage/overlaybd/config.h"
#include "microtest.h"

namespace image_test_support {

/// Rust `write_built_image` — one 10-byte layer and an `image.json` naming it
/// by `file=`, under `root`. Returns the config path.
inline std::string WriteBuiltImage(const std::string& root) {
    namespace core = agentenv::core;
    MT_EXPECT_TRUE(core::fs::CreateDirAll(root).ok());
    const std::string layer = core::fs::Join(root, "base.overlaybd.commit");
    MT_EXPECT_TRUE(core::fs::Write(layer, "base-layer").ok());
    const core::Expected<core::FileDigest, std::string> descriptor = core::DescribeFile(layer);
    MT_EXPECT_TRUE(descriptor.ok());
    core::Json lower = core::Json(core::JsonObject());
    lower["file"] = layer;
    lower["digest"] = descriptor.value().sha256;
    lower["size"] = static_cast<int64_t>(descriptor.value().size);
    core::Json config = core::Json(core::JsonObject());
    config["repoBlobUrl"] = "";
    config["lowers"] = core::Json(core::JsonArray(1, lower));
    config["upper"] = core::Json(core::JsonObject());
    config["resultFile"] = "";
    const std::string path = core::fs::Join(root, "image.json");
    MT_EXPECT_TRUE(core::fs::Write(path, config.ToString()).ok());
    return path;
}

/// Rust `store_layers` — copies every lower into the repository as a managed
/// rootfs layer.
inline agentenv::snapshot::repository::RepositoryResult<
    std::vector<agentenv::snapshot::OverlaybdLayerRef> >
StoreLayers(agentenv::snapshot::repository::SnapshotRepository& repository,
            const std::string& image_config_path) {
    namespace core = agentenv::core;
    namespace repo = agentenv::snapshot::repository;
    namespace overlaybd = agentenv::storage::overlaybd;
    const core::Expected<overlaybd::ImageConfig, std::string> config =
        overlaybd::LoadImageConfig(image_config_path);
    if (!config.ok()) {
        return core::make_unexpected(
            repo::RepositoryError::Backend("load image config", config.error()));
    }
    std::vector<agentenv::snapshot::OverlaybdLayerRef> layers;
    for (std::size_t i = 0; i < config.value().lowers.size(); ++i) {
        const overlaybd::LayerConfig& lower = config.value().lowers[i];
        const repo::RepositoryResult<core::Unit> put =
            repository.PutBlob(repo::BlobKey::Layer(lower.digest, repo::LayerRole::Rootfs),
                               repo::BlobBody::File(lower.file));
        if (!put.ok()) return core::make_unexpected(put.error());
        agentenv::snapshot::ManagedLayer managed;
        managed.digest = lower.digest;
        managed.size = lower.size;
        layers.push_back(agentenv::snapshot::OverlaybdLayerRef::Managed(managed));
    }
    return layers;
}

/// Rust `PosixFsRepository::new(root)` — catalog and blobs under one root.
inline std::shared_ptr<agentenv::snapshot::repository::SnapshotRepository> PosixRepository(
    const std::string& root) {
    namespace posixfs = agentenv::snapshot::repository::posixfs;
    return std::make_shared<posixfs::PosixFsSnapshotRepository>(
        std::make_shared<posixfs::PosixFsCatalogStore>(root),
        std::make_shared<posixfs::PosixFsArtifactStore>(root));
}

}  // namespace image_test_support

#endif  // AGENTENV_TESTS_IMAGE_TEST_SUPPORT_H_
