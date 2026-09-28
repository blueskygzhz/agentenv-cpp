// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/catalog.rs test module.
#include <string>
#include <vector>

#include "agentenv/core/file_lock.h"
#include "agentenv/core/fs.h"
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
namespace volume = agentenv::volume;

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
snapshot::SnapshotRecord Publish(Store& store, const SnapshotId& id,
                                 const Optional<snapshot::SnapshotAlias>& alias) {
    const repository::RepositoryResult<posixfs::PublishSession> session =
        store.BeginPublish(id);
    MT_EXPECT_TRUE(session.ok());
    const repository::RepositoryResult<snapshot::SnapshotRecord> record =
        store.CommitPublish(session.value(), Metadata(id, alias), MockCommitted());
    MT_EXPECT_TRUE(record.ok());
    return record.value();
}

volume::VolumeRecord Volume(const std::string& id, const std::string& name) {
    volume::VolumeRecord record;
    record.id = id;
    record.name = name;
    record.size_mb = 64;
    return record;
}

}  // namespace

// ---------------------------------------------------------------------------
// publish lifecycle
// ---------------------------------------------------------------------------

MT_TEST(begin_publish_creates_the_layout_and_snapshot_dir) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId id = FreshId();

    MT_EXPECT_TRUE(store.BeginPublish(id).ok());

    MT_EXPECT_TRUE(fs::IsDir(Paths::RecordsDir(root.path)));
    MT_EXPECT_TRUE(fs::IsDir(Paths::AliasesDir(root.path)));
    MT_EXPECT_TRUE(fs::IsDir(Paths::VolumeRecordsDir(root.path)));
    MT_EXPECT_TRUE(fs::IsDir(Paths::VolumeAliasesDir(root.path)));
    MT_EXPECT_TRUE(fs::IsDir(Paths(root.path, id).SnapshotDir()));
}

MT_TEST(commit_publish_writes_the_marker_the_record_and_the_alias) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId id = FreshId();

    const snapshot::SnapshotRecord record =
        Publish(store, id, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    MT_EXPECT_TRUE(record.committed.has_value());
    MT_EXPECT_TRUE(record.source.is_template());
    // A snapshot published without a prior build record is Ready with no
    // start time: it was never queued.
    MT_EXPECT_TRUE(record.source.build.status == snapshot::TemplateBuildStatus::Ready);
    MT_EXPECT_TRUE(!record.source.build.started_at_unix_ms.has_value());
    MT_EXPECT_TRUE(record.source.build.finished_at_unix_ms.has_value());

    MT_EXPECT_TRUE(fs::IsFile(Paths(root.path, id).Path("commit")));
    MT_EXPECT_TRUE(fs::IsFile(Paths::RecordPath(root.path, id)));
    MT_EXPECT_TRUE(fs::IsFile(Paths::AliasPath(root.path, Alias("prod"))));
}

MT_TEST(commit_publish_advances_a_pre_created_template_record) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId id = FreshId();

    snapshot::SnapshotRecord waiting = snapshot::SnapshotRecord::TemplateWaiting(
        id, Optional<snapshot::SnapshotAlias>(), sandbox::SandboxResources());
    waiting.created_at_unix_ms = 1000;
    MT_EXPECT_TRUE(store.Create(waiting).ok());

    const snapshot::SnapshotRecord committed =
        Publish(store, id, Optional<snapshot::SnapshotAlias>());

    // The original creation time survives, so the record's history is not
    // rewritten by the publish.
    MT_EXPECT_EQ(committed.created_at_unix_ms, static_cast<int64_t>(1000));
    MT_EXPECT_TRUE(committed.source.build.status == snapshot::TemplateBuildStatus::Ready);
    MT_EXPECT_TRUE(committed.committed.has_value());
}

MT_TEST(commit_publish_rejects_an_alias_held_by_a_live_snapshot) {
    TempRoot root;
    Store store(root.path);
    Publish(store, FreshId(), Optional<snapshot::SnapshotAlias>(Alias("prod")));

    const SnapshotId second = FreshId();
    const repository::RepositoryResult<posixfs::PublishSession> session =
        store.BeginPublish(second);
    MT_EXPECT_TRUE(session.ok());
    const repository::RepositoryResult<snapshot::SnapshotRecord> conflicted =
        store.CommitPublish(session.value(),
                            Metadata(second, Optional<snapshot::SnapshotAlias>(Alias("prod"))),
                            MockCommitted());

    MT_EXPECT_TRUE(!conflicted.ok());
    MT_EXPECT_TRUE(conflicted.error().kind == repository::RepositoryErrorKind::AliasConflict);

    // Rollback: the failed publish must not leave its directory behind.
    MT_EXPECT_TRUE(!fs::Exists(Paths(root.path, second).SnapshotDir()));
    MT_EXPECT_TRUE(!fs::Exists(Paths::RecordPath(root.path, second)));
}

