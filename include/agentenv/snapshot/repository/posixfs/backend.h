// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/backend.rs
//
// Bundles the durable POSIX repository state (catalog + artifact stores) with
// the node-local runtime resolver.
//
// Porting note. Rust's `PosixFsBackend` also wraps both stores in a
// `PosixFsSnapshotRepository` that implements the async `SnapshotRepository`
// trait. That adapter is not ported yet — `repository::SnapshotRepository` in
// `interfaces.h` is still the scaffold subset — so the backend exposes the two
// stores directly. Callers get the same components; only the trait object is
// missing.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_BACKEND_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_BACKEND_H_

#include <memory>
#include <string>

#include "agentenv/core/optional.h"
#include "agentenv/snapshot/artifact_cache.h"
#include "agentenv/snapshot/repository/posixfs/artifacts.h"
#include "agentenv/snapshot/repository/posixfs/catalog.h"
#include "agentenv/snapshot/repository/posixfs/runtime.h"
#include "agentenv/snapshot/runtime_support.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {

/// Rust `struct PosixFsBackendConfig`.
struct PosixFsBackendConfig {
    /// Durable repository root; may live on a shared filesystem.
    std::string root;
    /// Node-local cache root for downloaded/materialised artifacts.
    core::Optional<std::string> cache_root;
    /// Node-local root for runtime-materialised files. Defaults to
    /// `<cache_root>/runtime`, matching Rust.
    core::Optional<std::string> runtime_cache_root;
};

/// Rust `shared_runtime_cache_root` — the node-local default.
std::string SharedRuntimeCacheRoot();

/// Rust `struct PosixFsBackend`.
class PosixFsBackend {
 public:
    /// Rust `PosixFsBackend::from_parts`.
    PosixFsBackend(const PosixFsBackendConfig& config,
                   const OverlaybdLayerStore* store,
                   std::shared_ptr<LocalArtifactCache> cache);

    const std::shared_ptr<PosixFsCatalogStore>&  catalog_store() const { return catalog_store_; }
    const std::shared_ptr<PosixFsArtifactStore>& artifact_store() const { return artifact_store_; }

    /// Rust `runtime_resolver`.
    const std::shared_ptr<PosixFsRuntimeResolver>& runtime_resolver() const {
        return runtime_resolver_;
    }

 private:
    std::shared_ptr<PosixFsCatalogStore>   catalog_store_;
    std::shared_ptr<PosixFsArtifactStore>  artifact_store_;
    std::shared_ptr<PosixFsRuntimeResolver> runtime_resolver_;
};

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_POSIXFS_BACKEND_H_
