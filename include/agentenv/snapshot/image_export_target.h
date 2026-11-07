// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/target.rs
//
// Decides where a snapshot rootfs image is published. An explicit target is
// validated; otherwise the destination is inferred from the snapshot's own
// registry source, and inference fails loudly when there is no single
// unambiguous answer rather than guessing one.
#ifndef AGENTENV_SNAPSHOT_IMAGE_EXPORT_TARGET_H_
#define AGENTENV_SNAPSHOT_IMAGE_EXPORT_TARGET_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/record.h"

namespace agentenv {
namespace snapshot {

/// Rust `DEFAULT_TAG`.
extern const char* const kDefaultImageTag;

/// Rust `enum SnapshotImageTargetError`.
struct SnapshotImageTargetError {
    enum class Kind {
        InvalidTarget,
        /// Inference had nothing, or too much, to go on.
        CannotInfer,
    };

    Kind kind = Kind::InvalidTarget;
    std::string reason;

    std::string Message() const;

    bool operator==(const SnapshotImageTargetError& o) const {
        return kind == o.kind && reason == o.reason;
    }
    bool operator!=(const SnapshotImageTargetError& o) const { return !(*this == o); }
};

/// Rust `struct SnapshotImageTarget`.
struct SnapshotImageTarget {
    std::string registry;
    std::string repository;
    std::string tag;

    /// Rust `SnapshotImageTarget::parse`.
    static core::Expected<SnapshotImageTarget, SnapshotImageTargetError> Parse(
        const std::string& target_repository, const core::Optional<std::string>& tag);

    /// Rust `image_ref`.
    std::string ImageRef() const;

    bool operator==(const SnapshotImageTarget& o) const {
        return registry == o.registry && repository == o.repository && tag == o.tag;
    }
    bool operator!=(const SnapshotImageTarget& o) const { return !(*this == o); }
};

/// Rust `resolve_snapshot_image_target`.
///
/// `allow_loopback_http` is threaded through to
/// `image_export::SourceRegistryRepository::Parse`, which is where Rust's
/// `cfg!(test)` loopback exemption lives. Production must pass false: a
/// recorded `http://127.0.0.1/...` source is refused upstream, and accepting
/// it here would let inference aim a publish at a plaintext registry.
core::Expected<SnapshotImageTarget, SnapshotImageTargetError> ResolveSnapshotImageTarget(
    const core::SnapshotId& snapshot_id, const CommittedSnapshot& committed,
    const core::Optional<std::string>& target_repository,
    const core::Optional<std::string>& tag, bool allow_loopback_http = false);

/// Rust `snapshot_managed_publication_for_target`.
core::Optional<PersistedDiskImagePublication> SnapshotManagedPublicationForTarget(
    const CommittedSnapshot& committed, const SnapshotImageTarget& target,
    bool allow_loopback_http = false);

/// Rust `valid_repository` — non-empty, `/`-separated lowercase components.
bool IsValidRepository(const std::string& repository);

/// Rust `validated_tag`.
core::Expected<std::string, SnapshotImageTargetError> ValidatedTag(
    const core::Optional<std::string>& tag);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_IMAGE_EXPORT_TARGET_H_
