// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/catalog.rs
#include "agentenv/snapshot/repository/posixfs/catalog.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>

#include "agentenv/core/fs.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace posixfs {
namespace {

typedef PosixFsSnapshotArtifactLayout Paths;

/// Rust `RepositoryError::backend(message, io_error)`.
RepositoryError Backend(const std::string& message, const std::string& source) {
    return RepositoryError::Backend(message, source);
}

/// `<dir>/<stem>.json` -> `<stem>`; empty when the name is not a `.json`.
std::string JsonFileStem(const std::string& file_name) {
    const std::string suffix = ".json";
    if (file_name.size() <= suffix.size()) return std::string();
    if (file_name.compare(file_name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return std::string();
    }
    return file_name.substr(0, file_name.size() - suffix.size());
}

std::string CurrentPidText() {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%ld", static_cast<long>(::getpid()));
    return buffer;
}

}  // namespace

// ---- BuildCacheState ------------------------------------------------------

RepositoryResult<BuildCacheState> BuildCacheState::Decode(const std::string& bytes) {
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(bytes);
    if (!json.ok()) {
        return core::make_unexpected(
            Backend("read build cache state", "invalid json"));
    }
    if (json.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(Backend("read build cache state", "not an object"));
    }
    const core::JsonObject& fields = json.value().as_object();

    BuildCacheState state;
    const core::JsonObject::const_iterator current = fields.find("current");
    if (current != fields.end() && current->second.kind() == core::Json::Kind::String) {
        state.current = core::Optional<std::string>(current->second.as_string());
    }
    const core::JsonObject::const_iterator retired = fields.find("retired");
    if (retired != fields.end() && retired->second.kind() == core::Json::Kind::Array) {
        const core::JsonArray& values = retired->second.as_array();
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i].kind() != core::Json::Kind::String) {
                return core::make_unexpected(
                    Backend("read build cache state", "retired must contain strings"));
            }
            state.retired.insert(values[i].as_string());
        }
    }

    // Every id becomes a path component, so a `../` here would let the cache
    // file point outside the repository.
    if (state.current.has_value() && !volume::IsValidVolumeComponent(*state.current)) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("invalid build cache volume ID '") + *state.current + "'"));
    }
    for (std::set<std::string>::const_iterator it = state.retired.begin();
         it != state.retired.end(); ++it) {
        if (!volume::IsValidVolumeComponent(*it)) {
            return core::make_unexpected(RepositoryError::InvalidRequest(
                std::string("invalid build cache volume ID '") + *it + "'"));
        }
    }
    if (state.current.has_value() && state.retired.count(*state.current) > 0) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("current build cache seed is also retired"));
    }
    return state;
}

core::Json BuildCacheState::ToJson() const {
    core::JsonObject object;
    object["current"] = current.has_value() ? core::Json(*current) : core::Json();
    core::JsonArray values;
    for (std::set<std::string>::const_iterator it = retired.begin(); it != retired.end();
         ++it) {
        values.push_back(core::Json(*it));
    }
    object["retired"] = core::Json(values);
    return core::Json(object);
}

RepositoryResult<core::Optional<std::string> > BuildCacheState::Replace(
    const std::string& id) {
    if (!volume::IsValidVolumeComponent(id)) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("invalid build cache volume ID '") + id + "'"));
    }
    if (retired.count(id) > 0) {
        // Resurrecting a discarded seed would reintroduce whatever the
        // operator retired it for.
        return core::make_unexpected(
            RepositoryError::InvalidRequest("cannot publish a retired build cache seed"));
    }
    const core::Optional<std::string> previous = current;
    current = core::Optional<std::string>(id);
    if (previous.has_value() && *previous != id) {
        retired.insert(*previous);
    }
    return previous;
}

// ---- paths and layout -----------------------------------------------------

PosixFsSnapshotArtifactLayout PosixFsCatalogStore::Layout(
    const core::SnapshotId& snapshot_id) const {
    return PosixFsSnapshotArtifactLayout(root_, snapshot_id);
}

std::string PosixFsCatalogStore::CommitMarkerPath(const core::SnapshotId& id) const {
    return Layout(id).Path(kPosixFsSnapshotCommitMarker);
}

std::string PosixFsCatalogStore::RecordPath(const core::SnapshotId& id) const {
    return Paths::RecordPath(root_, id);
}

RepositoryResult<core::Unit> PosixFsCatalogStore::EnsureLayout() const {
    const std::string dirs[] = {
        Paths::CatalogDir(root_),      Paths::AliasesDir(root_),
        Paths::RecordsDir(root_),      Paths::VolumeAliasesDir(root_),
        Paths::VolumeRecordsDir(root_), Paths::SnapshotsDir(root_),
    };
    for (std::size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        const core::Expected<core::Unit, std::string> created = core::fs::CreateDirAll(dirs[i]);
        if (!created.ok()) {
            return core::make_unexpected(
                Backend("create catalog dir '" + dirs[i] + "'", created.error()));
        }
    }
    return core::Unit();
}

// ---- JSON I/O -------------------------------------------------------------

RepositoryResult<core::Json> PosixFsCatalogStore::ReadJson(const std::string& path) const {
    const core::Expected<std::string, std::string> bytes = core::fs::ReadToString(path);
    if (!bytes.ok()) {
        return core::make_unexpected(Backend("read '" + path + "'", bytes.error()));
    }
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(bytes.value());
    if (!json.ok()) {
        return core::make_unexpected(Backend("parse json '" + path + "'", "invalid json"));
    }
    return json.value();
}

RepositoryResult<core::Unit> PosixFsCatalogStore::WriteJson(const std::string& path,
                                                            const core::Json& value) const {
    // Rust writes through a sibling temp file and renames, so a reader never
    // observes a half-written record even though it takes no lock.
    const core::Expected<core::Unit, std::string> written =
        core::WriteAtomic(path, value.ToString());
    if (!written.ok()) {
        return core::make_unexpected(Backend("write json '" + path + "'", written.error()));
    }
    return core::Unit();
}

RepositoryResult<core::Unit> PosixFsCatalogStore::WriteCommitMarker(
    const core::SnapshotId& id) const {
    const std::string path = CommitMarkerPath(id);
    const core::Expected<core::Unit, std::string> written =
        core::WriteAtomic(path, "committed");
    if (!written.ok()) {
        return core::make_unexpected(
            Backend("persist commit marker '" + path + "'", written.error()));
    }
    return core::Unit();
}

RepositoryResult<core::Unit> PosixFsCatalogStore::RemoveFileIfExists(
    const std::string& path) const {
    if (!core::fs::Exists(path)) return core::Unit();
    const core::Expected<core::Unit, std::string> removed = core::fs::RemoveFile(path);
    if (!removed.ok()) {
        return core::make_unexpected(Backend("remove '" + path + "'", removed.error()));
    }
    return core::Unit();
}

