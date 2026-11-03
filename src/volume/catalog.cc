// SPDX-License-Identifier: MIT
// Rust: src/volume/catalog.rs — `impl<R: Repository + ?Sized> VolumeCatalog for R`.
#include "agentenv/volume/catalog.h"

#include <functional>
#include <limits>

#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"

namespace agentenv {
namespace volume {
namespace {

using core::Optional;
using core::Unit;
using snapshot::repository::DocumentChange;
using snapshot::repository::DocumentCollection;
using snapshot::repository::DocumentKey;
using snapshot::repository::DocumentUpdate;
using snapshot::repository::RepositoryError;
using snapshot::repository::RepositoryResult;
using snapshot::repository::SnapshotRepository;

/// Rust `validate`.
RepositoryResult<Unit> Validate(const std::string& value, const char* kind) {
    if (!IsValidVolumeComponent(value)) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("invalid volume ") + kind + " '" + value + "'"));
    }
    return Unit();
}

/// Rust `decode_record`.
RepositoryResult<VolumeRecord> DecodeRecord(const std::string& volume_id,
                                            const std::string& bytes) {
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(bytes);
    if (!json.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("parse volume record", json.error().what()));
    }
    const core::Expected<VolumeRecord, std::string> record = VolumeRecord::FromJson(json.value());
    if (!record.ok()) {
        return core::make_unexpected(RepositoryError::Backend("parse volume record", record.error()));
    }
    const RepositoryResult<Unit> valid = Validate(record.value().id, "id");
    if (!valid.ok()) return core::make_unexpected(valid.error());
    if (record.value().id != volume_id) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            "volume record id '" + record.value().id + "' does not match key '" + volume_id +
            "'"));
    }
    return record.value();
}

/// Rust `encode_alias` — `serde_json::to_vec(volume_id)`, a JSON string.
std::string EncodeAlias(const std::string& volume_id) { return core::Json(volume_id).ToString(); }

/// Rust `decode_alias`.
RepositoryResult<std::string> DecodeAlias(const std::string& bytes) {
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(bytes);
    if (!json.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("parse volume alias", json.error().what()));
    }
    if (json.value().kind() != core::Json::Kind::String) {
        return core::make_unexpected(
            RepositoryError::Backend("parse volume alias", "invalid type: expected a string"));
    }
    const std::string volume_id = json.value().as_string();
    const RepositoryResult<Unit> valid = Validate(volume_id, "id");
    if (!valid.ok()) return core::make_unexpected(valid.error());
    return volume_id;
}

/// Rust `read_record`.
RepositoryResult<Optional<VolumeRecord> > ReadRecord(SnapshotRepository& repository,
                                                     const std::string& volume_id) {
    const RepositoryResult<Unit> valid = Validate(volume_id, "id");
    if (!valid.ok()) return core::make_unexpected(valid.error());
    const RepositoryResult<Optional<std::string> > bytes =
        repository.GetDocument(DocumentKey::VolumeRecord(volume_id));
    if (!bytes.ok()) return core::make_unexpected(bytes.error());
    if (!bytes.value().has_value()) return Optional<VolumeRecord>();
    const RepositoryResult<VolumeRecord> record = DecodeRecord(volume_id, *bytes.value());
    if (!record.ok()) return core::make_unexpected(record.error());
    return Optional<VolumeRecord>(record.value());
}

/// Rust `read_alias`.
RepositoryResult<Optional<std::string> > ReadAlias(SnapshotRepository& repository,
                                                   const std::string& name) {
    const RepositoryResult<Optional<std::string> > bytes =
        repository.GetDocument(DocumentKey::VolumeAlias(name));
    if (!bytes.ok()) return core::make_unexpected(bytes.error());
    if (!bytes.value().has_value()) return Optional<std::string>();
    const RepositoryResult<std::string> volume_id = DecodeAlias(*bytes.value());
    if (!volume_id.ok()) return core::make_unexpected(volume_id.error());
    return Optional<std::string>(volume_id.value());
}

/// The `(Option<VolumeRecord>, T)` a Rust `update_record` decision returns:
/// the record to write, if any, and the outcome reported to the caller.
template <typename T>
struct Decision {
    Optional<VolumeRecord> next;
    T outcome;

    Decision(const Optional<VolumeRecord>& n, const T& o) : next(n), outcome(o) {}
};

