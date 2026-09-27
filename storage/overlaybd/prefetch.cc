// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/{prefetch.rs, backend/, metrics.rs}
#include "agentenv/storage/overlaybd/prefetch.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

std::unique_ptr<Prefetcher> MakePrefetcher(std::shared_ptr<Backend> /*backend*/) {
    return nullptr;  // TODO: trace-driven readahead worker.
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