RepositoryResult<core::Unit> PosixFsCatalogStore::RemoveDirIfExists(
    const std::string& path) const {
    if (!core::fs::Exists(path)) return core::Unit();
    const core::Expected<core::Unit, std::string> removed = core::fs::RemoveDirAll(path);
    if (!removed.ok()) {
        return core::make_unexpected(Backend("remove '" + path + "'", removed.error()));
    }
    return core::Unit();
}

// ---- locks ----------------------------------------------------------------

RepositoryResult<core::FileLockGuard> PosixFsCatalogStore::AcquireCatalogLock(
    const std::string& lock_path, const std::string& label) const {
    bool timed_out = false;
    core::Expected<core::FileLockGuard, std::string> guard =
        core::AcquireFileLock(lock_path, CurrentPidText(), kFileLockTimeoutMs, &timed_out);
    if (guard.ok()) return std::move(guard.value());

    if (timed_out) {
        // Rust reports the timeout without a source error, so the message
        // names the lock rather than an errno.
        return core::make_unexpected(RepositoryError::Backend(
            "timed out waiting for " + label + " lock '" + lock_path + "'"));
    }
    return core::make_unexpected(
        Backend("lock " + label + " lock '" + lock_path + "'", guard.error()));
}

RepositoryResult<core::FileLockGuard> PosixFsCatalogStore::AcquireAliasLock(
    const SnapshotAlias& alias) const {
    return AcquireCatalogLock(Paths::AliasLockPath(root_, alias), "alias");
}

RepositoryResult<core::FileLockGuard> PosixFsCatalogStore::AcquireRecordLock(
    const core::SnapshotId& id) const {
    return AcquireCatalogLock(Paths::RecordLockPath(root_, id), "record");
}

RepositoryResult<core::FileLockGuard> PosixFsCatalogStore::AcquireVolumeAliasLock(
    const std::string& alias) const {
    return AcquireCatalogLock(Paths::VolumeAliasLockPath(root_, alias), "volume alias");
}

RepositoryResult<core::FileLockGuard> PosixFsCatalogStore::AcquireVolumeRecordLock(
    const std::string& volume_id) const {
    return AcquireCatalogLock(Paths::VolumeRecordLockPath(root_, volume_id), "volume record");
}

// ---- record helpers -------------------------------------------------------

RepositoryResult<core::Optional<SnapshotRecord> >
PosixFsCatalogStore::LoadRecordByIdUnlocked(const core::SnapshotId& id) const {
    const std::string path = RecordPath(id);
    if (!core::fs::Exists(path)) return core::Optional<SnapshotRecord>();

    const RepositoryResult<core::Json> json = ReadJson(path);
    if (!json.ok()) return core::make_unexpected(json.error());
    const core::Expected<SnapshotRecord, std::string> record =
        SnapshotRecord::FromJson(json.value());
    if (!record.ok()) {
        return core::make_unexpected(Backend("parse json '" + path + "'", record.error()));
    }
    return core::Optional<SnapshotRecord>(record.value());
}

RepositoryResult<core::Optional<core::SnapshotId> > PosixFsCatalogStore::LoadAliasTarget(
    const SnapshotAlias& alias) const {
    const std::string path = Paths::AliasPath(root_, alias);
    if (!core::fs::Exists(path)) return core::Optional<core::SnapshotId>();

    const RepositoryResult<core::Json> json = ReadJson(path);
    if (!json.ok()) return core::make_unexpected(json.error());
    if (json.value().kind() != core::Json::Kind::String) {
        return core::make_unexpected(Backend("parse json '" + path + "'", "not a string"));
    }
    const core::Expected<core::SnapshotId, std::string> id =
        core::SnapshotId::Parse(json.value().as_string());
    if (!id.ok()) {
        return core::make_unexpected(Backend("parse json '" + path + "'", id.error()));
    }
    return core::Optional<core::SnapshotId>(id.value());
}

RepositoryResult<core::Unit> PosixFsCatalogStore::WriteRecordUnlocked(
    const SnapshotRecord& record) const {
    return WriteJson(RecordPath(record.id), record.ToJson());
}

RepositoryResult<core::Unit> PosixFsCatalogStore::EnsureAliasAvailable(
    const SnapshotAlias& alias, const core::SnapshotId& new_id) const {
    const std::string alias_path = Paths::AliasPath(root_, alias);
    const RepositoryResult<core::Optional<core::SnapshotId> > existing =
        LoadAliasTarget(alias);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (!existing.value().has_value()) return core::Unit();

    if (*existing.value() == new_id) return core::Unit();

    const RepositoryResult<core::Optional<SnapshotRecord> > record =
        LoadRecordByIdUnlocked(*existing.value());
    if (!record.ok()) return core::make_unexpected(record.error());
    if (record.value().has_value()) {
        return core::make_unexpected(RepositoryError::AliasConflict(
            alias.ToString(), existing.value()->ToString(), new_id.ToString()));
    }
    // The alias points at a record that no longer exists, so it is stale
    // rather than a real conflict and can be rebound.
    return RemoveFileIfExists(alias_path);
}

RepositoryResult<SnapshotRecord> PosixFsCatalogStore::CommittedRecordUnlocked(
    const SnapshotPublishMetadata& metadata, const CommittedSnapshot& committed,
    int64_t now_unix_ms) const {
    const RepositoryResult<core::Optional<SnapshotRecord> > existing =
        LoadRecordByIdUnlocked(metadata.id);
    if (!existing.ok()) return core::make_unexpected(existing.error());

    if (existing.value().has_value()) {
        // A pre-created template record: advance it rather than replacing it,
        // so its created_at and build history survive.
        SnapshotRecord record = *existing.value();
        record.MarkCommitted(metadata.alias, metadata.resources, committed, metadata.source,
                             now_unix_ms);
        return record;
    }

    SnapshotRecord record;
    record.id = metadata.id;
    record.alias = metadata.alias;
    if (metadata.source.kind == SnapshotSourceKind::Sandbox) {
        record.source = SnapshotSource::Sandbox(metadata.source.source_sandbox_id);
    } else {
        TemplateBuildInfo build;
        build.status = TemplateBuildStatus::Ready;
        // No start time: this record was never a queued build.
        build.finished_at_unix_ms = core::Optional<int64_t>(now_unix_ms);
        record.source = SnapshotSource::Template(build);
    }
    record.resources = metadata.resources;
    record.created_at_unix_ms = now_unix_ms;
    record.updated_at_unix_ms = now_unix_ms;
    record.committed = core::Optional<CommittedSnapshot>(committed);
    return record;
}