MT_TEST(commit_publish_rebinds_an_alias_whose_record_is_gone) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId first = FreshId();
    Publish(store, first, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    // Remove only the record, leaving the alias dangling as a crash would.
    MT_EXPECT_TRUE(fs::RemoveFile(Paths::RecordPath(root.path, first)).ok());

    const SnapshotId second = FreshId();
    const snapshot::SnapshotRecord record =
        Publish(store, second, Optional<snapshot::SnapshotAlias>(Alias("prod")));
    MT_EXPECT_TRUE(record.committed.has_value());

    const repository::RepositoryResult<Optional<SnapshotId> > resolved =
        store.ResolveAlias("prod");
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(resolved.value().has_value());
    MT_EXPECT_TRUE(*resolved.value() == second);
}

MT_TEST(abort_publish_removes_an_uncommitted_directory) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId id = FreshId();

    const repository::RepositoryResult<posixfs::PublishSession> session =
        store.BeginPublish(id);
    MT_EXPECT_TRUE(session.ok());
    MT_EXPECT_TRUE(fs::IsDir(Paths(root.path, id).SnapshotDir()));

    MT_EXPECT_TRUE(store.AbortPublish(session.value()).ok());
    MT_EXPECT_TRUE(!fs::Exists(Paths(root.path, id).SnapshotDir()));
}

MT_TEST(abort_publish_keeps_a_committed_directory) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId id = FreshId();
    Publish(store, id, Optional<snapshot::SnapshotAlias>());

    // A late abort on an already-committed snapshot must not delete live
    // artifacts.
    posixfs::PublishSession session;
    session.snapshot_id = id;
    MT_EXPECT_TRUE(store.AbortPublish(session).ok());
    MT_EXPECT_TRUE(fs::IsDir(Paths(root.path, id).SnapshotDir()));
}

MT_TEST(a_commit_marker_without_a_record_counts_as_uncommitted) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId id = FreshId();
    Publish(store, id, Optional<snapshot::SnapshotAlias>());

    // Simulate a crash between the marker and the record.
    MT_EXPECT_TRUE(fs::RemoveFile(Paths::RecordPath(root.path, id)).ok());

    posixfs::PublishSession session;
    session.snapshot_id = id;
    MT_EXPECT_TRUE(store.AbortPublish(session).ok());
    // The marker alone must not protect the directory.
    MT_EXPECT_TRUE(!fs::Exists(Paths(root.path, id).SnapshotDir()));
}

// ---------------------------------------------------------------------------
// create / get / list / delete
// ---------------------------------------------------------------------------

MT_TEST(create_only_accepts_an_uncommitted_template) {
    TempRoot root;
    Store store(root.path);

    snapshot::SnapshotRecord sandbox_record = snapshot::SnapshotRecord::TemplateWaiting(
        FreshId(), Optional<snapshot::SnapshotAlias>(), sandbox::SandboxResources());
    sandbox_record.source = snapshot::SnapshotSource::Sandbox("sbx-1");
    const repository::RepositoryResult<snapshot::SnapshotRecord> rejected_source =
        store.Create(sandbox_record);
    MT_EXPECT_TRUE(!rejected_source.ok());
    MT_EXPECT_TRUE(rejected_source.error().message.find("only template snapshots") !=
                   std::string::npos);

    snapshot::SnapshotRecord committed_record = snapshot::SnapshotRecord::TemplateWaiting(
        FreshId(), Optional<snapshot::SnapshotAlias>(), sandbox::SandboxResources());
    committed_record.committed =
        Optional<snapshot::CommittedSnapshot>(MockCommitted());
    const repository::RepositoryResult<snapshot::SnapshotRecord> rejected_committed =
        store.Create(committed_record);
    MT_EXPECT_TRUE(!rejected_committed.ok());
    MT_EXPECT_TRUE(rejected_committed.error().message.find("must not already be committed") !=
                   std::string::npos);
}

MT_TEST(create_rejects_a_duplicate_id) {
    TempRoot root;
    Store store(root.path);
    const snapshot::SnapshotRecord record = snapshot::SnapshotRecord::TemplateWaiting(
        FreshId(), Optional<snapshot::SnapshotAlias>(), sandbox::SandboxResources());

    MT_EXPECT_TRUE(store.Create(record).ok());
    const repository::RepositoryResult<snapshot::SnapshotRecord> again =
        store.Create(record);
    MT_EXPECT_TRUE(!again.ok());
    MT_EXPECT_TRUE(again.error().message.find("already exists") != std::string::npos);
}

MT_TEST(get_accepts_an_id_or_an_alias) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId id = FreshId();
    Publish(store, id, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    const repository::RepositoryResult<Optional<snapshot::SnapshotRecord> > by_id =
        store.Get(id.ToString());
    MT_EXPECT_TRUE(by_id.ok());
    MT_EXPECT_TRUE(by_id.value().has_value());
    MT_EXPECT_TRUE(by_id.value()->id == id);

    const repository::RepositoryResult<Optional<snapshot::SnapshotRecord> > by_alias =
        store.Get("prod");
    MT_EXPECT_TRUE(by_alias.ok());
    MT_EXPECT_TRUE(by_alias.value().has_value());
    MT_EXPECT_TRUE(by_alias.value()->id == id);
}

