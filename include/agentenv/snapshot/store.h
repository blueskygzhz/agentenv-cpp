// SPDX-License-Identifier: MIT
// Rust: src/snapshot/store.rs — `store_volume_layers`.
//
// Rust keeps one `LayerStore` for snapshots and volumes. The snapshot half of
// it is still served by the POSIX artifact store in this port; this is the
// volume entry point, written against the repository's blob interface like
// Rust's, so it works on any backend rather than only on POSIX.
#ifndef AGENTENV_SNAPSHOT_STORE_H_
#define AGENTENV_SNAPSHOT_STORE_H_

#include <string>
#include <vector>

#include "agentenv/snapshot/layers.h"
#include "agentenv/snapshot/repository/interfaces.h"

namespace agentenv {
namespace snapshot {

/// Rust `store_volume_layers` — stores a volume's node-local backing layers
/// and returns their references:
/// `LayerStore::new(repository).store_disk_layers(path, LayerRole::Rootfs, true)`.
///
/// Every local lower becomes a managed layer keyed by content digest (a
/// capture-side descriptor is trusted after a size check, otherwise the file
/// is hashed; a sparse layer is dense-exported first). Lowers that only carry
/// a `repoBlobUrl` stay external references.
///
/// Port gap: Rust picks the upload format from the repository's
/// `publish_policy().compression`. The C++ repository has no publish policy,
/// so layers are always stored as Rust's `OverlaybdCompactOutput::Raw` stores
/// them — passed through unchanged.
repository::RepositoryResult<std::vector<OverlaybdLayerRef> > StoreVolumeLayers(
    repository::SnapshotRepository& repository, const std::string& image_config_path);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_STORE_H_
