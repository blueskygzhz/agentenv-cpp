// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/p2p/cache.rs
//
// A TTL cache in front of P2P lookups. The reason it exists: overlaybd issues
// a lookup per byte-range read, so an uncached miss would put a discovery
// round-trip on every read of a layer that simply is not in the catalog.
//
// Negative results are cached too, but with a *much* shorter TTL than hits —
// a layer that is absent now is likely to be published soon, whereas a layer
// that exists rarely disappears.
#ifndef AGENTENV_OVERLAYBD_P2P_CACHE_H_
#define AGENTENV_OVERLAYBD_P2P_CACHE_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "agentenv/core/optional.h"
#include "agentenv/p2p/types.h"

namespace agentenv {
namespace overlaybd {
namespace p2p {

/// Rust struct `DescriptorCache`.
///
/// Rust uses a `DashMap` for lock-free sharding; this uses one mutex, which is
/// adequate because every operation is a short map access and the expensive
/// work (the lookup itself) happens outside the lock.
class DescriptorCache {
 public:
    /// Rust `new`. `max_entries == 0` disables caching entirely.
    DescriptorCache(uint64_t hit_ttl_ms, uint64_t miss_ttl_ms, std::size_t max_entries);

    /// Rust `get` — the nesting is meaningful:
    ///   * outer absent  => nothing cached, the caller must look up;
    ///   * outer present, inner absent => a cached *miss*;
    ///   * both present  => a cached descriptor.
    core::Optional<core::Optional<agentenv::p2p::P2pArtifactDescriptor> > Get(
        const agentenv::p2p::P2pArtifactKey& key);

    /// Rust `insert`. Pass an empty inner optional to record a miss.
    void Insert(const agentenv::p2p::P2pArtifactKey& key,
                const core::Optional<agentenv::p2p::P2pArtifactDescriptor>& descriptor);

    /// Rust `remove` — used after publishing, so the new artifact is visible
    /// immediately instead of after the miss TTL.
    void Remove(const agentenv::p2p::P2pArtifactKey& key);

    /// Current entry count, for tests.
    std::size_t Size();

    /// Monotonic clock hook. Tests override it to advance time without
    /// sleeping; Rust's `Instant` is not injectable, so its own tests can only
    /// cover size-based pruning.
    void SetClockForTesting(uint64_t (*clock_ms)());

 private:
    struct CachedDescriptor {
        uint64_t inserted_at_ms = 0;
        /// Monotonic insertion counter, used to break ties when pruning.
        ///
        /// Rust compares `Instant`s, which have nanosecond resolution, so its
        /// insertion order is always recoverable. A millisecond clock is not:
        /// several inserts can share a timestamp, which would make
        /// oldest-first eviction pick an arbitrary victim. The counter makes
        /// the order exact regardless of clock resolution.
        uint64_t sequence = 0;
        core::Optional<agentenv::p2p::P2pArtifactDescriptor> descriptor;
    };

    uint64_t NowMs() const;
    uint64_t TtlFor(const CachedDescriptor& cached) const;
    /// Rust `prune_if_needed` — expired entries first, then oldest-first.
    void PruneIfNeededLocked();

    mutable std::mutex mutex_;
    std::map<agentenv::p2p::P2pArtifactKey, CachedDescriptor> entries_;
    uint64_t hit_ttl_ms_;
    uint64_t miss_ttl_ms_;
    std::size_t max_entries_;
    uint64_t (*clock_ms_)();
    uint64_t next_sequence_;
};

}  // namespace p2p
}  // namespace overlaybd
}  // namespace agentenv
#endif  // AGENTENV_OVERLAYBD_P2P_CACHE_H_
