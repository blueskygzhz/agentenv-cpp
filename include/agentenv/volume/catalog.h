// SPDX-License-Identifier: MIT
// Rust: src/volume/catalog.rs — the shared volume catalog: one record per
// volume plus a name alias, stored as repository documents so every backend
// follows the same rules.
//
// Rust declares `trait VolumeCatalog` and blanket-implements it for every
// `R: Repository + ?Sized`, so `repository.get_volume(..)` works on any
// backend. C++ has no blanket impls; the trait's methods become free
// functions over `SnapshotRepository&` (the C++ name of `Repository`), which
// is the same shape: they are built only on `get_document`,
// `update_document`, `delete_document` and `list_documents`, and no backend
// can override them.
#ifndef AGENTENV_VOLUME_CATALOG_H_
#define AGENTENV_VOLUME_CATALOG_H_

#include <cstddef>
#include <string>
#include <vector>

#include "agentenv/core/optional.h"
#include "agentenv/snapshot/repository/interfaces.h"
#include "agentenv/volume/record.h"

namespace agentenv {
namespace volume {

/// Rust `MAX_ALIAS_CLAIM_ATTEMPTS`.
const std::size_t kMaxAliasClaimAttempts = 5;

/// Rust struct `VolumeRecordPage`.
struct VolumeRecordPage {
    std::vector<VolumeRecord> records;
    /// The last returned volume ID. Supplying it to the next request resumes
    /// after that record in the repository's stable ID order.
    core::Optional<std::string> next_volume_id;

    bool operator==(const VolumeRecordPage& other) const {
        if (records != other.records) return false;
        if (next_volume_id.has_value() != other.next_volume_id.has_value()) return false;
        return !next_volume_id.has_value() || *next_volume_id == *other.next_volume_id;
    }
};

/// Rust `VolumeCatalog::get_volume` — resolves a volume by ID first, then by
/// its unique name.
snapshot::repository::RepositoryResult<core::Optional<VolumeRecord> > GetVolume(
    snapshot::repository::SnapshotRepository& repository, const std::string& reference);

/// Rust `VolumeCatalog::list_volumes_page` — one bounded page ordered by the
/// repository's stable volume cursor.
snapshot::repository::RepositoryResult<VolumeRecordPage> ListVolumesPage(
    snapshot::repository::SnapshotRepository& repository,
    const core::Optional<std::string>& after_volume_id, std::size_t limit);

/// Rust `VolumeCatalog::create_volume` — creates one durable volume record and
/// claims its unique name.
snapshot::repository::RepositoryResult<core::Unit> CreateVolume(
    snapshot::repository::SnapshotRepository& repository, const VolumeRecord& record);

/// Rust `VolumeCatalog::put_volume` — updates one existing durable volume
/// record, keeping its reservations.
snapshot::repository::RepositoryResult<core::Unit> PutVolume(
    snapshot::repository::SnapshotRepository& repository, const VolumeRecord& record);

/// Rust `VolumeCatalog::delete_volume` — removes one unmounted volume. Missing
/// volumes are considered success.
snapshot::repository::RepositoryResult<core::Unit> DeleteVolume(
    snapshot::repository::SnapshotRepository& repository, const std::string& volume_id);

/// Rust `VolumeCatalog::reserve_volume` — acquires an exclusive reservation.
/// A returned owner reports a conflicting owner.
snapshot::repository::RepositoryResult<core::Optional<std::string> > ReserveVolume(
    snapshot::repository::SnapshotRepository& repository, const std::string& volume_id,
    const std::string& owner);

/// Rust `VolumeCatalog::reserve_read_only_volume` — adds a shared lease for a
/// read-only volume.
snapshot::repository::RepositoryResult<core::Unit> ReserveReadOnlyVolume(
    snapshot::repository::SnapshotRepository& repository, const std::string& volume_id,
    const std::string& owner);

/// Rust `VolumeCatalog::replace_volume_owner_for` — releases or rebinds one
/// volume, changing it only when `from` mounts it.
snapshot::repository::RepositoryResult<core::Unit> ReplaceVolumeOwnerFor(
    snapshot::repository::SnapshotRepository& repository, const std::string& volume_id,
    const std::string& from, const core::Optional<std::string>& to);

/// Rust `encode_record` — `serde_json::to_vec_pretty(record)`. Exposed because
/// the upstream catalog tests write a record document directly.
snapshot::repository::RepositoryResult<std::string> EncodeVolumeRecord(const VolumeRecord& record);

}  // namespace volume
}  // namespace agentenv
#endif  // AGENTENV_VOLUME_CATALOG_H_
