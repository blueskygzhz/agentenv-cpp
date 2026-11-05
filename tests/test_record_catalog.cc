// SPDX-License-Identifier: MIT
// Rust: src/snapshot/catalog.rs
//
// The record layer every backend shares. It is driven here over the *real*
// OSS repository on an HTTP-level stub, rather than over a hand-written
// document store, for one reason: every interesting rule in this layer is a
// race, and the races are only real if the compare-and-swap underneath them
// is. The stub answers ETag preconditions and has a `before_put` hook, so a
// competing writer can be made to land between a read and the write that
// depends on it.
#include "agentenv/snapshot/repository/record_catalog.h"

#include <memory>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>

#include "agentenv/snapshot/repository/oss/repository.h"
#include "microtest.h"
#include "oss_client_fixture.h"

using namespace agentenv;                              // NOLINT
using namespace agentenv::snapshot;                     // NOLINT
using namespace agentenv::snapshot::repository;         // NOLINT
using namespace agentenv::testing::oss_fixture;         // NOLINT

namespace {

const char* kIdA = "11111111-2222-3333-4444-555555555555";
const char* kIdB = "66666666-7777-8888-9999-aaaaaaaaaaaa";

core::SnapshotId Id(const char* text) {
    const core::Expected<core::SnapshotId, std::string> id = core::SnapshotId::Parse(text);
    MT_EXPECT_TRUE(id.ok());
    return id.ok() ? id.value() : core::SnapshotId();
}

/// Aborts rather than substituting a placeholder: `SnapshotAlias` has no
/// default state by design, and a test that mistypes an alias must stop here
/// instead of silently asserting against a different name.
SnapshotAlias Alias(const std::string& text) {
    const core::Expected<SnapshotAlias, std::string> alias = SnapshotAlias::Parse(text);
    if (!alias.ok()) {
        std::fprintf(stderr, "test bug: invalid alias '%s'\n", text.c_str());
        std::abort();
    }
    return alias.value();
}

/// A waiting template record, which is the only kind `Create` accepts.
SnapshotRecord WaitingTemplate(const char* id, const core::Optional<std::string>& alias,
                               int64_t created_at = 1000) {
    SnapshotRecord record;
    record.id = Id(id);
    if (alias.has_value()) record.alias = core::Optional<SnapshotAlias>(Alias(*alias));
    record.source = SnapshotSource::Template(TemplateBuildInfo());
    record.created_at_unix_ms = created_at;
    record.updated_at_unix_ms = created_at;
    return record;
}

/// The repository under test, plus the stub it talks to.
struct Fixture {
    StubObjectServer server;
    std::shared_ptr<oss::OssSnapshotRepository> repository;

    Fixture() {
        const RepositoryResult<oss::OssClient*> client =
            oss::OssClient::NewWithTransport(Normalized(GoodConfig()), server.Fn());
        MT_EXPECT_TRUE(client.ok());
        repository.reset(new oss::OssSnapshotRepository(
            std::shared_ptr<oss::OssClient>(client.ok() ? client.value() : 0),
            std::shared_ptr<oss::ObjectUploader>()));
    }

    SnapshotRepository& Repo() { return *repository; }

    /// The stored record, decoded, or absent.
    core::Optional<SnapshotRecord> Stored(const char* id) {
        const std::string key = DocumentKey::SnapshotRecord(Id(id)).Path();
        if (server.bodies.find(key) == server.bodies.end()) {
            return core::Optional<SnapshotRecord>();
        }
        const core::Expected<core::Json, core::AnyError> json =
            core::Json::Parse(server.bodies[key]);
        MT_EXPECT_TRUE(json.ok());
        const core::Expected<SnapshotRecord, std::string> record =
            SnapshotRecord::FromJson(json.value());
        MT_EXPECT_TRUE(record.ok());
        return record.ok() ? core::Optional<SnapshotRecord>(record.value())
                           : core::Optional<SnapshotRecord>();
    }

    void SeedRecord(const SnapshotRecord& record) {
        server.Seed(DocumentKey::SnapshotRecord(record.id).Path(),
                    record.ToJson().ToStringPretty());
    }

