// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/persistence/file_backed.rs (tests) +
//       src/orchestrator/tests.rs :: new_loads_persisted_sandboxes_into_store,
//       managed_seed_continuity_only_applies_to_token_protected_sandboxes,
//       new_returns_error_when_loading_persisted_sandboxes_fails
//
// Rust's persister tests use `LocalStoreDurability::Memory` and peek at the
// RocksDB handle the persister holds. This port's store keeps its state in an
// append log, so the tests use `Wal` and inspect the records by opening a
// second handle, which replays the same log.
#include "microtest.h"

#include <sys/stat.h>
#include <unistd.h>

#include "agentenv/core/fs.h"
#include "agentenv/local_store.h"
#include "agentenv/orchestrator/persistence.h"
#include "agentenv/orchestrator/service.h"
#include "orchestrator_fakes.h"

using namespace agentenv;
using namespace agentenv::orchestrator;
using agentenv::testing::BackendScript;
using agentenv::testing::FakeFactory;
using agentenv::testing::FakePausedState;

namespace {

struct TempDir {
    std::string path;
    TempDir() {
        core::Expected<std::string, std::string> d =
            core::fs::CreateTempDir("agentenv-persist-");
        path = d.ok() ? d.value() : std::string("/tmp/agentenv-persist-fallback");
    }
    ~TempDir() { (void)core::fs::RemoveDirAll(path); }
    std::string Join(const std::string& leaf) const { return path + "/" + leaf; }
};

std::string ArtifactRootOf(const std::string& root, const core::SandboxId& id) {
    return root + "/artifacts/" + id.ToString();
}

/// Rust `test_persister` (durability adjusted, see the file comment).
std::shared_ptr<FileBackedSandboxPersister>
TestPersister(const std::string& root,
              core::VirtualizationMode mode = core::VirtualizationMode::Kvm) {
    std::shared_ptr<FileBackedSandboxPersister> p(new FileBackedSandboxPersister(root, mode));
    p->WithDurability(local_store::Durability::Wal);
    return p;
}

/// Rust `has_record`.
bool HasRecord(const std::string& root, const core::SandboxId& id) {
    core::Expected<std::shared_ptr<local_store::KvStore>, std::string> db =
        local_store::KvStore::Open(root + "/records.db", local_store::Durability::Wal);
    if (!db.ok()) return false;
    core::Expected<core::Optional<std::string>, std::string> v = db.value()->Get(id.ToString());
    return v.ok() && v.value().has_value();
}

bool HasKey(const std::string& root, const std::string& key) {
    core::Expected<std::shared_ptr<local_store::KvStore>, std::string> db =
        local_store::KvStore::Open(root + "/records.db", local_store::Durability::Wal);
    if (!db.ok()) return false;
    core::Expected<core::Optional<std::string>, std::string> v = db.value()->Get(key);
    return v.ok() && v.value().has_value();
}

void PutRaw(const std::string& root, const std::string& key, const std::string& value) {
    core::Expected<std::shared_ptr<local_store::KvStore>, std::string> db =
        local_store::KvStore::Open(root + "/records.db", local_store::Durability::Wal);
    MT_EXPECT_TRUE(db.ok());
    MT_EXPECT_TRUE(db.value()->Put(key, value).ok());
}

SandboxMetadata MetaFor(core::VirtualizationMode mode = core::VirtualizationMode::Kvm) {
    SandboxMetadata m;
    m.id = core::SandboxId::Fresh();
    m.virtualization_mode = mode;
    return m;
}

/// Rust `persist_test_record`.
core::SandboxId PersistTestRecord(FileBackedSandboxPersister& p, const std::string& snapshot_root,
                                  core::VirtualizationMode mode = core::VirtualizationMode::Kvm,
                                  SandboxMetadata* out = NULL) {
    MT_EXPECT_TRUE(core::fs::CreateDirAll(snapshot_root).ok());
    SandboxMetadata m = MetaFor(mode);
    FakePausedState state;
    MT_EXPECT_TRUE(p.PersistPaused(m, core::Optional<std::string>(snapshot_root), &state).ok());
    if (out) *out = m;
    return m.id;
}

/// Records what `DecodePausedState` was handed, so the wire format of the
/// embedded state can be asserted.
class RecordingDecodeFactory : public FakeFactory {
 public:
    explicit RecordingDecodeFactory(BackendScript* s) : FakeFactory(s) {}
    std::string last_artifact_root;
    std::string last_state;
    core::Expected<std::shared_ptr<sandbox::PausedSandboxState>, core::AnyError>
    DecodePausedState(const std::string& artifact_root, const std::string& state) override {
        last_artifact_root = artifact_root;
        last_state = state;
        return FakeFactory::DecodePausedState(artifact_root, state);
    }
};

/// Rust `FailingEncodeState` — in this port, an encode that is not JSON.
class FailingEncodeState : public sandbox::PausedSandboxState {
 public:
    std::string Encode() const override { return "not json {"; }
    sandbox::RuntimeArtifactSet RuntimeArtifacts() const override {
        return sandbox::RuntimeArtifactSet::Empty();
    }
};

cfg::AppConfig TestConfig(const TempDir& home) {
    cfg::AppConfig config;
    config.home_path = home.path;
    config.orchestrator.persisted_sandbox_store_path = home.Join("persisted-sandboxes");
    config.sandbox.access_token_hash_seed = std::string("test-seed");
    return config;
}

}  // namespace

