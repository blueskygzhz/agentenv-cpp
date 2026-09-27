// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/queue.rs — UVMUblkQueue + UblkDescOperation.
#ifndef AGENTENV_STORAGE_UBLK_QUEUE_H_
#define AGENTENV_STORAGE_UBLK_QUEUE_H_

#include <cstdint>

namespace agentenv {
namespace storage {
namespace ublk {

/// Rust enum `UblkDescOperation` — what the kernel is asking us to do.
enum class UblkDescOperation : uint8_t {
    Read    = 0,
  Write   = 1,
    Flush   = 2,
    Discard = 3,
    WriteZeroes = 4,
};

/// Rust struct `UVMUblkQueue` — one hardware queue bound to an io_uring.
/// Skeleton: interface only.
class UblkQueue {
 public:
    virtual ~UblkQueue() {}
 virtual uint16_t QueueId() const = 0;
    virtual uint32_t Depth() const = 0;
    /// Rust: submit-and-commit the fetched descriptors for one round.
virtual void RunOnce() = 0;
};

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_QUEUE_H_