MT_TEST(get_drops_a_dangling_alias) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId id = FreshId();
    Publish(store, id, Optional<snapshot::SnapshotAlias>(Alias("prod")));
    MT_EXPECT_TRUE(fs::RemoveFile(Paths::RecordPath(root.path, id)).ok());

    const repository::RepositoryResult<Optional<snapshot::SnapshotRecord> > missing =
        store.Get("prod");
    MT_EXPECT_TRUE(missing.ok());
    MT_EXPECT_TRUE(!missing.value().has_value());
    // Cleaned up on the way, so the name becomes reusable.
    MT_EXPECT_TRUE(!fs::Exists(Paths::AliasPath(root.path, Alias("prod"))));
}

MT_TEST(get_reports_an_unusable_lookup_as_invalid) {
    TempRoot root;
    Store store(root.path);
    // Neither a uuid nor a legal alias.
    const repository::RepositoryResult<Optional<snapshot::SnapshotRecord> > result =
        store.Get("not a valid alias!");
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().kind == repository::RepositoryErrorKind::InvalidRequest);
}

MT_TEST(list_orders_newest_first_with_an_id_tiebreak) {
    TempRoot root;
    Store store(root.path);

    // Same creation time, so only the id tiebreak decides the order.
    for (int i = 0; i < 3; ++i) {
        snapshot::SnapshotRecord record = snapshot::SnapshotRecord::TemplateWaiting(
            FreshId(), Optional<snapshot::SnapshotAlias>(), sandbox::SandboxResources());
        record.created_at_unix_ms = 500;
        record.updated_at_unix_ms = 500;
        MT_EXPECT_TRUE(store.Create(record).ok());
    }
    snapshot::SnapshotRecord newest = snapshot::SnapshotRecord::TemplateWaiting(
        FreshId(), Optional<snapshot::SnapshotAlias>(), sandbox::SandboxResources());
    newest.created_at_unix_ms = 900;
    MT_EXPECT_TRUE(store.Create(newest).ok());

    const repository::RepositoryResult<std::vector<snapshot::SnapshotRecord> > records =
        store.List(posixfs::SnapshotListFilter());
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
    TempRoot root;
    Store store(root.path);
    const SnapshotId id = FreshId();
    // TryStart leaves a `<id>.lock` beside `<id>.json`.
    MT_EXPECT_TRUE(store.Create(snapshot::SnapshotRecord::TemplateWaiting(
                                   id, Optional<snapshot::SnapshotAlias>(),
                                   sandbox::SandboxResources()))
                       .ok());
    MT_EXPECT_TRUE(store.TryStart(id).ok());
    MT_EXPECT_TRUE(fs::IsFile(Paths::RecordLockPath(root.path, id)));

    // A lock file is not a record, so it must not be parsed as one.
    const repository::RepositoryResult<std::vector<snapshot::SnapshotRecord> > records =
        store.List(posixfs::SnapshotListFilter());
    MT_EXPECT_TRUE(records.ok());
    MT_EXPECT_EQ(records.value().size(), static_cast<std::size_t>(1));
}

MT_TEST(delete_record_is_idempotent_and_removes_everything) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId id = FreshId();
    Publish(store, id, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    MT_EXPECT_TRUE(store.DeleteRecord(id).ok());
    MT_EXPECT_TRUE(!fs::Exists(Paths::RecordPath(root.path, id)));
    MT_EXPECT_TRUE(!fs::Exists(Paths::AliasPath(root.path, Alias("prod"))));
    MT_EXPECT_TRUE(!fs::Exists(Paths(root.path, id).SnapshotDir()));

    // Deleting an absent record succeeds.
    MT_EXPECT_TRUE(store.DeleteRecord(id).ok());
}

MT_TEST(delete_record_keeps_an_alias_rebound_to_another_snapshot) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId first = FreshId();
    Publish(store, first, Optional<snapshot::SnapshotAlias>(Alias("prod")));
    MT_EXPECT_TRUE(fs::RemoveFile(Paths::RecordPath(root.path, first)).ok());

    const SnapshotId second = FreshId();
    Publish(store, second, Optional<snapshot::SnapshotAlias>(Alias("prod")));

    // Restore the first record directly, bypassing `Create`: it names the
    // same alias, which `Create` would (correctly) refuse now that the alias
    // belongs to `second`. What is under test is the delete path meeting a
    // record whose alias has since been rebound.
    const snapshot::SnapshotRecord stale = snapshot::SnapshotRecord::TemplateWaiting(
        first, Optional<snapshot::SnapshotAlias>(Alias("prod")),
        sandbox::SandboxResources());
    MT_EXPECT_TRUE(agentenv::core::WriteAtomic(Paths::RecordPath(root.path, first),
                                               stale.ToJson().ToString())
                       .ok());

    MT_EXPECT_TRUE(store.DeleteRecord(first).ok());
    // The alias now belongs to `second` and must survive.
    MT_EXPECT_TRUE(fs::Exists(Paths::AliasPath(root.path, Alias("prod"))));
    const repository::RepositoryResult<Optional<SnapshotId> > resolved =
        store.ResolveAlias("prod");
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(*resolved.value() == second);
}

