// SPDX-License-Identifier: MIT
// Rust: src/image/cache/service.rs — operation-hold lifecycle.
#include "agentenv/image/cache_hold.h"

#include <sstream>

#include "agentenv/core/logging.h"

namespace agentenv {
namespace image {
namespace cache {

const char* const kOperationHoldNamespaceName = "operation";

namespace {

/// Rust's `static NEXT_OPERATION_HOLD_ID: AtomicU64 = AtomicU64::new(1)`.
/// Single-threaded here for the same reason the rest of this port is: the
/// callers are serialised by the service's own locking.
uint64_t g_next_operation_hold_id = 1;

}  // namespace

void ResetOperationHoldCounterForTesting() { g_next_operation_hold_id = 1; }

core::Expected<ImageCacheHoldOwner, std::string> NextOperationHoldOwner(
    const std::string& operation) {
    const core::Expected<std::string, std::string> checked =
        NonEmpty("image cache operation", operation);
    if (!checked.ok()) return core::make_unexpected(checked.error());

    // Monotonic, so two concurrent operations with the same name get distinct
    // owners; sharing one would let the first to finish release the other's
    // protection while it is still running.
    const uint64_t hold_id = g_next_operation_hold_id++;
    std::ostringstream key;
    key << checked.value() << "/" << hold_id;
    return ImageCacheHoldOwner::New(kOperationHoldNamespaceName, key.str());
}

core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string>
ImageCacheOperationHold::AcquireForHardCommits(std::shared_ptr<ImageCacheMetadataStore> store,
                                               const std::string& operation,
                                               const std::set<std::string>& digests) {
    const core::Expected<ImageCacheHoldOwner, std::string> owner =
        NextOperationHoldOwner(operation);
    if (!owner.ok()) return core::make_unexpected(owner.error());

    std::set<HardCommitId> parsed;
    for (std::set<std::string>::const_iterator it = digests.begin(); it != digests.end(); ++it) {
        const core::Expected<HardCommitId, std::string> digest = HardCommitId::New(*it);
        if (!digest.ok()) return core::make_unexpected(digest.error());
        parsed.insert(digest.value());
    }

    std::shared_ptr<ImageCacheOperationHold> hold(
        new ImageCacheOperationHold(store, owner.value()));
    const core::Expected<core::Unit, std::string> written =
        hold->store_->CreateOrReplaceHold(owner.value(), parsed);
    if (!written.ok()) return core::make_unexpected(written.error());

    hold->digests_     = parsed;
    hold->materialized_ = true;
    return hold;
}

core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string>
ImageCacheOperationHold::Begin(std::shared_ptr<ImageCacheMetadataStore> store,
                               const std::string& operation) {
    const core::Expected<ImageCacheHoldOwner, std::string> owner =
        NextOperationHoldOwner(operation);
    if (!owner.ok()) return core::make_unexpected(owner.error());

    // Lazy: nothing is written until the first commit is protected. Most
    // resolves hit a cached config and protect nothing, so writing a hold per
    // resolve would churn the store for no benefit.
    return std::shared_ptr<ImageCacheOperationHold>(
        new ImageCacheOperationHold(store, owner.value()));
}

core::Expected<core::Unit, std::string> ImageCacheOperationHold::Protect(
    const std::string& digest) {
    const core::Expected<HardCommitId, std::string> parsed = HardCommitId::New(digest);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());

    // Idempotent: revisiting the same layer must not rewrite the entry.
    if (digests_.find(parsed.value()) != digests_.end()) return core::Unit();

    std::set<HardCommitId> next = digests_;
    next.insert(parsed.value());

    const core::Expected<core::Unit, std::string> written =
        store_->CreateOrReplaceHold(owner_, next);
    if (!written.ok()) return core::make_unexpected(written.error());

    digests_      = next;
    materialized_ = true;
    return core::Unit();
}

core::Expected<core::Unit, std::string> ImageCacheOperationHold::Release(
    const std::string& reason) {
    if (released_) return core::Unit();
    // Nothing was ever written, so there is nothing to release.
    if (!materialized_) {
        released_ = true;
        return core::Unit();
    }

    const core::Expected<core::Unit, std::string> dropped = store_->ReleaseHold(owner_);
    // Marked released either way: a retry could not do better, and the
    // transient `operation` namespace is reclaimed by the startup stale-hold
    // sweep.
    released_ = true;
    if (!dropped.ok()) {
        AGENTENV_WARN("failed to release image cache operation hold " + owner_.ToString() +
                      " (" + reason + "): " + dropped.error());
        return core::make_unexpected(dropped.error());
    }
    return core::Unit();
}

ImageCacheOperationHold::~ImageCacheOperationHold() {
    // Rust releases from `Drop`; RAII is the closer construct. Best-effort,
    // and deliberately never escalates: leaving a hold behind costs disk,
    // while dropping one too early costs a running operation its inputs.
    if (released_ || !materialized_) return;
    const core::Expected<core::Unit, std::string> dropped = store_->ReleaseHold(owner_);
    if (!dropped.ok()) {
        AGENTENV_WARN("skipping image cache operation hold release for " + owner_.ToString() +
                      ": " + dropped.error());
    }
    released_ = true;
}

}  // namespace cache
}  // namespace image
}  // namespace agentenv
