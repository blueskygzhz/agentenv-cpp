// SPDX-License-Identifier: MIT
// Rust: src/image/cache/service.rs — the deleting-GC engine
// (`check_collectable_hard_commit`, `delete_collectable_hard_commit`,
// `commit_file_block_reason`, `run_gc`).
//
// GC is fail-closed. A hard commit is deleted only when every one of these
// holds, and anything that cannot be *proved* safe is reported as blocked
// rather than collected:
//   * no source config roots it;
//   * no named hold pins it;
//   * no live runtime references it;
//   * its recorded file is inside the commit store, is a regular file, and its
//     on-disk size matches the record.
//
// The last group is what makes the check verifiable rather than advisory: a
// commit whose file moved, vanished or changed size is not something GC can
// reason about, so it is left alone and surfaced as `Unverifiable`.
//
// Re-check under hold. `run_gc` decides twice: once from the initial scan, and
// again after taking an operation hold on the candidate. Source-config publish
// is not serialised by that hold, so a config could start rooting the commit
// between the two points; the second check reads the roots fresh and ignores
// only GC's own hold.
#ifndef AGENTENV_IMAGE_CACHE_GC_H_
#define AGENTENV_IMAGE_CACHE_GC_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/image/cache.h"
#include "agentenv/image/cache_metadata_store.h"
#include "agentenv/p2p/transport.h"

namespace agentenv {
namespace image {
namespace cache {

/// Rust `RUNTIME_HOLD_NAMESPACE` / `PAUSED_HOLD_NAMESPACE`.
extern const char* const kRuntimeHoldNamespace;
extern const char* const kPausedHoldNamespace;
/// The namespace GC takes its own short-lived operation hold in.
extern const char* const kOperationHoldNamespace;

/// Rust `type ImageCacheLiveRuntimeRefs` — commits a running sandbox is using,
/// keyed by digest and valued by who is using them.
typedef std::map<HardCommitId, std::vector<ImageCacheHoldOwner> >
    ImageCacheLiveRuntimeRefs;

/// Rust `enum HardCommitGcDecision`.
///
/// `HardCommitId` and `ImageCacheGcBlocked` are both non-default-constructible
/// (the id validates in its factory), so the variants are held in optionals
/// rather than as plain members of a tagged struct.
struct HardCommitGcDecision {
    enum class Kind { Collectable, Blocked };

    Kind kind = Kind::Blocked;

    // Live when kind == Collectable.
    core::Optional<HardCommitId>  digest;
    std::string                   file;
    core::Optional<uint64_t>      size;
    std::set<p2p::P2pArtifactKey> p2p_keys;

    // Live when kind == Blocked.
    core::Optional<ImageCacheGcBlocked> blocked;

    static HardCommitGcDecision MakeCollectable(const HardCommitId& digest,
                                                const std::string& file,
                                                const core::Optional<uint64_t>& size,
                                                const std::set<p2p::P2pArtifactKey>& keys);
    static HardCommitGcDecision MakeBlocked(const ImageCacheGcBlocked& blocked);
};

/// The deleting-GC engine over one commit store.
///
/// Split out of Rust's `ImageCacheService` so it can be driven (and tested)
/// without the conversion/publish half of that type. The service owns the
/// store paths and passes them in.
class ImageCacheGc {
 public:
    /// `commit_store` bounds what GC is willing to delete: a recorded file
    /// outside it is never touched. `transport` may be null, in which case a
    /// commit carrying P2P keys cannot be unpublished and is therefore not
    /// collected.
    ImageCacheGc(const std::string& commit_store,
                 std::shared_ptr<ImageCacheMetadataStore> metadata,
                 std::shared_ptr<p2p::P2pTransport> transport)
        : commit_store_(commit_store),
          metadata_(std::move(metadata)),
          transport_(std::move(transport)) {}

    /// Rust `commit_file_block_reason` — an unset result means the file is
    /// verifiable and safe to consider.
    core::Optional<ImageCacheGcBlockedReason>
        CommitFileBlockReason(const std::string& file,
                              const core::Optional<uint64_t>& size) const;

    /// Rust `check_collectable_hard_commit`.
    ///
    /// `ignore_owner` is GC's own operation hold, which must not block the
    /// commit it was taken for.
    core::Expected<HardCommitGcDecision, std::string>
        CheckCollectableHardCommit(const HardCommitObjectRecord& record,
                                   const std::vector<ImageCacheConfigId>& config_referrers,
                                   const ImageCacheLiveRuntimeRefs& live_refs,
                                   const ImageCacheHoldOwner* ignore_owner) const;

    /// Rust `delete_collectable_hard_commit` — unpublish first, then unlink,
    /// then drop the record. Unpublishing first means a peer can never be
    /// pointed at bytes that are already gone.
    core::Expected<core::Unit, std::string>
        DeleteCollectableHardCommit(const HardCommitId& digest, const std::string& file,
                                    const std::set<p2p::P2pArtifactKey>& p2p_keys);

    /// Rust `run_gc` (without the metadata rebuild, which the caller does).
    core::Expected<ImageCacheGcReport, std::string>
        RunGc(const ImageCacheLiveRuntimeRefs& live_refs);

 private:
    std::string                              commit_store_;
    std::shared_ptr<ImageCacheMetadataStore> metadata_;
    std::shared_ptr<p2p::P2pTransport>       transport_;
};

}  // namespace cache
}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_CACHE_GC_H_