bool PosixFsCatalogStore::IsCommitted(const core::SnapshotId& id) const {
    if (!core::fs::Exists(CommitMarkerPath(id))) return false;
    const RepositoryResult<core::Optional<SnapshotRecord> > record =
        LoadRecordByIdUnlocked(id);
    if (!record.ok() || !record.value().has_value()) return false;
    // The marker alone is not enough: a crash between marker and record would
    // leave a directory with no payload behind it.
    return record.value()->committed.has_value();
}

RepositoryResult<core::Unit> PosixFsCatalogStore::CleanupUncommittedSnapshotDir(
    const core::SnapshotId& id) const {
    if (IsCommitted(id)) return core::Unit();
    return RemoveDirIfExists(Layout(id).SnapshotDir());
}

// ---- publish lifecycle ----------------------------------------------------

RepositoryResult<PublishSession> PosixFsCatalogStore::BeginPublish(
    const core::SnapshotId& snapshot_id) {
    const RepositoryResult<core::Unit> layout = EnsureLayout();
    if (!layout.ok()) return core::make_unexpected(layout.error());

    const std::string snapshot_dir = Layout(snapshot_id).SnapshotDir();
    const core::Expected<core::Unit, std::string> created =
        core::fs::CreateDirAll(snapshot_dir);
    if (!created.ok()) {
        return core::make_unexpected(
            Backend("create snapshot dir '" + snapshot_dir + "'", created.error()));
    }
    PublishSession session;
    session.snapshot_id = snapshot_id;
    return session;
}

RepositoryResult<SnapshotRecord> PosixFsCatalogStore::CommitPublish(
    const PublishSession& session, const SnapshotPublishMetadata& metadata,
    const CommittedSnapshot& committed) {
    const int64_t now = NowUnixMs();
    const core::SnapshotId snapshot_id = metadata.id;

    RepositoryResult<SnapshotRecord> write_result =
        core::make_unexpected(RepositoryError::Backend("unreachable"));

    if (metadata.alias.has_value()) {
        const SnapshotAlias& alias = *metadata.alias;
        RepositoryResult<core::FileLockGuard> guard = AcquireAliasLock(alias);
        if (!guard.ok()) {
            write_result = core::make_unexpected(guard.error());
        } else {
            // Held for the whole critical section below.
            const core::FileLockGuard held(std::move(guard.value()));
            write_result = [&]() -> RepositoryResult<SnapshotRecord> {
                const RepositoryResult<SnapshotRecord> record =
                    CommittedRecordUnlocked(metadata, committed, now);
                if (!record.ok()) return core::make_unexpected(record.error());

                const std::string alias_path = Paths::AliasPath(root_, alias);
                const RepositoryResult<core::Optional<core::SnapshotId> > existing =
                    LoadAliasTarget(alias);
                if (!existing.ok()) return core::make_unexpected(existing.error());
                if (existing.value().has_value() && *existing.value() != snapshot_id) {
                    const RepositoryResult<core::Optional<SnapshotRecord> > other =
                        LoadRecordByIdUnlocked(*existing.value());
                    if (!other.ok()) return core::make_unexpected(other.error());
                    if (other.value().has_value()) {
                        return core::make_unexpected(RepositoryError::AliasConflict(
                            alias.ToString(), existing.value()->ToString(),
                            snapshot_id.ToString()));
                    }
                    const RepositoryResult<core::Unit> removed =
                        RemoveFileIfExists(alias_path);
                    if (!removed.ok()) return core::make_unexpected(removed.error());
                }

                const RepositoryResult<core::Unit> bound =
                    WriteJson(alias_path, core::Json(snapshot_id.ToString()));
                if (!bound.ok()) return core::make_unexpected(bound.error());

                // Marker before record: a crash in between leaves a directory
                // that `is_committed` still recognises as uncommitted.
                const RepositoryResult<core::Unit> marker =
                    WriteCommitMarker(session.snapshot_id);
                if (!marker.ok()) return core::make_unexpected(marker.error());

                const RepositoryResult<core::Unit> written =
                    WriteRecordUnlocked(record.value());
                if (!written.ok()) return core::make_unexpected(written.error());
                return record.value();
            }();
        }
    } else {
        write_result = [&]() -> RepositoryResult<SnapshotRecord> {
            const RepositoryResult<SnapshotRecord> record =
                CommittedRecordUnlocked(metadata, committed, now);
            if (!record.ok()) return core::make_unexpected(record.error());
            const RepositoryResult<core::Unit> marker =
                WriteCommitMarker(session.snapshot_id);
            if (!marker.ok()) return core::make_unexpected(marker.error());
            const RepositoryResult<core::Unit> written = WriteRecordUnlocked(record.value());
            if (!written.ok()) return core::make_unexpected(written.error());
            return record.value();
        }();
    }

    if (write_result.ok()) return write_result;

    // Rollback. Both steps are best-effort: the original error is what the
    // caller needs to see, and a failure to clean up must not mask it.
    if (metadata.alias.has_value()) {
        const SnapshotAlias& alias = *metadata.alias;
        RepositoryResult<core::FileLockGuard> guard = AcquireAliasLock(alias);
        if (guard.ok()) {
            const core::FileLockGuard held(std::move(guard.value()));
            const RepositoryResult<core::Optional<core::SnapshotId> > existing =
                LoadAliasTarget(alias);
            // Only unbind the alias if it still points at *this* publish;
            // another writer may have rebound it in the meantime.
            if (existing.ok() && existing.value().has_value() &&
                *existing.value() == snapshot_id) {
                RemoveFileIfExists(Paths::AliasPath(root_, alias));
            }
        }
    }
    CleanupUncommittedSnapshotDir(session.snapshot_id);
    return write_result;
}

RepositoryResult<core::Unit> PosixFsCatalogStore::AbortPublish(
    const PublishSession& session) {
    return CleanupUncommittedSnapshotDir(session.snapshot_id);
}

// ---- snapshot records -----------------------------------------------------