    void SeedAlias(const std::string& alias, const char* id) {
        server.Seed(DocumentKey::SnapshotAlias(Alias(alias)).Path(),
                    core::Json(std::string(id)).ToString());
    }

    bool HasAlias(const std::string& alias) {
        return server.bodies.count(DocumentKey::SnapshotAlias(Alias(alias)).Path()) != 0;
    }
};

}  // namespace

// ---- create --------------------------------------------------------------

MT_TEST(create_stores_a_waiting_template_and_claims_its_alias) {
    Fixture f;
    const SnapshotRecord record = WaitingTemplate(kIdA, core::Optional<std::string>("base"));
    const RepositoryResult<SnapshotRecord> created = record_catalog::Create(f.Repo(), record);
    MT_EXPECT_TRUE(created.ok());

    MT_EXPECT_TRUE(f.Stored(kIdA).has_value());
    MT_EXPECT_TRUE(f.HasAlias("base"));
    // Readable by name and by id, through the same layer that wrote it.
    const RepositoryResult<core::Optional<core::SnapshotId> > resolved =
        record_catalog::ResolveAlias(f.Repo(), "base");
    MT_EXPECT_TRUE(resolved.ok() && resolved.value().has_value());
    MT_EXPECT_TRUE(resolved.ok() && resolved.value().has_value() &&
                   *resolved.value() == Id(kIdA));
}

// Only template snapshots can be pre-created, and never an already-committed
// one: `Create` writes create-only, so accepting a committed record would
// store a snapshot whose artifacts nothing ever uploaded.
MT_TEST(create_refuses_a_sandbox_record_or_a_committed_one) {
    Fixture f;
    SnapshotRecord sandbox = WaitingTemplate(kIdA, core::nullopt);
    sandbox.source = SnapshotSource::Sandbox("sbx_1");
    MT_EXPECT_TRUE(record_catalog::Create(f.Repo(), sandbox).error().kind ==
                   RepositoryErrorKind::InvalidRequest);

    SnapshotRecord committed = WaitingTemplate(kIdA, core::nullopt);
    committed.committed = core::Optional<CommittedSnapshot>(CommittedSnapshot());
    MT_EXPECT_TRUE(record_catalog::Create(f.Repo(), committed).error().kind ==
                   RepositoryErrorKind::InvalidRequest);

    // Neither attempt wrote anything.
    MT_EXPECT_TRUE(!f.Stored(kIdA).has_value());
}

// An existing record is reported, not overwritten: the pre-image returned by
// the create-only update is what distinguishes the two.
MT_TEST(create_reports_an_existing_record_without_overwriting_it) {
    Fixture f;
    SnapshotRecord first = WaitingTemplate(kIdA, core::nullopt, 1000);
    f.SeedRecord(first);

    const SnapshotRecord second = WaitingTemplate(kIdA, core::nullopt, 2000);
    const RepositoryResult<SnapshotRecord> created = record_catalog::Create(f.Repo(), second);
    MT_EXPECT_TRUE(!created.ok());
    MT_EXPECT_TRUE(!created.ok() && created.error().kind == RepositoryErrorKind::InvalidRequest);
    // The original survives, timestamps and all.
    MT_EXPECT_TRUE(f.Stored(kIdA).has_value() &&
                   f.Stored(kIdA)->created_at_unix_ms == 1000);
}

// A live snapshot owns the name, so the conflict is reported *before* a record
// is written -- there is nothing to roll back in the common case.
MT_TEST(create_refuses_an_alias_a_live_snapshot_owns) {
    Fixture f;
    f.SeedRecord(WaitingTemplate(kIdB, core::Optional<std::string>("base")));
    f.SeedAlias("base", kIdB);

    const RepositoryResult<SnapshotRecord> created =
        record_catalog::Create(f.Repo(), WaitingTemplate(kIdA, core::Optional<std::string>("base")));
    MT_EXPECT_TRUE(!created.ok());
    MT_EXPECT_TRUE(!created.ok() && created.error().kind == RepositoryErrorKind::AliasConflict);
    MT_EXPECT_TRUE(!f.Stored(kIdA).has_value());
}

