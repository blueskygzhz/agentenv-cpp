// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/common/ — in-memory reference impl.
#include "agentenv/snapshot/repository/backends.h"

#include "agentenv/core/fs.h"
#include "agentenv/snapshot/repository/oss/repository.h"
#include "agentenv/snapshot/repository/posixfs/backend.h"

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

// ---- backends/oss ----
//
// `MakePosixFsRepository(root_dir)` used to live here and was removed rather
// than fixed. It ignored `root_dir` and returned an `InMemorySnapshotRepository`
// -- so a caller that believed the name got a repository whose documents all
// answer "unsupported operation: repository documents", and whose data went
// nowhere near the root it passed in. Nothing called it: `main.cc` already
// builds `posixfs::PosixFsSnapshotRepository` directly, which is the only way
// to supply the catalog *and* artifact stores it needs. A factory that cannot
// take those is not a factory for this backend.

cfg::SnapshotImageStoragePolicy SnapshotImageStorageFor(const cfg::AppConfig& config) {
    return config.snapshot.image_publish.enabled
               ? cfg::SnapshotImageStoragePolicy::SourceRegistry
               : cfg::SnapshotImageStoragePolicy::ObjectStorage;
}

RepositoryResult<std::shared_ptr<SnapshotRepository> > MakeOssRepository(
    const cfg::OssBackendConfig& config,
    const cfg::SnapshotPublishCompressionConfig& publish_compression,
    cfg::SnapshotImageStoragePolicy image_storage,
    std::shared_ptr<oss::OssClient>* client_out) {
    // Rust `OssRepository::from_config`: normalise, then connect. Validation
    // first, so an incomplete section is reported as itself rather than as a
    // 403 on the first signed request.
    const RepositoryResult<oss::OssClientConfig> normalized =
        oss::OssClientConfig::FromBackendConfig(config);
    if (!normalized.ok()) return core::make_unexpected(normalized.error());

    const RepositoryResult<oss::OssClient*> client = oss::OssClient::New(normalized.value());
    if (!client.ok()) return core::make_unexpected(client.error());
    const std::shared_ptr<oss::OssClient> owned(client.value());

    // The multipart uploader is built here rather than lazily: it re-derives
    // the same endpoint and credentials, and a mistake in that derivation
    // should surface at startup, not at the first published layer.
    const RepositoryResult<std::shared_ptr<oss::ObjectUploader> > uploader =
        oss::MakeOssBackendUploader(normalized.value());
    if (!uploader.ok()) return core::make_unexpected(uploader.error());

    // Rust `OssRepository::new`: the policy is captured here, so it cannot
    // change under a publish already in flight.
    PublishPolicy policy;
    policy.compression = publish_compression;
    policy.image_storage = image_storage;

    // Handed out only after every fallible step above succeeded, so a caller
    // that ignores the error cannot end up holding a client built over a
    // config this function rejected.
    if (client_out != 0) *client_out = owned;
    return std::shared_ptr<SnapshotRepository>(
        new oss::OssSnapshotRepository(owned, uploader.value(), policy));
}

RepositoryResult<std::shared_ptr<SnapshotRepository> > MakeRepositoryFromConfig(
    const cfg::AppConfig& config) {
    switch (config.snapshot.repository_backend) {
        case cfg::SnapshotRepositoryBackendKind::PosixFs: {
            if (!config.backend.posix_fs.has_value()) {
                // Normalisation inserts a default section for this backend
                // (`cfg.cc:578`), so reaching here means the config was not
                // normalised -- not that an operator omitted it. Reported
                // rather than defaulted, because guessing a snapshot store
                // root would put a node's layers somewhere nobody chose.
                return core::make_unexpected(RepositoryError::InvalidRequest(
                    "backend.posix_fs config is required when repository_backend = posix_fs"));
            }
            // Rust `config.backend.posix_fs.snapshot_store.join("repository")`.
            const std::string root =
                core::fs::Join(config.backend.posix_fs->snapshot_store, "repository");
            // Both stores take the same root; the split is internal to this
            // backend (catalog records vs. blobs), not two configured paths.
            return std::shared_ptr<SnapshotRepository>(new posixfs::PosixFsSnapshotRepository(
                std::make_shared<posixfs::PosixFsCatalogStore>(root),
                std::make_shared<posixfs::PosixFsArtifactStore>(root)));
        }
        case cfg::SnapshotRepositoryBackendKind::Oss: {
            if (!config.backend.oss.has_value()) {
                return core::make_unexpected(RepositoryError::InvalidRequest(
                    "backend.oss config is required when repository_backend = oss"));
            }
            // Rust `backends/mod.rs:90-94` derives the policy from
            // `snapshot.image_publish.enabled`, and only on this backend.
            return MakeOssRepository(*config.backend.oss, config.snapshot.publish_compression,
                                     SnapshotImageStorageFor(config));
        }
    }
    // Unreachable for the two declared kinds; present because a `switch` over
    // an enum class without a default is only exhaustive as long as the enum
    // is, and a third backend should fail loudly here rather than fall
    // through to a null repository.
    return core::make_unexpected(
        RepositoryError::InvalidRequest("unknown snapshot.repository_backend"));
}

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
