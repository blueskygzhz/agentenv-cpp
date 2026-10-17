// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/io_buffer.rs
#include "agentenv/storage/ublk/io_buffer.h"

#include <cstdlib>
#include <cstring>
#include <sstream>

namespace agentenv {
namespace storage {
namespace ublk {

// ---------------------------------------------------------------------------
// UserBuffer
// ---------------------------------------------------------------------------

UserBuffer::UserBuffer() : data_(NULL), size_(0), index_(0) {}

UserBuffer::~UserBuffer() {
    // Unregistered before the memory is released, never after: the kernel has
    // these pages pinned, and freeing them first leaves the ring holding a
    // registration over memory the allocator may already have reused.
    if (ring_ && data_ != NULL) {
        ring_->UnregisterBuffer(index_);
    }
    if (data_ != NULL) std::free(data_);
}

core::Expected<std::shared_ptr<UserBuffer>, std::string> UserBuffer::Create(
    std::shared_ptr<util::Ring> ring, std::size_t size, std::size_t align) {
    if (!ring) {
        return core::make_unexpected(std::string("UserBuffer: null ring"));
    }
    if (size == 0) {
        return core::make_unexpected(std::string("UserBuffer size cannot be zero"));
    }
    // `posix_memalign` requires a power-of-two multiple of sizeof(void*).
    // Checked here so the failure names the alignment rather than surfacing
    // as a bare EINVAL from the allocator.
    if (align == 0 || (align & (align - 1)) != 0 || align % sizeof(void*) != 0) {
        std::ostringstream why;
        why << "UserBuffer: alignment " << align
            << " must be a power of two and a multiple of " << sizeof(void*);
        return core::make_unexpected(why.str());
    }

    void* raw = NULL;
    if (::posix_memalign(&raw, align, size) != 0 || raw == NULL) {
        std::ostringstream why;
        why << "allocate UserBuffer failed, size = " << size << " align = " << align;
        return core::make_unexpected(why.str());
    }

    std::shared_ptr<UserBuffer> buf(new UserBuffer());
    buf->data_ = static_cast<uint8_t*>(raw);
    buf->size_ = size;

    const core::Expected<uint32_t, std::string> idx = ring->RegisterBuffer(raw, size);
    if (!idx.ok()) {
        // `ring_` is still unset, so the destructor will not try to
        // unregister a slot that was never taken -- it only frees.
        return core::make_unexpected(idx.error());
    }
    buf->index_ = idx.value();
    // Assigned last: it is what the destructor keys its unregister off, so
    // setting it earlier would make a failed registration release a slot
    // belonging to someone else.
    buf->ring_ = ring;
    return buf;
}

uint8_t* UserBuffer::Subslice(uint64_t offset, std::size_t len) {
    // Overflow-safe: `offset + len` on a descriptor-supplied pair can wrap and
    // then compare as in-range.
    if (offset > size_ || len > size_ - static_cast<std::size_t>(offset)) return NULL;
    return data_ + offset;
}

const uint8_t* UserBuffer::Subslice(uint64_t offset, std::size_t len) const {
    if (offset > size_ || len > size_ - static_cast<std::size_t>(offset)) return NULL;
    return data_ + offset;
}

// ---------------------------------------------------------------------------
// AutoRegBuffer
// ---------------------------------------------------------------------------

AutoRegBuffer::AutoRegBuffer() : size_(0) {
    std::memset(&reg_, 0, sizeof(reg_));
}

AutoRegBuffer::~AutoRegBuffer() {
    if (ring_) {
        // Only the index is given back. Nothing was ever registered in the
        // slot, so there are no kernel-side pages to unpin.
        ring_->ReleaseSparseBufferIndex(reg_.index);
    }
}

core::Expected<std::shared_ptr<AutoRegBuffer>, std::string> AutoRegBuffer::Create(
    std::shared_ptr<util::Ring> ring, std::size_t size) {
    if (!ring) {
        return core::make_unexpected(std::string("AutoRegBuffer: null ring"));
    }
    const core::Expected<uint32_t, std::string> idx = ring->OccupySparseBufferIndex();
    if (!idx.ok()) return core::make_unexpected(idx.error());
    // The slot index is 16 bits in `ublk_auto_buf_reg`, so a table larger than
    // that cannot be addressed by the protocol at all.
    if (idx.value() > 0xFFFF) {
        ring->ReleaseSparseBufferIndex(idx.value());
        return core::make_unexpected(
            std::string("AutoRegBuffer: buffer index exceeds the protocol's 16 bits"));
    }

    std::shared_ptr<AutoRegBuffer> buf(new AutoRegBuffer());
    buf->reg_.index = static_cast<uint16_t>(idx.value());
    buf->reg_.flags = 0;
    buf->size_ = size;
    buf->ring_ = ring;
    return buf;
}

// ---------------------------------------------------------------------------
// IoBuffer
// ---------------------------------------------------------------------------

IoBuffer IoBuffer::OfUser(std::shared_ptr<UserBuffer> buf) {
    IoBuffer out;
    out.user_ = buf;
    return out;
}

IoBuffer IoBuffer::OfAutoReg(std::shared_ptr<AutoRegBuffer> buf) {
    IoBuffer out;
    out.auto_reg_ = buf;
    return out;
}

std::size_t IoBuffer::Size() const {
    if (user_) return user_->Size();
    if (auto_reg_) return auto_reg_->Size();
    return 0;
}

uint16_t IoBuffer::UringBufIndex() const {
    if (user_) return user_->UringBufIndex();
    if (auto_reg_) return auto_reg_->Index();
    return 0;
}

uint64_t IoBuffer::CmdAddr() const {
    // Linux requires `addr` to be unset under AUTO_BUF_REG, ZERO_COPY and
    // USER_COPY: the kernel owns the pages in those modes, and a non-zero
    // value here is rejected as a protocol violation.
    if (auto_reg_) return 0;
    if (user_) return reinterpret_cast<uint64_t>(user_->Data());
    return 0;
}

bool IoBuffer::SqeAddr(uint64_t* out) const {
    if (!auto_reg_) return false;
    // The reserved slot rides in the SQE's `addr` instead, packed the way the
    // kernel unpacks it.
    if (out != NULL) *out = abi::AutoBufRegToSqeAddr(auto_reg_->Raw());
    return true;
}

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
