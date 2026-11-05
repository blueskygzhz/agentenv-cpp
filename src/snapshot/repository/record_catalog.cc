// SPDX-License-Identifier: MIT
// Rust: src/snapshot/catalog.rs
#include "agentenv/snapshot/repository/record_catalog.h"

#include <algorithm>
#include <functional>
#include <utility>

#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"
#include "agentenv/snapshot/repository/documents.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace record_catalog {

namespace {

/// Rust `decode_record`.
RepositoryResult<SnapshotRecord> DecodeRecord(const std::string& id, const std::string& bytes) {
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(bytes);
    if (!json.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("parse snapshot record '" + id + "'", json.error().chain()));
    }
    const core::Expected<SnapshotRecord, std::string> record =
        SnapshotRecord::FromJson(json.value());
    if (!record.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("parse snapshot record '" + id + "'", record.error()));
    }
    return record.value();
}

/// Rust `encode_record` — `to_vec_pretty`, so a record written here is
/// byte-identical to one the Rust server writes.
std::string EncodeRecord(const SnapshotRecord& record) { return record.ToJson().ToStringPretty(); }

/// Rust `decode_alias` — an alias document is a bare JSON string.
RepositoryResult<core::SnapshotId> DecodeAlias(const SnapshotAlias& alias,
                                               const std::string& bytes) {
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(bytes);
    if (!json.ok() || json.value().kind() != core::Json::Kind::String) {
        return core::make_unexpected(RepositoryError::Backend(
            "parse alias '" + alias.ToString() + "'",
            json.ok() ? std::string("expected a string") : json.error().chain()));
    }
    const core::Expected<core::SnapshotId, std::string> id =
        core::SnapshotId::Parse(json.value().as_string());
    if (!id.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("parse alias '" + alias.ToString() + "'", id.error()));
    }
    return id.value();
}

/// Rust `encode_alias` — `to_vec`, compact.
std::string EncodeAlias(const core::SnapshotId& id) {
    return core::Json(id.ToString()).ToString();
}

/// Rust `parse_alias`.
RepositoryResult<SnapshotAlias> ParseAlias(const std::string& alias) {
    const core::Expected<SnapshotAlias, std::string> parsed = SnapshotAlias::Parse(alias);
    if (!parsed.ok()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("invalid alias '" + alias + "': " + parsed.error()));
    }
    return parsed.value();
}

/// Rust `alias_conflict`.
RepositoryError AliasConflict(const SnapshotAlias& alias, const core::SnapshotId& existing,
                              const core::SnapshotId& id) {
    return RepositoryError::AliasConflict(alias.ToString(), existing.ToString(), id.ToString());
}

/// Rust `read_record`.
RepositoryResult<core::Optional<SnapshotRecord> > ReadRecord(SnapshotRepository& repository,
                                                             const core::SnapshotId& id) {
    const RepositoryResult<core::Optional<std::string> > bytes =
        repository.GetDocument(DocumentKey::SnapshotRecord(id));
    if (!bytes.ok()) return core::make_unexpected(bytes.error());
    if (!bytes.value().has_value()) return core::Optional<SnapshotRecord>();

    const RepositoryResult<SnapshotRecord> record = DecodeRecord(id.ToString(), *bytes.value());
    if (!record.ok()) return core::make_unexpected(record.error());
    return core::Optional<SnapshotRecord>(record.value());
}

/// What one atomic record update decided: the record to write, if any, plus
/// the value to report.
template <typename T>
struct Decision {
    core::Optional<SnapshotRecord> write;
    T value;
};

