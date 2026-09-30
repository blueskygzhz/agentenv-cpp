// SPDX-License-Identifier: MIT
// Rust: src/image/cache/service.rs — the operation-hold lifecycle
// (`next_operation_hold_owner`, `acquire_operation_hold_for_hard_commits`,
// `begin_hard_commit_operation`, `ImageCacheOperationHold` + its `Drop`).
//
// An operation hold is a short-lived lease that stops GC from collecting the
// commits an in-flight resolve/import/GC pass depends on. Two properties carry
// the weight:
//
//  * Owners must be unique per hold, even for the same operation name. A
//    monotonic counter provides that: two concurrent resolves of the same
//    image would otherwise share one owner key, and the first to finish would
//    release the second's protection while it is still running.
//
//  * Release must be best-effort and must never turn into a deletion. Rust
//    releases from `Drop`, skipping the release entirely when no runtime is
//    available, because the transient `operation` namespace is reclaimed by
//    the startup stale-hold sweep anyway. Leaving a hold behind costs disk;
//    dropping one too early costs the running operation its inputs.
//
// C++ expresses the `Drop` half as RAII, which is the closer construct: the
// guard releases on scope exit unless it was explicitly released or the hold
// was never materialized.
#ifndef AGENTENV_IMAGE_CACHE_HOLD_H_
#define AGENTENV_IMAGE_CACHE_HOLD_H_

#include <memory>
#include <set>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/image/cache.h"
#include "agentenv/image/cache_metadata_store.h"

namespace agentenv {
namespace image {
namespace cache {

/// Rust's `"operation"` hold namespace — transient, and reclaimed wholesale by
/// the startup stale-hold sweep.
extern const char* const kOperationHoldNamespaceName;

/// Rust `ImageCacheService::next_operation_hold_owner`.
///
/// The id is monotonic across the process, so two concurrent operations with
/// the same name get distinct owners. Sharing an owner would let the first to
/// finish release the other's protection.
core::Expected<ImageCacheHoldOwner, std::string> NextOperationHoldOwner(
    const std::string& operation);

/// Test-only: resets the monotonic counter so a test can assert exact owner
/// keys. Rust's counter is a process-lifetime `AtomicU64` with no reset.
void ResetOperationHoldCounterForTesting();

/// Rust `ImageCacheOperationHold`.
///
/// Created either eagerly (with a known digest set) or lazily, in which case
/// nothing is written to the store until the first commit is protected. The
/// lazy form matters for resolve: most resolves hit a cached config and never
/// need to protect anything, and writing a hold per resolve would make the
/// store churn for no benefit.
class ImageCacheOperationHold {
 public:
    ~ImageCacheOperationHold();

    /// Rust `ImageCacheService::acquire_operation_hold_for_hard_commits` —
    /// eager: the hold exists in the store before this returns.
    static core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string>
        AcquireForHardCommits(std::shared_ptr<ImageCacheMetadataStore> store,
                              const std::string& operation,
                              const std::set<std::string>& digests);

    /// Rust `ImageCacheService::begin_hard_commit_operation` — lazy: the owner
    /// is reserved but nothing is stored until `Protect`.
    static core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string>
        Begin(std::shared_ptr<ImageCacheMetadataStore> store, const std::string& operation);

    const ImageCacheHoldOwner& owner() const { return owner_; }
    const std::set<HardCommitId>& digests() const { return digests_; }
    /// Whether anything has actually been written to the store.
    bool materialized() const { return materialized_; }
    bool released() const { return released_; }

    /// Adds a commit to the hold, writing (or rewriting) the store entry.
    ///
    /// Idempotent: protecting an already-held digest does not rewrite the
    /// entry, so a resolve that revisits the same layer stays cheap.
    core::Expected<core::Unit, std::string> Protect(const std::string& digest);

    /// Rust's explicit release path. Idempotent, so the destructor can call it
    /// unconditionally.
    core::Expected<core::Unit, std::string> Release(const std::string& reason);

 private:
    ImageCacheOperationHold(std::shared_ptr<ImageCacheMetadataStore> store,
                            const ImageCacheHoldOwner& owner)
        : store_(std::move(store)), owner_(owner) {}

    std::shared_ptr<ImageCacheMetadataStore> store_;
    ImageCacheHoldOwner                      owner_;
    std::set<HardCommitId>                   digests_;
    bool                                     materialized_ = false;
    bool                                     released_     = false;
};

}  // namespace cache
}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_CACHE_HOLD_H_