/// Rust `update_record` — atomically applies `decide` to one volume record and
/// writes the record it returns, if any. `decide` must be pure: it runs again
/// on the state the update was applied to, to report the outcome.
template <typename T>
RepositoryResult<T> UpdateRecord(
    SnapshotRepository& repository, const std::string& volume_id,
    const std::function<RepositoryResult<Decision<T> >(const Optional<VolumeRecord>&)>& decide) {
    const std::string id = volume_id;
    const DocumentUpdate update =
        [id, decide](const Optional<std::string>& bytes) -> RepositoryResult<DocumentChange> {
        Optional<VolumeRecord> record;
        if (bytes.has_value()) {
            const RepositoryResult<VolumeRecord> decoded = DecodeRecord(id, *bytes);
            if (!decoded.ok()) return core::make_unexpected(decoded.error());
            record = decoded.value();
        }
        const RepositoryResult<Decision<T> > decided = decide(record);
        if (!decided.ok()) return core::make_unexpected(decided.error());
        if (!decided.value().next.has_value()) return DocumentChange::Keep();
        const RepositoryResult<std::string> encoded = EncodeVolumeRecord(*decided.value().next);
        if (!encoded.ok()) return core::make_unexpected(encoded.error());
        return DocumentChange::Put(encoded.value());
    };
    const RepositoryResult<Optional<std::string> > applied_to =
        repository.UpdateDocument(DocumentKey::VolumeRecord(volume_id), update);
    if (!applied_to.ok()) return core::make_unexpected(applied_to.error());
    Optional<VolumeRecord> record;
    if (applied_to.value().has_value()) {
        const RepositoryResult<VolumeRecord> decoded =
            DecodeRecord(volume_id, *applied_to.value());
        if (!decoded.ok()) return core::make_unexpected(decoded.error());
        record = decoded.value();
    }
    const RepositoryResult<Decision<T> > decided = decide(record);
    if (!decided.ok()) return core::make_unexpected(decided.error());
    return decided.value().outcome;
}

/// Rust `usable_record` — a record that can take a new reservation.
RepositoryResult<VolumeRecord> UsableRecord(const Optional<VolumeRecord>& record,
                                            const std::string& volume_id) {
    if (!record.has_value()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            "volume '" + volume_id + "' does not exist in the repository"));
    }
    if (record->deleting || record->status != VolumeStatus::Ready) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("volume '" + record->id + "' is not usable"));
    }
    return *record;
}

bool Contains(const std::vector<std::string>& values, const std::string& value) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] == value) return true;
    }
    return false;
}

/// Rust `claim_alias` — points `name` at `volume_id` unless a live volume
/// already owns it.
RepositoryResult<bool> ClaimAlias(SnapshotRepository& repository, const std::string& name,
                                  const std::string& volume_id) {
    const DocumentKey key = DocumentKey::VolumeAlias(name);
    const std::string claimed = EncodeAlias(volume_id);
    for (std::size_t attempt = 0; attempt < kMaxAliasClaimAttempts; ++attempt) {
        const RepositoryResult<Optional<std::string> > current = repository.GetDocument(key);
        if (!current.ok()) return core::make_unexpected(current.error());
        if (current.value().has_value()) {
            const RepositoryResult<std::string> existing = DecodeAlias(*current.value());
            if (!existing.ok()) return core::make_unexpected(existing.error());
            if (existing.value() == volume_id) return true;
            // Records are written before their names, so a name whose record
            // is missing or being deleted is stale.
            const RepositoryResult<Optional<VolumeRecord> > owner =
                ReadRecord(repository, existing.value());
            if (!owner.ok()) return core::make_unexpected(owner.error());
            if (owner.value().has_value() && !owner.value()->deleting) return false;
        }
        const Optional<std::string> expected = current.value();
        const DocumentUpdate update =
            [expected, claimed](const Optional<std::string>& now) -> RepositoryResult<DocumentChange> {
            const bool unchanged = now.has_value() == expected.has_value() &&
                                   (!now.has_value() || *now == *expected);
            return unchanged ? DocumentChange::Put(claimed) : DocumentChange::Keep();
        };
        const RepositoryResult<Optional<std::string> > applied_to =
            repository.UpdateDocument(key, update);
        if (!applied_to.ok()) return core::make_unexpected(applied_to.error());
        const bool same = applied_to.value().has_value() == expected.has_value() &&
                          (!expected.has_value() || *applied_to.value() == *expected);
        if (same) return true;
    }
    return core::make_unexpected(RepositoryError::Backend(
        "volume alias '" + name + "' changed too often while claiming it"));
}

}  // namespace