MT_TEST(resolve_alias_drops_a_stale_binding) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId id = FreshId();
    Publish(store, id, Optional<snapshot::SnapshotAlias>(Alias("prod")));
    MT_EXPECT_TRUE(fs::RemoveFile(Paths::RecordPath(root.path, id)).ok());

    const repository::RepositoryResult<Optional<SnapshotId> > resolved =
        store.ResolveAlias("prod");
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(!resolved.value().has_value());
    MT_EXPECT_TRUE(!fs::Exists(Paths::AliasPath(root.path, Alias("prod"))));

    // An unknown alias is simply absent, not an error.
    MT_EXPECT_TRUE(store.ResolveAlias("nope").ok());
}

// ---------------------------------------------------------------------------
// template build state machine
// ---------------------------------------------------------------------------

MT_TEST(try_start_moves_waiting_to_building_exactly_once) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId id = FreshId();
    MT_EXPECT_TRUE(store.Create(snapshot::SnapshotRecord::TemplateWaiting(
                                   id, Optional<snapshot::SnapshotAlias>(),
                                   sandbox::SandboxResources()))
                       .ok());

    const repository::RepositoryResult<snapshot::SnapshotRecord> started =
        store.TryStart(id);
    MT_EXPECT_TRUE(started.ok());
    MT_EXPECT_TRUE(started.value().source.build.status ==
                   snapshot::TemplateBuildStatus::Building);
    MT_EXPECT_TRUE(started.value().source.build.started_at_unix_ms.has_value());

    // The second claim must fail, which is what stops two builders from
    // running the same template.
    const repository::RepositoryResult<snapshot::SnapshotRecord> again =
        store.TryStart(id);
    MT_EXPECT_TRUE(!again.ok());
    MT_EXPECT_TRUE(again.error().message.find("not in waiting state") != std::string::npos);
}

MT_TEST(try_start_rejects_a_missing_or_non_template_record) {
    TempRoot root;
    Store store(root.path);

    const repository::RepositoryResult<snapshot::SnapshotRecord> missing =
        store.TryStart(FreshId());
    MT_EXPECT_TRUE(!missing.ok());
    MT_EXPECT_TRUE(missing.error().kind == repository::RepositoryErrorKind::SnapshotNotFound);

    const SnapshotId id = FreshId();
    Publish(store, id, Optional<snapshot::SnapshotAlias>());
    snapshot::SnapshotRecord record = *store.Get(id.ToString()).value();
    record.source = snapshot::SnapshotSource::Sandbox("sbx-1");
    // Write it back through a fresh publish-free path.
    MT_EXPECT_TRUE(agentenv::core::WriteAtomic(Paths::RecordPath(root.path, id),
                                               record.ToJson().ToString())
                       .ok());

    const repository::RepositoryResult<snapshot::SnapshotRecord> not_template =
        store.TryStart(id);
    MT_EXPECT_TRUE(!not_template.ok());
    MT_EXPECT_TRUE(not_template.error().message.find("is not a template build") !=
                   std::string::npos);
}

MT_TEST(mark_error_records_the_reason) {
    TempRoot root;
    Store store(root.path);
    const SnapshotId id = FreshId();
    MT_EXPECT_TRUE(store.Create(snapshot::SnapshotRecord::TemplateWaiting(
                                   id, Optional<snapshot::SnapshotAlias>(),
                                   sandbox::SandboxResources()))
                       .ok());
    MT_EXPECT_TRUE(store.TryStart(id).ok());

    MT_EXPECT_TRUE(
        store.MarkError(id, snapshot::TemplateBuildErrorReason::WithStep("boom", "step-2"))
            .ok());

    const snapshot::SnapshotRecord record = *store.Get(id.ToString()).value();
    MT_EXPECT_TRUE(record.source.build.status == snapshot::TemplateBuildStatus::Error);
    MT_EXPECT_EQ(record.source.build.error_reason->message, std::string("boom"));
    MT_EXPECT_EQ(*record.source.build.error_reason->step, std::string("step-2"));
    MT_EXPECT_TRUE(record.source.build.finished_at_unix_ms.has_value());
}

// ---------------------------------------------------------------------------
// list filters
// ---------------------------------------------------------------------------

