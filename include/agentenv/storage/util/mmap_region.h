// SPDX-License-Identifier: MIT
// Rust: storage/util/src/mmap_region.rs — MMapRegion / MMapRegionSlice.
#ifndef AGENTENV_STORAGE_UTIL_MMAP_REGION_H_
#define AGENTENV_STORAGE_UTIL_MMAP_REGION_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace util {

/// Rust struct `MMapRegion` — an mmap(2)-backed region over a file.
class MMapRegion {
 public:
    /// Rust `MMapRegion::open(path, writable)`.
    static core::Expected<MMapRegion, std::string>
        Open(const std::string& path, bool writable);

    MMapRegion(MMapRegion&& o) noexcept;
    MMapRegion& operator=(MMapRegion&& o) noexcept;
    ~MMapRegion();
    MMapRegion(const MMapRegion&) = delete;
    MMapRegion& operator=(const MMapRegion&) = delete;

    const uint8_t* Data() const { return base_; }
size_t Len() const { return len_; }

    /// Rust `MMapRegionSlice` — a bounds-checked view.
    core::Expected<const uint8_t*, std::string>
        Slice(size_t offset, size_t len) const;

 private:
    MMapRegion() {}
    uint8_t* base_ = nullptr;
  size_t   len_  = 0;
};

}  // namespace util
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UTIL_MMAP_REGION_H_