// The alias is free because its owner is being deleted: that snapshot has
// already given up the name, and refusing here would keep a name reserved by
// a snapshot that is on its way out.
MT_TEST(create_takes_an_alias_whose_owner_is_being_deleted) {
    Fixture f;
    SnapshotRecord dying = WaitingTemplate(kIdB, core::Optional<std::string>("base"));
    dying.deleting = true;
    f.SeedRecord(dying);
    f.SeedAlias("base", kIdB);

    MT_EXPECT_TRUE(
        record_catalog::Create(f.Repo(), WaitingTemplate(kIdA, core::Optional<std::string>("base")))
            .ok());
    const RepositoryResult<core::Optional<core::SnapshotId> > resolved =
        record_catalog::ResolveAlias(f.Repo(), "base");
    MT_EXPECT_TRUE(resolved.ok() && resolved.value().has_value() &&
                   *resolved.value() == Id(kIdA));
}

// The record landed, then another node took the name. Rolling the record back
// is the point: a record keeping an alias it does not own is findable by id
// and invisible by name, and nothing would ever reconcile the two.
MT_TEST(create_rolls_the_record_back_when_the_alias_is_lost_after_the_write) {
    Fixture f;
    // The competing claim lands between `Create`'s availability check and its
    // own claim -- after its record is already stored.
    f.server.before_put = [&f]() {
        f.SeedRecord(WaitingTemplate(kIdB, core::Optional<std::string>("base")));
        f.SeedAlias("base", kIdB);
    };

    const RepositoryResult<SnapshotRecord> created =
        record_catalog::Create(f.Repo(), WaitingTemplate(kIdA, core::Optional<std::string>("base")));
    MT_EXPECT_TRUE(!created.ok());
    MT_EXPECT_TRUE(!created.ok() && created.error().kind == RepositoryErrorKind::AliasConflict);
    // Rolled back, and the winner keeps the name.
    MT_EXPECT_TRUE(!f.Stored(kIdA).has_value());
    const RepositoryResult<core::Optional<core::SnapshotId> > resolved =
        record_catalog::ResolveAlias(f.Repo(), "base");
    MT_EXPECT_TRUE(resolved.ok() && resolved.value().has_value() &&
                   *resolved.value() == Id(kIdB));
}

// ---- get / resolve -------------------------------------------------------

MT_TEST(get_finds_a_record_by_id_and_by_alias) {
    Fixture f;
    f.SeedRecord(WaitingTemplate(kIdA, core::Optional<std::string>("base")));
    f.SeedAlias("base", kIdA);

    const RepositoryResult<core::Optional<SnapshotRecord> > by_id =
        record_catalog::Get(f.Repo(), kIdA);
    MT_EXPECT_TRUE(by_id.ok() && by_id.value().has_value());

    const RepositoryResult<core::Optional<SnapshotRecord> > by_alias =
        record_catalog::Get(f.Repo(), "base");
    MT_EXPECT_TRUE(by_alias.ok() && by_alias.value().has_value());
    MT_EXPECT_TRUE(by_alias.ok() && by_alias.value().has_value() &&
                   by_alias.value()->id == Id(kIdA));
}

// A snapshot being deleted is hidden from `get` and `resolve_alias`, since
// its files may already be gone: handing it out would make a resume fail as
// if the artifacts were corrupt.
MT_TEST(a_record_being_deleted_is_hidden_from_get_and_resolve) {
    Fixture f;
    SnapshotRecord dying = WaitingTemplate(kIdA, core::Optional<std::string>("base"));
    dying.deleting = true;
    f.SeedRecord(dying);
    f.SeedAlias("base", kIdA);

    const RepositoryResult<core::Optional<SnapshotRecord> > got =
        record_catalog::Get(f.Repo(), kIdA);
    MT_EXPECT_TRUE(got.ok() && !got.value().has_value());

    const RepositoryResult<core::Optional<core::SnapshotId> > resolved =
        record_catalog::ResolveAlias(f.Repo(), "base");
    MT_EXPECT_TRUE(resolved.ok() && !resolved.value().has_value());

    // Still *there*, though: a delete retried by id has to find it to finish.
    MT_EXPECT_TRUE(f.Stored(kIdA).has_value() && f.Stored(kIdA)->deleting);
}