/// Rust `update_record`.
///
/// `decide` runs **twice**, as upstream: once inside the atomic update to
/// choose what to write, and once on the state the update was actually
/// applied to, to produce the answer. That is why it must be pure — the
/// second run is what makes the reported outcome describe the state the write
/// landed on rather than the state that was read first, which on a contended
/// record are not the same thing.
template <typename T>
RepositoryResult<T> UpdateRecord(
    SnapshotRepository& repository, const core::SnapshotId& id,
    const std::function<RepositoryResult<Decision<T> >(const core::SnapshotId&,
                                                       const core::Optional<SnapshotRecord>&)>&
        decide) {
    const DocumentUpdate update =
        [&decide, &id](
            const core::Optional<std::string>& bytes) -> RepositoryResult<DocumentChange> {
        core::Optional<SnapshotRecord> record;
        if (bytes.has_value()) {
            const RepositoryResult<SnapshotRecord> decoded = DecodeRecord(id.ToString(), *bytes);
            if (!decoded.ok()) return core::make_unexpected(decoded.error());
            record = core::Optional<SnapshotRecord>(decoded.value());
        }
        const RepositoryResult<Decision<T> > decided = decide(id, record);
        if (!decided.ok()) return core::make_unexpected(decided.error());
        if (!decided.value().write.has_value()) return DocumentChange::Keep();
        return DocumentChange::Put(EncodeRecord(*decided.value().write));
    };

    const RepositoryResult<core::Optional<std::string> > applied_to =
        repository.UpdateDocument(DocumentKey::SnapshotRecord(id), update);
    if (!applied_to.ok()) return core::make_unexpected(applied_to.error());

    core::Optional<SnapshotRecord> record;
    if (applied_to.value().has_value()) {
        const RepositoryResult<SnapshotRecord> decoded =
            DecodeRecord(id.ToString(), *applied_to.value());
        if (!decoded.ok()) return core::make_unexpected(decoded.error());
        record = core::Optional<SnapshotRecord>(decoded.value());
    }
    const RepositoryResult<Decision<T> > decided = decide(id, record);
    if (!decided.ok()) return core::make_unexpected(decided.error());
    return decided.value().value;
}

/// Rust `alias_owner` — the snapshot that owns `alias`, *including* one being
/// deleted, cleaning up an alias whose snapshot is gone.
///
/// The cleanup is why this is not just a document read: an alias left behind
/// by a half-finished delete would otherwise block the name forever.
RepositoryResult<core::Optional<std::pair<core::SnapshotId, SnapshotRecord> > > AliasOwner(
    SnapshotRepository& repository, const SnapshotAlias& alias) {
    typedef core::Optional<std::pair<core::SnapshotId, SnapshotRecord> > Owner;

    const RepositoryResult<core::Optional<std::string> > bytes =
        repository.GetDocument(DocumentKey::SnapshotAlias(alias));
    if (!bytes.ok()) return core::make_unexpected(bytes.error());
    if (!bytes.value().has_value()) return Owner();

    const RepositoryResult<core::SnapshotId> id = DecodeAlias(alias, *bytes.value());
    if (!id.ok()) return core::make_unexpected(id.error());

    const RepositoryResult<core::Optional<SnapshotRecord> > record =
        ReadRecord(repository, id.value());
    if (!record.ok()) return core::make_unexpected(record.error());
    if (record.value().has_value()) {
        return Owner(std::make_pair(id.value(), *record.value()));
    }

    AGENTENV_WARN("cleaning up stale alias '" + alias.ToString() +
                  "' pointing to missing snapshot '" + id.value().ToString() + "'");
    const RepositoryResult<core::Unit> released = ReleaseAlias(repository, alias, id.value());
    if (!released.ok()) return core::make_unexpected(released.error());
    return Owner();
}

/// Rust `find_record` — by id or alias, *including* one being deleted.
RepositoryResult<core::Optional<SnapshotRecord> > FindRecord(SnapshotRepository& repository,
                                                             const std::string& id_or_alias) {
    const core::Expected<core::SnapshotId, std::string> id = core::SnapshotId::Parse(id_or_alias);
    if (id.ok()) {
        const RepositoryResult<core::Optional<SnapshotRecord> > record =
            ReadRecord(repository, id.value());
        if (!record.ok()) return core::make_unexpected(record.error());
        if (record.value().has_value()) return record.value();
        // Falls through to the alias lookup: a well-formed id that names no
        // record may still be a valid alias.
    }

    const RepositoryResult<SnapshotAlias> alias = ParseAlias(id_or_alias);
    if (!alias.ok()) return core::make_unexpected(alias.error());

    const RepositoryResult<core::Optional<std::pair<core::SnapshotId, SnapshotRecord> > > owner =
        AliasOwner(repository, alias.value());
    if (!owner.ok()) return core::make_unexpected(owner.error());
    if (!owner.value().has_value()) return core::Optional<SnapshotRecord>();
    return core::Optional<SnapshotRecord>(owner.value()->second);
}