RepositoryResult<SnapshotRecord> PosixFsCatalogStore::Create(const SnapshotRecord& record) {
    const RepositoryResult<core::Unit> layout = EnsureLayout();
    if (!layout.ok()) return core::make_unexpected(layout.error());

    if (!record.source.is_template()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("only template snapshots can be pre-created"));
    }
    if (record.committed.has_value()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            "pre-created template snapshots must not already be committed"));
    }

    const RepositoryResult<core::Optional<SnapshotRecord> > existing =
        LoadRecordByIdUnlocked(record.id);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (existing.value().has_value()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("snapshot '") + record.id.ToString() + "' already exists"));
    }

    if (record.alias.has_value()) {
        const SnapshotAlias& alias = *record.alias;
        RepositoryResult<core::FileLockGuard> guard = AcquireAliasLock(alias);
        if (!guard.ok()) return core::make_unexpected(guard.error());
        const core::FileLockGuard held(std::move(guard.value()));

        const RepositoryResult<core::Unit> available =
            EnsureAliasAvailable(alias, record.id);
        if (!available.ok()) return core::make_unexpected(available.error());

        const RepositoryResult<core::Unit> written = WriteRecordUnlocked(record);
        if (!written.ok()) return core::make_unexpected(written.error());

        const RepositoryResult<core::Unit> bound = WriteJson(
            Paths::AliasPath(root_, alias), core::Json(record.id.ToString()));
        if (!bound.ok()) return core::make_unexpected(bound.error());
        return record;
    }

    const RepositoryResult<core::Unit> written = WriteRecordUnlocked(record);
    if (!written.ok()) return core::make_unexpected(written.error());
    return record;
}

RepositoryResult<core::Optional<SnapshotRecord> > PosixFsCatalogStore::Get(
    const std::string& id_or_alias) {
    const RepositoryResult<core::Unit> layout = EnsureLayout();
    if (!layout.ok()) return core::make_unexpected(layout.error());

    // A parseable id wins; only if that misses is the string treated as an
    // alias, which is how an alias shaped like a uuid stays reachable.
    const core::Expected<core::SnapshotId, std::string> direct_id =
        core::SnapshotId::Parse(id_or_alias);
    if (direct_id.ok()) {
        const RepositoryResult<core::Optional<SnapshotRecord> > record =
            LoadRecordByIdUnlocked(direct_id.value());
        if (!record.ok()) return core::make_unexpected(record.error());
        if (record.value().has_value()) return record;
    }

    const core::Expected<SnapshotAlias, std::string> alias =
        SnapshotAlias::Parse(id_or_alias);
    if (!alias.ok()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(alias.error()));
    }

    RepositoryResult<core::FileLockGuard> guard = AcquireAliasLock(alias.value());
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));

    const RepositoryResult<core::Optional<core::SnapshotId> > target =
        LoadAliasTarget(alias.value());
    if (!target.ok()) return core::make_unexpected(target.error());
    if (!target.value().has_value()) return core::Optional<SnapshotRecord>();

    const RepositoryResult<core::Optional<SnapshotRecord> > record =
        LoadRecordByIdUnlocked(*target.value());
    if (!record.ok()) return core::make_unexpected(record.error());
    if (record.value().has_value()) return record;

    // The alias outlived its record; drop it so the name becomes reusable.
    const RepositoryResult<core::Unit> removed =
        RemoveFileIfExists(Paths::AliasPath(root_, alias.value()));
    if (!removed.ok()) return core::make_unexpected(removed.error());
    return core::Optional<SnapshotRecord>();
}