MT_TEST(get_reports_absence_rather_than_an_error_for_an_unknown_lookup) {
    Fixture f;
    const RepositoryResult<core::Optional<SnapshotRecord> > by_id =
        record_catalog::Get(f.Repo(), kIdA);
    MT_EXPECT_TRUE(by_id.ok() && !by_id.value().has_value());
    const RepositoryResult<core::Optional<SnapshotRecord> > by_alias =
        record_catalog::Get(f.Repo(), "nothing");
    MT_EXPECT_TRUE(by_alias.ok() && !by_alias.value().has_value());
}

// An alias pointing at a snapshot that no longer exists is removed on sight.
// Left behind by a half-finished delete, it would block the name forever,
// since a claim only yields to a *live* owner.
MT_TEST(a_stale_alias_is_cleaned_up_when_resolved) {
    Fixture f;
    f.SeedAlias("base", kIdA);  // no record

    const RepositoryResult<core::Optional<core::SnapshotId> > resolved =
        record_catalog::ResolveAlias(f.Repo(), "base");
    MT_EXPECT_TRUE(resolved.ok() && !resolved.value().has_value());
    MT_EXPECT_TRUE(!f.HasAlias("base"));
}

MT_TEST(an_invalid_alias_is_an_invalid_request_not_an_absence) {
    Fixture f;
    // Reported rather than answered "no such snapshot": the caller passed
    // something that can never name one.
    MT_EXPECT_TRUE(record_catalog::ResolveAlias(f.Repo(), "not a valid alias!").error().kind ==
                   RepositoryErrorKind::InvalidRequest);
    MT_EXPECT_TRUE(record_catalog::Get(f.Repo(), "not a valid alias!").error().kind ==
                   RepositoryErrorKind::InvalidRequest);
}

// ---- list ----------------------------------------------------------------

MT_TEST(list_returns_matching_records_newest_first) {
    Fixture f;
    f.SeedRecord(WaitingTemplate(kIdA, core::nullopt, 1000));
    f.SeedRecord(WaitingTemplate(kIdB, core::nullopt, 3000));

    const RepositoryResult<std::vector<SnapshotRecord> > records =
        record_catalog::List(f.Repo(), SnapshotListFilter::MatchesAll());
    MT_EXPECT_TRUE(records.ok() && records.value().size() == 2);
    MT_EXPECT_TRUE(records.ok() && records.value().size() == 2 &&
                   records.value()[0].id == Id(kIdB));
    MT_EXPECT_TRUE(records.ok() && records.value().size() == 2 &&
                   records.value()[1].id == Id(kIdA));
}

MT_TEST(list_hides_records_being_deleted_and_applies_the_filter) {
    Fixture f;
    SnapshotRecord dying = WaitingTemplate(kIdA, core::nullopt, 3000);
    dying.deleting = true;
    f.SeedRecord(dying);
    f.SeedRecord(WaitingTemplate(kIdB, core::nullopt, 1000));

    const RepositoryResult<std::vector<SnapshotRecord> > all =
        record_catalog::List(f.Repo(), SnapshotListFilter::MatchesAll());
    MT_EXPECT_TRUE(all.ok() && all.value().size() == 1);
    MT_EXPECT_TRUE(all.ok() && all.value().size() == 1 && all.value()[0].id == Id(kIdB));

    const RepositoryResult<std::vector<SnapshotRecord> > none =
        record_catalog::List(f.Repo(), SnapshotListFilter::ByIds(std::vector<core::SnapshotId>(
                                           1, Id(kIdA))));
    MT_EXPECT_TRUE(none.ok() && none.value().empty());
}

// A record that will not parse fails the listing rather than being skipped: a
// quietly incomplete catalog is indistinguishable from a smaller one, and a
// caller would act on the gap.
MT_TEST(list_fails_on_an_unparseable_record) {
    Fixture f;
    f.SeedRecord(WaitingTemplate(kIdA, core::nullopt));
    f.server.Seed(DocumentKey::SnapshotRecord(Id(kIdB)).Path(), "{not json");

    const RepositoryResult<std::vector<SnapshotRecord> > records =
        record_catalog::List(f.Repo(), SnapshotListFilter::MatchesAll());
    MT_EXPECT_TRUE(!records.ok());
    MT_EXPECT_TRUE(!records.ok() && records.error().kind == RepositoryErrorKind::Backend);
}

