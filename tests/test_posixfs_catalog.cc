// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/catalog.rs test module.
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/file_lock.h"
#include "agentenv/core/fs.h"
#include "agentenv/snapshot/repository/documents.h"
#include "agentenv/snapshot/repository/posixfs/backend.h"
#include "agentenv/snapshot/repository/posixfs/catalog.h"
#include "microtest.h"

namespace {

using agentenv::core::Optional;
using agentenv::core::SnapshotId;
using agentenv::core::Unit;
using agentenv::core::Uuid;
namespace fs = agentenv::core::fs;
namespace posixfs = agentenv::snapshot::repository::posixfs;
namespace repository = agentenv::snapshot::repository;
namespace snapshot = agentenv::snapshot;
namespace sandbox = agentenv::sandbox;

typedef posixfs::PosixFsCatalogStore Store;
typedef posixfs::PosixFsSnapshotArtifactLayout Paths;

struct TempRoot {
    std::string path;

    TempRoot() {
        const agentenv::core::Expected<std::string, std::string> temp =
            fs::CreateTempDir("agentenv-catalog-");
        MT_EXPECT_TRUE(temp.ok());
        path = temp.value();
    }
    ~TempRoot() { fs::RemoveDirAll(path); }
};

/// The store plus the `SnapshotRepository` face over the same root.
///
/// The record methods are not on the store any more. Upstream implements them
/// once, in the backend-agnostic `impl<R: Repository> SnapshotCatalog for R`,
/// and this port now does the same in `repository/record_catalog.h`; the only
/// way in is through the repository. The publish lifecycle, the build cache
/// and the shared documents are still the store's own.
///
/// Both halves addressing the same files is not incidental here: a test that
/// publishes through `store` and then reads the record back through `repo` is
/// also the check that the move needs no migration of an existing root.
struct Catalog {
    TempRoot root;
    std::shared_ptr<Store> store;
    posixfs::PosixFsSnapshotRepository repo;

    Catalog()
        : store(new Store(root.path)),
          repo(store, std::shared_ptr<posixfs::PosixFsArtifactStore>(
                          new posixfs::PosixFsArtifactStore(root.path))) {}
};

SnapshotId FreshId() { return SnapshotId::Fresh(); }

snapshot::SnapshotAlias Alias(const std::string& value) {
    const agentenv::core::Expected<snapshot::SnapshotAlias, std::string> alias =
        snapshot::SnapshotAlias::Parse(value);
    MT_EXPECT_TRUE(alias.ok());
    return alias.value();
}

snapshot::CommittedSnapshot MockCommitted() {
    snapshot::CommittedSnapshot committed;
    committed.runtime_versions =
        snapshot::SnapshotRuntimeVersions::New("kernel", "firecracker", "envd", "0.1.0");
    return committed;
}

snapshot::SnapshotPublishMetadata Metadata(const SnapshotId& id,
                                           const Optional<snapshot::SnapshotAlias>& alias) {
    snapshot::SnapshotPublishMetadata metadata;
    metadata.id = id;
    metadata.alias = alias;
    metadata.source = snapshot::SnapshotPublishSource::Template();
    metadata.runtime_versions =
        snapshot::SnapshotRuntimeVersions::New("kernel", "firecracker", "envd", "0.1.0");
    return metadata;
}

/// Publishes a committed snapshot end to end.
snapshot::SnapshotRecord Publish(Catalog& catalog, const SnapshotId& id,
                                 const Optional<snapshot::SnapshotAlias>& alias) {
    const repository::RepositoryResult<posixfs::PublishSession> session =
        catalog.store->BeginPublish(id);
    MT_EXPECT_TRUE(session.ok());
    const repository::RepositoryResult<snapshot::SnapshotRecord> record =
        catalog.store->CommitPublish(session.value(), Metadata(id, alias), MockCommitted());
    MT_EXPECT_TRUE(record.ok());
    return record.value();
}

snapshot::SnapshotRecord Waiting(const SnapshotId& id,
                                 const Optional<snapshot::SnapshotAlias>& alias) {
    return snapshot::SnapshotRecord::TemplateWaiting(id, alias, sandbox::SandboxResources());
}

}  // namespace

// ---------------------------------------------------------------------------
// publish lifecycle
// ---------------------------------------------------------------------------

MT_TEST(begin_publish_creates_the_layout_and_snapshot_dir) {
    Catalog catalog;
    const SnapshotId id = FreshId();

    MT_EXPECT_TRUE(catalog.store->BeginPublish(id).ok());

    MT_EXPECT_TRUE(fs::IsDir(Paths::RecordsDir(catalog.root.path)));
    MT_EXPECT_TRUE(fs::IsDir(Paths::AliasesDir(catalog.root.path)));
    MT_EXPECT_TRUE(fs::IsDir(Paths::VolumeRecordsDir(catalog.root.path)));
    MT_EXPECT_TRUE(fs::IsDir(Paths::VolumeAliasesDir(catalog.root.path)));
    MT_EXPECT_TRUE(fs::IsDir(Paths(catalog.root.path, id).SnapshotDir()));
}