/// Rust `template_build` — the build info of a template record.
RepositoryResult<core::Unit> RequireTemplate(const SnapshotRecord& record) {
    if (record.source.is_template()) return core::Unit();
    return core::make_unexpected(RepositoryError::InvalidRequest(
        "snapshot '" + record.id.ToString() + "' is not a template build"));
}

/// Rust's `sort_by`: newest first, ties broken by id so the order is total.
///
/// A stable tie-break matters because the id is what a caller pages on; two
/// records sharing a millisecond would otherwise come back in an order that
/// varies between backends.
bool NewestFirst(const SnapshotRecord& left, const SnapshotRecord& right) {
    if (left.created_at_unix_ms != right.created_at_unix_ms) {
        return left.created_at_unix_ms > right.created_at_unix_ms;
    }
    return left.id.ToString() < right.id.ToString();
}

}  // namespace

RepositoryResult<core::Unit> ReleaseAlias(SnapshotRepository& repository,
                                          const SnapshotAlias& alias,
                                          const core::SnapshotId& id) {
    const std::string owned = EncodeAlias(id);
    const DocumentUpdate update =
        [&alias, &id,
         &owned](const core::Optional<std::string>& current) -> RepositoryResult<DocumentChange> {
        if (!current.has_value()) return DocumentChange::Keep();
        // Byte comparison first, then a decode: an alias written by an older
        // version encodes the same id differently, and refusing to release it
        // would strand the name.
        if (*current == owned) return DocumentChange::Delete();
        const RepositoryResult<core::SnapshotId> existing = DecodeAlias(alias, *current);
        if (!existing.ok()) return core::make_unexpected(existing.error());
        if (existing.value() == id) return DocumentChange::Delete();
        // Someone else owns the name now; leaving it is the point.
        return DocumentChange::Keep();
    };

    const RepositoryResult<core::Optional<std::string> > released =
        repository.UpdateDocument(DocumentKey::SnapshotAlias(alias), update);
    if (!released.ok()) return core::make_unexpected(released.error());
    return core::Unit();
}

RepositoryResult<core::Unit> ClaimAlias(SnapshotRepository& repository,
                                        const SnapshotAlias& alias,
                                        const core::SnapshotId& id) {
    const DocumentKey key = DocumentKey::SnapshotAlias(alias);
    const std::string claimed = EncodeAlias(id);

    for (int attempt = 0; attempt < kMaxAliasClaimAttempts; ++attempt) {
        const RepositoryResult<core::Optional<std::string> > current =
            repository.GetDocument(key);
        if (!current.ok()) return core::make_unexpected(current.error());

        if (current.value().has_value()) {
            const RepositoryResult<core::SnapshotId> existing =
                DecodeAlias(alias, *current.value());
            if (!existing.ok()) return core::make_unexpected(existing.error());
            // Already ours: claiming is idempotent, which is what lets a
            // retried publish finish.
            if (existing.value() == id) return core::Unit();

            const RepositoryResult<core::Optional<SnapshotRecord> > owner =
                ReadRecord(repository, existing.value());
            if (!owner.ok()) return core::make_unexpected(owner.error());
            // A *live* owner is a conflict. One that is gone or being deleted
            // is not: its name is free, and taking it is how an alias
            // survives the snapshot it used to point at.
            if (owner.value().has_value() && !owner.value()->deleting) {
                return core::make_unexpected(AliasConflict(alias, existing.value(), id));
            }
        }

        // Conditional on exactly the bytes just read, so a claim that races
        // another node's does not overwrite it: the loser sees the winner's
        // document on the next pass and re-decides against it.
        const core::Optional<std::string> expected = current.value();
        const DocumentUpdate update =
            [&expected, &claimed](
                const core::Optional<std::string>& now) -> RepositoryResult<DocumentChange> {
            if (now.has_value() != expected.has_value()) return DocumentChange::Keep();
            if (now.has_value() && *now != *expected) return DocumentChange::Keep();
            return DocumentChange::Put(claimed);
        };

        const RepositoryResult<core::Optional<std::string> > applied_to =
            repository.UpdateDocument(key, update);
        if (!applied_to.ok()) return core::make_unexpected(applied_to.error());

        const bool unchanged =
            applied_to.value().has_value() == expected.has_value() &&
            (!expected.has_value() || *applied_to.value() == *expected);
        if (unchanged) return core::Unit();
    }

    return core::make_unexpected(RepositoryError::Backend(
        "snapshot alias '" + alias.ToString() + "' changed too often while claiming it"));
}