// The collection is read with the paging listing, so a catalog larger than
// one page is returned whole. A single page would make the listing silently
// stop at 1000 records.
MT_TEST(list_pages_through_a_collection_larger_than_one_page) {
    Fixture f;
    // 1001 records: one more than the protocol's page ceiling.
    for (int i = 0; i < 1001; ++i) {
        char id[48];
        std::snprintf(id, sizeof(id), "%08d-0000-0000-0000-000000000000", i);
        f.SeedRecord(WaitingTemplate(id, core::nullopt, 1000 + i));
    }
    const RepositoryResult<std::vector<SnapshotRecord> > records =
        record_catalog::List(f.Repo(), SnapshotListFilter::MatchesAll());
    MT_EXPECT_TRUE(records.ok());
    MT_EXPECT_EQ(static_cast<int>(records.ok() ? records.value().size() : 0), 1001);
}

// ---- template builds -----------------------------------------------------

MT_TEST(try_start_build_moves_a_waiting_build_to_building) {
    Fixture f;
    f.SeedRecord(WaitingTemplate(kIdA, core::nullopt));

    const RepositoryResult<SnapshotRecord> started =
        record_catalog::TryStartBuild(f.Repo(), Id(kIdA));
    MT_EXPECT_TRUE(started.ok());
    MT_EXPECT_TRUE(started.ok() &&
                   started.value().source.build.status == TemplateBuildStatus::Building);
    MT_EXPECT_TRUE(started.ok() && started.value().source.build.started_at_unix_ms.has_value());
    // Written, not just returned.
    MT_EXPECT_TRUE(f.Stored(kIdA).has_value() &&
                   f.Stored(kIdA)->source.build.status == TemplateBuildStatus::Building);
}

// Exactly one caller may take a build out of Waiting. Without that, two nodes
// would build the same template, and the second would overwrite the first's
// artifacts while both reported success.
MT_TEST(only_one_caller_can_start_the_same_build) {
    Fixture f;
    f.SeedRecord(WaitingTemplate(kIdA, core::nullopt));

    MT_EXPECT_TRUE(record_catalog::TryStartBuild(f.Repo(), Id(kIdA)).ok());
    const RepositoryResult<SnapshotRecord> again =
        record_catalog::TryStartBuild(f.Repo(), Id(kIdA));
    MT_EXPECT_TRUE(!again.ok());
    MT_EXPECT_TRUE(!again.ok() && again.error().kind == RepositoryErrorKind::InvalidRequest);
}

// The decision is re-run against the state the write landed on, which is the
// whole reason it must be pure: another node started this build between the
// read and the write, so the answer must be its failure, not the success the
// first read would have implied.
MT_TEST(a_build_started_by_another_node_mid_update_is_reported_as_a_failure) {
    Fixture f;
    f.SeedRecord(WaitingTemplate(kIdA, core::nullopt));

    f.server.before_put = [&f]() {
        SnapshotRecord taken = WaitingTemplate(kIdA, core::nullopt);
        taken.source.build.status = TemplateBuildStatus::Building;
        f.SeedRecord(taken);
    };

    const RepositoryResult<SnapshotRecord> started =
        record_catalog::TryStartBuild(f.Repo(), Id(kIdA));
    MT_EXPECT_TRUE(!started.ok());
    MT_EXPECT_TRUE(!started.ok() && started.error().kind == RepositoryErrorKind::InvalidRequest);
}

MT_TEST(starting_a_build_clears_a_previous_error_reason) {
    Fixture f;
    SnapshotRecord failed = WaitingTemplate(kIdA, core::nullopt);
    failed.source.build.error_reason =
        core::Optional<TemplateBuildErrorReason>(TemplateBuildErrorReason());
    f.SeedRecord(failed);

    const RepositoryResult<SnapshotRecord> started =
        record_catalog::TryStartBuild(f.Repo(), Id(kIdA));
    // Stale reason dropped: it describes a build that is no longer running.
    MT_EXPECT_TRUE(started.ok() && !started.value().source.build.error_reason.has_value());
}

