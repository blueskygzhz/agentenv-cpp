// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/persistence/{mod,file_backed}.rs
#include "agentenv/orchestrator/persistence.h"

#include <set>
#include <sstream>

#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/local_store.h"
#include "agentenv/sandbox/backend.h"

namespace agentenv {
namespace orchestrator {

// ---- SandboxPersistenceError -----------------------------------------------

SandboxPersistenceError SandboxPersistenceError::Io(const std::string& operation,
                                                    const std::string& path,
                                                    const std::string& source) {
    SandboxPersistenceError e;
    e.kind      = Kind::Io;
    e.operation = operation;
    e.path      = path;
    e.source    = source;
    return e;
}

SandboxPersistenceError SandboxPersistenceError::Store(const std::string& operation,
                                                       const std::string& source) {
    SandboxPersistenceError e;
    e.kind      = Kind::Store;
    e.operation = operation;
    e.source    = source;
    return e;
}

SandboxPersistenceError SandboxPersistenceError::InvalidRecord(
    const std::string& reason, const std::string& source) {
    SandboxPersistenceError e;
    e.kind   = Kind::InvalidRecord;
    e.reason = reason;
    e.source = source;
    return e;
}

SandboxPersistenceError SandboxPersistenceError::RuntimeState(
    const std::string& reason) {
    SandboxPersistenceError e;
    e.kind   = Kind::RuntimeState;
    e.reason = reason;
    return e;
}

std::string SandboxPersistenceError::Message() const {
    std::ostringstream os;
    switch (kind) {
        case Kind::Io:
            os << "failed to " << operation << " " << path << ": " << source;
            return os.str();
        case Kind::InvalidRecord:
            os << "invalid sandbox record: " << reason;
            return os.str();
        case Kind::RuntimeState:
            os << "invalid paused sandbox runtime state: " << reason;
            return os.str();
        case Kind::Store:
            os << "paused sandbox store operation failed: " << operation
               << ": " << source;
            return os.str();
    }
    return "unknown sandbox persistence error";
}

// ---- DisabledSandboxPersister ----------------------------------------------

PersistenceResult<std::vector<SandboxMetadata> >
DisabledSandboxPersister::LoadAll() {
    return std::vector<SandboxMetadata>();
}

PersistenceResult<core::Optional<std::string> >
DisabledSandboxPersister::AllocateArtifactRoot(const core::SandboxId&) {
    // Rust returns Ok(None): persistence disabled, backend owns its artifacts.
    return core::Optional<std::string>(core::nullopt);
}

PersistenceResult<core::Unit>
DisabledSandboxPersister::PersistPaused(const SandboxMetadata&,
                                        const core::Optional<std::string>&,
                                        const sandbox::PausedSandboxState*) {
    return core::Unit{};
}

PersistenceResult<core::Unit>
DisabledSandboxPersister::MarkResuming(const core::SandboxId&) {
    return core::Unit{};
}

PersistenceResult<core::Unit>
DisabledSandboxPersister::RollbackResuming(const core::SandboxId&) {
    return core::Unit{};
}

PersistenceResult<core::Unit>
DisabledSandboxPersister::DeleteRecord(const core::SandboxId&) {
    return core::Unit{};
}

PersistenceResult<core::Unit>
DisabledSandboxPersister::DeleteRecordAndArtifacts(const core::SandboxId&) {
    return core::Unit{};
}

// ---- FileBackedSandboxPersister ---------------------------------------------
//
// Rust: src/orchestrator/persistence/file_backed.rs
//
// One `LocalKvStore` record per paused sandbox, keyed by sandbox id, plus a
// per-sandbox artifact directory. The record carries a version tag and a
// lifecycle marker so a crash mid-resume is detectable on the next load.

namespace {

/// Rust `RECORD_VERSION`.
const int64_t kRecordVersion = 1;
/// Rust `RECORD_DB_DIR`.
const char* const kRecordDbDir = "records.db";

/// Rust `enum PersistedPausedLifecycle`.
enum class PersistedLifecycle { Paused, Resuming };

const char* LifecycleName(PersistedLifecycle lifecycle) {
    return lifecycle == PersistedLifecycle::Resuming ? "resuming" : "paused";
}

/// Rust `struct PersistedPausedRecord`.
struct PersistedRecord {
    int64_t             version = kRecordVersion;
    PersistedLifecycle  lifecycle = PersistedLifecycle::Paused;
    SandboxMetadata     metadata;
    std::string         artifact_root;
    /// The backend's opaque `PausedSandboxState::Encode()` payload.
    std::string         state;

    core::Json ToJson() const {
        core::JsonObject object;
        object["version"]      = core::Json(version);
        object["lifecycle"]    = core::Json(std::string(LifecycleName(lifecycle)));
        object["metadata"]     = metadata.ToJson();
        object["artifactRoot"] = core::Json(artifact_root);
        object["state"]        = core::Json(state);
        return core::Json(object);
    }

