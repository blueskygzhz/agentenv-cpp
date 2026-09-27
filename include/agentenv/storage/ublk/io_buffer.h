// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/io_buffer.rs — IOBuffer / UserBuffer / AutoRegBuffer.
#ifndef AGENTENV_STORAGE_UBLK_IO_BUFFER_H_
#define AGENTENV_STORAGE_UBLK_IO_BUFFER_H_

#include <cstddef>
#include <cstdint>

namespace agentenv {
namespace storage {
namespace ublk {

/// Rust trait `IOBuffer` — a data-plane buffer the target reads/writes into.
class IOBuffer {
 public:
    virtual ~IOBuffer() {}
    virtual uint8_t* Ptr() = 0;
  virtual const uint8_t* Ptr() const = 0;
    virtual size_t Size() const = 0;
};

/// Rust struct `IOBufferView` — a bounded sub-view over an IOBuffer.
struct IOBufferView {
    uint8_t* ptr = nullptr;
    size_t   len = 0;
};

/// Rust struct `UserBuffer` — a plain heap buffer implementing IOBuffer.
class UserBuffer : public IOBuffer {
 public:
    explicit UserBuffer(size_t size);
    ~UserBuffer();
    uint8_t* Ptr() override { return data_; }
    const uint8_t* Ptr() const override { return data_; }
    size_t Size() const override { return size_; }
 private:
    uint8_t* data_ = nullptr;
    size_t   size_ = 0;
};

/// Rust struct `AutoRegBuffer` — io_uring auto-registered buffer (fixed buffer).
/// Skeleton: represented but the real registration needs liburing.
class AutoRegBuffer {
 public:
    explicit AutoRegBuffer(size_t size) : size_(size) {}
  size_t Size() const { return size_; }
    uint16_t Index() const { return index_; }
 private:
    size_t   size_ = 0;
    uint16_t index_ = 0;
};

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_IO_BUFFER_H_
