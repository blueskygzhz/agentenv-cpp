// SPDX-License-Identifier: MIT
// Rust: src/image/local_layer.rs
#ifndef AGENTENV_IMAGE_LOCAL_LAYER_H_
#define AGENTENV_IMAGE_LOCAL_LAYER_H_

#include <cstdint>
#include <string>

#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace image {

/// Rust `SNAPSHOT_DELTA_LAYER_FILE`.
extern const char* const kSnapshotDeltaLayerFile;
/// Rust `SELF_CONTAINED_BASE_LAYER_FILE`.
extern const char* const kSelfContainedBaseLayerFile;
/// Rust `SNAPSHOT_ZFILE_DELTA_LAYER_FILE` — the ZFile-recontainerized variant.
///
/// No longer produced: capture-time compression was removed and local layers
/// always stay raw. It stays in the allowlist below because layers published
/// while the old capture-time switches existed are still out there, and
/// dropping the name would make those repositories unreadable.
extern const char* const kSnapshotZfileDeltaLayerFile;

/// Rust struct `LocalLayer` — a layer that exists as a file on this node.
///
/// Three fields and no more. An earlier version of this struct carried an
/// `is_overlaybd` flag that Rust has no counterpart for; a layer's format is a
/// property of its content, read back from the file, not something a caller
/// gets to assert alongside the path.
struct LocalLayer {
    std::string path;
    std::string digest;
    /// Unsigned, matching Rust's `u64`. A signed size would let a corrupt
    /// descriptor present a negative length that passes a `> 0` check
    /// somewhere downstream.
    uint64_t size;

    LocalLayer() : size(0) {}

    bool operator==(const LocalLayer& other) const {
        return path == other.path && digest == other.digest && size == other.size;
    }
    bool operator!=(const LocalLayer& other) const { return !(*this == other); }
};

/// Rust `rootfs_layer_is_runtime_generated_delta`.
///
/// Matches on the **exact** file name, not a suffix or a substring: a
/// `snapshot.commit.tmp` left behind by an interrupted write is not a layer,
/// and treating it as one would publish a partially written file as image
/// content.
bool RootfsLayerIsRuntimeGeneratedDelta(const std::string& path);

/// Rust `impl From<LocalLayer> for LayerConfig`.
///
/// Every other `LayerConfig` field keeps its default, which is what
/// `..Default::default()` means upstream. In particular `dir` stays empty: a
/// layer named by `file` must not also name a directory, or the loader has two
/// candidate locations for the same layer.
storage::overlaybd::LayerConfig LayerConfigFromLocalLayer(const LocalLayer& layer);

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_LOCAL_LAYER_H_
