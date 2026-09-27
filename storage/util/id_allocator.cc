// SPDX-License-Identifier: MIT
// Rust: storage/util/src/id_allocator.rs
#include "agentenv/storage/util/id_allocator.h"

#include <algorithm>
#include <cassert>

namespace agentenv {
namespace storage {
namespace util {

ReloadableIDAllocator::ReloadableIDAllocator(uint32_t initial_idx)
    : next_id_(initial_idx) {}

void ReloadableIDAllocator::InsertFreeId(uint32_t id) {
    if (id >= next_id_) return;
    InsertKnownFreeId(id);
}

void ReloadableIDAllocator::InsertKnownFreeId(uint32_t id) {
    if (free_ids_lookup_.count(id)) return;
    free_ids_lookup_.insert(id);
    free_ids_.push_back(id);
}

bool ReloadableIDAllocator::RemoveFreeId(uint32_t id) {
    if (free_ids_lookup_.erase(id) == 0) return false;
    std::deque<uint32_t>::iterator it =
 std::find(free_ids_.begin(), free_ids_.end(), id);
    if (it != free_ids_.end()) free_ids_.erase(it);
    return true;
}

void ReloadableIDAllocator::OccupyIdx(uint32_t id) {
    assert(id <= 100000u && "try occupy big id");
    std::lock_guard<std::mutex> g(mu_);
    if (RemoveFreeId(id)) return;
    for (uint32_t v = next_id_; v < id; ++v) {
    InsertKnownFreeId(v);
    }
    if (next_id_ <= id) next_id_ = id + 1;
}

uint32_t ReloadableIDAllocator::Allocate() {
    std::lock_guard<std::mutex> g(mu_);
    if (!free_ids_.empty()) {
  uint32_t id = free_ids_.front();
        free_ids_.pop_front();
  free_ids_lookup_.erase(id);
        return id;
    }
    uint32_t res = next_id_;
    ++next_id_;
    return res;
}

void ReloadableIDAllocator::Recycle(uint32_t id) {
    std::lock_guard<std::mutex> g(mu_);
  InsertFreeId(id);
}

bool ReloadableIDAllocator::IsFree(uint32_t id) const {
    std::lock_guard<std::mutex> g(mu_);
    if (free_ids_lookup_.count(id)) return true;
    return id >= next_id_;
}

}  // namespace util
}  // namespace storage
}  // namespace agentenv