    static core::Expected<PersistedRecord, std::string> FromJson(const core::Json& json) {
        if (json.kind() != core::Json::Kind::Object) {
            return core::make_unexpected(std::string("record must be an object"));
        }
        const core::JsonObject& fields = json.as_object();
        PersistedRecord record;

        core::JsonObject::const_iterator it = fields.find("version");
        if (it == fields.end() || it->second.kind() != core::Json::Kind::Int) {
            return core::make_unexpected(std::string("missing field `version`"));
        }
        record.version = it->second.as_int();

        it = fields.find("lifecycle");
        if (it != fields.end() && it->second.kind() == core::Json::Kind::String) {
            record.lifecycle = it->second.as_string() == "resuming"
                ? PersistedLifecycle::Resuming
                : PersistedLifecycle::Paused;
        }

        it = fields.find("metadata");
        if (it == fields.end()) {
            return core::make_unexpected(std::string("missing field `metadata`"));
        }
        core::Expected<SandboxMetadata, std::string> parsed =
            SandboxMetadata::FromJson(it->second);
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        record.metadata = parsed.value();

        it = fields.find("artifactRoot");
        if (it != fields.end() && it->second.kind() == core::Json::Kind::String) {
            record.artifact_root = it->second.as_string();
        }

        it = fields.find("state");
        if (it != fields.end() && it->second.kind() == core::Json::Kind::String) {
            record.state = it->second.as_string();
        }
        return record;
    }
};

/// Rust `ensure_supported_version`.
PersistenceResult<core::Unit> EnsureSupportedVersion(int64_t version) {
    if (version == kRecordVersion) return core::Unit{};
    std::ostringstream os;
    os << "unsupported record version " << version;
    return core::make_unexpected(SandboxPersistenceError::InvalidRecord(os.str(), ""));
}

}  // namespace

class FileBackedSandboxPersister : public SandboxPersister {
 public:
    FileBackedSandboxPersister(const std::string& root,
                               core::VirtualizationMode mode,
                               local_store::Durability durability)
        : root_(root), virtualization_mode_(mode), durability_(durability) {}

    /// Rust `load_all`.
    ///
    /// The Rust signature takes a `SandboxBackendFactory` and calls
    /// `decode_paused_state` to rebuild the runtime handle. The C++ factory
    /// has no such method, so the metadata is restored without
    /// `paused_state`, matching Rust's
    /// `into_metadata_without_runtime_state` path. Callers that need the
    /// runtime handle re-derive it from `artifact_root` + `state`.
    PersistenceResult<std::vector<SandboxMetadata> > LoadAll() override {
        core::Expected<std::shared_ptr<local_store::KvStore>, std::string> db = Db();
        if (!db.ok()) {
            return core::make_unexpected(
                SandboxPersistenceError::Store("open record store", db.error()));
        }
        core::Expected<std::vector<local_store::Entry>, std::string> entries =
            db.value()->Entries();
        if (!entries.ok()) {
            return core::make_unexpected(SandboxPersistenceError::Store(
                "scan paused sandbox records", entries.error()));
        }

        std::vector<SandboxMetadata> sandboxes;
        std::set<std::string> retained;

        const std::vector<local_store::Entry>& records = entries.value();
        for (std::size_t i = 0; i < records.size(); ++i) {
            const std::string& key = records[i].first;
            core::Expected<core::Json, core::AnyError> json =
                core::Json::Parse(records[i].second);
            if (!json.ok()) {
                // Rust warns and drops the record.
                (void)db.value()->Delete(key);
                continue;
            }
            core::Expected<PersistedRecord, std::string> parsed =
                PersistedRecord::FromJson(json.value());
            if (!parsed.ok()) {
                (void)db.value()->Delete(key);
                continue;
            }
            PersistedRecord record = parsed.value();
            if (!EnsureSupportedVersion(record.version).ok()) {
                (void)db.value()->Delete(key);
                continue;
            }

            const std::string sandbox_id = record.metadata.id.ToString();

            // Rust: a record left mid-resume is unusable.
            if (record.lifecycle == PersistedLifecycle::Resuming) {
                PersistenceResult<core::Unit> cleanup = CleanupInvalidRecord(sandbox_id);
                if (!cleanup.ok()) return core::make_unexpected(cleanup.error());
                continue;
            }

            // Rust keeps the metadata visible but not resumable when the
            // node's virtualization mode differs.
            record.metadata.state = SandboxState::Paused;
            record.metadata.paused_state.reset();

            retained.insert(sandbox_id);
            sandboxes.push_back(record.metadata);
        }

        PersistenceResult<core::Unit> orphans = CleanupOrphanArtifacts(retained);
        if (!orphans.ok()) return core::make_unexpected(orphans.error());
        return sandboxes;
    }

