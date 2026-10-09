// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/persistence/{mod,file_backed}.rs
#include "agentenv/orchestrator/persistence.h"

#include <sys/stat.h>

#include <set>
#include <sstream>

#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"
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
DisabledSandboxPersister::LoadAll(sandbox::SandboxBackendFactory*) {
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

/// Rust `RECORD_VERSION: u32`.
const int64_t kRecordVersion = 1;
/// Rust `RECORD_DB_DIR`.
const char* const kRecordDbDir = "records.db";

/// Rust `enum PersistedPausedLifecycle` — `#[serde(rename_all = "snake_case")]`.
enum class PersistedLifecycle { Paused, Resuming };

const char* LifecycleName(PersistedLifecycle lifecycle) {
    return lifecycle == PersistedLifecycle::Resuming ? "resuming" : "paused";
}

/// Rust `struct PersistedPausedRecord` — `#[serde(rename_all = "camelCase")]`.
///
/// Every field is required: serde has no `#[serde(default)]` on any of them,
/// so a record missing one fails to deserialize and is discarded. `state` is
/// a `serde_json::Value` *embedded* in the record, not a string holding JSON —
/// writing it as a string would make every record this port writes unreadable
/// by the Rust server and vice versa.
struct PersistedRecord {
    int64_t             version = kRecordVersion;
    PersistedLifecycle  lifecycle = PersistedLifecycle::Paused;
    SandboxMetadata     metadata;
    std::string         artifact_root;
    core::Json          state;

    core::Json ToJson() const {
        core::JsonObject object;
        object["version"]      = core::Json(version);
        object["lifecycle"]    = core::Json(std::string(LifecycleName(lifecycle)));
        object["metadata"]     = metadata.ToJson();
        object["artifactRoot"] = core::Json(artifact_root);
        object["state"]        = state;
        return core::Json(object);
    }

    static core::Expected<PersistedRecord, std::string> FromJson(const core::Json& json) {
        if (json.kind() != core::Json::Kind::Object) {
            return core::make_unexpected(std::string("record must be an object"));
        }
        const core::JsonObject& fields = json.as_object();
        PersistedRecord record;

        core::JsonObject::const_iterator it = fields.find("version");
        if (it == fields.end()) {
            return core::make_unexpected(std::string("missing field `version`"));
        }
        // Rust `version: u32`.
        if (it->second.kind() != core::Json::Kind::Int || it->second.as_int() < 0 ||
            it->second.as_int() > static_cast<int64_t>(0xffffffffu)) {
            return core::make_unexpected(std::string("invalid type for `version`: expected u32"));
        }
        record.version = it->second.as_int();

        it = fields.find("lifecycle");
        if (it == fields.end()) {
            return core::make_unexpected(std::string("missing field `lifecycle`"));
        }
        if (it->second.kind() != core::Json::Kind::String) {
            return core::make_unexpected(std::string("invalid type for `lifecycle`"));
        }
        if (it->second.as_string() == "paused") {
            record.lifecycle = PersistedLifecycle::Paused;
        } else if (it->second.as_string() == "resuming") {
            record.lifecycle = PersistedLifecycle::Resuming;
        } else {
            // An unknown lifecycle is not "probably paused": guessing would
            // offer a resume of a record whose state nobody can vouch for.
            return core::make_unexpected("unknown variant `" + it->second.as_string() +
                                         "`, expected `paused` or `resuming`");
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
        if (it == fields.end()) {
            return core::make_unexpected(std::string("missing field `artifactRoot`"));
        }
        if (it->second.kind() != core::Json::Kind::String) {
            return core::make_unexpected(std::string("invalid type for `artifactRoot`"));
        }
        record.artifact_root = it->second.as_string();

        it = fields.find("state");
        if (it == fields.end()) {
            return core::make_unexpected(std::string("missing field `state`"));
        }
        record.state = it->second;
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

/// Rust `decode_record` — deserialize, then check the version.
PersistenceResult<PersistedRecord> DecodeRecord(const std::string& bytes) {
    core::Expected<core::Json, core::AnyError> json = core::Json::Parse(bytes);
    if (!json.ok()) {
        return core::make_unexpected(SandboxPersistenceError::InvalidRecord(
            "failed to deserialize record", json.error().chain()));
    }
    core::Expected<PersistedRecord, std::string> parsed = PersistedRecord::FromJson(json.value());
    if (!parsed.ok()) {
        return core::make_unexpected(SandboxPersistenceError::InvalidRecord(
            "failed to deserialize record", parsed.error()));
    }
    PersistenceResult<core::Unit> version = EnsureSupportedVersion(parsed.value().version);
    if (!version.ok()) return core::make_unexpected(version.error());
    return parsed.value();
}

/// Rust `entry.file_type()?.is_dir()` — `DirEntry::file_type` does not follow
/// symlinks. Following one here would let a link named like a sandbox id
/// point the orphan sweep's `remove_dir_all` at a directory outside the store.
bool IsRealDirectory(const std::string& path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace

class FileBackedSandboxPersister::Impl {
 public:
    Impl(const std::string& root, core::VirtualizationMode mode)
        : root_(root), virtualization_mode_(mode), durability_(local_store::Durability::Sync) {}

    void SetDurability(local_store::Durability durability) { durability_ = durability; }

    /// Rust `load_all`.
    PersistenceResult<std::vector<SandboxMetadata> >
    LoadAll(sandbox::SandboxBackendFactory* factory) {
        AGENTENV_INFO("loading paused sandbox records store=" << root_);
        core::Expected<std::shared_ptr<local_store::KvStore>, SandboxPersistenceError> db = Db();
        if (!db.ok()) return core::make_unexpected(db.error());
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
            PersistenceResult<PersistedRecord> decoded = DecodeRecord(records[i].second);
            if (!decoded.ok()) {
                AGENTENV_WARN("discarding invalid paused sandbox record record_key="
                              << key << " error=" << decoded.error().Message());
                // Rust removes the record only when its key is a sandbox id;
                // anything else is not this persister's to delete.
                core::Expected<core::SandboxId, std::string> id_from_key =
                    core::SandboxId::Parse(key);
                if (id_from_key.ok()) (void)RemoveRecord(id_from_key.value().ToString());
                continue;
            }
            PersistedRecord record = decoded.value();
            const std::string sandbox_id = record.metadata.id.ToString();

            if (record.lifecycle == PersistedLifecycle::Resuming) {
                AGENTENV_WARN("discarding paused sandbox record left in resuming state "
                              "sandbox_id=" << sandbox_id);
                PersistenceResult<core::Unit> cleanup = CleanupInvalidRecord(sandbox_id);
                if (!cleanup.ok()) return core::make_unexpected(cleanup.error());
                continue;
            }

            record.metadata.state = SandboxState::Paused;
            record.metadata.paused_state.reset();

            // Rust `into_metadata_without_runtime_state`: visible, listable and
            // deletable, but never resumable on this node.
            if (record.metadata.virtualization_mode != virtualization_mode_) {
                AGENTENV_WARN("loading paused sandbox metadata without resumable runtime "
                              "state because its virtualization mode is incompatible "
                              "sandbox_id=" << sandbox_id);
                retained.insert(sandbox_id);
                sandboxes.push_back(record.metadata);
                continue;
            }

            // Rust `into_metadata`. A null factory is the C++-only "metadata
            // only" read and skips decoding.
            if (factory != NULL) {
                core::Expected<std::shared_ptr<sandbox::PausedSandboxState>, core::AnyError>
                    state = factory->DecodePausedState(record.artifact_root,
                                                       record.state.ToString());
                if (!state.ok()) {
                    // Keeping it would strand disk space behind a sandbox that
                    // can never be resumed.
                    SandboxPersistenceError err = SandboxPersistenceError::InvalidRecord(
                        "failed to decode paused sandbox state", state.error().chain());
                    AGENTENV_WARN("discarding unusable paused sandbox record sandbox_id="
                                  << sandbox_id << " error=" << err.Message());
                    PersistenceResult<core::Unit> cleanup = CleanupInvalidRecord(sandbox_id);
                    if (!cleanup.ok()) return core::make_unexpected(cleanup.error());
                    continue;
                }
                record.metadata.paused_state = state.value();
            }

            retained.insert(sandbox_id);
            sandboxes.push_back(record.metadata);
        }

        PersistenceResult<core::Unit> orphans = CleanupOrphanArtifacts(retained);
        if (!orphans.ok()) return core::make_unexpected(orphans.error());

        AGENTENV_INFO("loaded paused sandbox records loaded=" << sandboxes.size()
                      << " retained=" << retained.size());
        return sandboxes;
    }

    /// Rust `allocate_artifact_root`.
    PersistenceResult<core::Optional<std::string> >
    AllocateArtifactRoot(const core::SandboxId& id) {
        const std::string artifact_root =
            SandboxArtifactRoot(id.ToString()) + "/" + core::Uuid::GenV7().ToString();
        core::Expected<core::Unit, std::string> created = core::fs::CreateDirAll(artifact_root);
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
                  const sandbox::PausedSandboxState* paused_state) {
        if (!artifact_root.has_value()) {
            return core::make_unexpected(SandboxPersistenceError::RuntimeState(
                "file-backed persister requires an allocated artifact root"));
        }
        // Rust takes `&dyn PausedSandboxState`, so there is no null case there.
        if (paused_state == NULL) {
            return core::make_unexpected(SandboxPersistenceError::RuntimeState(
                "file-backed persister requires a paused sandbox state"));
        }

        // Rust `paused_state.encode()` returns a `serde_json::Value`; the C++
        // trait returns its text, so a payload that is not JSON is this
        // port's form of an encode failure.
        core::Expected<core::Json, core::AnyError> state =
            core::Json::Parse(paused_state->Encode());
        if (!state.ok()) {
            (void)RemoveArtifactRoot(*artifact_root);
            return core::make_unexpected(SandboxPersistenceError::InvalidRecord(
                "failed to encode paused sandbox state", state.error().chain()));
        }

        PersistedRecord record;
        record.version       = kRecordVersion;
        record.lifecycle     = PersistedLifecycle::Paused;
        record.metadata      = metadata;
        record.artifact_root = *artifact_root;
        record.state         = state.value();

        PersistenceResult<core::Unit> stored = PutRecord(record);
        if (!stored.ok()) (void)RemoveArtifactRoot(*artifact_root);
        return stored;
    }

    /// Rust `mark_resuming`.
    PersistenceResult<core::Unit> MarkResuming(const core::SandboxId& id) {
        return SetLifecycle(id, PersistedLifecycle::Resuming);
    }

    /// Rust `rollback_resuming`.
    PersistenceResult<core::Unit> RollbackResuming(const core::SandboxId& id) {
        return SetLifecycle(id, PersistedLifecycle::Paused);
    }

    /// Rust `delete_record`.
    PersistenceResult<core::Unit> DeleteRecord(const core::SandboxId& id) {
        return RemoveRecord(id.ToString());
    }

    /// Rust `delete_record_and_artifacts`.
    PersistenceResult<core::Unit> DeleteRecordAndArtifacts(const core::SandboxId& id) {
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
    core::Expected<std::shared_ptr<local_store::KvStore>, SandboxPersistenceError> Db() {
        if (db_) return db_;
        core::Expected<std::shared_ptr<local_store::KvStore>, std::string> opened =
            local_store::KvStore::Open(RecordsDbPath(), durability_);
        if (!opened.ok()) {
            return core::make_unexpected(
                SandboxPersistenceError::Store("open RocksDB", opened.error()));
        }
        db_ = opened.value();
        return db_;
    }

    /// Rust `get_record`.
    PersistenceResult<PersistedRecord> GetRecord(const std::string& sandbox_id) {
        core::Expected<std::shared_ptr<local_store::KvStore>, SandboxPersistenceError> db = Db();
        if (!db.ok()) return core::make_unexpected(db.error());
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
        return DecodeRecord(*bytes.value());
    }

    /// Rust `put_record`.
    PersistenceResult<core::Unit> PutRecord(const PersistedRecord& record) {
        core::Expected<std::shared_ptr<local_store::KvStore>, SandboxPersistenceError> db = Db();
        if (!db.ok()) return core::make_unexpected(db.error());
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
        core::Expected<std::shared_ptr<local_store::KvStore>, SandboxPersistenceError> db = Db();
        if (!db.ok()) return core::make_unexpected(db.error());
        core::Expected<core::Unit, std::string> removed = db.value()->Delete(sandbox_id);
        if (!removed.ok()) {
            return core::make_unexpected(SandboxPersistenceError::Store(
                "remove paused sandbox record", removed.error()));
        }
        return core::Unit{};
    }

    /// Rust `remove_artifact_root` — an absent path is not an error.
    static PersistenceResult<core::Unit> RemoveArtifactRoot(const std::string& path) {
        core::Expected<core::Unit, std::string> removed = core::fs::RemoveDirAll(path);
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
    PersistenceResult<core::Unit> CleanupOrphanArtifacts(const std::set<std::string>& retained) {
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
            const std::string path = artifacts_root + "/" + names[i];
            if (!IsRealDirectory(path)) continue;
            // Rust skips names that are not a sandbox id.
            core::Expected<core::SandboxId, std::string> id = core::SandboxId::Parse(names[i]);
            if (!id.ok()) continue;
            if (retained.find(id.value().ToString()) != retained.end()) continue;
            AGENTENV_INFO("removing orphaned paused sandbox artifacts sandbox_id="
                          << names[i] << " artifacts=" << path);
            PersistenceResult<core::Unit> removed = RemoveArtifactRoot(path);
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

FileBackedSandboxPersister::FileBackedSandboxPersister(const std::string& root,
                                                       core::VirtualizationMode mode)
    : impl_(new Impl(root, mode)) {}

FileBackedSandboxPersister::~FileBackedSandboxPersister() {}

FileBackedSandboxPersister&
FileBackedSandboxPersister::WithDurability(local_store::Durability durability) {
    impl_->SetDurability(durability);
    return *this;
}

PersistenceResult<std::vector<SandboxMetadata> >
FileBackedSandboxPersister::LoadAll(sandbox::SandboxBackendFactory* factory) {
    return impl_->LoadAll(factory);
}

PersistenceResult<core::Optional<std::string> >
FileBackedSandboxPersister::AllocateArtifactRoot(const core::SandboxId& id) {
    return impl_->AllocateArtifactRoot(id);
}

PersistenceResult<core::Unit>
FileBackedSandboxPersister::PersistPaused(const SandboxMetadata& metadata,
                                          const core::Optional<std::string>& artifact_root,
                                          const sandbox::PausedSandboxState* paused_state) {
    return impl_->PersistPaused(metadata, artifact_root, paused_state);
}

PersistenceResult<core::Unit> FileBackedSandboxPersister::MarkResuming(const core::SandboxId& id) {
    return impl_->MarkResuming(id);
}

PersistenceResult<core::Unit>
FileBackedSandboxPersister::RollbackResuming(const core::SandboxId& id) {
    return impl_->RollbackResuming(id);
}

PersistenceResult<core::Unit> FileBackedSandboxPersister::DeleteRecord(const core::SandboxId& id) {
    return impl_->DeleteRecord(id);
}

PersistenceResult<core::Unit>
FileBackedSandboxPersister::DeleteRecordAndArtifacts(const core::SandboxId& id) {
    return impl_->DeleteRecordAndArtifacts(id);
}

}  // namespace orchestrator
}  // namespace agentenv