RepositoryResult<core::Unit> EnsureAliasAvailable(SnapshotRepository& repository,
                                                  const SnapshotAlias& alias,
                                                  const core::SnapshotId& id) {
    const RepositoryResult<core::Optional<std::pair<core::SnapshotId, SnapshotRecord> > > owner =
        AliasOwner(repository, alias);
    if (!owner.ok()) return core::make_unexpected(owner.error());
    if (!owner.value().has_value()) return core::Unit();
    if (owner.value()->first == id) return core::Unit();
    // A snapshot being deleted has already given up its name.
    if (owner.value()->second.deleting) return core::Unit();
    return core::make_unexpected(AliasConflict(alias, owner.value()->first, id));
}

RepositoryResult<SnapshotRecord> Create(SnapshotRepository& repository,
                                        const SnapshotRecord& record) {
    if (!record.source.is_template()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("only template snapshots can be pre-created"));
    }
    if (record.committed.has_value()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            "pre-created template snapshots must not already be committed"));
    }
    if (record.alias.has_value()) {
        // Checked before the write so the common conflict is reported without
        // leaving a record behind to roll back.
        const RepositoryResult<core::Unit> available =
            EnsureAliasAvailable(repository, *record.alias, record.id);
        if (!available.ok()) return core::make_unexpected(available.error());
    }

    const std::string bytes = EncodeRecord(record);
    const DocumentUpdate update =
        [&bytes](const core::Optional<std::string>& current) -> RepositoryResult<DocumentChange> {
        // Create-only: an existing record is reported, never overwritten.
        if (current.has_value()) return DocumentChange::Keep();
        return DocumentChange::Put(bytes);
    };

    const RepositoryResult<core::Optional<std::string> > existing =
        repository.UpdateDocument(DocumentKey::SnapshotRecord(record.id), update);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (existing.value().has_value()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            "snapshot '" + record.id.ToString() + "' already exists"));
    }

    if (record.alias.has_value()) {
        const RepositoryResult<core::Unit> claimed =
            ClaimAlias(repository, *record.alias, record.id);
        if (!claimed.ok()) {
            // Rolled back, because the alias was lost to a racing claim after
            // the record landed: a record that keeps an alias it does not own
            // is findable by id and invisible by name, and nothing would ever
            // reconcile it. The rollback failing is only logged -- the claim
            // error is what the caller needs.
            const RepositoryResult<core::Unit> rollback =
                repository.DeleteDocument(DocumentKey::SnapshotRecord(record.id));
            if (!rollback.ok()) {
                AGENTENV_WARN("failed to roll back snapshot record '" + record.id.ToString() +
                              "': " + rollback.error().ToString());
            }
            return core::make_unexpected(claimed.error());
        }
    }
    return record;
}

RepositoryResult<core::Optional<SnapshotRecord> > Get(SnapshotRepository& repository,
                                                      const std::string& id_or_alias) {
    const RepositoryResult<core::Optional<SnapshotRecord> > record =
        FindRecord(repository, id_or_alias);
    if (!record.ok()) return core::make_unexpected(record.error());
    // Hidden rather than returned: its files may already be gone, and a
    // caller that resumed from them would fail in a way that looks like
    // corruption instead of a deleted snapshot.
    if (record.value().has_value() && record.value()->deleting) {
        return core::Optional<SnapshotRecord>();
    }
    return record.value();
}

RepositoryResult<std::vector<SnapshotRecord> > List(SnapshotRepository& repository,
                                                    const SnapshotListFilter& filter) {
    const RepositoryResult<std::vector<std::pair<std::string, std::string> > > documents =
        repository.ReadDocuments(DocumentCollection::SnapshotRecords);
    if (!documents.ok()) return core::make_unexpected(documents.error());

    std::vector<SnapshotRecord> records;
    for (std::size_t i = 0; i < documents.value().size(); ++i) {
        const RepositoryResult<SnapshotRecord> record =
            DecodeRecord(documents.value()[i].first, documents.value()[i].second);
        // Not skipped: a record that will not parse is a corrupt catalog, and
        // omitting it would make the listing quietly incomplete.
        if (!record.ok()) return core::make_unexpected(record.error());
        if (record.value().deleting) continue;
        if (!filter.Matches(record.value())) continue;
        records.push_back(record.value());
    }
    std::sort(records.begin(), records.end(), NewestFirst);
    return records;
}

