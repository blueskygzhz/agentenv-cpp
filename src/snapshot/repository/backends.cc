// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/common/ — in-memory reference impl.
#include "agentenv/snapshot/repository/backends.h"

namespace agentenv {
namespace snapshot {
namespace repository {

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
    if (record.committed) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("create rejects committed records"));
    }
    if (record.source != SnapshotSourceKind::Template) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("create only accepts template records"));
    }
    by_id_[record.id] = record;
    if (!record.alias.empty()) alias_to_id_[record.alias] = record.id;
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
        by_id_.erase(rec->id);
        if (!rec->alias.empty()) alias_to_id_.erase(rec->alias);
    }
    return core::Unit{};
}

RepositoryResult<core::Optional<std::string> >
InMemorySnapshotRepository::ResolveAlias(const std::string& alias) {
    std::lock_guard<std::mutex> g(mu_);
    std::map<std::string, std::string>::const_iterator a = alias_to_id_.find(alias);
    if (a == alias_to_id_.end()) return core::Optional<std::string>();
    return core::Optional<std::string>(a->second);
}

RepositoryResult<SnapshotRecord>
InMemorySnapshotRepository::TryStartBuild(const std::string& id) {
    std::lock_guard<std::mutex> g(mu_);
    std::map<std::string, SnapshotRecord>::iterator it = by_id_.find(id);
    if (it == by_id_.end()) {
        return core::make_unexpected(RepositoryError::SnapshotNotFound(id));
    }
    if (it->second.source != SnapshotSourceKind::Template) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("try_start_build rejects non-template records"));
    }
    if (it->second.build_status != TemplateBuildStatus::Waiting) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("template build is no longer waiting"));
    }
    it->second.build_status = TemplateBuildStatus::Building;
    return it->second;
}

RepositoryResult<core::Unit>
InMemorySnapshotRepository::MarkBuildError(const std::string& id, const std::string& /*reason*/) {
    std::lock_guard<std::mutex> g(mu_);
    std::map<std::string, SnapshotRecord>::iterator it = by_id_.find(id);
    if (it == by_id_.end()) {
        return core::make_unexpected(RepositoryError::SnapshotNotFound(id));
    }
    it->second.build_status = TemplateBuildStatus::Error;
    return core::Unit{};
}

// ---- backends/posixfs, backends/oss: skeleton factories ----
std::unique_ptr<SnapshotRepository> MakePosixFsRepository(const std::string& /*root_dir*/) {
    // TODO: durable record files + alias symlinks under root_dir.
    return std::unique_ptr<SnapshotRepository>(new InMemorySnapshotRepository());
}
std::unique_ptr<SnapshotRepository> MakeOssRepository(const std::string& /*bucket_url*/) {
    // TODO: object-store backend (needs libcurl / object-store-operator).
    return std::unique_ptr<SnapshotRepository>(new InMemorySnapshotRepository());
}

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