bool PosixFsCatalogStore::MatchesRecordFilter(const SnapshotRecord& record,
                                              const SnapshotListFilter& filter) {
    if (filter.alias_prefix.has_value()) {
        if (!record.alias.has_value()) return false;
        const std::string alias = record.alias->ToString();
        if (alias.size() < filter.alias_prefix->size() ||
            alias.compare(0, filter.alias_prefix->size(), *filter.alias_prefix) != 0) {
            return false;
        }
    }

    if (filter.snapshot_ids.has_value()) {
        bool found = false;
        for (std::size_t i = 0; i < filter.snapshot_ids->size(); ++i) {
            if ((*filter.snapshot_ids)[i] == record.id) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    if (filter.snapshot_id_or_alias.has_value()) {
        const bool id_matches = record.id.ToString() == *filter.snapshot_id_or_alias;
        const bool alias_matches =
            record.alias.has_value() && record.alias->ToString() == *filter.snapshot_id_or_alias;
        if (!id_matches && !alias_matches) return false;
    }

    if (filter.source_sandbox_id.has_value()) {
        // Only a sandbox-sourced record can match; a template never does.
        if (record.source.kind != SnapshotSourceKind::Sandbox) return false;
        if (record.source.source_sandbox_id != *filter.source_sandbox_id) return false;
    }

    if (filter.sources.has_value()) {
        bool found = false;
        for (std::size_t i = 0; i < filter.sources->size(); ++i) {
            if ((*filter.sources)[i] == record.source.kind) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    if (filter.template_statuses.has_value()) {
        // A status filter implies a template: a sandbox record has no build.
        if (record.source.kind != SnapshotSourceKind::Template) return false;
        bool found = false;
        for (std::size_t i = 0; i < filter.template_statuses->size(); ++i) {
            if ((*filter.template_statuses)[i] == record.source.build.status) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    return true;
}

namespace {

/// Rust's sort: newest first, ties broken by ascending id so the order is
/// total and a page boundary is reproducible.
bool RecordOrder(const SnapshotRecord& left, const SnapshotRecord& right) {
    if (left.created_at_unix_ms != right.created_at_unix_ms) {
        return left.created_at_unix_ms > right.created_at_unix_ms;
    }
    return left.id.ToString() < right.id.ToString();
}

}  // namespace

RepositoryResult<std::vector<SnapshotRecord> > PosixFsCatalogStore::List(
    const SnapshotListFilter& filter) {
    const RepositoryResult<core::Unit> layout = EnsureLayout();
    if (!layout.ok()) return core::make_unexpected(layout.error());

    const std::string records_dir = Paths::RecordsDir(root_);
    const core::Expected<std::vector<std::string>, std::string> entries =
        core::fs::ReadDir(records_dir);
    if (!entries.ok()) {
        return core::make_unexpected(
            Backend("read records dir '" + records_dir + "'", entries.error()));
    }

    std::vector<SnapshotRecord> records;
    for (std::size_t i = 0; i < entries.value().size(); ++i) {
        const std::string& name = entries.value()[i];
        // Skip the `.lock` siblings that live in the same directory.
        if (JsonFileStem(name).empty()) continue;

        const std::string path = core::fs::Join(records_dir, name);
        if (!core::fs::IsFile(path)) continue;

        const RepositoryResult<core::Json> json = ReadJson(path);
        if (!json.ok()) return core::make_unexpected(json.error());
        const core::Expected<SnapshotRecord, std::string> record =
            SnapshotRecord::FromJson(json.value());
        if (!record.ok()) {
            return core::make_unexpected(Backend("parse json '" + path + "'", record.error()));
        }
        if (MatchesRecordFilter(record.value(), filter)) {
            records.push_back(record.value());
        }
    }

    std::sort(records.begin(), records.end(), RecordOrder);
    return records;
}

RepositoryResult<core::Unit> PosixFsCatalogStore::DeleteRecord(const core::SnapshotId& id) {
    const RepositoryResult<core::Optional<SnapshotRecord> > existing =
        LoadRecordByIdUnlocked(id);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    // Idempotent: deleting an absent record is a success.
    if (!existing.value().has_value()) return core::Unit();

    const SnapshotRecord record = *existing.value();
    const PosixFsSnapshotArtifactLayout snapshot_layout = Layout(id);

    if (record.alias.has_value()) {
        const SnapshotAlias& alias = *record.alias;
        RepositoryResult<core::FileLockGuard> guard = AcquireAliasLock(alias);
        if (!guard.ok()) return core::make_unexpected(guard.error());
        const core::FileLockGuard held(std::move(guard.value()));

        const RepositoryResult<core::Unit> marker =
            RemoveFileIfExists(snapshot_layout.Path(kPosixFsSnapshotCommitMarker));
        if (!marker.ok()) return core::make_unexpected(marker.error());

        const RepositoryResult<core::Optional<core::SnapshotId> > target =
            LoadAliasTarget(alias);
        if (!target.ok()) return core::make_unexpected(target.error());
        if (target.value().has_value() && *target.value() == id) {
            // Only unbind if the alias still points here; it may have been
            // rebound to a newer snapshot.
            const RepositoryResult<core::Unit> removed =
                RemoveFileIfExists(Paths::AliasPath(root_, alias));
            if (!removed.ok()) return core::make_unexpected(removed.error());
        }
        if (record.committed.has_value()) {
            const RepositoryResult<core::Unit> removed =
                RemoveDirIfExists(snapshot_layout.SnapshotDir());
            if (!removed.ok()) return core::make_unexpected(removed.error());
        }
        return RemoveFileIfExists(RecordPath(id));
    }

    const RepositoryResult<core::Unit> marker = RemoveFileIfExists(CommitMarkerPath(id));
    if (!marker.ok()) return core::make_unexpected(marker.error());
    if (record.committed.has_value()) {
        // An uncommitted record owns no artifact directory yet.
        const RepositoryResult<core::Unit> removed =
            RemoveDirIfExists(snapshot_layout.SnapshotDir());
        if (!removed.ok()) return core::make_unexpected(removed.error());
    }
    return RemoveFileIfExists(RecordPath(id));
}

RepositoryResult<core::Optional<core::SnapshotId> > PosixFsCatalogStore::ResolveAlias(
    const std::string& alias_text) {
    const core::Expected<SnapshotAlias, std::string> alias = SnapshotAlias::Parse(alias_text);
    if (!alias.ok()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(alias.error()));
    }

    RepositoryResult<core::FileLockGuard> guard = AcquireAliasLock(alias.value());
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));

    const RepositoryResult<core::Optional<core::SnapshotId> > target =
        LoadAliasTarget(alias.value());
    if (!target.ok()) return core::make_unexpected(target.error());
    if (!target.value().has_value()) return core::Optional<core::SnapshotId>();

    const RepositoryResult<core::Optional<SnapshotRecord> > record =
        LoadRecordByIdUnlocked(*target.value());
    if (!record.ok()) return core::make_unexpected(record.error());
    if (record.value().has_value()) return target;

    const RepositoryResult<core::Unit> removed =
        RemoveFileIfExists(Paths::AliasPath(root_, alias.value()));
    if (!removed.ok()) return core::make_unexpected(removed.error());
    return core::Optional<core::SnapshotId>();
}

RepositoryResult<SnapshotRecord> PosixFsCatalogStore::TryStart(const core::SnapshotId& id) {
    RepositoryResult<core::FileLockGuard> guard = AcquireRecordLock(id);
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));

    const RepositoryResult<core::Optional<SnapshotRecord> > existing =
        LoadRecordByIdUnlocked(id);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (!existing.value().has_value()) {
        return core::make_unexpected(RepositoryError::SnapshotNotFound(id.ToString()));
    }

    SnapshotRecord record = *existing.value();
    const int64_t now = NowUnixMs();
    if (!record.source.is_template()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("snapshot '") + id.ToString() + "' is not a template build"));
    }
    if (record.source.build.status != TemplateBuildStatus::Waiting) {
        // The lock plus this check is what keeps two builders from claiming
        // the same template.
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("template build '") + id.ToString() + "' is not in waiting state"));
    }

    record.source.build.status = TemplateBuildStatus::Building;
    record.source.build.started_at_unix_ms = core::Optional<int64_t>(now);
    record.source.build.error_reason = core::Optional<TemplateBuildErrorReason>();
    record.updated_at_unix_ms = now;

    const RepositoryResult<core::Unit> written = WriteRecordUnlocked(record);
    if (!written.ok()) return core::make_unexpected(written.error());
    return record;
}

RepositoryResult<core::Unit> PosixFsCatalogStore::MarkError(
    const core::SnapshotId& id, const TemplateBuildErrorReason& reason) {
    RepositoryResult<core::FileLockGuard> guard = AcquireRecordLock(id);
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));

    const RepositoryResult<core::Optional<SnapshotRecord> > existing =
        LoadRecordByIdUnlocked(id);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (!existing.value().has_value()) {
        return core::make_unexpected(RepositoryError::SnapshotNotFound(id.ToString()));
    }

    SnapshotRecord record = *existing.value();
    const int64_t now = NowUnixMs();
    if (!record.source.is_template()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("snapshot '") + id.ToString() + "' is not a template build"));
    }

    record.source.build.status = TemplateBuildStatus::Error;
    record.source.build.finished_at_unix_ms = core::Optional<int64_t>(now);
    record.source.build.error_reason = core::Optional<TemplateBuildErrorReason>(reason);
    record.updated_at_unix_ms = now;
    return WriteRecordUnlocked(record);
}

// ---- volumes --------------------------------------------------------------

RepositoryResult<core::Unit> PosixFsCatalogStore::EnsureVolumeComponent(
    const std::string& value, const std::string& kind) const {
    if (!volume::IsValidVolumeComponent(value)) {
        // These become path components, so a `..` or `/` would escape the
        // volumes directory.
        return core::make_unexpected(RepositoryError::InvalidRequest(
            "invalid volume " + kind + " '" + value + "'"));
    }
    return core::Unit();
}

RepositoryResult<core::Unit> PosixFsCatalogStore::EnsureVolumeId(
    const std::string& volume_id) const {
    return EnsureVolumeComponent(volume_id, "id");
}