// ============================================================
// file_backed.rs tests
// ============================================================

MT_TEST(file_persister_round_trips_paused_record) {
    TempDir t;
    BackendScript script;
    FakeFactory factory(&script);
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    SandboxMetadata m;
    core::SandboxId id = PersistTestRecord(*p, t.Join("snap"), core::VirtualizationMode::Kvm, &m);

    PersistenceResult<std::vector<SandboxMetadata> > loaded = p->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_EQ(loaded.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(loaded.value()[0].id == id);
    MT_EXPECT_TRUE(loaded.value()[0].state == SandboxState::Paused);
    MT_EXPECT_TRUE(loaded.value()[0].paused_state != NULL);
    MT_EXPECT_EQ(factory.decode_paused_calls, 1);
}

MT_TEST(paused_record_from_other_mode_is_visible_but_not_resumable) {
    TempDir t;
    BackendScript script;
    FakeFactory factory(&script);
    const std::string snap = t.Join("snap");
    core::SandboxId id;
    {
        std::shared_ptr<FileBackedSandboxPersister> kvm = TestPersister(t.path);
        id = PersistTestRecord(*kvm, snap);
    }
    std::shared_ptr<FileBackedSandboxPersister> pvm =
        TestPersister(t.path, core::VirtualizationMode::Pvm);

    PersistenceResult<std::vector<SandboxMetadata> > loaded = pvm->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_EQ(loaded.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(loaded.value()[0].id == id);
    MT_EXPECT_TRUE(loaded.value()[0].state == SandboxState::Paused);
    MT_EXPECT_TRUE(loaded.value()[0].virtualization_mode == core::VirtualizationMode::Kvm);
    MT_EXPECT_TRUE(loaded.value()[0].paused_state == NULL);
    // Not even offered to the decoder: its state belongs to another VMM ABI.
    MT_EXPECT_EQ(factory.decode_paused_calls, 0);
    MT_EXPECT_TRUE(HasRecord(t.path, id));
    MT_EXPECT_TRUE(core::fs::Exists(snap));
}

MT_TEST(mixed_mode_records_are_both_visible_and_retained) {
    TempDir t;
    BackendScript script;
    FakeFactory factory(&script);
    core::SandboxId kvm_id;
    {
        std::shared_ptr<FileBackedSandboxPersister> kvm = TestPersister(t.path);
        kvm_id = PersistTestRecord(*kvm, t.Join("kvm-artifacts"));
    }
    std::shared_ptr<FileBackedSandboxPersister> pvm =
        TestPersister(t.path, core::VirtualizationMode::Pvm);
    core::SandboxId pvm_id =
        PersistTestRecord(*pvm, t.Join("pvm-artifacts"), core::VirtualizationMode::Pvm);

    PersistenceResult<std::vector<SandboxMetadata> > loaded = pvm->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_EQ(loaded.value().size(), static_cast<std::size_t>(2));
    bool saw_kvm = false, saw_pvm = false;
    for (std::size_t i = 0; i < loaded.value().size(); ++i) {
        const SandboxMetadata& m = loaded.value()[i];
        if (m.id == kvm_id) {
            saw_kvm = true;
            MT_EXPECT_TRUE(m.virtualization_mode == core::VirtualizationMode::Kvm);
            MT_EXPECT_TRUE(m.paused_state == NULL);
        } else if (m.id == pvm_id) {
            saw_pvm = true;
            MT_EXPECT_TRUE(m.virtualization_mode == core::VirtualizationMode::Pvm);
            MT_EXPECT_TRUE(m.paused_state != NULL);
        }
    }
    MT_EXPECT_TRUE(saw_kvm && saw_pvm);
    MT_EXPECT_TRUE(HasRecord(t.path, kvm_id));
    MT_EXPECT_TRUE(HasRecord(t.path, pvm_id));
    MT_EXPECT_TRUE(core::fs::Exists(t.Join("kvm-artifacts")));
    MT_EXPECT_TRUE(core::fs::Exists(t.Join("pvm-artifacts")));
}

MT_TEST(allocate_artifact_root_creates_unique_snapshot_roots) {
    TempDir t;
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    core::SandboxId id = core::SandboxId::Fresh();

    PersistenceResult<core::Optional<std::string> > first = p->AllocateArtifactRoot(id);
    PersistenceResult<core::Optional<std::string> > second = p->AllocateArtifactRoot(id);
    MT_EXPECT_TRUE(first.ok() && first.value().has_value());
    MT_EXPECT_TRUE(second.ok() && second.value().has_value());
    MT_EXPECT_TRUE(*first.value() != *second.value());
    MT_EXPECT_TRUE(core::fs::IsDir(*first.value()));
    MT_EXPECT_TRUE(core::fs::IsDir(*second.value()));
    // <root>/artifacts/<sandbox-id>/<uuid>
    core::Optional<std::string> parent = core::fs::Parent(*first.value());
    MT_EXPECT_TRUE(parent.has_value());
    MT_EXPECT_EQ(*core::fs::FileName(*parent), id.ToString());
    MT_EXPECT_EQ(*core::fs::FileName(*core::fs::Parent(*parent)), std::string("artifacts"));
}

MT_TEST(resuming_records_are_cleaned_on_load) {
    TempDir t;
    BackendScript script;
    FakeFactory factory(&script);
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    SandboxMetadata m = MetaFor();
    const std::string snap = ArtifactRootOf(t.path, m.id) + "/snapshot";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(snap).ok());
    FakePausedState state;
    MT_EXPECT_TRUE(p->PersistPaused(m, core::Optional<std::string>(snap), &state).ok());
    MT_EXPECT_TRUE(p->MarkResuming(m.id).ok());

    PersistenceResult<std::vector<SandboxMetadata> > loaded = p->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_TRUE(loaded.value().empty());
    MT_EXPECT_TRUE(!HasRecord(t.path, m.id));
    MT_EXPECT_TRUE(!core::fs::Exists(ArtifactRootOf(t.path, m.id)));
}

MT_TEST(persist_paused_cleans_artifacts_when_encode_fails) {
    TempDir t;
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    const std::string snap = t.Join("snap");
    MT_EXPECT_TRUE(core::fs::CreateDirAll(snap).ok());
    FailingEncodeState state;

    PersistenceResult<core::Unit> r =
        p->PersistPaused(MetaFor(), core::Optional<std::string>(snap), &state);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().kind == SandboxPersistenceError::Kind::InvalidRecord);
    MT_EXPECT_EQ(r.error().reason, std::string("failed to encode paused sandbox state"));
    MT_EXPECT_TRUE(!core::fs::Exists(snap));
}