MT_TEST(mark_build_error_records_the_reason_and_finish_time) {
    Fixture f;
    f.SeedRecord(WaitingTemplate(kIdA, core::nullopt));

    TemplateBuildErrorReason reason;
    reason.message = "builder exploded";
    MT_EXPECT_TRUE(record_catalog::MarkBuildError(f.Repo(), Id(kIdA), reason).ok());

    const core::Optional<SnapshotRecord> stored = f.Stored(kIdA);
    MT_EXPECT_TRUE(stored.has_value() &&
                   stored->source.build.status == TemplateBuildStatus::Error);
    MT_EXPECT_TRUE(stored.has_value() && stored->source.build.error_reason.has_value() &&
                   stored->source.build.error_reason->message == "builder exploded");
    MT_EXPECT_TRUE(stored.has_value() && stored->source.build.finished_at_unix_ms.has_value());
}

// A sandbox snapshot has no build to start or fail.
MT_TEST(the_build_transitions_refuse_a_sandbox_record) {
    Fixture f;
    SnapshotRecord sandbox = WaitingTemplate(kIdA, core::nullopt);
    sandbox.source = SnapshotSource::Sandbox("sbx_1");
    f.SeedRecord(sandbox);

    MT_EXPECT_TRUE(record_catalog::TryStartBuild(f.Repo(), Id(kIdA)).error().kind ==
                   RepositoryErrorKind::InvalidRequest);
    MT_EXPECT_TRUE(record_catalog::MarkBuildError(f.Repo(), Id(kIdA), TemplateBuildErrorReason())
                       .error()
                       .kind == RepositoryErrorKind::InvalidRequest);
}

// `SnapshotNotFound`, and the same for one being deleted: a build cannot be
// started on a snapshot whose files are going away.
MT_TEST(the_build_transitions_report_a_missing_or_deleting_record_as_not_found) {
    Fixture f;
    MT_EXPECT_TRUE(record_catalog::TryStartBuild(f.Repo(), Id(kIdA)).error().kind ==
                   RepositoryErrorKind::SnapshotNotFound);

    SnapshotRecord dying = WaitingTemplate(kIdA, core::nullopt);
    dying.deleting = true;
    f.SeedRecord(dying);
    MT_EXPECT_TRUE(record_catalog::TryStartBuild(f.Repo(), Id(kIdA)).error().kind ==
                   RepositoryErrorKind::SnapshotNotFound);
    MT_EXPECT_TRUE(record_catalog::MarkBuildError(f.Repo(), Id(kIdA), TemplateBuildErrorReason())
                       .error()
                       .kind == RepositoryErrorKind::SnapshotNotFound);
}

// ---- aliases -------------------------------------------------------------

MT_TEST(claiming_an_alias_twice_for_the_same_snapshot_is_idempotent) {
    Fixture f;
    MT_EXPECT_TRUE(record_catalog::ClaimAlias(f.Repo(), Alias("base"), Id(kIdA)).ok());
    // Idempotent, which is what lets a retried publish finish.
    MT_EXPECT_TRUE(record_catalog::ClaimAlias(f.Repo(), Alias("base"), Id(kIdA)).ok());
}

MT_TEST(releasing_an_alias_owned_by_someone_else_leaves_it_alone) {
    Fixture f;
    f.SeedAlias("base", kIdB);
    MT_EXPECT_TRUE(record_catalog::ReleaseAlias(f.Repo(), Alias("base"), Id(kIdA)).ok());
    // Still the other snapshot's name.
    MT_EXPECT_TRUE(f.HasAlias("base"));

    MT_EXPECT_TRUE(record_catalog::ReleaseAlias(f.Repo(), Alias("base"), Id(kIdB)).ok());
    MT_EXPECT_TRUE(!f.HasAlias("base"));
}

MT_TEST(releasing_a_missing_alias_is_success) {
    Fixture f;
    // Idempotent so a delete retried after a partial failure can finish.
    MT_EXPECT_TRUE(record_catalog::ReleaseAlias(f.Repo(), Alias("base"), Id(kIdA)).ok());
}

MT_MAIN
