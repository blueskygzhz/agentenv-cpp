// SPDX-License-Identifier: MIT
#include "agentenv/storage/overlaybd/types.h"

#include <algorithm>

namespace agentenv {
namespace storage {
namespace overlaybd {

core::Expected<const SegmentMapping*, core::AnyError>
IndexTree::Lookup(uint64_t vaddr) const {
    // Sorted by vaddr; use upper_bound and step back one.
    auto it = std::upper_bound(
        segments.begin(), segments.end(), vaddr,
        [](uint64_t v, const SegmentMapping& s) { return v < s.vaddr; });
    if (it == segments.begin())
        return core::make_unexpected(core::err("vaddr not covered"));
    --it;
    if (vaddr < it->vaddr + it->length) return &(*it);
    return core::make_unexpected(core::err("vaddr not covered"));
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
