// SPDX-License-Identifier: MIT
// Rust: storage/util/src/aligned_buffer.rs — AlignedBuffer.
//
// A heap-allocated byte buffer whose starting address is aligned to a given
// power-of-two boundary. Required by Linux O_DIRECT I/O.
#ifndef AGENTENV_STORAGE_UTIL_ALIGNED_BUFFER_H_
#define AGENTENV_STORAGE_UTIL_ALIGNED_BUFFER_H_

#include <cstddef>
#include <cstdint>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace util {

/// Rust struct `AlignedBuffer`. Move-only (matches Rust `Send`, not `Sync`).
class AlignedBuffer {
 public:
  /// Rust `AlignedBuffer::new(len, align)`.
    static core::Expected<AlignedBuffer, std::string>
        New(size_t len, size_t align);

    AlignedBuffer(AlignedBuffer&& o) noexcept;
    AlignedBuffer& operator=(AlignedBuffer&& o) noexcept;
    ~AlignedBuffer();

    // Non-copyable (Rust type is not Clone).
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    /// Rust `len()` — length of the visible slice (respects sub-range).
    size_t Len() const { return range_end_ - range_begin_; }
    /// Rust `is_empty()`.
  bool IsEmpty() const { return range_begin_ >= range_end_; }

    /// Rust `as_ref()` / `as_mut()`.
    const uint8_t* Data() const;
    uint8_t*       Data();

    /// Rust `into_sub_range(range)` — narrows the visible slice in place.
    core::Expected<core::Unit, std::string>
        IntoSubRange(size_t begin, size_t end);

    size_t Capacity() const { return capacity_; }
    size_t Align() const { return align_; }

 private:
    AlignedBuffer() {}

    uint8_t* ptr_        = nullptr;
    size_t   capacity_   = 0;   // full allocation size
    size_t   align_      = 0;
    size_t   range_begin_ = 0;
    size_t   range_end_ = 0;
};

}  // namespace util
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UTIL_ALIGNED_BUFFER_H_
