// SPDX-License-Identifier: MIT
// Rust: storage/util/src/io_ring/ — io_uring worker + IoRingHandle.
//   Rust exposes `spawn_io_ring_worker<E>(worker_id) -> (IoRingHandle, JoinHandle)`.
// C++11 skeleton: an interface only; the real impl is gated on AGENTENV_WITH_LIBURING.
#ifndef AGENTENV_STORAGE_UTIL_IO_RING_H_
#define AGENTENV_STORAGE_UTIL_IO_RING_H_

#include <cstdint>
#include <functional>
#include <future>
#include <memory>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace util {

/// One submission entry — the C++ analogue of `io_uring::squeue::Entry`.
struct IoSqe {
 uint8_t  op = 0;        // read/write/fsync...
    int32_t  fd = -1;
    uint64_t offset = 0;
    void*    buf = nullptr;
    uint32_t len = 0;
};

/// Rust `IoRingHandle` — cross-thread submission handle to a ring worker.
class IoRingHandle {
 public:
    virtual ~IoRingHandle() {}
    /// Submit an sqe; the future resolves with bytes done (>=0) or -errno.
    virtual std::future<int32_t> Submit(const IoSqe& sqe) = 0;
    /// Ask the worker to drain and exit.
    virtual void Shutdown() = 0;
};

/// Rust `spawn_io_ring_worker` — starts a dedicated worker thread.
/// Skeleton returns nullptr unless AGENTENV_WITH_LIBURING is defined.
std::shared_ptr<IoRingHandle> SpawnIoRingWorker(size_t worker_id);

}  // namespace util
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UTIL_IO_RING_H_