RepositoryResult<std::string> EncodeVolumeRecord(const VolumeRecord& record) {
    return record.ToJson().ToStringPretty();
}

RepositoryResult<Optional<VolumeRecord> > GetVolume(SnapshotRepository& repository,
                                                    const std::string& reference) {
    const RepositoryResult<Unit> valid = Validate(reference, "reference");
    if (!valid.ok()) return core::make_unexpected(valid.error());
    const RepositoryResult<Optional<VolumeRecord> > by_id = ReadRecord(repository, reference);
    if (!by_id.ok() || by_id.value().has_value()) return by_id;
    const RepositoryResult<Optional<std::string> > alias = ReadAlias(repository, reference);
    if (!alias.ok()) return core::make_unexpected(alias.error());
    if (!alias.value().has_value()) return Optional<VolumeRecord>();
    return ReadRecord(repository, *alias.value());
}

RepositoryResult<VolumeRecordPage> ListVolumesPage(SnapshotRepository& repository,
                                                   const Optional<std::string>& after_volume_id,
                                                   std::size_t limit) {
    if (limit == 0) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("volume page limit must be greater than zero"));
    }
    if (after_volume_id.has_value()) {
        const RepositoryResult<Unit> valid = Validate(*after_volume_id, "id");
        if (!valid.ok()) return core::make_unexpected(valid.error());
    }
    // Rust `limit.saturating_add(1)`.
    const std::size_t fetch =
        limit == std::numeric_limits<std::size_t>::max() ? limit : limit + 1;
    RepositoryResult<std::vector<std::string> > listed =
        repository.ListDocuments(DocumentCollection::VolumeRecords, after_volume_id, fetch);
    if (!listed.ok()) return core::make_unexpected(listed.error());
    std::vector<std::string> ids = listed.value();
    const bool has_more = ids.size() > limit;
    if (ids.size() > limit) ids.resize(limit);

    VolumeRecordPage page;
    if (has_more && !ids.empty()) page.next_volume_id = ids.back();
    page.records.reserve(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        // Skips a volume deleted since the listing.
        const RepositoryResult<Optional<VolumeRecord> > record = ReadRecord(repository, ids[i]);
        if (!record.ok()) return core::make_unexpected(record.error());
        if (record.value().has_value()) page.records.push_back(*record.value());
    }
    return page;
}

RepositoryResult<Unit> CreateVolume(SnapshotRepository& repository, const VolumeRecord& input) {
    RepositoryResult<Unit> valid = Validate(input.id, "id");
    if (!valid.ok()) return valid;
    valid = Validate(input.name, "name");
    if (!valid.ok()) return valid;
    VolumeRecord record = input;
    record.backing_image_config = Optional<std::string>();

    // IDs and names share one lookup namespace.
    const RepositoryResult<Optional<VolumeRecord> > named = ReadRecord(repository, record.name);
    if (!named.ok()) return core::make_unexpected(named.error());
    bool conflict = named.value().has_value();
    if (!conflict) {
        const RepositoryResult<Optional<std::string> > aliased = ReadAlias(repository, record.id);
        if (!aliased.ok()) return core::make_unexpected(aliased.error());
        conflict = aliased.value().has_value();
    }
    if (conflict) {
        return core::make_unexpected(RepositoryError::VolumeNameConflict(record.name));
    }

    // Write the record before claiming its name, so a name whose record is
    // missing is always stale rather than mid-creation.
    const RepositoryResult<std::string> bytes = EncodeVolumeRecord(record);
    if (!bytes.ok()) return core::make_unexpected(bytes.error());
    const std::string encoded = bytes.value();
    const DocumentUpdate insert =
        [encoded](const Optional<std::string>& current) -> RepositoryResult<DocumentChange> {
        return current.has_value() ? DocumentChange::Keep() : DocumentChange::Put(encoded);
    };
    const RepositoryResult<Optional<std::string> > existing =
        repository.UpdateDocument(DocumentKey::VolumeRecord(record.id), insert);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (existing.value().has_value()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("volume '" + record.id + "' already exists"));
    }

    const RepositoryResult<bool> claimed = ClaimAlias(repository, record.name, record.id);
    if (!(claimed.ok() && claimed.value())) {
        const RepositoryResult<Unit> rolled_back =
            repository.DeleteDocument(DocumentKey::VolumeRecord(record.id));
        if (!rolled_back.ok()) {
            AGENTENV_WARN("failed to roll back unnamed volume record volume_id="
                          << record.id << " error=" << rolled_back.error().ToString());
        }
    }
    if (!claimed.ok()) return core::make_unexpected(claimed.error());
    if (!claimed.value()) {
        return core::make_unexpected(RepositoryError::VolumeNameConflict(record.name));
    }
    return Unit();
}

