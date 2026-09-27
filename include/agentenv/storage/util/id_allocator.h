// SPDX-License-Identifier: MIT
// Rust: storage/util/src/id_allocator.rs — ReloadableIDAllocator.
#ifndef AGENTENV_STORAGE_UTIL_ID_ALLOCATOR_H_
#define AGENTENV_STORAGE_UTIL_ID_ALLOCATOR_H_

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_set>

namespace agentenv {
namespace storage {
namespace util {

/// Rust struct `ReloadableIDAllocator` — hands out u32 ids, supports recycle
/// and reload-time `occupy_idx`.
class ReloadableIDAllocator {
 public:
    explicit ReloadableIDAllocator(uint32_t initial_idx);

    /// Rust `occupy_idx`: mark `id` occupied (reload path).
  void OccupyIdx(uint32_t id);
    /// Rust `allocate`.
    uint32_t Allocate();
    /// Rust `recycle`.
    void Recycle(uint32_t id);
    /// Rust `is_free`.
    bool IsFree(uint32_t id) const;
    /// Rust `is_occupied`.
    bool IsOccupied(uint32_t id) const { return !IsFree(id); }

 private:
    void InsertFreeId(uint32_t id);   // guarded by caller
    void InsertKnownFreeId(uint32_t id);
    bool RemoveFreeId(uint32_t id);

    mutable std::mutex mu_;
    uint32_t next_id_;
    std::unordered_set<uint32_t> free_ids_lookup_;
    std::deque<uint32_t> free_ids_;
};

}  // namespace util
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UTIL_ID_ALLOCATOR_H_
