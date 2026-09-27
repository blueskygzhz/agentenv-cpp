// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/p2p/cache.rs
#include "agentenv/overlaybd/p2p/cache.h"

#include <algorithm>
#include <vector>

#include <time.h>

namespace agentenv {
namespace overlaybd {
namespace p2p {
namespace {

using core::Optional;

/// Monotonic, so a wall-clock adjustment cannot make entries look expired (or
/// immortal). Rust's `Instant` has the same guarantee.
uint64_t DefaultClockMs() {
    struct timespec ts;
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
}

}  // namespace

DescriptorCache::DescriptorCache(uint64_t hit_ttl_ms, uint64_t miss_ttl_ms,
                                 std::size_t max_entries)
    : hit_ttl_ms_(hit_ttl_ms),
      miss_ttl_ms_(miss_ttl_ms),
      max_entries_(max_entries),
      clock_ms_(&DefaultClockMs),
      next_sequence_(0) {}

void DescriptorCache::SetClockForTesting(uint64_t (*clock_ms)()) {
    std::lock_guard<std::mutex> guard(mutex_);
    clock_ms_ = clock_ms != NULL ? clock_ms : &DefaultClockMs;
}

uint64_t DescriptorCache::NowMs() const { return clock_ms_(); }

uint64_t DescriptorCache::TtlFor(const CachedDescriptor& cached) const {
    // A hit and a miss expire on very different schedules; see the header.
    return cached.descriptor.has_value() ? hit_ttl_ms_ : miss_ttl_ms_;
}

Optional<Optional<agentenv::p2p::P2pArtifactDescriptor> > DescriptorCache::Get(
    const agentenv::p2p::P2pArtifactKey& key) {
    std::lock_guard<std::mutex> guard(mutex_);

    const std::map<agentenv::p2p::P2pArtifactKey, CachedDescriptor>::iterator found =
        entries_.find(key);
    if (found == entries_.end()) {
        return Optional<Optional<agentenv::p2p::P2pArtifactDescriptor> >();
    }

    const uint64_t now = NowMs();
    // Guard against a clock that went backwards: treating that as a huge
    // elapsed time would evict everything.
    const uint64_t elapsed =
        now >= found->second.inserted_at_ms ? now - found->second.inserted_at_ms : 0;
    if (elapsed <= TtlFor(found->second)) {
        return Optional<Optional<agentenv::p2p::P2pArtifactDescriptor> >(found->second.descriptor);
    }

    // Expired entries are dropped on read, so a key that is never read again
    // still gets cleaned up by the size-based prune.
    entries_.erase(found);
    return Optional<Optional<agentenv::p2p::P2pArtifactDescriptor> >();
}

void DescriptorCache::Insert(const agentenv::p2p::P2pArtifactKey& key,
                             const Optional<agentenv::p2p::P2pArtifactDescriptor>& descriptor) {
    std::lock_guard<std::mutex> guard(mutex_);

    CachedDescriptor cached;
    cached.inserted_at_ms = NowMs();
    cached.sequence = next_sequence_++;
    cached.descriptor = descriptor;
    entries_[key] = cached;

    PruneIfNeededLocked();
}

void DescriptorCache::Remove(const agentenv::p2p::P2pArtifactKey& key) {
    std::lock_guard<std::mutex> guard(mutex_);
    entries_.erase(key);
}

std::size_t DescriptorCache::Size() {
    std::lock_guard<std::mutex> guard(mutex_);
    return entries_.size();
}

void DescriptorCache::PruneIfNeededLocked() {
    if (max_entries_ == 0) {
        // Caching disabled: keep nothing rather than growing unbounded.
        entries_.clear();
        return;
    }
    if (entries_.size() <= max_entries_) return;

    const uint64_t now = NowMs();

    // First pass: drop anything already expired. This is preferred over
    // evicting live entries, which would cost a fresh lookup.
    std::vector<agentenv::p2p::P2pArtifactKey> expired;
    for (std::map<agentenv::p2p::P2pArtifactKey, CachedDescriptor>::const_iterator it =
             entries_.begin();
         it != entries_.end(); ++it) {
        const uint64_t elapsed =
            now >= it->second.inserted_at_ms ? now - it->second.inserted_at_ms : 0;
        if (elapsed > TtlFor(it->second)) expired.push_back(it->first);
    }
    for (std::size_t i = 0; i < expired.size(); ++i) entries_.erase(expired[i]);

    if (entries_.size() <= max_entries_) return;

    // Second pass: oldest-first by insertion sequence.
    std::vector<std::pair<uint64_t, agentenv::p2p::P2pArtifactKey> > ordered;
    ordered.reserve(entries_.size());
    for (std::map<agentenv::p2p::P2pArtifactKey, CachedDescriptor>::const_iterator it =
             entries_.begin();
         it != entries_.end(); ++it) {
        ordered.push_back(std::make_pair(it->second.sequence, it->first));
    }
    std::sort(ordered.begin(), ordered.end());

    const std::size_t remove_count = entries_.size() - max_entries_;
    for (std::size_t i = 0; i < remove_count && i < ordered.size(); ++i) {
        entries_.erase(ordered[i].second);
    }
}

}  // namespace p2p
}  // namespace overlaybd
}  // namespace agentenv