MT_TEST(filter_matches_on_alias_prefix) {
    snapshot::SnapshotRecord record = snapshot::SnapshotRecord::TemplateWaiting(
        FreshId(), Optional<snapshot::SnapshotAlias>(Alias("prod-api")),
        sandbox::SandboxResources());

    posixfs::SnapshotListFilter filter;
    filter.alias_prefix = Optional<std::string>(std::string("prod"));
    MT_EXPECT_TRUE(Store::MatchesRecordFilter(record, filter));

    filter.alias_prefix = Optional<std::string>(std::string("dev"));
    MT_EXPECT_TRUE(!Store::MatchesRecordFilter(record, filter));

    // A record with no alias cannot match a prefix filter.
    record.alias = Optional<snapshot::SnapshotAlias>();
    filter.alias_prefix = Optional<std::string>(std::string("prod"));
    MT_EXPECT_TRUE(!Store::MatchesRecordFilter(record, filter));
}

MT_TEST(filter_matches_on_id_set_and_id_or_alias) {
    const SnapshotId id = FreshId();
    const snapshot::SnapshotRecord record = snapshot::SnapshotRecord::TemplateWaiting(
        id, Optional<snapshot::SnapshotAlias>(Alias("prod")), sandbox::SandboxResources());

    posixfs::SnapshotListFilter by_ids;
    by_ids.snapshot_ids =
        Optional<std::vector<SnapshotId> >(std::vector<SnapshotId>(1, id));
    MT_EXPECT_TRUE(Store::MatchesRecordFilter(record, by_ids));

    by_ids.snapshot_ids =
        Optional<std::vector<SnapshotId> >(std::vector<SnapshotId>(1, FreshId()));
    MT_EXPECT_TRUE(!Store::MatchesRecordFilter(record, by_ids));

    // `snapshot_id_or_alias` matches either spelling.
    posixfs::SnapshotListFilter by_either;
    by_either.snapshot_id_or_alias = Optional<std::string>(id.ToString());
    MT_EXPECT_TRUE(Store::MatchesRecordFilter(record, by_either));
    by_either.snapshot_id_or_alias = Optional<std::string>(std::string("prod"));
    MT_EXPECT_TRUE(Store::MatchesRecordFilter(record, by_either));
    by_either.snapshot_id_or_alias = Optional<std::string>(std::string("other"));
    MT_EXPECT_TRUE(!Store::MatchesRecordFilter(record, by_either));
}

MT_TEST(filter_matches_on_source_and_status) {
    snapshot::SnapshotRecord template_record = snapshot::SnapshotRecord::TemplateWaiting(
        FreshId(), Optional<snapshot::SnapshotAlias>(), sandbox::SandboxResources());
    snapshot::SnapshotRecord sandbox_record = template_record;
    sandbox_record.source = snapshot::SnapshotSource::Sandbox("sbx-1");

    posixfs::SnapshotListFilter by_sandbox;
    by_sandbox.source_sandbox_id = Optional<std::string>(std::string("sbx-1"));
    MT_EXPECT_TRUE(Store::MatchesRecordFilter(sandbox_record, by_sandbox));
    // A template never matches a source-sandbox filter.
    MT_EXPECT_TRUE(!Store::MatchesRecordFilter(template_record, by_sandbox));

    posixfs::SnapshotListFilter by_kind;
    by_kind.sources = Optional<std::vector<snapshot::SnapshotSourceKind> >(
        std::vector<snapshot::SnapshotSourceKind>(1,
                                                  snapshot::SnapshotSourceKind::Template));
    MT_EXPECT_TRUE(Store::MatchesRecordFilter(template_record, by_kind));
    MT_EXPECT_TRUE(!Store::MatchesRecordFilter(sandbox_record, by_kind));

    posixfs::SnapshotListFilter by_status;
    by_status.template_statuses = Optional<std::vector<snapshot::TemplateBuildStatus> >(
        std::vector<snapshot::TemplateBuildStatus>(1,
                                                   snapshot::TemplateBuildStatus::Waiting));
    MT_EXPECT_TRUE(Store::MatchesRecordFilter(template_record, by_status));
    // A status filter implies a template: a sandbox record has no build.
    MT_EXPECT_TRUE(!Store::MatchesRecordFilter(sandbox_record, by_status));
}

MT_TEST(filter_conjuncts_are_combined) {
    const SnapshotId id = FreshId();
    const snapshot::SnapshotRecord record = snapshot::SnapshotRecord::TemplateWaiting(
        id, Optional<snapshot::SnapshotAlias>(Alias("prod")), sandbox::SandboxResources());

    posixfs::SnapshotListFilter filter;
    filter.alias_prefix = Optional<std::string>(std::string("prod"));
    filter.template_statuses = Optional<std::vector<snapshot::TemplateBuildStatus> >(
        std::vector<snapshot::TemplateBuildStatus>(1,
                                                   snapshot::TemplateBuildStatus::Waiting));
    MT_EXPECT_TRUE(Store::MatchesRecordFilter(record, filter));

    // One failing conjunct rejects the record.
    filter.template_statuses = Optional<std::vector<snapshot::TemplateBuildStatus> >(
        std::vector<snapshot::TemplateBuildStatus>(1, snapshot::TemplateBuildStatus::Ready));
    MT_EXPECT_TRUE(!Store::MatchesRecordFilter(record, filter));
}

