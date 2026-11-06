// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/catalog.rs
#include "agentenv/snapshot/repository/posixfs/catalog.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

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
    // Rust `acquire_file_lock` creates the lock's parent first, and it has to:
    // `EnsureLayout` only makes the directories the *record* layer uses, so a
    // document collection outside that set -- `snapshots/build-logs/` is one --
    // has no directory until something writes to it, and opening the lock file
    // would fail with ENOENT. `WriteAtomic` already creates the document's own
    // parent, so without this the lock is the only thing standing between a
    // fresh repository and its first build log.
    const core::Optional<std::string> parent = core::fs::Parent(lock_path);
    if (parent.has_value()) {
        const core::Expected<core::Unit, std::string> created = core::fs::CreateDirAll(*parent);
        if (!created.ok()) {
            return core::make_unexpected(
                Backend("create " + label + " lock dir '" + *parent + "'", created.error()));
        }
    }

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

RepositoryResult<core::FileLockGuard> PosixFsCatalogStore::AcquireVolumeAliasLock(
    const std::string& alias) const {
    return AcquireCatalogLock(Paths::VolumeAliasLockPath(root_, alias), "volume alias");
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

// ---- shared documents -------------------------------------------------------

RepositoryResult<core::Optional<std::string> > PosixFsCatalogStore::GetDocument(
    const DocumentKey& key) const {
    const std::string path = core::fs::Join(root_, key.PosixPath());
    const core::Expected<core::Optional<core::fs::FileDescriptor>, std::string> opened =
        core::fs::OpenReadFollowOptional(path);
    if (!opened.ok()) {
        return core::make_unexpected(Backend("read '" + path + "'", opened.error()));
    }
    if (!opened.value().has_value()) return core::Optional<std::string>();
    const core::Expected<std::string, std::string> bytes =
        core::fs::ReadFdToString(opened.value()->get(), static_cast<std::size_t>(-1));
    if (!bytes.ok()) {
        return core::make_unexpected(Backend("read '" + path + "'", bytes.error()));
    }
    return core::Optional<std::string>(bytes.value());
}

RepositoryResult<core::Unit> PosixFsCatalogStore::PutDocument(const DocumentKey& key,
                                                              const std::string& bytes) const {
    RepositoryResult<core::FileLockGuard> guard =
        AcquireCatalogLock(core::fs::Join(root_, key.PosixLockPath()), "document");
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));

    const std::string path = core::fs::Join(root_, key.PosixPath());
    const core::Expected<core::Unit, std::string> written = core::WriteAtomic(path, bytes);
    if (!written.ok()) {
        return core::make_unexpected(Backend("write '" + path + "'", written.error()));
    }
    return core::Unit();
}

RepositoryResult<core::Optional<std::string> > PosixFsCatalogStore::UpdateDocument(
    const DocumentKey& key, const DocumentUpdate& update) const {
    RepositoryResult<core::FileLockGuard> guard =
        AcquireCatalogLock(core::fs::Join(root_, key.PosixLockPath()), "document");
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));

    const RepositoryResult<core::Optional<std::string> > current = GetDocument(key);
    if (!current.ok()) return current;
    const RepositoryResult<DocumentChange> change = update(current.value());
    if (!change.ok()) return core::make_unexpected(change.error());

    const std::string path = core::fs::Join(root_, key.PosixPath());
    switch (change.value().kind) {
        case DocumentChange::Kind::Keep:
            break;
        case DocumentChange::Kind::Put: {
            const core::Expected<core::Unit, std::string> written =
                core::WriteAtomic(path, change.value().bytes);
            if (!written.ok()) {
                return core::make_unexpected(Backend("write '" + path + "'", written.error()));
            }
            break;
        }
        case DocumentChange::Kind::Delete: {
            const RepositoryResult<core::Unit> removed = RemoveFileIfExists(path);
            if (!removed.ok()) return core::make_unexpected(removed.error());
            break;
        }
    }
    return current;
}

RepositoryResult<core::Unit> PosixFsCatalogStore::DeleteDocument(const DocumentKey& key) const {
    RepositoryResult<core::FileLockGuard> guard =
        AcquireCatalogLock(core::fs::Join(root_, key.PosixLockPath()), "document");
    if (!guard.ok()) return core::make_unexpected(guard.error());
    const core::FileLockGuard held(std::move(guard.value()));
    return RemoveFileIfExists(core::fs::Join(root_, key.PosixPath()));
}

RepositoryResult<std::vector<std::string> > PosixFsCatalogStore::ListDocuments(
    DocumentCollection collection, const core::Optional<std::string>& after,
    std::size_t limit) const {
    const std::string directory = core::fs::Join(root_, DocumentCollectionPrefix(collection));
    DIR* entries = ::opendir(directory.c_str());
    if (entries == NULL) {
        if (errno == ENOENT) return std::vector<std::string>();
        return core::make_unexpected(Backend("read '" + directory + "'", std::strerror(errno)));
    }
    std::vector<std::string> ids;
    errno = 0;
    struct dirent* entry = NULL;
    while ((entry = ::readdir(entries)) != NULL) {
        const std::string file_name = entry->d_name;
        if (file_name == "." || file_name == "..") continue;
        // Rust `entry.file_type()` — the entry itself, symlinks not followed.
        const std::string path = core::fs::Join(directory, file_name);
        struct stat info;
        if (::lstat(path.c_str(), &info) != 0) {
            const std::string error = std::strerror(errno);
            ::closedir(entries);
            return core::make_unexpected(Backend("inspect '" + path + "'", error));
        }
        // Lock files share the directory and are not documents.
        const core::Optional<std::string> id =
            DocumentCollectionIdFromFileName(collection, file_name);
        if (!id.has_value() || !S_ISREG(info.st_mode)) continue;
        if (!after.has_value() || *id > *after) ids.push_back(*id);
        errno = 0;
    }
    const int read_error = errno;
    ::closedir(entries);
    if (read_error != 0) {
        return core::make_unexpected(Backend("read '" + directory + "'", std::strerror(read_error)));
    }
    std::sort(ids.begin(), ids.end());
    if (ids.size() > limit) ids.resize(limit);
    return ids;
}

RepositoryResult<std::vector<std::pair<std::string, std::string> > >
PosixFsCatalogStore::ReadDocuments(DocumentCollection collection) const {
    // Rust `usize::MAX`: the whole collection, since the caller asked for all
    // of it and a cap here would silently truncate a catalog listing.
    const RepositoryResult<std::vector<std::string> > ids =
        ListDocuments(collection, core::nullopt, static_cast<std::size_t>(-1));
    if (!ids.ok()) return core::make_unexpected(ids.error());

    std::vector<std::pair<std::string, std::string> > documents;
    documents.reserve(ids.value().size());
    for (std::size_t i = 0; i < ids.value().size(); ++i) {
        const core::Optional<DocumentKey> key =
            DocumentCollectionKey(collection, ids.value()[i]);
        if (!key.has_value()) {
            return core::make_unexpected(
                RepositoryError::InvalidRequest("invalid document id '" + ids.value()[i] + "'"));
        }
        const RepositoryResult<core::Optional<std::string> > bytes = GetDocument(*key);
        if (!bytes.ok()) return core::make_unexpected(bytes.error());
        // Removed since the listing, which is normal on a directory other
        // processes write to.
        if (!bytes.value().has_value()) continue;
        documents.push_back(std::make_pair(ids.value()[i], *bytes.value()));
    }
    return documents;
}

}  // namespace posixfs
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