RepositoryResult<core::Optional<volume::VolumeRecord> >
PosixFsCatalogStore::LoadVolumeByIdUnlocked(const std::string& volume_id) const {
    const RepositoryResult<core::Unit> valid = EnsureVolumeId(volume_id);
    if (!valid.ok()) return core::make_unexpected(valid.error());

    const std::string path = Paths::VolumeRecordPath(root_, volume_id);
    if (!core::fs::Exists(path)) return core::Optional<volume::VolumeRecord>();

    const RepositoryResult<core::Json> json = ReadJson(path);
    if (!json.ok()) return core::make_unexpected(json.error());
    const core::Expected<volume::VolumeRecord, std::string> record =
        volume::VolumeRecord::FromJson(json.value());
    if (!record.ok()) {
        return core::make_unexpected(Backend("parse json '" + path + "'", record.error()));
    }

    const RepositoryResult<core::Unit> id_valid = EnsureVolumeId(record.value().id);
    if (!id_valid.ok()) return core::make_unexpected(id_valid.error());
    if (record.value().id != volume_id) {
        // The file name is the lookup key, so a record whose own id differs
        // would be reachable under two names.
        return core::make_unexpected(RepositoryError::InvalidRequest(
            "volume record id '" + record.value().id + "' does not match path '" + path + "'"));
    }
    return core::Optional<volume::VolumeRecord>(record.value());
}

RepositoryResult<std::vector<std::string> > PosixFsCatalogStore::VolumeIdsUnlocked() const {
    const std::string directory = Paths::VolumeRecordsDir(root_);
    if (!core::fs::Exists(directory)) return std::vector<std::string>();

    const core::Expected<std::vector<std::string>, std::string> entries =
        core::fs::ReadDir(directory);
    if (!entries.ok()) {
        return core::make_unexpected(
            Backend("read volume directory '" + directory + "'", entries.error()));
    }

    std::vector<std::string> volume_ids;
    for (std::size_t i = 0; i < entries.value().size(); ++i) {
        const std::string stem = JsonFileStem(entries.value()[i]);
        if (stem.empty()) continue;
        if (!core::fs::IsFile(core::fs::Join(directory, entries.value()[i]))) continue;

        const RepositoryResult<core::Unit> valid = EnsureVolumeId(stem);
        if (!valid.ok()) return core::make_unexpected(valid.error());
        volume_ids.push_back(stem);
    }
    std::sort(volume_ids.begin(), volume_ids.end());
    return volume_ids;
}

RepositoryResult<core::Optional<volume::VolumeRecord> > PosixFsCatalogStore::GetVolume(
    const std::string& reference) {
    const RepositoryResult<core::Unit> valid =
        EnsureVolumeComponent(reference, "reference");
    if (!valid.ok()) return core::make_unexpected(valid.error());

    const RepositoryResult<core::Optional<volume::VolumeRecord> > by_id =
        LoadVolumeByIdUnlocked(reference);
    if (!by_id.ok()) return core::make_unexpected(by_id.error());
    if (by_id.value().has_value()) return by_id;

    RepositoryResult<core::FileLockGuard> guard = AcquireVolumeAliasLock(reference);
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));

    const std::string alias_path = Paths::VolumeAliasPath(root_, reference);
    if (!core::fs::Exists(alias_path)) return core::Optional<volume::VolumeRecord>();

    const RepositoryResult<core::Json> json = ReadJson(alias_path);
    if (!json.ok()) return core::make_unexpected(json.error());
    if (json.value().kind() != core::Json::Kind::String) {
        return core::make_unexpected(
            Backend("parse json '" + alias_path + "'", "not a string"));
    }
    const std::string volume_id = json.value().as_string();
    const RepositoryResult<core::Unit> id_valid = EnsureVolumeId(volume_id);
    if (!id_valid.ok()) return core::make_unexpected(id_valid.error());

    const RepositoryResult<core::Optional<volume::VolumeRecord> > record =
        LoadVolumeByIdUnlocked(volume_id);
    if (!record.ok()) return core::make_unexpected(record.error());
    if (record.value().has_value()) return record;

    const RepositoryResult<core::Unit> removed = RemoveFileIfExists(alias_path);
    if (!removed.ok()) return core::make_unexpected(removed.error());
    return core::Optional<volume::VolumeRecord>();
}

RepositoryResult<VolumeRecordPage> PosixFsCatalogStore::ListVolumesPage(
    const core::Optional<std::string>& after_volume_id, std::size_t limit) {
    if (limit == 0) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("volume page limit must be greater than zero"));
    }
    if (after_volume_id.has_value()) {
        const RepositoryResult<core::Unit> valid = EnsureVolumeId(*after_volume_id);
        if (!valid.ok()) return core::make_unexpected(valid.error());
    }

    const RepositoryResult<std::vector<std::string> > all = VolumeIdsUnlocked();
    if (!all.ok()) return core::make_unexpected(all.error());

    std::vector<std::string> selected;
    for (std::size_t i = 0; i < all.value().size(); ++i) {
        // Strictly greater, so the cursor's own volume is not repeated.
        if (after_volume_id.has_value() && !(all.value()[i] > *after_volume_id)) continue;
        selected.push_back(all.value()[i]);
    }

    const bool has_more = selected.size() > limit;
    if (has_more) selected.resize(limit);

    VolumeRecordPage page;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        const RepositoryResult<core::Optional<volume::VolumeRecord> > record =
            LoadVolumeByIdUnlocked(selected[i]);
        if (!record.ok()) return core::make_unexpected(record.error());
        // A record deleted between listing and loading is simply skipped.
        if (record.value().has_value()) page.records.push_back(*record.value());
    }
    if (has_more && !page.records.empty()) {
        page.next_volume_id =
            core::Optional<std::string>(page.records[page.records.size() - 1].id);
    }
    return page;
}