// ---------------------------------------------------------------------------
// volumes
// ---------------------------------------------------------------------------

MT_TEST(create_volume_writes_the_record_and_its_name_binding) {
    TempRoot root;
    Store store(root.path);
    MT_EXPECT_TRUE(store.CreateVolume(Volume("vol_a", "data")).ok());

    MT_EXPECT_TRUE(fs::IsFile(Paths::VolumeRecordPath(root.path, "vol_a")));
    MT_EXPECT_TRUE(fs::IsFile(Paths::VolumeAliasPath(root.path, "data")));

    const repository::RepositoryResult<Optional<volume::VolumeRecord> > by_id =
        store.GetVolume("vol_a");
    MT_EXPECT_TRUE(by_id.ok());
    MT_EXPECT_TRUE(by_id.value().has_value());

    const repository::RepositoryResult<Optional<volume::VolumeRecord> > by_name =
        store.GetVolume("data");
    MT_EXPECT_TRUE(by_name.ok());
    MT_EXPECT_TRUE(by_name.value().has_value());
    MT_EXPECT_EQ(by_name.value()->id, std::string("vol_a"));
}

MT_TEST(create_volume_rejects_a_duplicate_id_or_name) {
    TempRoot root;
    Store store(root.path);
    MT_EXPECT_TRUE(store.CreateVolume(Volume("vol_a", "data")).ok());

    const repository::RepositoryResult<Unit> same_id =
        store.CreateVolume(Volume("vol_a", "other"));
    MT_EXPECT_TRUE(!same_id.ok());
    MT_EXPECT_TRUE(same_id.error().message.find("already exists") != std::string::npos);

    const repository::RepositoryResult<Unit> same_name =
        store.CreateVolume(Volume("vol_b", "data"));
    MT_EXPECT_TRUE(!same_name.ok());
    MT_EXPECT_TRUE(same_name.error().kind ==
                   repository::RepositoryErrorKind::VolumeNameConflict);
}

MT_TEST(create_volume_rejects_a_name_colliding_with_another_id) {
    TempRoot root;
    Store store(root.path);
    MT_EXPECT_TRUE(store.CreateVolume(Volume("vol_a", "data")).ok());

    // Names and ids share one namespace on disk, so a name equal to an
    // existing id would make one of the two unreachable.
    const repository::RepositoryResult<Unit> collides =
        store.CreateVolume(Volume("vol_b", "vol_a"));
    MT_EXPECT_TRUE(!collides.ok());
    MT_EXPECT_TRUE(collides.error().kind ==
                   repository::RepositoryErrorKind::VolumeNameConflict);
}

MT_TEST(volume_components_are_validated) {
    TempRoot root;
    Store store(root.path);
    // These become path components, so an escape must be refused.
    MT_EXPECT_TRUE(!store.GetVolume("../escape").ok());
    MT_EXPECT_TRUE(!store.CreateVolume(Volume("../escape", "data")).ok());
    MT_EXPECT_TRUE(!store.CreateVolume(Volume("vol_a", "../escape")).ok());
    MT_EXPECT_TRUE(!store.ReserveVolume("vol_a", "../escape").ok());
}

MT_TEST(put_volume_preserves_reservation_state) {
    TempRoot root;
    Store store(root.path);
    MT_EXPECT_TRUE(store.CreateVolume(Volume("vol_a", "data")).ok());
    MT_EXPECT_TRUE(store.ReserveVolume("vol_a", "sbx1").ok());

    // A concurrent status update carries no reservation; it must not clear
    // the live one.
    volume::VolumeRecord update = Volume("vol_a", "data");
    update.status = volume::VolumeStatus::Ready;
    MT_EXPECT_TRUE(store.PutVolume(update).ok());

    const volume::VolumeRecord stored = *store.GetVolume("vol_a").value();
    MT_EXPECT_TRUE(stored.reserved_by_sandbox_id.has_value());
    MT_EXPECT_EQ(*stored.reserved_by_sandbox_id, std::string("sbx1"));
}

MT_TEST(put_volume_requires_an_existing_record) {
    TempRoot root;
    Store store(root.path);
    const repository::RepositoryResult<Unit> missing =
        store.PutVolume(Volume("vol_a", "data"));
    MT_EXPECT_TRUE(!missing.ok());
    MT_EXPECT_TRUE(missing.error().kind == repository::RepositoryErrorKind::VolumeNotFound);
}

