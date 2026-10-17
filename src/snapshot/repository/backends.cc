// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/common/ — in-memory reference impl.
#include "agentenv/snapshot/repository/backends.h"

namespace agentenv {
namespace snapshot {
namespace repository {

namespace {

/// The alias a record is indexed under, or empty when it has none.
std::string AliasKey(const SnapshotRecord& record) {
    return record.alias.has_value() ? record.alias->ToString() : std::string();
}

}  // namespace

core::Optional<SnapshotRecord>
InMemorySnapshotRepository::FindLocked(const std::string& id_or_alias) const {
    std::map<std::string, SnapshotRecord>::const_iterator it = by_id_.find(id_or_alias);
    if (it != by_id_.end()) return core::Optional<SnapshotRecord>(it->second);
    std::map<std::string, std::string>::const_iterator a = alias_to_id_.find(id_or_alias);
    if (a != alias_to_id_.end()) {
        std::map<std::string, SnapshotRecord>::const_iterator it2 = by_id_.find(a->second);
        if (it2 != by_id_.end()) return core::Optional<SnapshotRecord>(it2->second);
    }
    return core::Optional<SnapshotRecord>();
}

RepositoryResult<SnapshotRecord>
InMemorySnapshotRepository::Create(const SnapshotRecord& record) {
    std::lock_guard<std::mutex> g(mu_);
    // A committed payload means the record describes artifacts that already
    // exist; `create` only pre-registers a template that has yet to be built.
    if (record.committed.has_value()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("create rejects committed records"));
    }
    if (!record.source.is_template()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("create only accepts template records"));
    }
    by_id_[record.id.ToString()] = record;
    const std::string alias = AliasKey(record);
    if (!alias.empty()) alias_to_id_[alias] = record.id.ToString();
    return record;
}

RepositoryResult<core::Optional<SnapshotRecord> >
InMemorySnapshotRepository::Get(const std::string& id_or_alias) {
    std::lock_guard<std::mutex> g(mu_);
    return FindLocked(id_or_alias);
}

RepositoryResult<std::vector<SnapshotRecord> >
InMemorySnapshotRepository::List(const SnapshotListFilter& filter) {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<SnapshotRecord> out;
    for (std::map<std::string, SnapshotRecord>::const_iterator it = by_id_.begin();
         it != by_id_.end(); ++it) {
        if (filter.Matches(it->second)) out.push_back(it->second);
    }
    return out;
}

RepositoryResult<core::Unit>
InMemorySnapshotRepository::Delete(const std::string& id_or_alias) {
    std::lock_guard<std::mutex> g(mu_);
    // Idempotent — deleting a missing record is success.
    core::Optional<SnapshotRecord> rec = FindLocked(id_or_alias);
    if (rec.has_value()) {
        by_id_.erase(rec->id.ToString());
        const std::string alias = AliasKey(*rec);
        if (!alias.empty()) alias_to_id_.erase(alias);
    }
    return core::Unit{};
}

RepositoryResult<core::Optional<core::SnapshotId> >
InMemorySnapshotRepository::ResolveAlias(const std::string& alias) {
    std::lock_guard<std::mutex> g(mu_);
    std::map<std::string, std::string>::const_iterator a = alias_to_id_.find(alias);
    if (a == alias_to_id_.end()) return core::Optional<core::SnapshotId>();
    // The index stores the id as text; parsing it back cannot fail for an
    // entry this class wrote, and a corrupt one is reported rather than
    // returned as a nil id that would then miss on every lookup.
    const core::Expected<core::SnapshotId, std::string> parsed =
        core::SnapshotId::Parse(a->second);
    if (!parsed.ok()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            "alias '" + alias + "' maps to a malformed snapshot id"));
    }
    return core::Optional<core::SnapshotId>(parsed.value());
}

RepositoryResult<SnapshotRecord>
InMemorySnapshotRepository::TryStartBuild(const core::SnapshotId& id) {
    std::lock_guard<std::mutex> g(mu_);
    std::map<std::string, SnapshotRecord>::iterator it = by_id_.find(id.ToString());
    if (it == by_id_.end()) {
        return core::make_unexpected(RepositoryError::SnapshotNotFound(id.ToString()));
    }
    if (!it->second.source.is_template()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("snapshot '") + id.ToString() + "' is not a template build"));
    }
    if (it->second.source.build.status != TemplateBuildStatus::Waiting) {
        // What keeps two builders from claiming the same template. The real
        // backends hold a file lock across this check; here the mutex is it.
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("template build '") + id.ToString() + "' is not in waiting state"));
    }
    it->second.source.build.status = TemplateBuildStatus::Building;
    // Any reason from an earlier failed attempt is cleared: the record now
    // describes a build in flight, and a stale error would be reported
    // alongside it.
    it->second.source.build.error_reason = core::Optional<TemplateBuildErrorReason>();
    return it->second;
}

RepositoryResult<core::Unit>
InMemorySnapshotRepository::MarkBuildError(const core::SnapshotId& id,
                                           const TemplateBuildErrorReason& reason) {
    std::lock_guard<std::mutex> g(mu_);
    std::map<std::string, SnapshotRecord>::iterator it = by_id_.find(id.ToString());
    if (it == by_id_.end()) {
        return core::make_unexpected(RepositoryError::SnapshotNotFound(id.ToString()));
    }
    it->second.source.build.status = TemplateBuildStatus::Error;
    it->second.source.build.error_reason = core::Optional<TemplateBuildErrorReason>(reason);
    return core::Unit{};
}

// ---- backends/posixfs, backends/oss ----

std::unique_ptr<SnapshotRepository> MakePosixFsRepository(const std::string& /*root_dir*/) {
    // Still the in-memory store, but no longer for want of an implementation:
    // `posixfs::PosixFsBackend::repository()` is the real one. What is missing
    // here is the rest of its construction — an `OverlaybdLayerStore` and a
    // `LocalArtifactCache`, neither of which this factory signature carries.
    // Callers that have them should build `PosixFsBackend` directly.
    return std::unique_ptr<SnapshotRepository>(new InMemorySnapshotRepository());
}
std::unique_ptr<SnapshotRepository> MakeOssRepository(const std::string& /*bucket_url*/) {
    // TODO: object-store backend (needs libcurl / object-store-operator).
    return std::unique_ptr<SnapshotRepository>(new InMemorySnapshotRepository());
}

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