MT_TEST(persist_paused_requires_artifact_root) {
    TempDir t;
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    FakePausedState state;
    PersistenceResult<core::Unit> r = p->PersistPaused(MetaFor(), core::nullopt, &state);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().kind == SandboxPersistenceError::Kind::RuntimeState);
}

MT_TEST(mark_resuming_and_rollback_preserve_loadability) {
    TempDir t;
    BackendScript script;
    FakeFactory factory(&script);
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    const std::string snap = t.Join("snap");
    core::SandboxId id = PersistTestRecord(*p, snap);

    MT_EXPECT_TRUE(p->MarkResuming(id).ok());
    MT_EXPECT_TRUE(p->RollbackResuming(id).ok());
    PersistenceResult<std::vector<SandboxMetadata> > loaded = p->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_EQ(loaded.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(loaded.value()[0].id == id);
    MT_EXPECT_TRUE(core::fs::Exists(snap));
}

MT_TEST(mark_resuming_missing_record_is_invalid_record) {
    TempDir t;
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    core::SandboxId id = core::SandboxId::Fresh();
    PersistenceResult<core::Unit> r = p->MarkResuming(id);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().kind == SandboxPersistenceError::Kind::InvalidRecord);
    MT_EXPECT_EQ(r.error().reason,
                 std::string("paused sandbox record ") + id.ToString() + " not found");
}

