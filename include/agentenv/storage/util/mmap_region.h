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

    /// Rust `MMapRegion::from_fd(fd, offset, len)` — map an already-open file.
    ///
    /// Always `PROT_READ | PROT_WRITE` and `MAP_SHARED`, matching Rust: the
    /// cache refill path writes blocks straight into the mapping, and shared
    /// is what makes those stores reach the file rather than a private copy.
    ///
    /// A zero length is refused. `mmap` would reject it with EINVAL anyway,
    /// but the caller deriving `len` from a file size needs to hear about an
    /// empty file as its own error rather than as a confusing mmap failure.
    static core::Expected<MMapRegion, std::string>
        FromFd(int fd, uint64_t offset, size_t len);

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

    /// Rust `MMapRegion::get_mut` — a writable view of the same bytes.
    ///
    /// Returns null when the range falls outside the mapping, which is the
    /// one failure mode that matters: an out-of-bounds store through an mmap
    /// is a segfault, not an error return.
    ///
    /// Writing the returned bytes still requires the disk range to have been
    /// reserved first (`sys::ReserveSpace`). Storing into an unallocated hole
    /// of a sparse file makes the kernel allocate at fault time, where
    /// `ENOSPC` cannot be reported and becomes `SIGBUS`.
    ///
    /// Callers must also guarantee exclusive access to the range. In the
    /// cache that comes from the refill election, which admits at most one
    /// loader per block.
    uint8_t* GetMut(size_t offset, size_t len);

 private:
    MMapRegion() {}
    uint8_t* base_ = nullptr;
  size_t   len_  = 0;
};

}  // namespace util
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UTIL_MMAP_REGION_H_
