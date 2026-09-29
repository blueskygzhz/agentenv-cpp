// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/backend.rs
#include "agentenv/snapshot/repository/posixfs/backend.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {

std::string SharedRuntimeCacheRoot() {
    // Rust anchors the default under the process temp directory so it stays
    // node-local and survives across sandbox launches.
    return "/tmp/agentenv-snapshot-cache";
}

PosixFsBackend::PosixFsBackend(const PosixFsBackendConfig& config,
                               const OverlaybdLayerStore* store,
                               std::shared_ptr<LocalArtifactCache> cache) {
    const std::string cache_root =
        config.cache_root.has_value() ? *config.cache_root : SharedRuntimeCacheRoot();
    const std::string runtime_cache_root = config.runtime_cache_root.has_value()
        ? *config.runtime_cache_root
        : cache_root + "/runtime";

    catalog_store_.reset(new PosixFsCatalogStore(config.root));
    artifact_store_.reset(new PosixFsArtifactStore(config.root));
    runtime_resolver_.reset(
        new PosixFsRuntimeResolver(config.root, runtime_cache_root, store, cache));
}

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