RepositoryResult<Unit> PutVolume(SnapshotRepository& repository, const VolumeRecord& input) {
    const RepositoryResult<Unit> valid = Validate(input.id, "id");
    if (!valid.ok()) return valid;
    VolumeRecord record = input;
    record.backing_image_config = Optional<std::string>();
    const std::function<RepositoryResult<Decision<Unit> >(const Optional<VolumeRecord>&)> decide =
        [record](const Optional<VolumeRecord>& existing) -> RepositoryResult<Decision<Unit> > {
        if (!existing.has_value()) {
            return core::make_unexpected(RepositoryError::VolumeNotFound(record.id));
        }
        const core::Expected<Unit, std::string> allowed = existing->ValidateCatalogUpdate(record);
        if (!allowed.ok()) {
            return core::make_unexpected(RepositoryError::InvalidRequest(allowed.error()));
        }
        VolumeRecord next = record;
        // Reservation state is owned exclusively by the reservation APIs. A
        // concurrent backing/status update must not restore a released owner.
        next.reserved_by_sandbox_id = existing->reserved_by_sandbox_id;
        next.read_only_mounts = existing->read_only_mounts;
        return Decision<Unit>(Optional<VolumeRecord>(next), Unit());
    };
    return UpdateRecord<Unit>(repository, record.id, decide);
}

RepositoryResult<Unit> DeleteVolume(SnapshotRepository& repository, const std::string& volume_id) {
    const RepositoryResult<Unit> valid = Validate(volume_id, "id");
    if (!valid.ok()) return valid;
    const std::string id = volume_id;
    typedef Optional<VolumeRecord> Deleted;
    const std::function<RepositoryResult<Decision<Deleted> >(const Optional<VolumeRecord>&)>
        decide = [id](const Optional<VolumeRecord>& current) -> RepositoryResult<Decision<Deleted> > {
        if (!current.has_value()) return Decision<Deleted>(Deleted(), Deleted());
        VolumeRecord record = *current;
        if (record.reserved_by_sandbox_id.has_value()) {
            return core::make_unexpected(RepositoryError::InvalidRequest(
                "volume '" + id + "' is reserved by sandbox '" + *record.reserved_by_sandbox_id +
                "'"));
        }
        if (!record.read_only_mounts.empty()) {
            return core::make_unexpected(RepositoryError::InvalidRequest(
                "volume '" + id + "' is mounted read-only by sandbox '" +
                record.read_only_mounts[0] + "'"));
        }
        if (record.deleting) return Decision<Deleted>(Deleted(), Deleted(record));
        // Mark the record first so reservations and name claims stop
        // treating it as live before it disappears.
        record.deleting = true;
        record.status = VolumeStatus::Failed;
        return Decision<Deleted>(Deleted(record), Deleted(record));
    };
    const RepositoryResult<Deleted> deleted = UpdateRecord<Deleted>(repository, volume_id, decide);
    if (!deleted.ok()) return core::make_unexpected(deleted.error());
    if (!deleted.value().has_value()) return Unit();

    const RepositoryResult<Unit> removed =
        repository.DeleteDocument(DocumentKey::VolumeRecord(volume_id));
    if (!removed.ok()) return removed;
    // A create may already have reclaimed the name from the deleting record.
    const std::string owned = EncodeAlias(volume_id);
    const DocumentUpdate release =
        [owned](const Optional<std::string>& current) -> RepositoryResult<DocumentChange> {
        return current.has_value() && *current == owned ? DocumentChange::Delete()
                                                        : DocumentChange::Keep();
    };
    const RepositoryResult<Optional<std::string> > released =
        repository.UpdateDocument(DocumentKey::VolumeAlias(deleted.value()->name), release);
    if (!released.ok()) return core::make_unexpected(released.error());
    return Unit();
}

