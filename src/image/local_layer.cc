// SPDX-License-Identifier: MIT
// Rust: src/image/local_layer.rs
#include "agentenv/image/local_layer.h"

#include "agentenv/core/fs.h"

namespace agentenv {
namespace image {

const char* const kSnapshotDeltaLayerFile = "snapshot.commit";
const char* const kSelfContainedBaseLayerFile = "managed-base.commit";
const char* const kSnapshotZfileDeltaLayerFile = "snapshot.zfile.commit";

bool RootfsLayerIsRuntimeGeneratedDelta(const std::string& path) {
    const core::Optional<std::string> name = core::fs::FileName(path);
    // No file name at all (a bare "/" or an empty path) is not a delta. Rust
    // reaches the same answer through `file_name()` returning `None`.
    if (!name.has_value()) return false;
    return *name == kSnapshotDeltaLayerFile || *name == kSelfContainedBaseLayerFile ||
           *name == kSnapshotZfileDeltaLayerFile;
}

storage::overlaybd::LayerConfig LayerConfigFromLocalLayer(const LocalLayer& layer) {
    storage::overlaybd::LayerConfig config;
    config.file = layer.path;
    config.digest = layer.digest;
    config.size = layer.size;
    return config;
}

}  // namespace image
}  // namespace agentenv
