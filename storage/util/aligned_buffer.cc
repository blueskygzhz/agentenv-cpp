// SPDX-License-Identifier: MIT
// Rust: storage/util/src/aligned_buffer.rs
#include "agentenv/storage/util/aligned_buffer.h"

#include <cstdlib>

namespace agentenv {
namespace storage {
namespace util {

static bool is_power_of_two(size_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

core::Expected<AlignedBuffer, std::string>
AlignedBuffer::New(size_t len, size_t align) {
    if (len == 0) {
        return core::make_unexpected(std::string("zero sized AlignedBuffer"));
    }
    if (!is_power_of_two(align)) {
   return core::make_unexpected(std::string("align must be a power of two"));
    }
    // posix_memalign requires align to be a multiple of sizeof(void*).
  size_t eff_align = align < sizeof(void*) ? sizeof(void*) : align;
void* p = nullptr;
    if (::posix_memalign(&p, eff_align, len) != 0 || p == nullptr) {
        return core::make_unexpected(std::string("posix_memalign failed"));
    }
    AlignedBuffer buf;
    buf.ptr_ = static_cast<uint8_t*>(p);
    buf.capacity_ = len;
    buf.align_ = align;
    buf.range_begin_ = 0;
    buf.range_end_ = len;
    return core::Expected<AlignedBuffer, std::string>(std::move(buf));
}

AlignedBuffer::AlignedBuffer(AlignedBuffer&& o) noexcept {
    ptr_ = o.ptr_;
    capacity_ = o.capacity_;
    align_ = o.align_;
    range_begin_ = o.range_begin_;
    range_end_ = o.range_end_;
    o.ptr_ = nullptr;
    o.capacity_ = 0;
    o.range_begin_ = 0;
    o.range_end_ = 0;
}

AlignedBuffer& AlignedBuffer::operator=(AlignedBuffer&& o) noexcept {
    if (this != &o) {
        if (ptr_) ::free(ptr_);
     ptr_ = o.ptr_;
    capacity_ = o.capacity_;
        align_ = o.align_;
    range_begin_ = o.range_begin_;
        range_end_ = o.range_end_;
        o.ptr_ = nullptr;
  o.capacity_ = 0;
        o.range_begin_ = 0;
   o.range_end_ = 0;
    }
    return *this;
}

AlignedBuffer::~AlignedBuffer() {
    if (ptr_) ::free(ptr_);
}

const uint8_t* AlignedBuffer::Data() const {
    return ptr_ ? ptr_ + range_begin_ : nullptr;
}
uint8_t* AlignedBuffer::Data() {
    return ptr_ ? ptr_ + range_begin_ : nullptr;
}

core::Expected<core::Unit, std::string>
AlignedBuffer::IntoSubRange(size_t begin, size_t end) {
    if (begin >= end || end > capacity_) {
  return core::make_unexpected(std::string("invalid subrange for AlignedBuffer"));
    }
    range_begin_ = begin;
    range_end_ = end;
    return core::Unit{};
}

}  // namespace util
}  // namespace storage
}  // namespace agentenv