RepositoryResult<Optional<std::string> > ReserveVolume(SnapshotRepository& repository,
                                                       const std::string& volume_id,
                                                       const std::string& owner) {
    RepositoryResult<Unit> valid = Validate(volume_id, "id");
    if (!valid.ok()) return core::make_unexpected(valid.error());
    valid = Validate(owner, "owner");
    if (!valid.ok()) return core::make_unexpected(valid.error());
    const std::string id = volume_id;
    typedef Optional<std::string> Conflict;
    const std::function<RepositoryResult<Decision<Conflict> >(const Optional<VolumeRecord>&)>
        decide = [id, owner](const Optional<VolumeRecord>& current)
        -> RepositoryResult<Decision<Conflict> > {
        RepositoryResult<VolumeRecord> usable = UsableRecord(current, id);
        if (!usable.ok()) return core::make_unexpected(usable.error());
        VolumeRecord record = usable.value();
        if (record.mode == VolumeMode::ReadOnly) {
            return Decision<Conflict>(Optional<VolumeRecord>(), Conflict());
        }
        if (record.reserved_by_sandbox_id.has_value()) {
            const std::string existing = *record.reserved_by_sandbox_id;
            return Decision<Conflict>(Optional<VolumeRecord>(),
                                      existing != owner ? Conflict(existing) : Conflict());
        }
        record.reserved_by_sandbox_id = owner;
        return Decision<Conflict>(Optional<VolumeRecord>(record), Conflict());
    };
    return UpdateRecord<Conflict>(repository, volume_id, decide);
}

RepositoryResult<Unit> ReserveReadOnlyVolume(SnapshotRepository& repository,
                                             const std::string& volume_id,
                                             const std::string& owner) {
    RepositoryResult<Unit> valid = Validate(volume_id, "id");
    if (!valid.ok()) return valid;
    valid = Validate(owner, "owner");
    if (!valid.ok()) return valid;
    const std::string id = volume_id;
    const std::function<RepositoryResult<Decision<Unit> >(const Optional<VolumeRecord>&)> decide =
        [id, owner](const Optional<VolumeRecord>& current) -> RepositoryResult<Decision<Unit> > {
        RepositoryResult<VolumeRecord> usable = UsableRecord(current, id);
        if (!usable.ok()) return core::make_unexpected(usable.error());
        VolumeRecord record = usable.value();
        if (record.mode != VolumeMode::ReadOnly) {
            return core::make_unexpected(
                RepositoryError::InvalidRequest("volume '" + record.id + "' is not read-only"));
        }
        if (Contains(record.read_only_mounts, owner)) {
            return Decision<Unit>(Optional<VolumeRecord>(), Unit());
        }
        record.read_only_mounts.push_back(owner);
        return Decision<Unit>(Optional<VolumeRecord>(record), Unit());
    };
    return UpdateRecord<Unit>(repository, volume_id, decide);
}

RepositoryResult<Unit> ReplaceVolumeOwnerFor(SnapshotRepository& repository,
                                             const std::string& volume_id,
                                             const std::string& from,
                                             const Optional<std::string>& to) {
    RepositoryResult<Unit> valid = Validate(volume_id, "id");
    if (!valid.ok()) return valid;
    valid = Validate(from, "owner");
    if (!valid.ok()) return valid;
    if (to.has_value()) {
        valid = Validate(*to, "owner");
        if (!valid.ok()) return valid;
        if (*to == from) return Unit();
    }
    const std::function<RepositoryResult<Decision<Unit> >(const Optional<VolumeRecord>&)> decide =
        [from, to](const Optional<VolumeRecord>& current) -> RepositoryResult<Decision<Unit> > {
        // Missing and deleting volumes have no owners left to change.
        if (!current.has_value() || current->deleting) {
            return Decision<Unit>(Optional<VolumeRecord>(), Unit());
        }
        VolumeRecord record = *current;
        const bool changed = record.ReplaceOwner(from, to);
        return Decision<Unit>(changed ? Optional<VolumeRecord>(record) : Optional<VolumeRecord>(),
                              Unit());
    };
    return UpdateRecord<Unit>(repository, volume_id, decide);
}

}  // namespace volume
}  // namespace agentenv