RepositoryResult<core::Unit> PosixFsCatalogStore::CreateVolume(
    const volume::VolumeRecord& record) {
    const RepositoryResult<core::Unit> id_valid = EnsureVolumeId(record.id);
    if (!id_valid.ok()) return core::make_unexpected(id_valid.error());
    const RepositoryResult<core::Unit> name_valid =
        EnsureVolumeComponent(record.name, "name");
    if (!name_valid.ok()) return core::make_unexpected(name_valid.error());

    RepositoryResult<core::FileLockGuard> alias_guard = AcquireVolumeAliasLock(record.name);
    if (!alias_guard.ok()) return core::make_unexpected(alias_guard.error());
    const core::FileLockGuard alias_held(std::move(alias_guard.value()));

    RepositoryResult<core::FileLockGuard> record_guard =
        AcquireVolumeRecordLock(record.id);
    if (!record_guard.ok()) return core::make_unexpected(record_guard.error());
    const core::FileLockGuard record_held(std::move(record_guard.value()));

    const std::string record_path = Paths::VolumeRecordPath(root_, record.id);
    if (core::fs::Exists(record_path)) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("volume '") + record.id + "' already exists"));
    }

    const std::string alias_path = Paths::VolumeAliasPath(root_, record.name);
    if (core::fs::Exists(alias_path)) {
        const RepositoryResult<core::Json> json = ReadJson(alias_path);
        if (!json.ok()) return core::make_unexpected(json.error());
        if (json.value().kind() != core::Json::Kind::String) {
            return core::make_unexpected(
                Backend("parse json '" + alias_path + "'", "not a string"));
        }
        const std::string existing_id = json.value().as_string();
        const RepositoryResult<core::Unit> existing_valid = EnsureVolumeId(existing_id);
        if (!existing_valid.ok()) return core::make_unexpected(existing_valid.error());

        if (core::fs::Exists(Paths::VolumeRecordPath(root_, existing_id))) {
            return core::make_unexpected(RepositoryError::VolumeNameConflict(record.name));
        }
        // The alias outlived its record, so it is stale rather than taken.
        const RepositoryResult<core::Unit> removed = RemoveFileIfExists(alias_path);
        if (!removed.ok()) return core::make_unexpected(removed.error());
    }

    // Names and ids share one namespace on disk: a name that collides with
    // some other volume's id (or vice versa) would make one of them
    // unreachable.
    if (core::fs::Exists(Paths::VolumeRecordPath(root_, record.name)) ||
        core::fs::Exists(Paths::VolumeAliasPath(root_, record.id))) {
        return core::make_unexpected(RepositoryError::VolumeNameConflict(record.name));
    }

    volume::VolumeRecord durable = record;
    // `backing_image_config` is a node-local cache path; persisting it would
    // make the catalog node-specific.
    durable.backing_image_config = core::Optional<std::string>();

    const RepositoryResult<core::Unit> written = WriteJson(record_path, durable.ToJson());
    if (!written.ok()) return core::make_unexpected(written.error());

    const RepositoryResult<core::Unit> bound =
        WriteJson(alias_path, core::Json(record.id));
    if (!bound.ok()) {
        // Roll the record back, or the volume would exist with no name.
        RemoveFileIfExists(record_path);
        return core::make_unexpected(bound.error());
    }
    return core::Unit();
}

RepositoryResult<core::Unit> PosixFsCatalogStore::PutVolume(
    const volume::VolumeRecord& record) {
    const RepositoryResult<core::Unit> id_valid = EnsureVolumeId(record.id);
    if (!id_valid.ok()) return core::make_unexpected(id_valid.error());
    const RepositoryResult<core::Unit> name_valid =
        EnsureVolumeComponent(record.name, "name");
    if (!name_valid.ok()) return core::make_unexpected(name_valid.error());

    RepositoryResult<core::FileLockGuard> guard = AcquireVolumeRecordLock(record.id);
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));

    const std::string path = Paths::VolumeRecordPath(root_, record.id);
    if (!core::fs::Exists(path)) {
        return core::make_unexpected(RepositoryError::VolumeNotFound(record.id));
    }

    const RepositoryResult<core::Json> json = ReadJson(path);
    if (!json.ok()) return core::make_unexpected(json.error());
    const core::Expected<volume::VolumeRecord, std::string> existing =
        volume::VolumeRecord::FromJson(json.value());
    if (!existing.ok()) {
        return core::make_unexpected(Backend("parse json '" + path + "'", existing.error()));
    }
    const RepositoryResult<core::Unit> existing_valid = EnsureVolumeId(existing.value().id);
    if (!existing_valid.ok()) return core::make_unexpected(existing_valid.error());

    const core::Expected<core::Unit, std::string> allowed =
        existing.value().ValidateCatalogUpdate(record);
    if (!allowed.ok()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(allowed.error()));
    }

    volume::VolumeRecord durable = record;
    durable.backing_image_config = core::Optional<std::string>();
    // Reservation state belongs to the reservation APIs alone: a concurrent
    // backing or status update must not resurrect a released owner.
    durable.reserved_by_sandbox_id = existing.value().reserved_by_sandbox_id;
    durable.read_only_mounts = existing.value().read_only_mounts;
    return WriteJson(path, durable.ToJson());
}

RepositoryResult<core::Unit> PosixFsCatalogStore::DeleteVolume(
    const std::string& volume_id) {
    const RepositoryResult<core::Unit> valid = EnsureVolumeId(volume_id);
    if (!valid.ok()) return core::make_unexpected(valid.error());

    const RepositoryResult<core::Optional<volume::VolumeRecord> > existing =
        LoadVolumeByIdUnlocked(volume_id);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (!existing.value().has_value()) return core::Unit();

    RepositoryResult<core::FileLockGuard> alias_guard =
        AcquireVolumeAliasLock(existing.value()->name);
    if (!alias_guard.ok()) return core::make_unexpected(alias_guard.error());
    const core::FileLockGuard alias_held(std::move(alias_guard.value()));

    RepositoryResult<core::FileLockGuard> record_guard =
        AcquireVolumeRecordLock(volume_id);
    if (!record_guard.ok()) return core::make_unexpected(record_guard.error());
    const core::FileLockGuard record_held(std::move(record_guard.value()));

    // Re-read under the lock: the record may have changed since the check
    // above, which ran unlocked.
    const RepositoryResult<core::Optional<volume::VolumeRecord> > current =
        LoadVolumeByIdUnlocked(volume_id);
    if (!current.ok()) return core::make_unexpected(current.error());
    if (!current.value().has_value()) return core::Unit();

    const volume::VolumeRecord record = *current.value();
    if (record.reserved_by_sandbox_id.has_value()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            "volume '" + volume_id + "' is reserved by sandbox '" +
            *record.reserved_by_sandbox_id + "'"));
    }
    if (!record.read_only_mounts.empty()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            "volume '" + volume_id + "' is mounted read-only by sandbox '" +
            record.read_only_mounts[0] + "'"));
    }

    const RepositoryResult<core::Unit> removed =
        RemoveFileIfExists(Paths::VolumeRecordPath(root_, volume_id));
    if (!removed.ok()) return core::make_unexpected(removed.error());

    const std::string alias_path = Paths::VolumeAliasPath(root_, record.name);
    if (core::fs::Exists(alias_path)) {
        const RepositoryResult<core::Json> json = ReadJson(alias_path);
        if (!json.ok()) return core::make_unexpected(json.error());
        // Only unbind a name that still points at this volume.
        if (json.value().kind() == core::Json::Kind::String &&
            json.value().as_string() == volume_id) {
            return RemoveFileIfExists(alias_path);
        }
    }
    return core::Unit();
}

