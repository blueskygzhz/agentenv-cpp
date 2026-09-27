// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/dev.rs — UVMUblkDev / UVMUblkTarget.
#ifndef AGENTENV_STORAGE_UBLK_DEV_H_
#define AGENTENV_STORAGE_UBLK_DEV_H_

#include <cstdint>
#include <memory>

#include "agentenv/core/expected.h"
#include "agentenv/storage/ublk/target.h"

namespace agentenv {
namespace storage {
namespace ublk {

/// Rust trait `UVMUblkTarget` — the per-device data-plane handler.
/// (Alias to the existing `Target` interface for structural parity.)
using UblkTarget = Target;

/// Rust struct `UVMUblkDev` — one active ublk block device.
class UblkDev {
 public:
    virtual ~UblkDev() {}
    virtual uint32_t DevId() const = 0;
    virtual int64_t  DiskBytes() const = 0;
    virtual core::Expected<core::Unit, std::string> Start() = 0;
virtual void Stop() = 0;
};

/// Rust struct `UVMUblkDevBuilder`.
class UblkDevBuilder {
 public:
    UblkDevBuilder& NrHwQueues(uint32_t n) { nr_hw_queues_ = n; return *this; }
    UblkDevBuilder& QueueDepth(uint32_t d) { queue_depth_ = d; return *this; }
    core::Expected<std::unique_ptr<UblkDev>, std::string>
        Build(std::shared_ptr<UblkTarget> target);
 private:
    uint32_t nr_hw_queues_ = 1;
    uint32_t queue_depth_  = 128;
};

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_DEV_H_