MT_TEST(delete_record_removes_record_but_keeps_artifacts) {
    TempDir t;
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    const std::string snap = t.Join("snap");
    core::SandboxId id = PersistTestRecord(*p, snap);

    MT_EXPECT_TRUE(p->DeleteRecord(id).ok());
    MT_EXPECT_TRUE(!HasRecord(t.path, id));
    MT_EXPECT_TRUE(core::fs::Exists(snap));
}

MT_TEST(load_all_removes_orphan_artifacts_without_records) {
    TempDir t;
    BackendScript script;
    FakeFactory factory(&script);
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    core::SandboxId id = core::SandboxId::Fresh();
    MT_EXPECT_TRUE(core::fs::CreateDirAll(ArtifactRootOf(t.path, id) + "/resumed-generation").ok());

    PersistenceResult<std::vector<SandboxMetadata> > loaded = p->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_TRUE(loaded.value().empty());
    MT_EXPECT_TRUE(!core::fs::Exists(ArtifactRootOf(t.path, id)));
}

MT_TEST(load_all_orphan_sweep_skips_non_ids_files_and_symlinks) {
    TempDir t;
    BackendScript script;
    FakeFactory factory(&script);
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    const std::string artifacts = t.Join("artifacts");
    MT_EXPECT_TRUE(core::fs::CreateDirAll(artifacts + "/not-a-sandbox-id/keep").ok());

    // A *file* named like a sandbox id is not a sandbox artifact root.
    const std::string file_id = core::SandboxId::Fresh().ToString();
    MT_EXPECT_TRUE(core::fs::Write(artifacts + "/" + file_id, "x").ok());

    // A *symlink* named like a sandbox id must not lead the sweep outside the
    // store: Rust's `DirEntry::file_type` does not follow links.
    const std::string outside = t.Join("outside");
    MT_EXPECT_TRUE(core::fs::CreateDirAll(outside + "/precious").ok());
    const std::string link_id = core::SandboxId::Fresh().ToString();
    MT_EXPECT_EQ(::symlink(outside.c_str(), (artifacts + "/" + link_id).c_str()), 0);

    PersistenceResult<std::vector<SandboxMetadata> > loaded = p->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_TRUE(core::fs::Exists(artifacts + "/not-a-sandbox-id/keep"));
    MT_EXPECT_TRUE(core::fs::IsFile(artifacts + "/" + file_id));
    MT_EXPECT_TRUE(core::fs::Exists(outside + "/precious"));
    // The link itself is skipped, not removed — Rust never acts on it.
    struct stat link_st;
    MT_EXPECT_EQ(::lstat((artifacts + "/" + link_id).c_str(), &link_st), 0);
    MT_EXPECT_TRUE(S_ISLNK(link_st.st_mode));
}

MT_TEST(load_all_keeps_artifacts_for_valid_paused_record) {
    TempDir t;
    BackendScript script;
    FakeFactory factory(&script);
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    SandboxMetadata m = MetaFor();
    const std::string snap = ArtifactRootOf(t.path, m.id) + "/paused-generation";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(snap).ok());
    FakePausedState state;
    MT_EXPECT_TRUE(p->PersistPaused(m, core::Optional<std::string>(snap), &state).ok());

    PersistenceResult<std::vector<SandboxMetadata> > loaded = p->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_EQ(loaded.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(core::fs::Exists(ArtifactRootOf(t.path, m.id)));
}

MT_TEST(delete_record_and_artifacts_removes_both) {
    TempDir t;
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    SandboxMetadata m = MetaFor();
    const std::string snap = ArtifactRootOf(t.path, m.id) + "/snapshot";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(snap).ok());
    FakePausedState state;
    MT_EXPECT_TRUE(p->PersistPaused(m, core::Optional<std::string>(snap), &state).ok());

    MT_EXPECT_TRUE(p->DeleteRecordAndArtifacts(m.id).ok());
    MT_EXPECT_TRUE(!HasRecord(t.path, m.id));
    MT_EXPECT_TRUE(!core::fs::Exists(ArtifactRootOf(t.path, m.id)));
}

