// SPDX-License-Identifier: MIT
// Rust: src/image/resolver.rs — `ResolvedBlockImage`.
#include "agentenv/image/layers.h"

namespace agentenv {
namespace image {

bool ResolvedBlockImage::operator==(const ResolvedBlockImage& other) const {
    if (image_ref != other.image_ref) return false;
    if (overlaybd_config_path != other.overlaybd_config_path) return false;
    if (base_context != other.base_context) return false;
    if (raw_config.has_value() != other.raw_config.has_value()) return false;
    if (!raw_config.has_value()) return true;
    // Textual comparison, as in `ImageResolutionMetadata`: the document is an
    // opaque pass-through and `core::Json` has no structural equality.
    return raw_config->ToString() == other.raw_config->ToString();
}

}  // namespace image
}  // namespace agentenv