    /// Rust `allocate_artifact_root`.
    PersistenceResult<core::Optional<std::string> >
    AllocateArtifactRoot(const core::SandboxId& id) override {
        const std::string artifact_root =
            SandboxArtifactRoot(id.ToString()) + "/" + core::Uuid::GenV7().ToString();
        core::Expected<core::Unit, std::string> created =
            core::fs::CreateDirAll(artifact_root);
        if (!created.ok()) {
            return core::make_unexpected(SandboxPersistenceError::Io(
                "allocate paused sandbox artifact root", artifact_root, created.error()));
        }
        return core::Optional<std::string>(artifact_root);
    }

    /// Rust `persist_paused`.
    PersistenceResult<core::Unit>
    PersistPaused(const SandboxMetadata& metadata,
                  const core::Optional<std::string>& artifact_root,
                  const sandbox::PausedSandboxState* paused_state) override {
        if (!artifact_root.has_value()) {
            return core::make_unexpected(SandboxPersistenceError::RuntimeState(
                "file-backed persister requires an allocated artifact root"));
        }
        if (paused_state == NULL) {
            return core::make_unexpected(SandboxPersistenceError::RuntimeState(
                "file-backed persister requires a paused sandbox state"));
        }

        PersistedRecord record;
        record.version       = kRecordVersion;
        record.lifecycle     = PersistedLifecycle::Paused;
        record.metadata      = metadata;
        record.artifact_root = *artifact_root;
        record.state         = paused_state->Encode();

        PersistenceResult<core::Unit> stored = PutRecord(record);
        if (!stored.ok()) {
            // Rust drops the artifacts when the record cannot be written.
            (void)core::fs::RemoveAll(*artifact_root);
        }
        return stored;
    }

    /// Rust `mark_resuming`.
    PersistenceResult<core::Unit> MarkResuming(const core::SandboxId& id) override {
        return SetLifecycle(id, PersistedLifecycle::Resuming);
    }

    /// Rust `rollback_resuming`.
    PersistenceResult<core::Unit> RollbackResuming(const core::SandboxId& id) override {
        return SetLifecycle(id, PersistedLifecycle::Paused);
    }

    /// Rust `delete_record`.
    PersistenceResult<core::Unit> DeleteRecord(const core::SandboxId& id) override {
        return RemoveRecord(id.ToString());
    }

    /// Rust `delete_record_and_artifacts`.
    PersistenceResult<core::Unit>
    DeleteRecordAndArtifacts(const core::SandboxId& id) override {
        PersistenceResult<core::Unit> removed = RemoveRecord(id.ToString());
        if (!removed.ok()) return removed;
        return RemoveArtifactRoot(SandboxArtifactRoot(id.ToString()));
    }

 private:
    std::string RecordsDbPath() const { return root_ + "/" + kRecordDbDir; }
    std::string ArtifactsRoot() const { return root_ + "/artifacts"; }
    std::string SandboxArtifactRoot(const std::string& sandbox_id) const {
        return ArtifactsRoot() + "/" + sandbox_id;
    }

    /// Rust `db` — `OnceCell`, so the store is opened at most once.
    core::Expected<std::shared_ptr<local_store::KvStore>, std::string> Db() {
        if (db_) return db_;
        core::Expected<std::shared_ptr<local_store::KvStore>, std::string> opened =
            local_store::KvStore::Open(RecordsDbPath(), durability_);
        if (!opened.ok()) return core::make_unexpected(opened.error());
        db_ = opened.value();
        return db_;
    }

    /// Rust `get_record`.
    PersistenceResult<PersistedRecord> GetRecord(const std::string& sandbox_id) {
        core::Expected<std::shared_ptr<local_store::KvStore>, std::string> db = Db();
        if (!db.ok()) {
            return core::make_unexpected(
                SandboxPersistenceError::Store("open record store", db.error()));
        }
        core::Expected<core::Optional<std::string>, std::string> bytes =
            db.value()->Get(sandbox_id);
        if (!bytes.ok()) {
            return core::make_unexpected(SandboxPersistenceError::Store(
                "read paused sandbox record", bytes.error()));
        }
        if (!bytes.value().has_value()) {
            return core::make_unexpected(SandboxPersistenceError::InvalidRecord(
                std::string("paused sandbox record ") + sandbox_id + " not found", ""));
        }
        core::Expected<core::Json, core::AnyError> json =
            core::Json::Parse(*bytes.value());
        if (!json.ok()) {
            return core::make_unexpected(SandboxPersistenceError::InvalidRecord(
                "failed to deserialize record", json.error().chain()));
        }
        core::Expected<PersistedRecord, std::string> parsed =
            PersistedRecord::FromJson(json.value());
        if (!parsed.ok()) {
            return core::make_unexpected(SandboxPersistenceError::InvalidRecord(
                "failed to deserialize record", parsed.error()));
        }
        PersistenceResult<core::Unit> version = EnsureSupportedVersion(parsed.value().version);
        if (!version.ok()) return core::make_unexpected(version.error());
        return parsed.value();
    }