MT_TEST(delete_record_and_artifacts_removes_artifacts_without_record) {
    TempDir t;
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    core::SandboxId id = core::SandboxId::Fresh();
    MT_EXPECT_TRUE(core::fs::CreateDirAll(ArtifactRootOf(t.path, id) + "/stale-generation").ok());

    MT_EXPECT_TRUE(p->DeleteRecordAndArtifacts(id).ok());
    MT_EXPECT_TRUE(!core::fs::Exists(ArtifactRootOf(t.path, id)));
}

MT_TEST(delete_record_and_artifacts_removes_invalid_record) {
    TempDir t;
    core::SandboxId id = core::SandboxId::Fresh();
    PutRaw(t.path, id.ToString(), "not-json");
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);

    MT_EXPECT_TRUE(p->DeleteRecordAndArtifacts(id).ok());
    MT_EXPECT_TRUE(!HasRecord(t.path, id));
}

MT_TEST(load_all_discards_invalid_record) {
    TempDir t;
    BackendScript script;
    FakeFactory factory(&script);
    core::SandboxId id = core::SandboxId::Fresh();
    PutRaw(t.path, id.ToString(), "not-json");
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);

    PersistenceResult<std::vector<SandboxMetadata> > loaded = p->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_TRUE(loaded.value().empty());
    MT_EXPECT_TRUE(!HasRecord(t.path, id));
}

MT_TEST(load_all_discards_unusable_record_and_artifacts) {
    TempDir t;
    BackendScript script;
    FakeFactory factory(&script);
    factory.fail_decode = true;  // Rust `RejectingFactory`
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    SandboxMetadata m = MetaFor();
    const std::string snap = ArtifactRootOf(t.path, m.id) + "/snapshot";
    MT_EXPECT_TRUE(core::fs::CreateDirAll(snap).ok());
    FakePausedState state;
    MT_EXPECT_TRUE(p->PersistPaused(m, core::Optional<std::string>(snap), &state).ok());

    PersistenceResult<std::vector<SandboxMetadata> > loaded = p->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_TRUE(loaded.value().empty());
    MT_EXPECT_TRUE(!HasRecord(t.path, m.id));
    MT_EXPECT_TRUE(!core::fs::Exists(ArtifactRootOf(t.path, m.id)));
}

// ---- record wire format (serde `PersistedPausedRecord`) ----

namespace {
std::string RecordJson(const SandboxMetadata& m, const std::string& lifecycle_field,
                       const std::string& artifact_root, const std::string& state_field) {
    std::string s = "{\"version\":1,";
    s += lifecycle_field;
    s += "\"metadata\":" + m.ToJson().ToString() + ",";
    s += "\"artifactRoot\":\"" + artifact_root + "\"";
    s += state_field;
    s += "}";
    return s;
}
}  // namespace

MT_TEST(record_state_is_embedded_json_not_a_string) {
    TempDir t;
    BackendScript script;
    RecordingDecodeFactory factory(&script);
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);
    const std::string snap = t.Join("snap");
    core::SandboxId id = PersistTestRecord(*p, snap);

    // What the persister wrote: `"state": {}` — an object, as serde writes a
    // `serde_json::Value`, not `"state": "{}"`.
    core::Expected<std::shared_ptr<local_store::KvStore>, std::string> db =
        local_store::KvStore::Open(t.path + "/records.db", local_store::Durability::Wal);
    MT_EXPECT_TRUE(db.ok());
    core::Expected<core::Json, core::AnyError> written =
        core::Json::Parse(*db.value()->Get(id.ToString()).value());
    MT_EXPECT_TRUE(written.ok());
    const core::JsonObject& fields = written.value().as_object();
    MT_EXPECT_TRUE(fields.find("state")->second.kind() == core::Json::Kind::Object);
    MT_EXPECT_EQ(fields.find("lifecycle")->second.as_string(), std::string("paused"));
    MT_EXPECT_EQ(fields.find("artifactRoot")->second.as_string(), snap);
    MT_EXPECT_EQ(fields.find("version")->second.as_int(), static_cast<int64_t>(1));
}

