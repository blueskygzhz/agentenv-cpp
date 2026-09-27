// SPDX-License-Identifier: MIT
#include "agentenv/image/image.h"

// Anchor TU for the target. ImageLoader's dtor is defaulted in the header, so
// no out-of-line definition is needed. Concrete ImageLoader implementations
// (registry client, local cache) will be added in follow-up TUs.
namespace agentenv { namespace image {}}
