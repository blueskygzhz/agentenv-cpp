// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/io_buffer.rs — IOBuffer / UserBuffer / AutoRegBuffer.
//
// Two buffer shapes, and the difference is a protocol difference rather than
// an implementation detail:
//
//   * `UserBuffer` owns aligned memory, registers it with the ring, and hands
//     its address to the kernel in `ublksrv_io_cmd::addr`.
//   * `AutoRegBuffer` owns no memory at all. Under `UBLK_F_AUTO_BUF_REG` the
//     kernel installs the request's own pages into a fixed-buffer slot, so the
//     server only reserves the index -- and `addr` must be left zero, with the
//     slot travelling in the SQE's `addr` field instead.
//
// Sending the wrong one of those two is not a performance mistake: a non-zero
// `addr` under auto-registration is a protocol violation, and a zero `addr`
// without it points the kernel at address zero.
#ifndef AGENTENV_STORAGE_UBLK_IO_BUFFER_H_
#define AGENTENV_STORAGE_UBLK_IO_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/storage/ublk/abi.h"
#include "agentenv/storage/util/io_ring.h"

namespace agentenv {
namespace storage {
namespace ublk {

/// Rust `UserBuffer` — aligned memory registered as a fixed buffer.
class UserBuffer {
 public:
    /// Rust `UserBuffer::new(ring, size, align)`.
    ///
    /// Allocation and registration are one step because they cannot be
    /// separated safely: the kernel pins these pages, so a buffer that were
    /// freed or moved after registration would leave the ring pointing at
    /// memory this process no longer owns.
    static core::Expected<std::shared_ptr<UserBuffer>, std::string> Create(
        std::shared_ptr<util::Ring> ring, std::size_t size, std::size_t align);

    ~UserBuffer();
    UserBuffer(const UserBuffer&) = delete;
    UserBuffer& operator=(const UserBuffer&) = delete;

    uint8_t* Data() { return data_; }
    const uint8_t* Data() const { return data_; }
    std::size_t Size() const { return size_; }
    /// Rust `uring_buf_idx`.
    uint16_t UringBufIndex() const { return static_cast<uint16_t>(index_); }

    /// Rust `subslice` / `subslice_mut`, as a checked pointer.
    ///
    /// Returns null rather than trapping on an out-of-range request: the
    /// extents come from a descriptor the kernel wrote, so a bad one is
    /// untrusted input to be rejected, not a bug in this process to abort on.
    uint8_t* Subslice(uint64_t offset, std::size_t len);
    const uint8_t* Subslice(uint64_t offset, std::size_t len) const;

 private:
    UserBuffer();

    std::shared_ptr<util::Ring> ring_;
    uint8_t* data_;
    std::size_t size_;
    uint32_t index_;
};

/// Rust `AutoRegBuffer` — a reserved fixed-buffer slot and nothing else.
class AutoRegBuffer {
 public:
    /// Rust `AutoRegBuffer::new(ring, size)`.
    ///
    /// `size` is recorded but nothing is allocated: it is what the descriptor
    /// will say the request spans, and the pages come from the kernel.
    static core::Expected<std::shared_ptr<AutoRegBuffer>, std::string> Create(
        std::shared_ptr<util::Ring> ring, std::size_t size);

    ~AutoRegBuffer();
    AutoRegBuffer(const AutoRegBuffer&) = delete;
    AutoRegBuffer& operator=(const AutoRegBuffer&) = delete;

    std::size_t Size() const { return size_; }
    uint16_t Index() const { return reg_.index; }
    /// Rust `as_ublk_auto_buf_reg`.
    const abi::AutoBufReg& Raw() const { return reg_; }

 private:
    AutoRegBuffer();

    std::shared_ptr<util::Ring> ring_;
    abi::AutoBufReg reg_;
    std::size_t size_;
};

/// Rust `enum IOBuffer` — whichever of the two a slot was armed with.
///
/// A tagged pair rather than a base class with virtuals: the queue does not
/// call methods on it, it asks which shape it is in order to decide what goes
/// in `ublksrv_io_cmd::addr` and what goes in the SQE. Those two answers are
/// the entire interface.
class IoBuffer {
 public:
    IoBuffer() {}
    static IoBuffer OfUser(std::shared_ptr<UserBuffer> buf);
    static IoBuffer OfAutoReg(std::shared_ptr<AutoRegBuffer> buf);

    bool IsAutoReg() const { return auto_reg_ != NULL; }
    bool IsUser() const { return user_ != NULL; }
    bool Empty() const { return !IsAutoReg() && !IsUser(); }

    const std::shared_ptr<UserBuffer>& User() const { return user_; }
    const std::shared_ptr<AutoRegBuffer>& AutoReg() const { return auto_reg_; }

    /// Rust `IOBuffer::len`.
    std::size_t Size() const;
    /// Rust `IOBuffer::uring_buf_idx`.
    uint16_t UringBufIndex() const;

    /// What belongs in `ublksrv_io_cmd::addr` for this buffer.
    ///
    /// Zero for auto-registration, which Linux requires: the kernel supplies
    /// the pages there and reads a non-zero value as a protocol violation.
    uint64_t CmdAddr() const;

    /// What belongs in the SQE's `addr`, or absent.
    ///
    /// Only auto-registration uses it, to carry the reserved slot. The two
    /// fields are mutually exclusive by design -- whichever one is unused for
    /// a given mode must stay zero.
    bool SqeAddr(uint64_t* out) const;

 private:
    std::shared_ptr<UserBuffer> user_;
    std::shared_ptr<AutoRegBuffer> auto_reg_;
};

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_IO_BUFFER_H_
