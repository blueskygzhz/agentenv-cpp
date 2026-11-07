// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/acr/publisher.rs — the tag layer.
//
// Decides the OCI tag a snapshot is published under, and refuses one the
// registry would not accept. This runs *before* any blob is uploaded, which is
// the point: a tag rejected late would leave the blobs pushed and no manifest
// naming them, so the next publish re-uploads them and nothing ever collects
// the orphans.
//
// `AcrDiskImageExporter` — this module's `export` and `rollback_publication`
// — lives in `acr.h` instead, next to the client both halves drive.
#ifndef AGENTENV_SNAPSHOT_IMAGE_EXPORT_ACR_PUBLISHER_H_
#define AGENTENV_SNAPSHOT_IMAGE_EXPORT_ACR_PUBLISHER_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/layers.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/errors.h"

namespace agentenv {
namespace snapshot {
namespace image_export {

/// Rust `enum DiskImageSubject` — which disk of a sandbox is being published.
struct DiskImageSubject {
    enum class Kind { Rootfs, AttachedDrive };

    Kind kind = Kind::Rootfs;
    /// `AttachedDrive` only.
    std::string drive_id;

    DiskImageSubject() {}

    static DiskImageSubject Rootfs() {
        DiskImageSubject out;
        out.kind = Kind::Rootfs;
        return out;
    }
    static DiskImageSubject AttachedDrive(const std::string& drive_id) {
        DiskImageSubject out;
        out.kind = Kind::AttachedDrive;
        out.drive_id = drive_id;
        return out;
    }

    bool is_rootfs() const { return kind == Kind::Rootfs; }

    /// Rust `log_label`.
    const char* LogLabel() const {
        return kind == Kind::Rootfs ? "rootfs" : "attached_drive";
    }

    bool operator==(const DiskImageSubject& o) const {
        if (kind != o.kind) return false;
        return kind == Kind::Rootfs || drive_id == o.drive_id;
    }
    bool operator!=(const DiskImageSubject& o) const { return !(*this == o); }
};

/// Rust `sanitize_drive_tag_component`.
///
/// A drive id is operator-supplied and may contain anything; an OCI tag may
/// not. Every disallowed character becomes `-`, and a 12-hex-digit prefix of
/// the id's SHA-256 is appended.
///
/// **The hash is what makes the result injective, and it is computed over the
/// original id, not the sanitised one.** Sanitising alone collapses `data:1`
/// and `data/1` onto `data-1`, and truncating to 32 characters collapses every
/// id sharing a long prefix. Two drives of one snapshot would then publish to
/// the same tag, and the second would overwrite the first.
std::string SanitizeDriveTagComponent(const std::string& drive_id);

/// Rust `snapshot_tag` — the rootfs tag, with a per-drive suffix for an
/// attached drive.
std::string SnapshotTag(const core::SnapshotId& snapshot_id,
                        const DiskImageSubject& subject);

/// Rust `validate_snapshot_tag`.
///
/// An OCI tag is at most 128 characters, starts with an alphanumeric or `_`,
/// and continues with alphanumerics or `_`, `.`, `-`. Notably `-` may not
/// *lead*, which is why the start and continuation rules differ.
repository::RepositoryResult<core::Unit> ValidateSnapshotTag(const std::string& tag);

/// Rust `struct DiskImageExportOutcome`.
///
/// Rust also carries `local_layers: Vec<PreparedLayerUpload>`, the prepared
/// upload files kept alive past the push for the post-commit P2P
/// announcement. That transport is not ported, and every upload has already
/// completed by the time `Export` returns, so the field is dropped rather
/// than returned empty — an empty one would read as "nothing to announce"
/// instead of "announcing is not a thing here". `Export` still holds the
/// uploads for its own duration, because they own the temporary files.
///
/// Declared in this header rather than next to `AcrDiskImageExporter` in
/// `acr.h` so that `store.h` can name it in `DiskImageExport` without
/// pulling the HTTP transport into every translation unit that stores a
/// snapshot. Its two fields need only `layers.h` and `record.h`, which
/// `store.h` already includes.
struct DiskImageExportOutcome {
    std::vector<OverlaybdLayerRef> layers;
    /// Always present from `Export`. Optional because upstream's exporter
    /// interface allows a backend that publishes nothing to report so.
    core::Optional<PersistedDiskImagePublication> publication;
};

}  // namespace image_export
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_IMAGE_EXPORT_ACR_PUBLISHER_H_
