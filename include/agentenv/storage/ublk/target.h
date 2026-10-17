// SPDX-License-Identifier: MIT
// An early abstraction with no counterpart in the Rust tree.
//
// SUPERSEDED by `UblkTarget` in `dev.h`, which is the port of Rust's
// `UVMUblkTarget`. The differences are not cosmetic: this one has no queue
// id, no per-slot buffers and no notion of the auto-registered buffer mode,
// so it cannot express what the data plane actually asks of a target.
// `TargetRunner` has no Rust counterpart at all -- Rust drives queues from
// `UVMUblkDev`, not from a runner object.
//
// Kept only because `impls.h` still names these types. Nothing in the
// production path constructs either, and new code should use `dev.h`.
#ifndef AGENTENV_STORAGE_UBLK_TARGET_H_
#define AGENTENV_STORAGE_UBLK_TARGET_H_

#include <cstdint>
#include <memory>
#include <string>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace ublk {

/// A single IO request delivered from the ublk kernel driver.
struct IoRequest {
    uint32_t tag       = 0;
    uint8_t  op        = 0;   // 0=read, 1=write, 2=flush, 3=discard
    uint64_t offset    = 0;   // in bytes
    uint32_t length    = 0;   // in bytes
    void*    buffer    = nullptr;  // for read: destination; for write: source
};

/// Result posted back to the kernel.
struct IoResult {
    uint32_t tag = 0;
    int32_t  status = 0;   // >=0 bytes done; <0 = -errno
};

/// User-provided target trait — matches Rust `trait Target`.
class Target {
 public:
    virtual ~Target() = default;
    virtual int64_t DiskBytes() const = 0;   // device size
    virtual IoResult Handle(const IoRequest& req) = 0;
};

/// Register a target with the ublk driver at /dev/ublk-control. Only compiles
/// on Linux with AGENTENV_WITH_UBLK on; skeleton returns "not implemented".
class TargetRunner {
 public:
    virtual ~TargetRunner() = default;
    virtual core::Expected<core::Unit, core::AnyError> Run(std::shared_ptr<Target>) = 0;
    virtual void Stop() = 0;
};

std::unique_ptr<TargetRunner> MakeRunner();

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_TARGET_H_