MT_TEST(reserve_volume_reports_a_conflicting_owner) {
    TempRoot root;
    Store store(root.path);
    MT_EXPECT_TRUE(store.CreateVolume(Volume("vol_a", "data")).ok());

    // First reservation succeeds and reports no prior owner.
    const repository::RepositoryResult<Optional<std::string> > first =
        store.ReserveVolume("vol_a", "sbx1");
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(!first.value().has_value());

    // Re-reserving by the same owner is a no-op.
    MT_EXPECT_TRUE(!store.ReserveVolume("vol_a", "sbx1").value().has_value());

    // A different owner gets the current holder back rather than an error.
    const repository::RepositoryResult<Optional<std::string> > conflict =
        store.ReserveVolume("vol_a", "sbx2");
    MT_EXPECT_TRUE(conflict.ok());
    MT_EXPECT_TRUE(conflict.value().has_value());
    MT_EXPECT_EQ(*conflict.value(), std::string("sbx1"));
}

MT_TEST(read_only_volumes_take_no_exclusive_lease) {
    TempRoot root;
    Store store(root.path);
    volume::VolumeRecord record = Volume("vol_a", "data");
    record.mode = volume::VolumeMode::ReadOnly;
    MT_EXPECT_TRUE(store.CreateVolume(record).ok());

    // Nothing to reserve exclusively, so this reports no conflict and
    // records no owner.
    MT_EXPECT_TRUE(!store.ReserveVolume("vol_a", "sbx1").value().has_value());
    MT_EXPECT_TRUE(!store.GetVolume("vol_a").value()->reserved_by_sandbox_id.has_value());

    MT_EXPECT_TRUE(store.ReserveReadOnlyVolume("vol_a", "sbx1").ok());
    // Idempotent.
    MT_EXPECT_TRUE(store.ReserveReadOnlyVolume("vol_a", "sbx1").ok());
    MT_EXPECT_TRUE(store.ReserveReadOnlyVolume("vol_a", "sbx2").ok());
    MT_EXPECT_EQ(store.GetVolume("vol_a").value()->read_only_mounts.size(),
                 static_cast<std::size_t>(2));
}

MT_TEST(reserve_read_only_rejects_an_exclusive_volume) {
    TempRoot root;
    Store store(root.path);
    MT_EXPECT_TRUE(store.CreateVolume(Volume("vol_a", "data")).ok());

    const repository::RepositoryResult<Unit> rejected =
        store.ReserveReadOnlyVolume("vol_a", "sbx1");
    MT_EXPECT_TRUE(!rejected.ok());
    MT_EXPECT_TRUE(rejected.error().message.find("is not read-only") != std::string::npos);
}

MT_TEST(delete_volume_refuses_while_mounted) {
    TempRoot root;
    Store store(root.path);
    MT_EXPECT_TRUE(store.CreateVolume(Volume("vol_a", "data")).ok());
    MT_EXPECT_TRUE(store.ReserveVolume("vol_a", "sbx1").ok());

    const repository::RepositoryResult<Unit> reserved = store.DeleteVolume("vol_a");
    MT_EXPECT_TRUE(!reserved.ok());
    MT_EXPECT_TRUE(reserved.error().message.find("is reserved by sandbox") !=
                   std::string::npos);

    // Releasing the lease makes the delete succeed.
    MT_EXPECT_TRUE(
        store.ReplaceVolumeOwnerFor("vol_a", "sbx1", Optional<std::string>()).ok());
    MT_EXPECT_TRUE(store.DeleteVolume("vol_a").ok());
    MT_EXPECT_TRUE(!fs::Exists(Paths::VolumeRecordPath(root.path, "vol_a")));
    MT_EXPECT_TRUE(!fs::Exists(Paths::VolumeAliasPath(root.path, "data")));

    // Idempotent.
    MT_EXPECT_TRUE(store.DeleteVolume("vol_a").ok());
}

MT_TEST(delete_volume_refuses_while_read_only_mounted) {
    TempRoot root;
    Store store(root.path);
    volume::VolumeRecord record = Volume("vol_a", "data");
    record.mode = volume::VolumeMode::ReadOnly;
    MT_EXPECT_TRUE(store.CreateVolume(record).ok());
    MT_EXPECT_TRUE(store.ReserveReadOnlyVolume("vol_a", "sbx1").ok());

    const repository::RepositoryResult<Unit> mounted = store.DeleteVolume("vol_a");
    MT_EXPECT_TRUE(!mounted.ok());
    MT_EXPECT_TRUE(mounted.error().message.find("mounted read-only") != std::string::npos);
}