    /// Rust `put_record`.
    PersistenceResult<core::Unit> PutRecord(const PersistedRecord& record) {
        core::Expected<std::shared_ptr<local_store::KvStore>, std::string> db = Db();
        if (!db.ok()) {
            return core::make_unexpected(
                SandboxPersistenceError::Store("open record store", db.error()));
        }
        core::Expected<core::Unit, std::string> written =
            db.value()->Put(record.metadata.id.ToString(), record.ToJson().ToString());
        if (!written.ok()) {
            return core::make_unexpected(SandboxPersistenceError::Store(
                "persist paused sandbox record", written.error()));
        }
        return core::Unit{};
    }

    /// Rust `remove_record`.
    PersistenceResult<core::Unit> RemoveRecord(const std::string& sandbox_id) {
        core::Expected<std::shared_ptr<local_store::KvStore>, std::string> db = Db();
        if (!db.ok()) {
            return core::make_unexpected(
                SandboxPersistenceError::Store("open record store", db.error()));
        }
        core::Expected<core::Unit, std::string> removed = db.value()->Delete(sandbox_id);
        if (!removed.ok()) {
            return core::make_unexpected(SandboxPersistenceError::Store(
                "remove paused sandbox record", removed.error()));
        }
        return core::Unit{};
    }

    /// Rust `remove_artifact_root` — an absent path is not an error.
    static PersistenceResult<core::Unit> RemoveArtifactRoot(const std::string& path) {
        core::Expected<core::Unit, std::string> removed = core::fs::RemoveAll(path);
        if (!removed.ok()) {
            return core::make_unexpected(SandboxPersistenceError::Io(
                "remove paused sandbox artifacts", path, removed.error()));
        }
        return core::Unit{};
    }

    /// Rust `cleanup_invalid_record`.
    PersistenceResult<core::Unit> CleanupInvalidRecord(const std::string& sandbox_id) {
        PersistenceResult<core::Unit> removed = RemoveRecord(sandbox_id);
        if (!removed.ok()) return removed;
        return RemoveArtifactRoot(SandboxArtifactRoot(sandbox_id));
    }

    /// Rust `cleanup_orphan_artifacts`.
    PersistenceResult<core::Unit>
    CleanupOrphanArtifacts(const std::set<std::string>& retained) {
        const std::string artifacts_root = ArtifactsRoot();
        if (!core::fs::Exists(artifacts_root)) return core::Unit{};

        core::Expected<std::vector<std::string>, std::string> entries =
            core::fs::ReadDir(artifacts_root);
        if (!entries.ok()) {
            return core::make_unexpected(SandboxPersistenceError::Io(
                "read paused sandbox artifacts", artifacts_root, entries.error()));
        }

        const std::vector<std::string>& names = entries.value();
        for (std::size_t i = 0; i < names.size(); ++i) {
            // Rust skips names that are not a sandbox id.
            if (!core::SandboxId::Parse(names[i]).ok()) continue;
            if (retained.find(names[i]) != retained.end()) continue;
            PersistenceResult<core::Unit> removed =
                RemoveArtifactRoot(artifacts_root + "/" + names[i]);
            if (!removed.ok()) return removed;
        }
        return core::Unit{};
    }

    /// Shared body of `mark_resuming` / `rollback_resuming`.
    PersistenceResult<core::Unit> SetLifecycle(const core::SandboxId& id,
                                               PersistedLifecycle lifecycle) {
        PersistenceResult<PersistedRecord> record = GetRecord(id.ToString());
        if (!record.ok()) return core::make_unexpected(record.error());
        PersistedRecord updated = record.value();
        updated.lifecycle = lifecycle;
        return PutRecord(updated);
    }

    std::string                              root_;
    core::VirtualizationMode                 virtualization_mode_;
    local_store::Durability                  durability_;
    std::shared_ptr<local_store::KvStore>    db_;
};

// ---- factory ---------------------------------------------------------------

std::unique_ptr<SandboxPersister> MakePersister(const std::string& kind,
                                                const std::string& path) {
    if (kind == "file") {
        return std::unique_ptr<SandboxPersister>(new FileBackedSandboxPersister(
            path, core::VirtualizationMode::Kvm, local_store::Durability::Sync));
    }
    return std::unique_ptr<SandboxPersister>(new DisabledSandboxPersister());
}

}  // namespace orchestrator
}  // namespace agentenv