RepositoryResult<core::Optional<std::string> > PosixFsCatalogStore::ReserveVolume(
    const std::string& volume_id, const std::string& owner) {
    const RepositoryResult<core::Unit> id_valid = EnsureVolumeId(volume_id);
    if (!id_valid.ok()) return core::make_unexpected(id_valid.error());
    const RepositoryResult<core::Unit> owner_valid = EnsureVolumeComponent(owner, "owner");
    if (!owner_valid.ok()) return core::make_unexpected(owner_valid.error());

    RepositoryResult<core::FileLockGuard> guard = AcquireVolumeRecordLock(volume_id);
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));

    const RepositoryResult<core::Optional<volume::VolumeRecord> > existing =
        LoadVolumeByIdUnlocked(volume_id);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (!existing.value().has_value()) {
        return core::make_unexpected(RepositoryError::VolumeNotFound(volume_id));
    }

    volume::VolumeRecord record = *existing.value();
    // A read-only volume has no exclusive lease to take.
    if (record.mode == volume::VolumeMode::ReadOnly) return core::Optional<std::string>();

    if (record.reserved_by_sandbox_id.has_value()) {
        // Held by someone else: report the owner rather than failing, so the
        // caller can surface a useful conflict.
        if (*record.reserved_by_sandbox_id != owner) {
            return core::Optional<std::string>(*record.reserved_by_sandbox_id);
        }
        // Already ours; re-reserving is a no-op.
        return core::Optional<std::string>();
    }

    record.reserved_by_sandbox_id = core::Optional<std::string>(owner);
    const RepositoryResult<core::Unit> written =
        WriteJson(Paths::VolumeRecordPath(root_, volume_id), record.ToJson());
    if (!written.ok()) return core::make_unexpected(written.error());
    return core::Optional<std::string>();
}

RepositoryResult<core::Unit> PosixFsCatalogStore::ReserveReadOnlyVolume(
    const std::string& volume_id, const std::string& owner) {
    const RepositoryResult<core::Unit> id_valid = EnsureVolumeId(volume_id);
    if (!id_valid.ok()) return core::make_unexpected(id_valid.error());
    const RepositoryResult<core::Unit> owner_valid = EnsureVolumeComponent(owner, "owner");
    if (!owner_valid.ok()) return core::make_unexpected(owner_valid.error());

    RepositoryResult<core::FileLockGuard> guard = AcquireVolumeRecordLock(volume_id);
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));

    const RepositoryResult<core::Optional<volume::VolumeRecord> > existing =
        LoadVolumeByIdUnlocked(volume_id);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (!existing.value().has_value()) {
        return core::make_unexpected(RepositoryError::VolumeNotFound(volume_id));
    }

    volume::VolumeRecord record = *existing.value();
    if (record.mode != volume::VolumeMode::ReadOnly) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("volume '" + volume_id + "' is not read-only"));
    }
    for (std::size_t i = 0; i < record.read_only_mounts.size(); ++i) {
        // Idempotent: mounting twice from one sandbox is not an error.
        if (record.read_only_mounts[i] == owner) return core::Unit();
    }

    record.read_only_mounts.push_back(owner);
    return WriteJson(Paths::VolumeRecordPath(root_, volume_id), record.ToJson());
}

RepositoryResult<core::Unit> PosixFsCatalogStore::ReplaceVolumeOwnerFor(
    const std::string& volume_id, const std::string& from,
    const core::Optional<std::string>& to) {
    const RepositoryResult<core::Unit> id_valid = EnsureVolumeId(volume_id);
    if (!id_valid.ok()) return core::make_unexpected(id_valid.error());
    const RepositoryResult<core::Unit> from_valid = EnsureVolumeComponent(from, "owner");
    if (!from_valid.ok()) return core::make_unexpected(from_valid.error());
    if (to.has_value()) {
        const RepositoryResult<core::Unit> to_valid = EnsureVolumeComponent(*to, "owner");
        if (!to_valid.ok()) return core::make_unexpected(to_valid.error());
        // Nothing to do, and taking the lock would be pointless contention.
        if (*to == from) return core::Unit();
    }

    RepositoryResult<core::FileLockGuard> guard = AcquireVolumeRecordLock(volume_id);
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));

    const RepositoryResult<core::Optional<volume::VolumeRecord> > existing =
        LoadVolumeByIdUnlocked(volume_id);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (!existing.value().has_value()) {
        return core::make_unexpected(RepositoryError::VolumeNotFound(volume_id));
    }

    volume::VolumeRecord record = *existing.value();
    if (record.ReplaceOwner(from, to)) {
        return WriteJson(Paths::VolumeRecordPath(root_, volume_id), record.ToJson());
    }
    // `from` did not hold anything, so there is nothing to persist.
    return core::Unit();
}

// ---- build cache ----------------------------------------------------------

RepositoryResult<BuildCacheState> PosixFsCatalogStore::GetBuildCacheState() const {
    const std::string path = core::fs::Join(root_, "template-build/cache-head.json");
    if (!core::fs::Exists(path)) {
        // An absent file is an empty cache, not an error.
        return BuildCacheState();
    }
    const core::Expected<std::string, std::string> bytes = core::fs::ReadToString(path);
    if (!bytes.ok()) {
        return core::make_unexpected(Backend("read build cache head", bytes.error()));
    }
    return BuildCacheState::Decode(bytes.value());
}

RepositoryResult<core::Optional<std::string> > PosixFsCatalogStore::ReplaceBuildCacheHead(
    const std::string& volume_id) {
    // The head is guarded by a volume-alias lock under a reserved name, so
    // two publishers cannot both advance it.
    RepositoryResult<core::FileLockGuard> guard =
        AcquireVolumeAliasLock("aenv-buildkit-cache-head");
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));

    RepositoryResult<BuildCacheState> state = GetBuildCacheState();
    if (!state.ok()) return core::make_unexpected(state.error());

    const RepositoryResult<core::Optional<std::string> > previous =
        state.value().Replace(volume_id);
    if (!previous.ok()) return core::make_unexpected(previous.error());

    const RepositoryResult<core::Unit> written = WriteJson(
        core::fs::Join(root_, "template-build/cache-head.json"), state.value().ToJson());
    if (!written.ok()) return core::make_unexpected(written.error());
    return previous;
}

RepositoryResult<core::Unit> PosixFsCatalogStore::ForgetRetiredBuildCache(
    const std::string& volume_id) {
    RepositoryResult<core::FileLockGuard> guard =
        AcquireVolumeAliasLock("aenv-buildkit-cache-head");
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));

    RepositoryResult<BuildCacheState> state = GetBuildCacheState();
    if (!state.ok()) return core::make_unexpected(state.error());

    state.value().retired.erase(volume_id);
    return WriteJson(core::fs::Join(root_, "template-build/cache-head.json"),
                     state.value().ToJson());
}

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
