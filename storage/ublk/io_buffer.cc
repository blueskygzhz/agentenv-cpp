// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/io_buffer.rs
#include "agentenv/storage/ublk/io_buffer.h"

#include <cstdlib>

namespace agentenv {
namespace storage {
namespace ublk {

UserBuffer::UserBuffer(size_t size)
    : data_(static_cast<uint8_t*>(size ? std::malloc(size) : nullptr)),
      size_(size) {}

UserBuffer::~UserBuffer() {
    if (data_) std::free(data_);
}

}  // namespace ublk
}  // namespace storage
}// namespace agentenv