MT_TEST(record_written_by_rust_server_loads_and_hands_state_to_decoder) {
    TempDir t;
    BackendScript script;
    RecordingDecodeFactory factory(&script);
    SandboxMetadata m = MetaFor();
    PutRaw(t.path, m.id.ToString(),
           RecordJson(m, "\"lifecycle\":\"paused\",", "/snap/root",
                      ",\"state\":{\"memory\":\"mem.bin\",\"slots\":[1,2]}"));
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);

    PersistenceResult<std::vector<SandboxMetadata> > loaded = p->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_EQ(loaded.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(loaded.value()[0].paused_state != NULL);
    MT_EXPECT_EQ(factory.last_artifact_root, std::string("/snap/root"));
    core::Expected<core::Json, core::AnyError> handed = core::Json::Parse(factory.last_state);
    MT_EXPECT_TRUE(handed.ok());
    MT_EXPECT_TRUE(handed.value().kind() == core::Json::Kind::Object);
    MT_EXPECT_EQ(handed.value().as_object().find("memory")->second.as_string(),
                 std::string("mem.bin"));
}

MT_TEST(record_missing_or_unknown_lifecycle_is_discarded_not_guessed) {
    TempDir t;
    BackendScript script;
    FakeFactory factory(&script);
    SandboxMetadata missing = MetaFor();
    SandboxMetadata unknown = MetaFor();
    SandboxMetadata no_state = MetaFor();
    SandboxMetadata no_root = MetaFor();
    PutRaw(t.path, missing.id.ToString(), RecordJson(missing, "", "/r", ",\"state\":{}"));
    PutRaw(t.path, unknown.id.ToString(),
           RecordJson(unknown, "\"lifecycle\":\"hibernating\",", "/r", ",\"state\":{}"));
    PutRaw(t.path, no_state.id.ToString(),
           RecordJson(no_state, "\"lifecycle\":\"paused\",", "/r", ""));
    PutRaw(t.path, no_root.id.ToString(),
           "{\"version\":1,\"lifecycle\":\"paused\",\"metadata\":" +
               no_root.ToJson().ToString() + ",\"state\":{}}");
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);

    PersistenceResult<std::vector<SandboxMetadata> > loaded = p->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_TRUE(loaded.value().empty());
    MT_EXPECT_EQ(factory.decode_paused_calls, 0);
    MT_EXPECT_TRUE(!HasRecord(t.path, missing.id));
    MT_EXPECT_TRUE(!HasRecord(t.path, unknown.id));
    MT_EXPECT_TRUE(!HasRecord(t.path, no_state.id));
    MT_EXPECT_TRUE(!HasRecord(t.path, no_root.id));
}

MT_TEST(record_with_unsupported_version_is_discarded) {
    TempDir t;
    BackendScript script;
    FakeFactory factory(&script);
    SandboxMetadata m = MetaFor();
    std::string json = RecordJson(m, "\"lifecycle\":\"paused\",", "/r", ",\"state\":{}");
    json.replace(json.find("\"version\":1"), 11, "\"version\":2");
    PutRaw(t.path, m.id.ToString(), json);
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);

    PersistenceResult<std::vector<SandboxMetadata> > loaded = p->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_TRUE(loaded.value().empty());
    MT_EXPECT_TRUE(!HasRecord(t.path, m.id));
}

MT_TEST(invalid_record_under_non_sandbox_key_is_left_alone) {
    // Rust removes an undecodable record only when its key parses as a
    // sandbox id; a foreign key is not this persister's to delete.
    TempDir t;
    BackendScript script;
    FakeFactory factory(&script);
    PutRaw(t.path, "schema-marker", "not-json");
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.path);

    PersistenceResult<std::vector<SandboxMetadata> > loaded = p->LoadAll(&factory);
    MT_EXPECT_TRUE(loaded.ok());
    MT_EXPECT_TRUE(loaded.value().empty());
    MT_EXPECT_TRUE(HasKey(t.path, "schema-marker"));
}

MT_TEST(unopenable_store_reports_open_rocksdb) {
    TempDir t;
    MT_EXPECT_TRUE(core::fs::Write(t.Join("blocker"), "file").ok());
    std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(t.Join("blocker/sub"));
    PersistenceResult<std::vector<SandboxMetadata> > loaded = p->LoadAll(NULL);
    MT_EXPECT_TRUE(!loaded.ok());
    MT_EXPECT_TRUE(loaded.error().kind == SandboxPersistenceError::Kind::Store);
    MT_EXPECT_EQ(loaded.error().operation, std::string("open RocksDB"));
}

