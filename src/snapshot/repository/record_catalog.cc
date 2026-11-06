// SPDX-License-Identifier: MIT
// Rust: src/snapshot/catalog.rs
#include "agentenv/snapshot/repository/record_catalog.h"

#include <algorithm>
#include <functional>
#include <sstream>
#include <utility>

#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"
#include "agentenv/snapshot/repository/documents.h"
#include "agentenv/snapshot/store.h"

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

/// Rust `new_committed_record` — the record a commit creates when none exists.
///
/// Not `TemplateWaiting` followed by `MarkCommitted`: a template publish must
/// land on `Ready` with `started_at` left absent, which that pair cannot
/// express — it would report a build that was never started as having been.
SnapshotRecord NewCommittedRecord(const SnapshotPublishMetadata& metadata,
                                  const CommittedSnapshot& committed, int64_t now) {
    SnapshotRecord record;
    record.id = metadata.id;
    record.alias = metadata.alias;
    if (metadata.source.kind == SnapshotSourceKind::Template) {
        TemplateBuildInfo build;
        build.status = TemplateBuildStatus::Ready;
        build.finished_at_unix_ms = core::Optional<int64_t>(now);
        record.source = SnapshotSource::Template(build);
    } else {
        record.source = SnapshotSource::Sandbox(metadata.source.source_sandbox_id,
                                                metadata.source.source_sandbox_metadata);
    }
    record.resources = metadata.resources;
    record.created_at_unix_ms = now;
    record.updated_at_unix_ms = now;
    record.committed = core::Optional<CommittedSnapshot>(committed);
    record.deleting = false;
    return record;
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

RepositoryResult<core::Unit> Delete(SnapshotRepository& repository,
                                    const std::string& id_or_alias) {
    const RepositoryResult<core::Optional<SnapshotRecord> > found =
        FindRecord(repository, id_or_alias);
    if (!found.ok()) return core::make_unexpected(found.error());

    if (!found.value().has_value()) {
        // No record, but a build log may still be there: a delete that got
        // as far as removing the record left it behind, and retrying by id
        // is what finishes the job.
        const core::Expected<core::SnapshotId, std::string> id =
            core::SnapshotId::Parse(id_or_alias);
        if (!id.ok()) return core::Unit();
        return repository.DeleteDocument(DocumentKey::BuildLog(id.value()));
    }
    const core::SnapshotId id = found.value()->id;

    // Marks the record, or reports what is already there. Note the two
    // outcomes that are *not* interchangeable: an absent record means another
    // delete finished first (stop), while an already-marked one means a
    // previous attempt stopped partway (continue the cleanup).
    const std::function<RepositoryResult<Decision<core::Optional<SnapshotRecord> > >(
        const core::SnapshotId&, const core::Optional<SnapshotRecord>&)>
        mark = [](const core::SnapshotId&, const core::Optional<SnapshotRecord>& current)
            -> RepositoryResult<Decision<core::Optional<SnapshotRecord> > > {
        Decision<core::Optional<SnapshotRecord> > decision;
        if (!current.has_value()) return decision;

        if (current->deleting) {
            // Already marked: nothing to write, but the cleanup continues
            // from the record as it stands.
            decision.value = current;
            return decision;
        }
        SnapshotRecord record = *current;
        record.deleting = true;
        decision.write = core::Optional<SnapshotRecord>(record);
        decision.value = core::Optional<SnapshotRecord>(record);
        return decision;
    };

    const RepositoryResult<core::Optional<SnapshotRecord> > marked =
        UpdateRecord<core::Optional<SnapshotRecord> >(repository, id, mark);
    if (!marked.ok()) return core::make_unexpected(marked.error());
    // Another delete finished first.
    if (!marked.value().has_value()) return core::Unit();

    if (marked.value()->alias.has_value()) {
        const RepositoryResult<core::Unit> released =
            ReleaseAlias(repository, *marked.value()->alias, id);
        if (!released.ok()) return core::make_unexpected(released.error());
    }

    // Read off the record *as marked*, which includes a commit that landed
    // before the mark: that commit is the only place the publications are
    // named, and the record is about to be removed.
    std::vector<PersistedDiskImagePublication> publications;
    if (marked.value()->committed.has_value()) {
        publications = marked.value()->committed->disk_publications;
    }

    // Files before the record, so a failure here leaves the record that still
    // names them and the delete can be retried. The source-registry
    // publications are rolled back first, inside `DeleteSnapshot`.
    const RepositoryResult<core::Unit> blobs =
        DeleteSnapshot(repository, id, publications, AcrPublicationRollback());
    if (!blobs.ok()) return core::make_unexpected(blobs.error());

    const RepositoryResult<core::Unit> log =
        repository.DeleteDocument(DocumentKey::BuildLog(id));
    if (!log.ok()) return core::make_unexpected(log.error());

    return repository.DeleteDocument(DocumentKey::SnapshotRecord(id));
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

// ---- commit ----------------------------------------------------------------

RepositoryResult<SnapshotRecord> Commit(SnapshotRepository& repository,
                                        const SnapshotPublishMetadata& metadata,
                                        const CommittedSnapshot& committed) {
    const int64_t now = NowUnixMs();

    // Rust's `next` closure: the pure pre-state -> committed-record step.
    const std::function<RepositoryResult<SnapshotRecord>(const core::Optional<SnapshotRecord>&)>
        next = [&metadata, &committed, now](const core::Optional<SnapshotRecord>& current)
        -> RepositoryResult<SnapshotRecord> {
        if (!current.has_value()) {
            // A sandbox snapshot has no pre-created `Waiting` record the way a
            // template build does, so the record is built outright.
            return NewCommittedRecord(metadata, committed, now);
        }
        SnapshotRecord record = *current;
        // Republishing over a live record would overwrite the files it points
        // at, and `publish` deletes what it stored when commit fails — which
        // would take the existing snapshot's artifacts with it.
        if (record.committed.has_value()) {
            return core::make_unexpected(RepositoryError::InvalidRequest(
                "snapshot '" + record.id.ToString() + "' is already committed"));
        }
        // Its artifacts may already be gone. `InvalidRequest` rather than
        // `SnapshotNotFound`: the caller asked to commit something that is on
        // its way out, and a not-found would invite a retry that recreates the
        // record a delete is in the middle of removing.
        if (record.deleting) {
            return core::make_unexpected(RepositoryError::InvalidRequest(
                "snapshot '" + record.id.ToString() + "' is being deleted"));
        }
        record.MarkCommitted(metadata.alias, metadata.resources, committed, metadata.source,
                             now);
        return record;
    };

    // `UpdateRecord` reports the outcome by running `decide` a second time on
    // the state the write was applied to, so the pair below is `(pre-state,
    // committed record)`. The committed record is *computed*, never read back:
    // `UpdateDocument` returns the document the update was applied to, which
    // is the state *before* the write — decoding that as the result would
    // return the uncommitted record this commit just replaced.
    typedef std::pair<core::Optional<SnapshotRecord>, SnapshotRecord> CommitPair;
    const std::function<RepositoryResult<Decision<CommitPair> >(
        const core::SnapshotId&, const core::Optional<SnapshotRecord>&)>
        decide = [&next](const core::SnapshotId&,
                         const core::Optional<SnapshotRecord>& current)
            -> RepositoryResult<Decision<CommitPair> > {
        const RepositoryResult<SnapshotRecord> computed = next(current);
        if (!computed.ok()) return core::make_unexpected(computed.error());
        Decision<CommitPair> decision;
        decision.write = core::Optional<SnapshotRecord>(computed.value());
        decision.value = CommitPair(current, computed.value());
        return decision;
    };

    const RepositoryResult<CommitPair> pair =
        UpdateRecord<CommitPair>(repository, metadata.id, decide);
    if (!pair.ok()) return core::make_unexpected(pair.error());

    const core::Optional<SnapshotRecord> previous = pair.value().first;
    const SnapshotRecord record = pair.value().second;

    // The record is written first and the alias claimed second: a record
    // holding an alias it does not own is findable by id, invisible by name,
    // and reconciled by nothing, so the claim is what confirms the commit.
    if (!metadata.alias.has_value()) return record;

    const RepositoryResult<core::Unit> claimed =
        ClaimAlias(repository, *metadata.alias, metadata.id);
    if (claimed.ok()) return record;

    // Restore the record this commit replaced, unless it changed since.
    // Reverting unconditionally would discard a concurrent writer's record;
    // not reverting would leave a committed snapshot nobody can reach by the
    // name it claims.
    const std::string written = EncodeRecord(record);
    const core::Optional<std::string> restore =
        previous.has_value() ? core::Optional<std::string>(EncodeRecord(*previous))
                             : core::Optional<std::string>();
    const DocumentUpdate revert =
        [&written, &restore](
            const core::Optional<std::string>& current) -> RepositoryResult<DocumentChange> {
        if (!current.has_value() || *current != written) return DocumentChange::Keep();
        if (!restore.has_value()) return DocumentChange::Delete();
        return DocumentChange::Put(*restore);
    };
    const RepositoryResult<core::Optional<std::string> > rolled_back =
        repository.UpdateDocument(DocumentKey::SnapshotRecord(metadata.id), revert);
    if (!rolled_back.ok()) {
        // Logged, not returned: the caller must see the alias conflict, which
        // is the actionable cause. Reporting the rollback failure instead
        // would name a symptom.
        AGENTENV_WARN("failed to roll back snapshot record " << metadata.id.ToString() << ": "
                                                            << rolled_back.error().ToString());
    }
    return core::make_unexpected(claimed.error());
}

// ---- attach_startup_manifest ----------------------------------------------

RepositoryResult<bool> AttachStartupManifest(SnapshotRepository& repository,
                                             const core::SnapshotId& id,
                                             const MemoryStartupPackInfo& info) {
    const int64_t now = NowUnixMs();
    const std::function<RepositoryResult<Decision<bool> >(
        const core::SnapshotId&, const core::Optional<SnapshotRecord>&)>
        decide = [now, &info](const core::SnapshotId& id,
                              const core::Optional<SnapshotRecord>& current)
            -> RepositoryResult<Decision<bool> > {
        Decision<bool> decision;

        // Gone or being deleted is *not* an error: the manifest is built
        // after publication returns, so a delete can legitimately win the
        // race. The caller uses the false to remove the manifest file it just
        // stored, which is otherwise referenced by no record and collected by
        // nothing.
        if (!current.has_value() || current->deleting) {
            decision.value = false;
            return decision;
        }

        SnapshotRecord record = *current;
        if (!record.committed.has_value()) {
            // A caller bug rather than a race. Reported as a race, it would
            // silently drop the manifest.
            return core::make_unexpected(RepositoryError::InvalidRequest(
                "snapshot '" + id.ToString() + "' is not committed"));
        }
        // Attaching twice is a no-op: the first manifest is already the one
        // the record names.
        if (record.committed->memory_startup.has_value()) {
            decision.value = true;
            return decision;
        }

        record.committed->memory_startup = core::Optional<MemoryStartupPackInfo>(info);
        record.updated_at_unix_ms = now;
        decision.write = core::Optional<SnapshotRecord>(record);
        decision.value = true;
        return decision;
    };
    return UpdateRecord<bool>(repository, id, decide);
}

}  // namespace record_catalog
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
