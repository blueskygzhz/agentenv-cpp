// SPDX-License-Identifier: MIT
// Rust: src/image/cache/service.rs — the deleting-GC engine.
#include "agentenv/image/cache_gc.h"

#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"

namespace agentenv {
namespace image {
namespace cache {

const char* const kRuntimeHoldNamespace   = "runtime";
const char* const kPausedHoldNamespace    = "paused";
const char* const kOperationHoldNamespace = "operation";

HardCommitGcDecision HardCommitGcDecision::MakeCollectable(
    const HardCommitId& digest, const std::string& file,
    const core::Optional<uint64_t>& size,
    const std::set<p2p::P2pArtifactKey>& keys) {
    HardCommitGcDecision decision;
    decision.kind     = Kind::Collectable;
    decision.digest   = core::Optional<HardCommitId>(digest);
    decision.file     = file;
    decision.size     = size;
    decision.p2p_keys = keys;
    return decision;
}

HardCommitGcDecision HardCommitGcDecision::MakeBlocked(const ImageCacheGcBlocked& blocked) {
    HardCommitGcDecision decision;
    decision.kind    = Kind::Blocked;
    decision.blocked = core::Optional<ImageCacheGcBlocked>(blocked);
    return decision;
}

namespace {

ImageCacheGcBlockedReason Unverifiable(const std::string& detail) {
    ImageCacheGcBlockedReason reason;
    reason.kind   = ImageCacheGcBlockedReason::Unverifiable;
    reason.detail = detail;
    return reason;
}

ImageCacheGcBlocked BlockedWith(const HardCommitId& digest,
                                const ImageCacheGcBlockedReason& reason) {
    return ImageCacheGcBlocked(digest, reason);
}

}  // namespace

core::Optional<ImageCacheGcBlockedReason>
ImageCacheGc::CommitFileBlockReason(const std::string& file,
                                    const core::Optional<uint64_t>& size) const {
    // GC must never delete outside the store it was pointed at, even if the
    // metadata claims ownership of the path.
    if (!PathIsInside(file, commit_store_)) {
        return core::Optional<ImageCacheGcBlockedReason>(
            Unverifiable("commit file is outside the commit store"));
    }

    // `lstat`, not `stat`: a symlink must not be followed, or GC could be
    // tricked into judging (and then deleting) a different file.
    struct stat st;
    if (::lstat(file.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return core::Optional<ImageCacheGcBlockedReason>(
                Unverifiable("commit file is missing"));
        }
        std::ostringstream os;
        os << "stat commit file failed: errno " << errno;
        return core::Optional<ImageCacheGcBlockedReason>(Unverifiable(os.str()));
    }
    if (!S_ISREG(st.st_mode)) {
        return core::Optional<ImageCacheGcBlockedReason>(
            Unverifiable("commit path is not a regular file"));
    }
    if (size.has_value()) {
        const uint64_t actual = static_cast<uint64_t>(st.st_size);
        if (actual != *size) {
            // A size mismatch means the record and the bytes disagree, so
            // neither can be trusted to identify what is on disk.
            std::ostringstream os;
            os << "commit file size mismatch: expected " << *size << ", got " << actual;
            return core::Optional<ImageCacheGcBlockedReason>(Unverifiable(os.str()));
        }
    }
    return core::Optional<ImageCacheGcBlockedReason>();
}

core::Expected<HardCommitGcDecision, std::string>
ImageCacheGc::CheckCollectableHardCommit(
    const HardCommitObjectRecord& record,
    const std::vector<ImageCacheConfigId>& config_referrers,
    const ImageCacheLiveRuntimeRefs& live_refs,
    const ImageCacheHoldOwner* ignore_owner) const {
    const HardCommitId digest = record.digest;

    if (!config_referrers.empty()) {
        ImageCacheGcBlockedReason reason;
        reason.kind = ImageCacheGcBlockedReason::RootedByConfig;
        for (std::size_t i = 0; i < config_referrers.size(); ++i) {
            reason.configs.push_back(config_referrers[i].AsStr());
        }
        return HardCommitGcDecision::MakeBlocked(BlockedWith(digest, reason));
    }

    core::Expected<std::vector<ImageCacheHoldOwner>, std::string> hold_owners =
        metadata_->HardCommitHoldReferrers(digest);
    if (!hold_owners.ok()) return core::make_unexpected(hold_owners.error());

    std::vector<ImageCacheHoldOwner> owners;
    for (std::size_t i = 0; i < hold_owners.value().size(); ++i) {
        // GC's own operation hold is not a reason to skip the commit it was
        // taken for.
        if (ignore_owner != NULL && hold_owners.value()[i] == *ignore_owner) continue;
        owners.push_back(hold_owners.value()[i]);
    }
    if (!owners.empty()) {
        ImageCacheGcBlockedReason reason;
        reason.kind   = ImageCacheGcBlockedReason::Held;
        reason.owners = owners;
        return HardCommitGcDecision::MakeBlocked(BlockedWith(digest, reason));
    }

    if (!record.file.has_value()) {
        return HardCommitGcDecision::MakeBlocked(BlockedWith(
            digest, Unverifiable("hard commit object has no recorded file")));
    }
    core::Optional<ImageCacheGcBlockedReason> file_reason =
        CommitFileBlockReason(*record.file, record.size);
    if (file_reason.has_value()) {
        return HardCommitGcDecision::MakeBlocked(BlockedWith(digest, *file_reason));
    }

    ImageCacheLiveRuntimeRefs::const_iterator live = live_refs.find(digest);
    if (live != live_refs.end() && !live->second.empty()) {
        ImageCacheGcBlockedReason reason;
        reason.kind   = ImageCacheGcBlockedReason::LiveRuntime;
        reason.owners = live->second;
        return HardCommitGcDecision::MakeBlocked(BlockedWith(digest, reason));
    }

    return HardCommitGcDecision::MakeCollectable(digest, *record.file, record.size,
                                             record.p2p_keys);
}

core::Expected<core::Unit, std::string>
ImageCacheGc::DeleteCollectableHardCommit(const HardCommitId& digest,
                                          const std::string& file,
                                          const std::set<p2p::P2pArtifactKey>& p2p_keys) {
    if (!p2p_keys.empty()) {
        if (!transport_) {
            // Rust refuses rather than deleting: unlinking a published blob
            // would leave peers advertising bytes that no longer exist.
            return core::make_unexpected(
                std::string("image cache P2P transport is not initialized"));
        }
        for (std::set<p2p::P2pArtifactKey>::const_iterator it = p2p_keys.begin();
             it != p2p_keys.end(); ++it) {
            p2p::P2pResult<bool> unpublished = transport_->Unpublish(*it);
            if (!unpublished.ok()) {
                return core::make_unexpected(std::string("unpublish image cache P2P key '") +
                                             *it + "': " + unpublished.error().ToString());
            }
        }
    }

    core::Expected<core::Unit, std::string> removed = core::fs::RemoveFile(file);
    if (!removed.ok()) {
        return core::make_unexpected(std::string("remove image cache commit file ") + file +
                                     ": " + removed.error());
    }

    // Rust prunes the now-possibly-empty digest directory, but never the store
    // root itself, and only when the parent is genuinely inside the store.
    core::Optional<std::string> parent = core::fs::Parent(file);
    if (parent.has_value() && *parent != commit_store_ &&
        PathIsInside(*parent, commit_store_)) {
        // Best-effort: a non-empty directory simply stays.
        (void)::rmdir(parent->c_str());
    }

    return metadata_->RemoveHardCommitObject(digest);
}

core::Expected<ImageCacheGcReport, std::string>
ImageCacheGc::RunGc(const ImageCacheLiveRuntimeRefs& live_refs) {
    core::Expected<std::map<HardCommitId, std::vector<ImageCacheConfigId> >, std::string>
        config_referrers = metadata_->HardCommitConfigReferrerMap();
    if (!config_referrers.ok()) return core::make_unexpected(config_referrers.error());

    core::Expected<std::vector<HardCommitObjectRecord>, std::string> objects =
        metadata_->ListHardCommitObjects();
    if (!objects.ok()) return core::make_unexpected(objects.error());

    ImageCacheGcReport report;

    for (std::size_t i = 0; i < objects.value().size(); ++i) {
        const HardCommitObjectRecord& record = objects.value()[i];

        std::vector<ImageCacheConfigId> referrers;
        std::map<HardCommitId, std::vector<ImageCacheConfigId> >::const_iterator found =
            config_referrers.value().find(record.digest);
        if (found != config_referrers.value().end()) referrers = found->second;

        core::Expected<HardCommitGcDecision, std::string> first =
            CheckCollectableHardCommit(record, referrers, live_refs, NULL);
        if (!first.ok()) return core::make_unexpected(first.error());
        if (first.value().kind == HardCommitGcDecision::Kind::Blocked) {
            report.blocked.push_back(*first.value().blocked);
            continue;
        }

        // Take an operation hold so a concurrent resume cannot start using the
        // commit while it is being deleted.
        core::Expected<ImageCacheHoldOwner, std::string> owner =
            ImageCacheHoldOwner::New(kOperationHoldNamespace,
                                     std::string("gc/") + record.digest.AsStr());
        if (!owner.ok()) return core::make_unexpected(owner.error());

        std::set<HardCommitId> held;
        held.insert(record.digest);
        core::Expected<core::Unit, std::string> hold =
            metadata_->CreateOrReplaceHold(owner.value(), held);
        if (!hold.ok()) return core::make_unexpected(hold.error());

        // Re-read the record: it may have been re-recorded or removed between
        // the scan and the hold.
        core::Expected<core::Optional<HardCommitObjectRecord>, std::string> fresh =
            metadata_->GetHardCommitObject(record.digest);
        if (!fresh.ok()) {
            (void)metadata_->ReleaseHold(owner.value());
            return core::make_unexpected(fresh.error());
        }

        if (!fresh.value().has_value()) {
            report.blocked.push_back(BlockedWith(
                record.digest,
                Unverifiable("hard commit object record disappeared before delete")));
            (void)metadata_->ReleaseHold(owner.value());
            continue;
        }

        // Source-config publish is not serialised by the operation hold, so
        // the roots are read fresh here.
        core::Expected<std::vector<ImageCacheConfigId>, std::string> fresh_referrers =
            metadata_->HardCommitConfigReferrers(record.digest);
        if (!fresh_referrers.ok()) {
            (void)metadata_->ReleaseHold(owner.value());
            return core::make_unexpected(fresh_referrers.error());
        }

        core::Expected<HardCommitGcDecision, std::string> second =
            CheckCollectableHardCommit(*fresh.value(), fresh_referrers.value(), live_refs,
                                       &owner.value());
        if (!second.ok()) {
            (void)metadata_->ReleaseHold(owner.value());
            return core::make_unexpected(second.error());
        }

        if (second.value().kind == HardCommitGcDecision::Kind::Blocked) {
            report.blocked.push_back(*second.value().blocked);
            (void)metadata_->ReleaseHold(owner.value());
            continue;
        }

        const HardCommitGcDecision& collectable = second.value();
        core::Expected<core::Unit, std::string> deleted = DeleteCollectableHardCommit(
            *collectable.digest, collectable.file, collectable.p2p_keys);
        if (deleted.ok()) {
            ++report.collected;
            report.freed_bytes += collectable.size.has_value() ? *collectable.size : 0;
        } else {
            // A failed delete is reported, not fatal: the next pass retries.
            AGENTENV_WARN("failed to delete collectable image cache commit " +
                          collectable.digest->AsStr() + ": " + deleted.error());
            ImageCacheGcBlockedReason reason;
            reason.kind   = ImageCacheGcBlockedReason::DeleteFailed;
            reason.detail = deleted.error();
            report.blocked.push_back(BlockedWith(*collectable.digest, reason));
        }

        // Release even on the delete path: the record is already gone, so the
        // hold would otherwise be a permanent dangling reference.
        (void)metadata_->ReleaseHold(owner.value());
    }

    return report;
}

}  // namespace cache
}  // namespace image
}  // namespace agentenv