// ============================================================
// tests.rs — Orchestrator::new startup path
// ============================================================

MT_TEST(managed_seed_continuity_only_applies_to_token_protected_sandboxes) {
    SandboxMetadata public_insecure;
    std::vector<SandboxMetadata> v(1, public_insecure);
    MT_EXPECT_TRUE(!PersistedSandboxesRequireManagedSeed(v));

    SandboxMetadata secure = public_insecure;
    secure.secure = true;
    MT_EXPECT_TRUE(PersistedSandboxesRequireManagedSeed(std::vector<SandboxMetadata>(1, secure)));

    SandboxMetadata priv = public_insecure;
    priv.network_policy.allow_public_traffic = false;
    MT_EXPECT_TRUE(PersistedSandboxesRequireManagedSeed(std::vector<SandboxMetadata>(1, priv)));

    MT_EXPECT_TRUE(!PersistedSandboxesRequireManagedSeed(std::vector<SandboxMetadata>()));
}

MT_TEST(new_loads_persisted_sandboxes_into_store) {
    TempDir home;
    cfg::AppConfig config = TestConfig(home);
    BackendScript script;
    std::shared_ptr<FakeFactory> factory(new FakeFactory(&script));
    const std::string root = config.orchestrator.persisted_sandbox_store_path;

    SandboxMetadata m = MetaFor();
    m.auto_resume = true;
    {
        std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(root);
        MT_EXPECT_TRUE(core::fs::CreateDirAll(root + "/snap").ok());
        FakePausedState state;
        MT_EXPECT_TRUE(p->PersistPaused(m, core::Optional<std::string>(root + "/snap"), &state).ok());
    }

    std::shared_ptr<InMemoryMetadataStore> store(new InMemoryMetadataStore());
    OrchestratorResult<std::shared_ptr<Orchestrator> > orch =
        Orchestrator::New(store, factory, TestPersister(root), config);
    MT_EXPECT_TRUE(orch.ok());

    OrchestratorResult<core::Optional<SandboxMetadata> > restored = orch.value()->GetSandbox(m.id);
    MT_EXPECT_TRUE(restored.ok() && restored.value().has_value());
    MT_EXPECT_TRUE(restored.value()->state == SandboxState::Paused);
    MT_EXPECT_TRUE(restored.value()->paused_state != NULL);
    OrchestratorResult<ProxyLookupResult> lookup = orch.value()->ProxyLookupFor(m.id);
    MT_EXPECT_TRUE(lookup.ok());
    MT_EXPECT_TRUE(lookup.value() == ProxyLookupResult::Paused(true));
}

MT_TEST(new_returns_error_when_loading_persisted_sandboxes_fails) {
    TempDir home;
    cfg::AppConfig config = TestConfig(home);
    MT_EXPECT_TRUE(core::fs::Write(home.Join("blocker"), "file").ok());
    BackendScript script;
    std::shared_ptr<FakeFactory> factory(new FakeFactory(&script));

    OrchestratorResult<std::shared_ptr<Orchestrator> > orch = Orchestrator::New(
        std::shared_ptr<MetadataStore>(new InMemoryMetadataStore()), factory,
        TestPersister(home.Join("blocker/sub")), config);
    MT_EXPECT_TRUE(!orch.ok());
    MT_EXPECT_TRUE(orch.error().kind == OrchestratorErrorKind::SandboxPersistenceFailed);
}

