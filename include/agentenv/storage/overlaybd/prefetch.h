// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/{prefetch.rs, backend/} — prefetch + remote backends.
#ifndef AGENTENV_STORAGE_OVERLAYBD_PREFETCH_H_
#define AGENTENV_STORAGE_OVERLAYBD_PREFETCH_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust: storage/overlaybd/src/backend/ — a pluggable remote blob backend
/// (registry HTTP, object store, local file).
class Backend {
 public:
    virtual ~Backend() {}
    /// Rust: fetch bytes [offset, offset+len) of a blob identified by `key`.
    virtual core::Expected<int64_t, std::string>
        ReadRange(const std::string& key, uint64_t offset,
      uint8_t* dst, int64_t len) = 0;
    virtual core::Expected<int64_t, std::string>
        BlobSize(const std::string& key) = 0;
};

/// Rust struct `Prefetch` — a trace-driven readahead scheduler (prefetch.rs).
struct PrefetchEntry {
    uint64_t offset = 0;
    uint32_t length = 0;
};

class Prefetcher {
 public:
    virtual ~Prefetcher() {}
    /// Rust: load a prefetch trace and warm the cache in the background.
    virtual void Submit(const std::vector<PrefetchEntry>& trace) = 0;
  virtual void Stop() = 0;
};

std::unique_ptr<Prefetcher> MakePrefetcher(std::shared_ptr<Backend> backend);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_PREFETCH_H_