MT_TEST(replace_volume_owner_is_a_no_op_when_unchanged) {
    TempRoot root;
    Store store(root.path);
    MT_EXPECT_TRUE(store.CreateVolume(Volume("vol_a", "data")).ok());
    MT_EXPECT_TRUE(store.ReserveVolume("vol_a", "sbx1").ok());

    // Same owner: returns early without taking the record lock.
    MT_EXPECT_TRUE(store
                       .ReplaceVolumeOwnerFor("vol_a", "sbx1",
                                              Optional<std::string>(std::string("sbx1")))
                       .ok());
    MT_EXPECT_EQ(*store.GetVolume("vol_a").value()->reserved_by_sandbox_id,
                 std::string("sbx1"));

    MT_EXPECT_TRUE(store
                       .ReplaceVolumeOwnerFor("vol_a", "sbx1",
                                              Optional<std::string>(std::string("sbx2")))
                       .ok());
    MT_EXPECT_EQ(*store.GetVolume("vol_a").value()->reserved_by_sandbox_id,
                 std::string("sbx2"));
}

MT_TEST(list_volumes_page_walks_in_id_order) {
    TempRoot root;
    Store store(root.path);
    MT_EXPECT_TRUE(store.CreateVolume(Volume("vol_c", "c")).ok());
    MT_EXPECT_TRUE(store.CreateVolume(Volume("vol_a", "a")).ok());
    MT_EXPECT_TRUE(store.CreateVolume(Volume("vol_b", "b")).ok());

    const repository::RepositoryResult<posixfs::VolumeRecordPage> first =
        store.ListVolumesPage(Optional<std::string>(), 2);
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_EQ(first.value().records.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(first.value().records[0].id, std::string("vol_a"));
    MT_EXPECT_EQ(first.value().records[1].id, std::string("vol_b"));
    MT_EXPECT_TRUE(first.value().next_volume_id.has_value());
    MT_EXPECT_EQ(*first.value().next_volume_id, std::string("vol_b"));

    const repository::RepositoryResult<posixfs::VolumeRecordPage> second =
        store.ListVolumesPage(first.value().next_volume_id, 2);
    MT_EXPECT_TRUE(second.ok());
    MT_EXPECT_EQ(second.value().records.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(second.value().records[0].id, std::string("vol_c"));
    // Last page carries no cursor.
    MT_EXPECT_TRUE(!second.value().next_volume_id.has_value());
}

MT_TEST(list_volumes_page_rejects_a_zero_limit) {
    TempRoot root;
    Store store(root.path);
    const repository::RepositoryResult<posixfs::VolumeRecordPage> page =
        store.ListVolumesPage(Optional<std::string>(), 0);
    MT_EXPECT_TRUE(!page.ok());
    MT_EXPECT_TRUE(page.error().message.find("greater than zero") != std::string::npos);
}

// ---------------------------------------------------------------------------
// build cache
// ---------------------------------------------------------------------------

MT_TEST(build_cache_head_starts_empty_and_advances) {
    TempRoot root;
    Store store(root.path);

    const repository::RepositoryResult<posixfs::BuildCacheState> initial =
        store.GetBuildCacheState();
    MT_EXPECT_TRUE(initial.ok());
    MT_EXPECT_TRUE(!initial.value().current.has_value());

    const repository::RepositoryResult<Optional<std::string> > first =
        store.ReplaceBuildCacheHead("vol_a");
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(!first.value().has_value());

    const repository::RepositoryResult<Optional<std::string> > second =
        store.ReplaceBuildCacheHead("vol_b");
    MT_EXPECT_TRUE(second.ok());
    MT_EXPECT_EQ(*second.value(), std::string("vol_a"));

    const posixfs::BuildCacheState state = store.GetBuildCacheState().value();
    MT_EXPECT_EQ(*state.current, std::string("vol_b"));
    // The displaced seed is retired rather than forgotten, so its storage
    // can be reclaimed deliberately.
    MT_EXPECT_EQ(state.retired.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(state.retired.count("vol_a") > 0);
}

MT_TEST(build_cache_refuses_to_republish_a_retired_seed) {
    TempRoot root;
    Store store(root.path);
    MT_EXPECT_TRUE(store.ReplaceBuildCacheHead("vol_a").ok());
    MT_EXPECT_TRUE(store.ReplaceBuildCacheHead("vol_b").ok());

    const repository::RepositoryResult<Optional<std::string> > resurrect =
        store.ReplaceBuildCacheHead("vol_a");
    MT_EXPECT_TRUE(!resurrect.ok());
    MT_EXPECT_TRUE(resurrect.error().message.find("retired build cache seed") !=
                   std::string::npos);

    // Forgetting it first makes the id reusable.
    MT_EXPECT_TRUE(store.ForgetRetiredBuildCache("vol_a").ok());
    MT_EXPECT_TRUE(store.ReplaceBuildCacheHead("vol_a").ok());
}

MT_TEST(build_cache_replacing_the_same_seed_does_not_retire_it) {
    TempRoot root;
    Store store(root.path);
    MT_EXPECT_TRUE(store.ReplaceBuildCacheHead("vol_a").ok());
    MT_EXPECT_TRUE(store.ReplaceBuildCacheHead("vol_a").ok());

    const posixfs::BuildCacheState state = store.GetBuildCacheState().value();
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
