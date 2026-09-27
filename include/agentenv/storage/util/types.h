// SPDX-License-Identifier: MIT
// Rust: storage/util/ — offsets, ranges, bytes helpers.
#ifndef AGENTENV_STORAGE_UTIL_TYPES_H_
#define AGENTENV_STORAGE_UTIL_TYPES_H_

#include <cstdint>

namespace agentenv {
namespace storage {
namespace util {

/// Half-open [begin, end) byte range.
struct Range {
    int64_t begin = 0;
    int64_t end   = 0;
    int64_t Len() const { return end - begin; }
    bool Contains(int64_t p) const { return p >= begin && p < end; }
    bool Overlaps(const Range& o) const { return begin < o.end && o.begin < end; }
};

}  // namespace util
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UTIL_TYPES_H_
