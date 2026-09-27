// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/layer/ — LayerMetadata + the stacked read path.
#ifndef AGENTENV_STORAGE_OVERLAYBD_LAYER_H_
#define AGENTENV_STORAGE_OVERLAYBD_LAYER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust struct `LayerMetadata` (layer/layer_metadata.rs).
struct LayerMetadata {
    std::string digest;
    uint64_t    uncompressed_size = 0;
    uint64_t    block_size = 4096;
    bool    compressed = false;
};

/// Rust: a stack of read-only layers, resolved bottom-up. Implements VirtualFile.
class LayerStack : public VirtualFile {
 public:
    virtual ~LayerStack() {}
    /// Push a lower layer (called from bottom to top when building the stack).
    virtual void PushLayer(std::shared_ptr<VirtualFile> layer,
      const LayerMetadata& meta) = 0;
};

std::unique_ptr<LayerStack> MakeLayerStack();

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_LAYER_H_
