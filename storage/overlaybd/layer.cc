// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/layer/
#include "agentenv/storage/overlaybd/layer.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

std::unique_ptr<LayerStack> MakeLayerStack() {
    return nullptr;  // TODO: concrete stack once VirtualFile impls are ported.
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
