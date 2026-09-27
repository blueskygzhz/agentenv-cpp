// SPDX-License-Identifier: MIT
// Rust: src/snapshot/types/snapshot.rs
#include "agentenv/snapshot/layers.h"

namespace agentenv {
namespace snapshot {

bool LayerRefsEqual(const std::vector<OverlaybdLayerRef>& left,
                    const std::vector<OverlaybdLayerRef>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i] != right[i]) return false;
    }
    return true;
}

}  // namespace snapshot
}  // namespace agentenv
