// SPDX-License-Identifier: MIT
// Rust: storage/util/src/io_ring/
#include "agentenv/storage/util/io_ring.h"

namespace agentenv {
namespace storage {
namespace util {

std::shared_ptr<IoRingHandle> SpawnIoRingWorker(size_t /*worker_id*/) {
#ifdef AGENTENV_WITH_LIBURING
    // TODO: create io_uring, spawn worker thread, return concrete handle.
    return nullptr;
#else
    return nullptr;  // io_uring not compiled in.
#endif
}

}  // namespace util
}  // namespace storage
}  // namespace agentenv