MT_TEST(new_refuses_to_regenerate_seed_under_token_protected_sandboxes) {
    TempDir home;
    cfg::AppConfig config = TestConfig(home);
    config.sandbox.access_token_hash_seed = core::nullopt;  // managed seed path
    BackendScript script;
    std::shared_ptr<FakeFactory> factory(new FakeFactory(&script));
    const std::string root = config.orchestrator.persisted_sandbox_store_path;

    SandboxMetadata m = MetaFor();
    m.secure = true;
    {
        std::shared_ptr<FileBackedSandboxPersister> p = TestPersister(root);
        MT_EXPECT_TRUE(core::fs::CreateDirAll(root + "/snap").ok());
        FakePausedState state;
        MT_EXPECT_TRUE(p->PersistPaused(m, core::Optional<std::string>(root + "/snap"), &state).ok());
    }

    std::shared_ptr<InMemoryMetadataStore> store(new InMemoryMetadataStore());
    OrchestratorResult<std::shared_ptr<Orchestrator> > orch =
        Orchestrator::New(store, factory, TestPersister(root), config);
    MT_EXPECT_TRUE(!orch.ok());
    MT_EXPECT_TRUE(orch.error().kind == OrchestratorErrorKind::ConfigLoadFailed);
    // Fails before anything is published into the store.
    MT_EXPECT_TRUE(!store->Get(m.id).value().has_value());
    // And no fresh seed was written: that would lock the sandbox out for good.
    MT_EXPECT_TRUE(!core::fs::Exists(home.Join("secrets/sandbox-access-token-hash-seed")));
}

MT_TEST(new_takes_timeout_mode_and_machine_shape_from_config) {
    TempDir home;
    cfg::AppConfig config = TestConfig(home);
    config.orchestrator.default_sandbox_timeout_secs = 42;
    config.virtualization_mode = core::VirtualizationMode::Pvm;
    config.machine.vcpu_count = 3;
    config.machine.mem_size_mib = 3072;
    BackendScript script;
    std::shared_ptr<FakeFactory> factory(new FakeFactory(&script));

    OrchestratorResult<std::shared_ptr<Orchestrator> > orch = Orchestrator::New(
        std::shared_ptr<MetadataStore>(new InMemoryMetadataStore()), factory,
        std::shared_ptr<SandboxPersister>(new DisabledSandboxPersister()), config);
    MT_EXPECT_TRUE(orch.ok());
    MT_EXPECT_EQ(orch.value()->DefaultSandboxTimeoutMs(), static_cast<int64_t>(42000));
    MT_EXPECT_TRUE(orch.value()->VirtualizationMode() == core::VirtualizationMode::Pvm);

    sandbox::SandboxResources shape = DefaultFreshSandboxResources(config);
    MT_EXPECT_EQ(shape.cpu_count, 3u);
    MT_EXPECT_EQ(shape.memory_mib, 3072u);
    MT_EXPECT_EQ(shape.disk_size_mib, 0u);

    // The token generator is live: a secure sandbox gets a verifiable token.
    core::SandboxId id = core::SandboxId::Fresh();
    std::string traffic = orch.value()->TrafficAccessToken(id);
    MT_EXPECT_TRUE(!traffic.empty());
    MT_EXPECT_TRUE(orch.value()->ValidateTrafficAccessToken(id, traffic));
}

MT_TEST(with_file_backed_store_and_factory_restores_from_configured_path) {
    TempDir home;
    cfg::AppConfig config = TestConfig(home);
    config.virtualization_mode = core::VirtualizationMode::Pvm;
    BackendScript script;
    std::shared_ptr<FakeFactory> factory(new FakeFactory(&script));
    const std::string root = config.orchestrator.persisted_sandbox_store_path;

    // One record per mode: the node's mode comes from config, so the PVM one
    // is resumable and the KVM one is listed without runtime state.
    core::SandboxId kvm_id, pvm_id;
    {
        std::shared_ptr<FileBackedSandboxPersister> p =
            TestPersister(root, core::VirtualizationMode::Pvm);
        kvm_id = PersistTestRecord(*p, root + "/kvm", core::VirtualizationMode::Kvm);
        pvm_id = PersistTestRecord(*p, root + "/pvm", core::VirtualizationMode::Pvm);
    }

    OrchestratorResult<std::shared_ptr<Orchestrator> > orch =
        Orchestrator::WithFileBackedStoreAndFactory(factory, config);
    MT_EXPECT_TRUE(orch.ok());
    OrchestratorResult<core::Optional<SandboxMetadata> > kvm = orch.value()->GetSandbox(kvm_id);
    OrchestratorResult<core::Optional<SandboxMetadata> > pvm = orch.value()->GetSandbox(pvm_id);
    MT_EXPECT_TRUE(kvm.ok() && kvm.value().has_value());
    MT_EXPECT_TRUE(pvm.ok() && pvm.value().has_value());
    MT_EXPECT_TRUE(kvm.value()->paused_state == NULL);
    MT_EXPECT_TRUE(pvm.value()->paused_state != NULL);
}

MT_MAIN