MT_TEST(commit_publish_writes_the_marker_the_record_and_the_alias) {
    Catalog catalog;
    const SnapshotId id = FreshId();

    const snapshot::SnapshotRecord record =
        Publish(catalog, id, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    MT_EXPECT_TRUE(record.committed.has_value());
    MT_EXPECT_TRUE(record.source.is_template());
    // A snapshot published without a prior build record is Ready with no
    // start time: it was never queued.
    MT_EXPECT_TRUE(record.source.build.status == snapshot::TemplateBuildStatus::Ready);
    MT_EXPECT_TRUE(!record.source.build.started_at_unix_ms.has_value());
    MT_EXPECT_TRUE(record.source.build.finished_at_unix_ms.has_value());

    MT_EXPECT_TRUE(fs::IsFile(Paths(catalog.root.path, id).Path("commit")));
    MT_EXPECT_TRUE(fs::IsFile(Paths::RecordPath(catalog.root.path, id)));
    MT_EXPECT_TRUE(fs::IsFile(Paths::AliasPath(catalog.root.path, Alias("prod"))));
}

MT_TEST(the_publish_path_and_the_shared_record_layer_address_the_same_files) {
    // The record methods moved off this store and onto the shared catalog,
    // which addresses records through `DocumentKey` rather than through
    // `Paths`. If those two spellings ever diverged, every repository root
    // already on disk would become invisible, so assert they agree rather
    // than trusting that they do.
    Catalog catalog;
    const SnapshotId id = FreshId();
    Publish(catalog, id, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    const repository::DocumentKey record_key = repository::DocumentKey::SnapshotRecord(id);
    MT_EXPECT_EQ(fs::Join(catalog.root.path, record_key.PosixPath()),
                 Paths::RecordPath(catalog.root.path, id));
    MT_EXPECT_EQ(fs::Join(catalog.root.path, record_key.PosixLockPath()),
                 Paths::RecordLockPath(catalog.root.path, id));

    const repository::DocumentKey alias_key =
        repository::DocumentKey::SnapshotAlias(Alias("prod"));
    MT_EXPECT_EQ(fs::Join(catalog.root.path, alias_key.PosixPath()),
                 Paths::AliasPath(catalog.root.path, Alias("prod")));
    MT_EXPECT_EQ(fs::Join(catalog.root.path, alias_key.PosixLockPath()),
                 Paths::AliasLockPath(catalog.root.path, Alias("prod")));

    // And the record the publish wrote is readable through the shared layer,
    // by id and by name.
    MT_EXPECT_TRUE(catalog.repo.Get(id.ToString()).value().has_value());
    MT_EXPECT_TRUE(*catalog.repo.ResolveAlias("prod").value() == id);
}

MT_TEST(commit_publish_advances_a_pre_created_template_record) {
    Catalog catalog;
    const SnapshotId id = FreshId();

    snapshot::SnapshotRecord waiting = Waiting(id, Optional<snapshot::SnapshotAlias>());
    waiting.created_at_unix_ms = 1000;
    MT_EXPECT_TRUE(catalog.repo.Create(waiting).ok());

    const snapshot::SnapshotRecord committed =
        Publish(catalog, id, Optional<snapshot::SnapshotAlias>());

    // The original creation time survives, so the record's history is not
    // rewritten by the publish. This also crosses the two layers: the record
    // was written by the shared catalog and read back by the store.
    MT_EXPECT_EQ(committed.created_at_unix_ms, static_cast<int64_t>(1000));
    MT_EXPECT_TRUE(committed.source.build.status == snapshot::TemplateBuildStatus::Ready);
    MT_EXPECT_TRUE(committed.committed.has_value());
}

MT_TEST(commit_publish_rejects_an_alias_held_by_a_live_snapshot) {
    Catalog catalog;
    Publish(catalog, FreshId(), Optional<snapshot::SnapshotAlias>(Alias("prod")));

    const SnapshotId second = FreshId();
    const repository::RepositoryResult<posixfs::PublishSession> session =
        catalog.store->BeginPublish(second);
    MT_EXPECT_TRUE(session.ok());
    const repository::RepositoryResult<snapshot::SnapshotRecord> conflicted =
        catalog.store->CommitPublish(
            session.value(),
            Metadata(second, Optional<snapshot::SnapshotAlias>(Alias("prod"))),
            MockCommitted());

    MT_EXPECT_TRUE(!conflicted.ok());
    MT_EXPECT_TRUE(conflicted.error().kind == repository::RepositoryErrorKind::AliasConflict);

    // Rollback: the failed publish must not leave its directory behind.
    MT_EXPECT_TRUE(!fs::Exists(Paths(catalog.root.path, second).SnapshotDir()));
    MT_EXPECT_TRUE(!fs::Exists(Paths::RecordPath(catalog.root.path, second)));
}

MT_TEST(commit_publish_rebinds_an_alias_whose_record_is_gone) {
    Catalog catalog;
    const SnapshotId first = FreshId();
    Publish(catalog, first, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    // Remove only the record, leaving the alias dangling as a crash would.
    MT_EXPECT_TRUE(fs::RemoveFile(Paths::RecordPath(catalog.root.path, first)).ok());

    const SnapshotId second = FreshId();
    const snapshot::SnapshotRecord record =
        Publish(catalog, second, Optional<snapshot::SnapshotAlias>(Alias("prod")));
    MT_EXPECT_TRUE(record.committed.has_value());

    const repository::RepositoryResult<Optional<SnapshotId> > resolved =
        catalog.repo.ResolveAlias("prod");
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(resolved.value().has_value());
    MT_EXPECT_TRUE(*resolved.value() == second);
}

MT_TEST(abort_publish_removes_an_uncommitted_directory) {
    Catalog catalog;
    const SnapshotId id = FreshId();

    const repository::RepositoryResult<posixfs::PublishSession> session =
        catalog.store->BeginPublish(id);
    MT_EXPECT_TRUE(session.ok());
    MT_EXPECT_TRUE(fs::IsDir(Paths(catalog.root.path, id).SnapshotDir()));

    MT_EXPECT_TRUE(catalog.store->AbortPublish(session.value()).ok());
    MT_EXPECT_TRUE(!fs::Exists(Paths(catalog.root.path, id).SnapshotDir()));
}

MT_TEST(abort_publish_keeps_a_committed_directory) {
    Catalog catalog;
    const SnapshotId id = FreshId();
    Publish(catalog, id, Optional<snapshot::SnapshotAlias>());

    // A late abort on an already-committed snapshot must not delete live
    // artifacts.
    posixfs::PublishSession session;
    session.snapshot_id = id;
    MT_EXPECT_TRUE(catalog.store->AbortPublish(session).ok());
    MT_EXPECT_TRUE(fs::IsDir(Paths(catalog.root.path, id).SnapshotDir()));
}

MT_TEST(a_commit_marker_without_a_record_counts_as_uncommitted) {
    Catalog catalog;
    const SnapshotId id = FreshId();
    Publish(catalog, id, Optional<snapshot::SnapshotAlias>());

    // Simulate a crash between the marker and the record.
    MT_EXPECT_TRUE(fs::RemoveFile(Paths::RecordPath(catalog.root.path, id)).ok());

    posixfs::PublishSession session;
    session.snapshot_id = id;
    MT_EXPECT_TRUE(catalog.store->AbortPublish(session).ok());
    // The marker alone must not protect the directory.
    MT_EXPECT_TRUE(!fs::Exists(Paths(catalog.root.path, id).SnapshotDir()));
}

// ---------------------------------------------------------------------------
// create / get / list / delete, through the shared record catalog
// ---------------------------------------------------------------------------

MT_TEST(create_only_accepts_an_uncommitted_template) {
    Catalog catalog;

    snapshot::SnapshotRecord sandbox_record =
        Waiting(FreshId(), Optional<snapshot::SnapshotAlias>());
    sandbox_record.source = snapshot::SnapshotSource::Sandbox("sbx-1");
    const repository::RepositoryResult<snapshot::SnapshotRecord> rejected_source =
        catalog.repo.Create(sandbox_record);
    MT_EXPECT_TRUE(!rejected_source.ok());
    MT_EXPECT_TRUE(rejected_source.error().message.find("only template snapshots") !=
                   std::string::npos);

    snapshot::SnapshotRecord committed_record =
        Waiting(FreshId(), Optional<snapshot::SnapshotAlias>());
    committed_record.committed = Optional<snapshot::CommittedSnapshot>(MockCommitted());
    const repository::RepositoryResult<snapshot::SnapshotRecord> rejected_committed =
        catalog.repo.Create(committed_record);
    MT_EXPECT_TRUE(!rejected_committed.ok());
    MT_EXPECT_TRUE(rejected_committed.error().message.find("must not already be committed") !=
                   std::string::npos);
}

MT_TEST(create_rejects_a_duplicate_id) {
    Catalog catalog;
    const snapshot::SnapshotRecord record =
        Waiting(FreshId(), Optional<snapshot::SnapshotAlias>());

    MT_EXPECT_TRUE(catalog.repo.Create(record).ok());
    const repository::RepositoryResult<snapshot::SnapshotRecord> again =
        catalog.repo.Create(record);
    MT_EXPECT_TRUE(!again.ok());
    MT_EXPECT_TRUE(again.error().message.find("already exists") != std::string::npos);
}

MT_TEST(get_accepts_an_id_or_an_alias) {
    Catalog catalog;
    const SnapshotId id = FreshId();
    Publish(catalog, id, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    const repository::RepositoryResult<Optional<snapshot::SnapshotRecord> > by_id =
        catalog.repo.Get(id.ToString());
    MT_EXPECT_TRUE(by_id.ok());
    MT_EXPECT_TRUE(by_id.value().has_value());
    MT_EXPECT_TRUE(by_id.value()->id == id);

    const repository::RepositoryResult<Optional<snapshot::SnapshotRecord> > by_alias =
        catalog.repo.Get("prod");
    MT_EXPECT_TRUE(by_alias.ok());
    MT_EXPECT_TRUE(by_alias.value().has_value());
    MT_EXPECT_TRUE(by_alias.value()->id == id);
}

MT_TEST(get_drops_a_dangling_alias) {
    Catalog catalog;
    const SnapshotId id = FreshId();
    Publish(catalog, id, Optional<snapshot::SnapshotAlias>(Alias("prod")));
    MT_EXPECT_TRUE(fs::RemoveFile(Paths::RecordPath(catalog.root.path, id)).ok());

    const repository::RepositoryResult<Optional<snapshot::SnapshotRecord> > missing =
        catalog.repo.Get("prod");
    MT_EXPECT_TRUE(missing.ok());
    MT_EXPECT_TRUE(!missing.value().has_value());
    // Cleaned up on the way, so the name becomes reusable.
    MT_EXPECT_TRUE(!fs::Exists(Paths::AliasPath(catalog.root.path, Alias("prod"))));
}

MT_TEST(get_reports_an_unusable_lookup_as_invalid) {
    Catalog catalog;
    // Neither a uuid nor a legal alias.
    const repository::RepositoryResult<Optional<snapshot::SnapshotRecord> > result =
        catalog.repo.Get("not a valid alias!");
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().kind == repository::RepositoryErrorKind::InvalidRequest);
}

MT_TEST(a_record_being_deleted_is_hidden_from_every_read) {
    // The implementation that used to live in this backend ignored the
    // `deleting` marker entirely: it handed the record out, resolved its
    // alias and let a builder claim it while its files were being removed.
    Catalog catalog;
    const SnapshotId id = FreshId();
    snapshot::SnapshotRecord record =
        Waiting(id, Optional<snapshot::SnapshotAlias>(Alias("prod")));
    MT_EXPECT_TRUE(catalog.repo.Create(record).ok());

    record.deleting = true;
    MT_EXPECT_TRUE(catalog.repo
                       .PutDocument(repository::DocumentKey::SnapshotRecord(id),
                                    record.ToJson().ToStringPretty())
                       .ok());

    MT_EXPECT_TRUE(!catalog.repo.Get(id.ToString()).value().has_value());
    MT_EXPECT_TRUE(!catalog.repo.Get("prod").value().has_value());
    MT_EXPECT_TRUE(!catalog.repo.ResolveAlias("prod").value().has_value());
    MT_EXPECT_TRUE(catalog.repo.List(repository::SnapshotListFilter()).value().empty());
    MT_EXPECT_TRUE(catalog.repo.TryStartBuild(id).error().kind ==
                   repository::RepositoryErrorKind::SnapshotNotFound);
}

MT_TEST(list_orders_newest_first_with_an_id_tiebreak) {
    Catalog catalog;

    // Same creation time, so only the id tiebreak decides the order.
    for (int i = 0; i < 3; ++i) {
        snapshot::SnapshotRecord record =
            Waiting(FreshId(), Optional<snapshot::SnapshotAlias>());
        record.created_at_unix_ms = 500;
        record.updated_at_unix_ms = 500;
        MT_EXPECT_TRUE(catalog.repo.Create(record).ok());
    }
    snapshot::SnapshotRecord newest = Waiting(FreshId(), Optional<snapshot::SnapshotAlias>());
    newest.created_at_unix_ms = 900;
    MT_EXPECT_TRUE(catalog.repo.Create(newest).ok());

    const repository::RepositoryResult<std::vector<snapshot::SnapshotRecord> > records =
        catalog.repo.List(repository::SnapshotListFilter());
    MT_EXPECT_TRUE(records.ok());
    MT_EXPECT_EQ(records.value().size(), static_cast<std::size_t>(4));
    MT_EXPECT_TRUE(records.value()[0].id == newest.id);
    // Within one timestamp the ids ascend, so the order is total.
    for (std::size_t i = 2; i < records.value().size(); ++i) {
        MT_EXPECT_TRUE(records.value()[i - 1].id.ToString() <
                       records.value()[i].id.ToString());
    }
}

MT_TEST(list_skips_lock_files_in_the_records_directory) {
    Catalog catalog;
    const SnapshotId id = FreshId();
    // Every record write leaves a `<id>.lock` beside `<id>.json`.
    MT_EXPECT_TRUE(catalog.repo.Create(Waiting(id, Optional<snapshot::SnapshotAlias>())).ok());
    MT_EXPECT_TRUE(catalog.repo.TryStartBuild(id).ok());
    MT_EXPECT_TRUE(fs::IsFile(Paths::RecordLockPath(catalog.root.path, id)));

    // A lock file is not a record, so it must not be parsed as one.
    const repository::RepositoryResult<std::vector<snapshot::SnapshotRecord> > records =
        catalog.repo.List(repository::SnapshotListFilter());
    MT_EXPECT_TRUE(records.ok());
    MT_EXPECT_EQ(records.value().size(), static_cast<std::size_t>(1));
}

MT_TEST(delete_is_idempotent_and_removes_everything) {
    Catalog catalog;
    const SnapshotId id = FreshId();
    Publish(catalog, id, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    MT_EXPECT_TRUE(catalog.repo.Delete(id.ToString()).ok());
    MT_EXPECT_TRUE(!fs::Exists(Paths::RecordPath(catalog.root.path, id)));
    MT_EXPECT_TRUE(!fs::Exists(Paths::AliasPath(catalog.root.path, Alias("prod"))));
    // The snapshot directory, and with it the commit marker inside it.
    MT_EXPECT_TRUE(!fs::Exists(Paths(catalog.root.path, id).SnapshotDir()));

    // Deleting an absent record succeeds, which is what lets a delete that
    // was interrupted be retried to completion.
    MT_EXPECT_TRUE(catalog.repo.Delete(id.ToString()).ok());
}

MT_TEST(delete_succeeds_when_the_files_are_already_gone) {
    // The retry after a partial delete: the directory went in the first
    // attempt, so its absence is the expected state rather than a failure.
    // A record that was only pre-created never had a directory at all.
    Catalog catalog;
    const SnapshotId published = FreshId();
    Publish(catalog, published, Optional<snapshot::SnapshotAlias>());
    MT_EXPECT_TRUE(fs::RemoveDirAll(Paths(catalog.root.path, published).SnapshotDir()).ok());

    MT_EXPECT_TRUE(catalog.repo.Delete(published.ToString()).ok());
    MT_EXPECT_TRUE(!fs::Exists(Paths::RecordPath(catalog.root.path, published)));

    const SnapshotId waiting = FreshId();
    MT_EXPECT_TRUE(
        catalog.repo.Create(Waiting(waiting, Optional<snapshot::SnapshotAlias>())).ok());
    MT_EXPECT_TRUE(catalog.repo.Delete(waiting.ToString()).ok());
    MT_EXPECT_TRUE(!fs::Exists(Paths::RecordPath(catalog.root.path, waiting)));
}

MT_TEST(delete_resumes_from_a_record_already_marked) {
    // A delete that stopped after marking must still be finishable, and the
    // marked record must not block its own cleanup.
    Catalog catalog;
    const SnapshotId id = FreshId();
    Publish(catalog, id, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    snapshot::SnapshotRecord marked = *catalog.repo.Get(id.ToString()).value();
    marked.deleting = true;
    MT_EXPECT_TRUE(catalog.repo
                       .PutDocument(repository::DocumentKey::SnapshotRecord(id),
                                    marked.ToJson().ToStringPretty())
                       .ok());

    MT_EXPECT_TRUE(catalog.repo.Delete(id.ToString()).ok());
    MT_EXPECT_TRUE(!fs::Exists(Paths::RecordPath(catalog.root.path, id)));
    MT_EXPECT_TRUE(!fs::Exists(Paths::AliasPath(catalog.root.path, Alias("prod"))));
    MT_EXPECT_TRUE(!fs::Exists(Paths(catalog.root.path, id).SnapshotDir()));
}

MT_TEST(delete_keeps_a_shared_managed_layer) {
    // A managed layer is content-addressed and lives outside the snapshot's
    // directory precisely so several snapshots can share it. Deleting one of
    // them must not take the layer with it.
    Catalog catalog;
    const SnapshotId id = FreshId();
    Publish(catalog, id, Optional<snapshot::SnapshotAlias>());

    const std::string layer = Paths::ManagedLayerPath(catalog.root.path, "sha256:abc");
    MT_EXPECT_TRUE(agentenv::core::WriteAtomic(layer, "layer-bytes").ok());

    MT_EXPECT_TRUE(catalog.repo.Delete(id.ToString()).ok());
    MT_EXPECT_TRUE(!fs::Exists(Paths(catalog.root.path, id).SnapshotDir()));
    MT_EXPECT_TRUE(fs::IsFile(layer));
}

MT_TEST(delete_keeps_an_alias_rebound_to_another_snapshot) {
    Catalog catalog;
    const SnapshotId first = FreshId();
    Publish(catalog, first, Optional<snapshot::SnapshotAlias>(Alias("prod")));
    MT_EXPECT_TRUE(fs::RemoveFile(Paths::RecordPath(catalog.root.path, first)).ok());

    const SnapshotId second = FreshId();
    Publish(catalog, second, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    // Restore the first record directly, bypassing `Create`: it names the
    // same alias, which `Create` would (correctly) refuse now that the alias
    // belongs to `second`. What is under test is the delete path meeting a
    // record whose alias has since been rebound.
    const snapshot::SnapshotRecord stale =
        Waiting(first, Optional<snapshot::SnapshotAlias>(Alias("prod")));
    MT_EXPECT_TRUE(catalog.repo
                       .PutDocument(repository::DocumentKey::SnapshotRecord(first),
                                    stale.ToJson().ToStringPretty())
                       .ok());

    MT_EXPECT_TRUE(catalog.repo.Delete(first.ToString()).ok());
    // The alias now belongs to `second` and must survive.
    MT_EXPECT_TRUE(fs::Exists(Paths::AliasPath(catalog.root.path, Alias("prod"))));
    const repository::RepositoryResult<Optional<SnapshotId> > resolved =
        catalog.repo.ResolveAlias("prod");
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(*resolved.value() == second);
}

MT_TEST(delete_refuses_a_snapshot_with_a_published_disk_image) {
    // Rolling a publication back needs Rust's `AcrDiskImageExporter`, which
    // is not ported. Removing the snapshot anyway would leave the image live
    // in the registry with nothing left pointing at it, so this is refused --
    // and refused from inside the atomic update, so the record is not even
    // marked.
    Catalog catalog;
    const SnapshotId id = FreshId();
    Publish(catalog, id, Optional<snapshot::SnapshotAlias>());

    snapshot::SnapshotRecord record = *catalog.repo.Get(id.ToString()).value();
    snapshot::PersistedDiskImagePublication publication;
    publication.image_ref = "registry.example.com/env";
    publication.tag = "1";
    publication.manifest_digest = "sha256:" + std::string(64, 'a');
    publication.repo_blob_url = "https://registry.example.com/v2/env/blobs/";
    record.committed->disk_publications.push_back(publication);
    MT_EXPECT_TRUE(catalog.repo
                       .PutDocument(repository::DocumentKey::SnapshotRecord(id),
                                    record.ToJson().ToStringPretty())
                       .ok());

    const repository::RepositoryResult<Unit> refused = catalog.repo.Delete(id.ToString());
    MT_EXPECT_TRUE(!refused.ok());
    MT_EXPECT_TRUE(refused.error().kind == repository::RepositoryErrorKind::Unsupported);

    // Untouched: still visible, still has its files.
    MT_EXPECT_TRUE(catalog.repo.Get(id.ToString()).value().has_value());
    MT_EXPECT_TRUE(!catalog.repo.Get(id.ToString()).value()->deleting);
    MT_EXPECT_TRUE(fs::Exists(Paths(catalog.root.path, id).SnapshotDir()));
}

MT_TEST(resolve_alias_drops_a_stale_binding) {
    Catalog catalog;
    const SnapshotId id = FreshId();
    Publish(catalog, id, Optional<snapshot::SnapshotAlias>(Alias("prod")));
    MT_EXPECT_TRUE(fs::RemoveFile(Paths::RecordPath(catalog.root.path, id)).ok());

    const repository::RepositoryResult<Optional<SnapshotId> > resolved =
        catalog.repo.ResolveAlias("prod");
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(!resolved.value().has_value());
    MT_EXPECT_TRUE(!fs::Exists(Paths::AliasPath(catalog.root.path, Alias("prod"))));

    // An unknown alias is simply absent, not an error.
    MT_EXPECT_TRUE(catalog.repo.ResolveAlias("nope").ok());
}

// ---------------------------------------------------------------------------
// template build state machine
// ---------------------------------------------------------------------------

MT_TEST(try_start_moves_waiting_to_building_exactly_once) {
    Catalog catalog;
    const SnapshotId id = FreshId();
    MT_EXPECT_TRUE(catalog.repo.Create(Waiting(id, Optional<snapshot::SnapshotAlias>())).ok());

    const repository::RepositoryResult<snapshot::SnapshotRecord> started =
        catalog.repo.TryStartBuild(id);
    MT_EXPECT_TRUE(started.ok());
    MT_EXPECT_TRUE(started.value().source.build.status ==
                   snapshot::TemplateBuildStatus::Building);
    MT_EXPECT_TRUE(started.value().source.build.started_at_unix_ms.has_value());

    // The second claim must fail, which is what stops two builders from
    // running the same template.
    const repository::RepositoryResult<snapshot::SnapshotRecord> again =
        catalog.repo.TryStartBuild(id);
    MT_EXPECT_TRUE(!again.ok());
    MT_EXPECT_TRUE(again.error().message.find("not in waiting state") != std::string::npos);
}

MT_TEST(try_start_rejects_a_missing_or_non_template_record) {
    Catalog catalog;

    const repository::RepositoryResult<snapshot::SnapshotRecord> missing =
        catalog.repo.TryStartBuild(FreshId());
    MT_EXPECT_TRUE(!missing.ok());
    MT_EXPECT_TRUE(missing.error().kind == repository::RepositoryErrorKind::SnapshotNotFound);

    const SnapshotId id = FreshId();
    Publish(catalog, id, Optional<snapshot::SnapshotAlias>());
    snapshot::SnapshotRecord record = *catalog.repo.Get(id.ToString()).value();
    record.source = snapshot::SnapshotSource::Sandbox("sbx-1");
    MT_EXPECT_TRUE(catalog.repo
                       .PutDocument(repository::DocumentKey::SnapshotRecord(id),
                                    record.ToJson().ToStringPretty())
                       .ok());

    const repository::RepositoryResult<snapshot::SnapshotRecord> not_template =
        catalog.repo.TryStartBuild(id);
    MT_EXPECT_TRUE(!not_template.ok());
    MT_EXPECT_TRUE(not_template.error().message.find("is not a template build") !=
                   std::string::npos);
}

MT_TEST(mark_error_records_the_reason) {
    Catalog catalog;
    const SnapshotId id = FreshId();
    MT_EXPECT_TRUE(catalog.repo.Create(Waiting(id, Optional<snapshot::SnapshotAlias>())).ok());
    MT_EXPECT_TRUE(catalog.repo.TryStartBuild(id).ok());

    MT_EXPECT_TRUE(catalog.repo
                       .MarkBuildError(
                           id, snapshot::TemplateBuildErrorReason::WithStep("boom", "step-2"))
                       .ok());

    const snapshot::SnapshotRecord record = *catalog.repo.Get(id.ToString()).value();
    MT_EXPECT_TRUE(record.source.build.status == snapshot::TemplateBuildStatus::Error);
    MT_EXPECT_EQ(record.source.build.error_reason->message, std::string("boom"));
    MT_EXPECT_EQ(*record.source.build.error_reason->step, std::string("step-2"));
    MT_EXPECT_TRUE(record.source.build.finished_at_unix_ms.has_value());
}

// ---------------------------------------------------------------------------
// list filters
//
// The rules live on `SnapshotListFilter` itself; this backend used to carry a
// `MatchesRecordFilter` copy of them.
// ---------------------------------------------------------------------------

MT_TEST(filter_matches_on_alias_prefix) {
    snapshot::SnapshotRecord record =
        Waiting(FreshId(), Optional<snapshot::SnapshotAlias>(Alias("prod-api")));

    repository::SnapshotListFilter filter;
    filter.alias_prefix = Optional<std::string>(std::string("prod"));
    MT_EXPECT_TRUE(filter.Matches(record));

    filter.alias_prefix = Optional<std::string>(std::string("dev"));
    MT_EXPECT_TRUE(!filter.Matches(record));

    // A record with no alias cannot match a prefix filter.
    record.alias = Optional<snapshot::SnapshotAlias>();
    filter.alias_prefix = Optional<std::string>(std::string("prod"));
    MT_EXPECT_TRUE(!filter.Matches(record));
}

MT_TEST(filter_matches_on_id_set_and_id_or_alias) {
    const SnapshotId id = FreshId();
    const snapshot::SnapshotRecord record =
        Waiting(id, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    repository::SnapshotListFilter by_ids;
    by_ids.snapshot_ids =
        Optional<std::vector<SnapshotId> >(std::vector<SnapshotId>(1, id));
    MT_EXPECT_TRUE(by_ids.Matches(record));

    by_ids.snapshot_ids =
        Optional<std::vector<SnapshotId> >(std::vector<SnapshotId>(1, FreshId()));
    MT_EXPECT_TRUE(!by_ids.Matches(record));

    // `snapshot_id_or_alias` matches either spelling.
    repository::SnapshotListFilter by_either;
    by_either.snapshot_id_or_alias = Optional<std::string>(id.ToString());
    MT_EXPECT_TRUE(by_either.Matches(record));
    by_either.snapshot_id_or_alias = Optional<std::string>(std::string("prod"));
    MT_EXPECT_TRUE(by_either.Matches(record));
    by_either.snapshot_id_or_alias = Optional<std::string>(std::string("other"));
    MT_EXPECT_TRUE(!by_either.Matches(record));
}

MT_TEST(filter_matches_on_source_and_status) {
    snapshot::SnapshotRecord template_record =
        Waiting(FreshId(), Optional<snapshot::SnapshotAlias>());
    snapshot::SnapshotRecord sandbox_record = template_record;
    sandbox_record.source = snapshot::SnapshotSource::Sandbox("sbx-1");

    repository::SnapshotListFilter by_sandbox;
    by_sandbox.source_sandbox_id = Optional<std::string>(std::string("sbx-1"));
    MT_EXPECT_TRUE(by_sandbox.Matches(sandbox_record));
    // A template never matches a source-sandbox filter.
    MT_EXPECT_TRUE(!by_sandbox.Matches(template_record));

    repository::SnapshotListFilter by_kind;
    by_kind.sources = Optional<std::vector<snapshot::SnapshotSourceKind> >(
        std::vector<snapshot::SnapshotSourceKind>(1,
                                                  snapshot::SnapshotSourceKind::Template));
    MT_EXPECT_TRUE(by_kind.Matches(template_record));
    MT_EXPECT_TRUE(!by_kind.Matches(sandbox_record));

    repository::SnapshotListFilter by_status;
    by_status.template_statuses = Optional<std::vector<snapshot::TemplateBuildStatus> >(
        std::vector<snapshot::TemplateBuildStatus>(1,
                                                   snapshot::TemplateBuildStatus::Waiting));
    MT_EXPECT_TRUE(by_status.Matches(template_record));
    // A status filter implies a template: a sandbox record has no build.
    MT_EXPECT_TRUE(!by_status.Matches(sandbox_record));
}

MT_TEST(filter_conjuncts_are_combined) {
    const SnapshotId id = FreshId();
    const snapshot::SnapshotRecord record =
        Waiting(id, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    repository::SnapshotListFilter filter;
    filter.alias_prefix = Optional<std::string>(std::string("prod"));
    filter.template_statuses = Optional<std::vector<snapshot::TemplateBuildStatus> >(
        std::vector<snapshot::TemplateBuildStatus>(1,
                                                   snapshot::TemplateBuildStatus::Waiting));
    MT_EXPECT_TRUE(filter.Matches(record));

    // One failing conjunct rejects the record.
    filter.template_statuses = Optional<std::vector<snapshot::TemplateBuildStatus> >(
        std::vector<snapshot::TemplateBuildStatus>(1, snapshot::TemplateBuildStatus::Ready));
    MT_EXPECT_TRUE(!filter.Matches(record));
}

// ---------------------------------------------------------------------------
// build cache
// ---------------------------------------------------------------------------

MT_TEST(build_cache_head_starts_empty_and_advances) {
    Catalog catalog;

    const repository::RepositoryResult<posixfs::BuildCacheState> initial =
        catalog.store->GetBuildCacheState();
    MT_EXPECT_TRUE(initial.ok());
    MT_EXPECT_TRUE(!initial.value().current.has_value());

    const repository::RepositoryResult<Optional<std::string> > first =
        catalog.store->ReplaceBuildCacheHead("vol_a");
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(!first.value().has_value());

    const repository::RepositoryResult<Optional<std::string> > second =
        catalog.store->ReplaceBuildCacheHead("vol_b");
    MT_EXPECT_TRUE(second.ok());
    MT_EXPECT_EQ(*second.value(), std::string("vol_a"));

    const posixfs::BuildCacheState state = catalog.store->GetBuildCacheState().value();
    MT_EXPECT_EQ(*state.current, std::string("vol_b"));
    // The displaced seed is retired rather than forgotten, so its storage
    // can be reclaimed deliberately.
    MT_EXPECT_EQ(state.retired.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(state.retired.count("vol_a") > 0);
}

MT_TEST(build_cache_refuses_to_republish_a_retired_seed) {
    Catalog catalog;
    MT_EXPECT_TRUE(catalog.store->ReplaceBuildCacheHead("vol_a").ok());
    MT_EXPECT_TRUE(catalog.store->ReplaceBuildCacheHead("vol_b").ok());

    const repository::RepositoryResult<Optional<std::string> > resurrect =
        catalog.store->ReplaceBuildCacheHead("vol_a");
    MT_EXPECT_TRUE(!resurrect.ok());
    MT_EXPECT_TRUE(resurrect.error().message.find("retired build cache seed") !=
                   std::string::npos);

    // Forgetting it first makes the id reusable.
    MT_EXPECT_TRUE(catalog.store->ForgetRetiredBuildCache("vol_a").ok());
    MT_EXPECT_TRUE(catalog.store->ReplaceBuildCacheHead("vol_a").ok());
}

MT_TEST(build_cache_replacing_the_same_seed_does_not_retire_it) {
    Catalog catalog;
    MT_EXPECT_TRUE(catalog.store->ReplaceBuildCacheHead("vol_a").ok());
    MT_EXPECT_TRUE(catalog.store->ReplaceBuildCacheHead("vol_a").ok());

    const posixfs::BuildCacheState state = catalog.store->GetBuildCacheState().value();
    MT_EXPECT_EQ(*state.current, std::string("vol_a"));
    // Retiring the current seed would make the next publish fail.
    MT_EXPECT_TRUE(state.retired.empty());
}

MT_TEST(build_cache_state_validates_ids_on_decode) {
    // Rust: `validates_cache_volume_ids`.
    MT_EXPECT_EQ(
        *posixfs::BuildCacheState::Decode("{\"current\":\"vol_seed\",\"retired\":[]}")
             .value()
             .current,
        std::string("vol_seed"));

    const char* const rejected[] = {
        "{\"current\":\"../invalid\",\"retired\":[]}",
        "{\"current\":\"vol_a\",\"retired\":[\"../invalid\"]}",
        // A seed cannot be both current and retired.
        "{\"current\":\"vol_a\",\"retired\":[\"vol_a\"]}",
    };
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        MT_EXPECT_TRUE(!posixfs::BuildCacheState::Decode(rejected[i]).ok());
    }
}

int main() { return microtest::RunAll(); }