RepositoryResult<core::Optional<core::SnapshotId> > ResolveAlias(SnapshotRepository& repository,
                                                                 const std::string& alias) {
    const RepositoryResult<SnapshotAlias> parsed = ParseAlias(alias);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());

    const RepositoryResult<core::Optional<std::pair<core::SnapshotId, SnapshotRecord> > > owner =
        AliasOwner(repository, parsed.value());
    if (!owner.ok()) return core::make_unexpected(owner.error());
    if (!owner.value().has_value()) return core::Optional<core::SnapshotId>();
    if (owner.value()->second.deleting) return core::Optional<core::SnapshotId>();
    return core::Optional<core::SnapshotId>(owner.value()->first);
}

RepositoryResult<SnapshotRecord> TryStartBuild(SnapshotRepository& repository,
                                               const core::SnapshotId& id) {
    const int64_t now = NowUnixMs();
    const std::function<RepositoryResult<Decision<SnapshotRecord> >(
        const core::SnapshotId&, const core::Optional<SnapshotRecord>&)>
        decide = [now](const core::SnapshotId& id, const core::Optional<SnapshotRecord>& current)
            -> RepositoryResult<Decision<SnapshotRecord> > {
        if (!current.has_value() || current->deleting) {
            return core::make_unexpected(RepositoryError::SnapshotNotFound(id.ToString()));
        }
        SnapshotRecord record = *current;
        const RepositoryResult<core::Unit> is_template = RequireTemplate(record);
        if (!is_template.ok()) return core::make_unexpected(is_template.error());

        // Exactly one caller may move a build out of Waiting: this is what
        // keeps two nodes from building the same template twice.
        if (record.source.build.status != TemplateBuildStatus::Waiting) {
            return core::make_unexpected(RepositoryError::InvalidRequest(
                "template build '" + id.ToString() + "' is not in waiting state"));
        }
        record.source.build.status = TemplateBuildStatus::Building;
        record.source.build.started_at_unix_ms = core::Optional<int64_t>(now);
        // Cleared: a retry after a failure must not keep the previous reason,
        // which would describe the build that is now running.
        record.source.build.error_reason = core::Optional<TemplateBuildErrorReason>();
        record.updated_at_unix_ms = now;

        Decision<SnapshotRecord> decision;
        decision.write = core::Optional<SnapshotRecord>(record);
        decision.value = record;
        return decision;
    };
    return UpdateRecord<SnapshotRecord>(repository, id, decide);
}

RepositoryResult<core::Unit> MarkBuildError(SnapshotRepository& repository,
                                            const core::SnapshotId& id,
                                            const TemplateBuildErrorReason& reason) {
    const int64_t now = NowUnixMs();
    const std::function<RepositoryResult<Decision<core::Unit> >(
        const core::SnapshotId&, const core::Optional<SnapshotRecord>&)>
        decide = [now, &reason](const core::SnapshotId& id,
                                const core::Optional<SnapshotRecord>& current)
            -> RepositoryResult<Decision<core::Unit> > {
        if (!current.has_value() || current->deleting) {
            return core::make_unexpected(RepositoryError::SnapshotNotFound(id.ToString()));
        }
        SnapshotRecord record = *current;
        const RepositoryResult<core::Unit> is_template = RequireTemplate(record);
        if (!is_template.ok()) return core::make_unexpected(is_template.error());

        record.source.build.status = TemplateBuildStatus::Error;
        record.source.build.finished_at_unix_ms = core::Optional<int64_t>(now);
        record.source.build.error_reason = core::Optional<TemplateBuildErrorReason>(reason);
        record.updated_at_unix_ms = now;

        Decision<core::Unit> decision;
        decision.write = core::Optional<SnapshotRecord>(record);
        decision.value = core::Unit();
        return decision;
    };
    return UpdateRecord<core::Unit>(repository, id, decide);
}

}  // namespace record_catalog
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
