// SPDX-License-Identifier: MIT
// Rust: src/api/impls/template.rs :: v3_templates_post
//
// `POST /v3/templates` is the first half of upstream's two-step build, and it
// had no counterpart here. The consequence was not a missing endpoint but an
// unreachable pipeline: `TemplateBuilder` and
// `TemplateBuildRecordFromV3Request` were both ported, both tested, and
// callable from nothing outside their own tests, so `POST
// /sandboxes/{id}/snapshots` was the only way to obtain a `SnapshotRecord` at
// all. The record builder in particular ran only in tests -- it built a
// `TemplateWaiting` record that nobody stored, which is why the name it
// claimed was never actually reserved and `TryStartBuild` had no record to
// find.
//
// The repository here is the *real* OSS one over an HTTP-level stub, as
// `test_record_catalog.cc` does and for the same reason: committing the
// waiting record is a compare-and-swap, and a hand-written document store
// would make the one interesting failure -- two requests racing for the same
// template name -- unobservable.
#include "microtest.h"

#include <memory>
#include <string>
#include <vector>

#include "agentenv/api/impls.h"
#include "agentenv/snapshot/repository/oss/repository.h"
#include "agentenv/snapshot/repository/record_catalog.h"
// The status route reads a build's log document; the tests seed it the way a
// build would, through `WriteBuildLogs`.
#include "agentenv/template/logs.h"
#include "no_docker_credentials.h"
#include "orchestrator_fakes.h"
#include "oss_client_fixture.h"

using namespace agentenv;                        // NOLINT
using namespace agentenv::api;                   // NOLINT
using namespace agentenv::snapshot;              // NOLINT
using namespace agentenv::snapshot::repository;  // NOLINT
using namespace agentenv::testing::oss_fixture;  // NOLINT

namespace {

/// An `ApiImpl` with a working repository, plus the object stub under it.
struct Fixture {
    StubObjectServer                            server;
    agentenv::testing::Fixture                  orch;
    std::shared_ptr<oss::OssSnapshotRepository> repository;
    std::shared_ptr<ApiImpl>                    impl;

    Fixture() {
        const RepositoryResult<oss::OssClient*> client =
            oss::OssClient::NewWithTransport(Normalized(GoodConfig()), server.Fn());
        MT_EXPECT_TRUE(client.ok());
        repository.reset(new oss::OssSnapshotRepository(
            std::shared_ptr<oss::OssClient>(client.ok() ? client.value() : 0),
            std::shared_ptr<oss::ObjectUploader>()));
        impl.reset(new ApiImpl(orch.orch, std::shared_ptr<volume::VolumeManager>(),
                               std::shared_ptr<image::ImageResolver>(), repository,
                               std::shared_ptr<SnapshotRuntimeResolver>()));
    }

    /// The record as it was actually written, decoded from the stub's bytes.
    ///
    /// Read back through the store rather than from the response, because the
    /// response is exactly what a handler that committed nothing could still
    /// produce.
    core::Optional<SnapshotRecord> Stored(const std::string& id_text) {
        const core::Expected<core::SnapshotId, std::string> id =
            core::SnapshotId::Parse(id_text);
        MT_EXPECT_TRUE(id.ok());
        if (!id.ok()) return core::Optional<SnapshotRecord>();
        const std::string key = DocumentKey::SnapshotRecord(id.value()).Path();
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

    /// The id the alias document points at, or empty.
    std::string AliasTarget(const std::string& text) {
        const core::Expected<SnapshotAlias, std::string> alias = SnapshotAlias::Parse(text);
        MT_EXPECT_TRUE(alias.ok());
        if (!alias.ok()) return std::string();
        const std::string key = DocumentKey::SnapshotAlias(alias.value()).Path();
        if (server.bodies.find(key) == server.bodies.end()) return std::string();
        const core::Expected<core::Json, core::AnyError> json =
            core::Json::Parse(server.bodies[key]);
        MT_EXPECT_TRUE(json.ok());
        return json.ok() && json.value().kind() == core::Json::Kind::String
                   ? json.value().as_string()
                   : std::string();
    }
};

TemplateBuildRequestV3 Request(const std::string& name) {
    TemplateBuildRequestV3 body;
    body.name = core::Optional<std::string>(name);
    return body;
}

}  // namespace

// ---- the commit that was missing -------------------------------------------

MT_TEST(a_v3_build_request_commits_a_waiting_template_record) {
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> response =
        f.impl->CreateTemplateBuild(Request("my-template"));

    MT_EXPECT_TRUE(response.ok());
    if (!response.ok()) return;

    // Asserted against the store, not the response: the response is what a
    // handler that built the record and dropped it would also return.
    const core::Optional<SnapshotRecord> stored = f.Stored(response.value().template_id);
    MT_EXPECT_TRUE(stored.has_value());
    if (!stored.has_value()) return;
    // `waiting`, with nothing committed: the build has not run.
    MT_EXPECT_TRUE(stored->source.is_template());
    MT_EXPECT_TRUE(!stored->committed.has_value());
    // And the name is really claimed, which is what makes the reservation
    // mean anything.
    MT_EXPECT_EQ(f.AliasTarget("my-template"), response.value().template_id);
}

MT_TEST(the_response_carries_the_ids_the_next_step_needs) {
    // `POST /v2/templates/{templateID}/builds/{buildID}` refuses unless the
    // two path segments match, so a response whose ids differed would hand
    // the caller a pair it is guaranteed to be rejected for.
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> response =
        f.impl->CreateTemplateBuild(Request("my-template"));
    MT_EXPECT_TRUE(response.ok());
    if (!response.ok()) return;

    MT_EXPECT_TRUE(!response.value().template_id.empty());
    MT_EXPECT_EQ(response.value().template_id, response.value().build_id);
    // The id is the stored record's, not a hopeful local one.
    MT_EXPECT_TRUE(f.Stored(response.value().build_id).has_value());

    MT_EXPECT_EQ(response.value().names.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(response.value().names[0], std::string("my-template"));
    MT_EXPECT_EQ(response.value().aliases.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(response.value().aliases[0], std::string("my-template"));
    // Upstream passes `Vec::new()`, and by here there is nothing to echo:
    // the record builder refuses a non-empty `tags`.
    MT_EXPECT_TRUE(response.value().tags.empty());
}

MT_TEST(two_requests_for_one_template_name_do_not_both_win) {
    // The reason the record has to be committed rather than merely built.
    // Without the write, each request would claim the name locally, answer
    // 202, and leave two callers each believing the name was theirs.
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> first =
        f.impl->CreateTemplateBuild(Request("my-template"));
    MT_EXPECT_TRUE(first.ok());

    const core::Expected<TemplateRequestResponseV3, ApiError> second =
        f.impl->CreateTemplateBuild(Request("my-template"));
    MT_EXPECT_TRUE(!second.ok());
    // 400, not the 409 that `repository_error` alone would give: upstream
    // routes this through `bad_request_for_repository_build_error`, which
    // treats a taken name on a build route as bad input.
    MT_EXPECT_EQ(second.error().code, 400);

    // The winner still owns it.
    MT_EXPECT_EQ(f.AliasTarget("my-template"), first.value().template_id);
}

MT_TEST(a_build_request_carries_the_requested_machine_shape) {
    Fixture f;
    TemplateBuildRequestV3 body = Request("my-template");
    body.cpu_count = core::Optional<uint32_t>(4);
    body.memory_mb = core::Optional<uint32_t>(2048);

    const core::Expected<TemplateRequestResponseV3, ApiError> response =
        f.impl->CreateTemplateBuild(body);
    MT_EXPECT_TRUE(response.ok());
    if (!response.ok()) return;

    const core::Optional<SnapshotRecord> stored = f.Stored(response.value().template_id);
    MT_EXPECT_TRUE(stored.has_value());
    if (!stored.has_value()) return;
    // The resources are what the build will be run with, and the record is
    // the only place they survive between the two requests.
    MT_EXPECT_EQ(stored->resources.cpu_count, static_cast<uint32_t>(4));
    MT_EXPECT_EQ(stored->resources.memory_mib, static_cast<uint32_t>(2048));
}

// ---- refusals --------------------------------------------------------------

MT_TEST(a_build_request_without_a_name_is_refused) {
    // Optional in the schema, required by this branch: the name is both the
    // alias the record claims and the only handle the caller gets back.
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> response =
        f.impl->CreateTemplateBuild(TemplateBuildRequestV3());
    MT_EXPECT_TRUE(!response.ok());
    MT_EXPECT_EQ(response.error().code, 400);
    MT_EXPECT_EQ(response.error().message, std::string("template name must be provided"));
    // Nothing was written, so a retry with a name is not blocked by a
    // half-made reservation.
    MT_EXPECT_TRUE(f.server.bodies.empty());

    // `""` is not a name either, and it must be reported as the *missing*
    // field rather than passed on. Both are 400, so the distinction is only
    // in the message -- but the message is the whole difference between "you
    // left the name out" and `SnapshotAlias::Parse`'s complaint about a
    // malformed one, which tells the caller to go fix a value they never set.
    const core::Expected<TemplateRequestResponseV3, ApiError> empty =
        f.impl->CreateTemplateBuild(Request(""));
    MT_EXPECT_TRUE(!empty.ok());
    MT_EXPECT_EQ(empty.error().code, 400);
    MT_EXPECT_EQ(empty.error().message, std::string("template name must be provided"));
    MT_EXPECT_TRUE(f.server.bodies.empty());
}

MT_TEST(an_image_only_request_is_refused_rather_than_built_as_a_template) {
    // `imageOnly` selects a different artifact: upstream sends it to
    // `create_image_build`, which produces an image and no template record.
    // Ignoring the flag would answer 202 and hand over the one thing the
    // request explicitly asked not to have, with the tags pointing at it.
    Fixture f;
    TemplateBuildRequestV3 body = Request("my-image");
    body.image_only = core::Optional<bool>(true);

    const core::Expected<TemplateRequestResponseV3, ApiError> response =
        f.impl->CreateTemplateBuild(body);
    MT_EXPECT_TRUE(!response.ok());
    // 501: the node is correctly configured and cannot do this, so a retry
    // will not help and the operator needs to see which capability is absent.
    MT_EXPECT_EQ(response.error().code, 501);
    MT_EXPECT_TRUE(response.error().message.find("imageOnly") != std::string::npos);
    MT_EXPECT_TRUE(f.server.bodies.empty());

    // `false` is not the same as set: it takes the template path normally.
    TemplateBuildRequestV3 explicit_false = Request("my-template");
    explicit_false.image_only = core::Optional<bool>(false);
    MT_EXPECT_TRUE(f.impl->CreateTemplateBuild(explicit_false).ok());
}

MT_TEST(a_build_request_with_a_tagged_name_is_refused) {
    // Rust `template_build_record_from_v3_request` rejects it, and the handler
    // must surface that as a 400 rather than a 500.
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> response =
        f.impl->CreateTemplateBuild(Request("my-template:v1"));
    MT_EXPECT_TRUE(!response.ok());
    MT_EXPECT_EQ(response.error().code, 400);
    MT_EXPECT_TRUE(f.server.bodies.empty());
}

MT_TEST(a_build_request_with_a_zero_resource_is_refused) {
    // `ResolveTemplateResources` rejects a zero in either dimension, and the
    // failure must reach the client as its own 400 rather than being
    // flattened into a 500 by the commit that follows.
    Fixture f;
    TemplateBuildRequestV3 body = Request("my-template");
    body.cpu_count = core::Optional<uint32_t>(0);

    const core::Expected<TemplateRequestResponseV3, ApiError> response =
        f.impl->CreateTemplateBuild(body);
    MT_EXPECT_TRUE(!response.ok());
    MT_EXPECT_EQ(response.error().code, 400);
    MT_EXPECT_TRUE(f.server.bodies.empty());
}

MT_TEST(a_node_without_a_repository_refuses_rather_than_reserving_nothing) {
    // There is nowhere to commit the record, so the reservation cannot be
    // made. Answering 202 would promise a name this node never claimed and
    // hand back ids that `TryStartBuild` will not find.
    agentenv::testing::Fixture orch;
    ApiImpl impl(orch.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(),
                 std::shared_ptr<SnapshotRepository>(),
                 std::shared_ptr<SnapshotRuntimeResolver>());

    const core::Expected<TemplateRequestResponseV3, ApiError> response =
        impl.CreateTemplateBuild(Request("my-template"));
    MT_EXPECT_TRUE(!response.ok());
    MT_EXPECT_TRUE(response.error().code != 202 && response.error().code >= 500);
}

// ---- GET /templates --------------------------------------------------------
//
// This answered 501 claiming "the template store is not ported". There is no
// template store, upstream or here -- templates *are* snapshot records, and
// `record_catalog::List` was ported and tested with the repository already
// held by this class. The missing piece was the call, and the stale diagnosis
// is what kept it from being obvious.

MT_TEST(the_template_list_shows_a_reserved_build) {
    // The pair that had no way to meet: a build reserved by the route above
    // must be visible to the one that lists them. Before, the first wrote a
    // record and the second refused to read any.
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> created =
        f.impl->CreateTemplateBuild(Request("my-template"));
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;

    const core::Expected<std::vector<TemplateModel>, ApiError> listed = f.impl->ListTemplates();
    MT_EXPECT_TRUE(listed.ok());
    if (!listed.ok()) return;
    MT_EXPECT_EQ(listed.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(listed.value()[0].template_id, created.value().template_id);
    // `waiting`: reserved, not built. This is the field that tells a client
    // whether the template can be launched yet.
    MT_EXPECT_TRUE(listed.value()[0].build_status == TemplateBuildStatus::Waiting);
    // Named by its alias, which is how a caller addresses it.
    MT_EXPECT_EQ(listed.value()[0].names.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(listed.value()[0].names[0], std::string("my-template"));
    // No committed snapshot yet, so no envd version is known.
    MT_EXPECT_EQ(listed.value()[0].envd_version, std::string("unknown"));
}

MT_TEST(an_empty_catalog_lists_nothing_rather_than_refusing) {
    // An empty list is now a true answer rather than the wrong one the 501
    // was avoiding: this node really can tell the client it has no templates.
    Fixture f;
    const core::Expected<std::vector<TemplateModel>, ApiError> listed = f.impl->ListTemplates();
    MT_EXPECT_TRUE(listed.ok());
    MT_EXPECT_TRUE(listed.value().empty());
}

MT_TEST(the_template_list_carries_the_requested_machine_shape) {
    Fixture f;
    TemplateBuildRequestV3 body = Request("sized");
    body.cpu_count = core::Optional<uint32_t>(4);
    body.memory_mb = core::Optional<uint32_t>(2048);
    MT_EXPECT_TRUE(f.impl->CreateTemplateBuild(body).ok());

    const core::Expected<std::vector<TemplateModel>, ApiError> listed = f.impl->ListTemplates();
    MT_EXPECT_TRUE(listed.ok());
    if (!listed.ok() || listed.value().empty()) return;
    MT_EXPECT_EQ(listed.value()[0].cpu_count, static_cast<uint32_t>(4));
    MT_EXPECT_EQ(listed.value()[0].memory_mb, static_cast<uint32_t>(2048));
}

MT_TEST(the_template_list_does_not_offer_sandbox_snapshots) {
    // Rust `SnapshotListFilter::templates()`. A sandbox-sourced record is a
    // snapshot, not a template: listing it here would advertise an entry that
    // was never built as one, and whose build status is only `Ready` because
    // it has no build at all.
    Fixture f;
    MT_EXPECT_TRUE(f.impl->CreateTemplateBuild(Request("a-template")).ok());

    // A committed, sandbox-sourced record written straight into the catalog,
    // which is the shape `POST /sandboxes/{id}/snapshots` publishes.
    SnapshotRecord from_sandbox;
    from_sandbox.id     = core::SnapshotId::Fresh();
    from_sandbox.source = SnapshotSource::Sandbox("sb-1");
    from_sandbox.committed = core::Optional<CommittedSnapshot>(CommittedSnapshot());
    f.server.Seed(DocumentKey::SnapshotRecord(from_sandbox.id).Path(),
                  from_sandbox.ToJson().ToString());

    const core::Expected<std::vector<TemplateModel>, ApiError> listed = f.impl->ListTemplates();
    MT_EXPECT_TRUE(listed.ok());
    if (!listed.ok()) return;
    MT_EXPECT_EQ(listed.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(listed.value()[0].names[0], std::string("a-template"));
}

MT_TEST(a_corrupt_catalog_is_reported_rather_than_listed_as_empty) {
    // `record_catalog::List` refuses a record it cannot decode rather than
    // skipping it, because a silently incomplete listing is worse than none.
    // The handler has to preserve that: answering 200 with an empty array
    // tells a client this node has no templates, which is a different claim
    // from "this node cannot read its catalog" -- and the client would go on
    // to create a duplicate of a template that is already there.
    Fixture f;
    MT_EXPECT_TRUE(f.impl->CreateTemplateBuild(Request("my-template")).ok());

    // A record document that is valid JSON and not a record. Corruption, not
    // a malformed request: nothing a caller sent can produce this.
    const core::SnapshotId other = core::SnapshotId::Fresh();
    f.server.Seed(DocumentKey::SnapshotRecord(other).Path(), "{\"nonsense\":true}");

    const core::Expected<std::vector<TemplateModel>, ApiError> listed = f.impl->ListTemplates();
    MT_EXPECT_TRUE(!listed.ok());
    MT_EXPECT_TRUE(listed.error().code >= 500);
}

MT_TEST(a_node_without_a_repository_cannot_list_templates) {
    // An empty list would be a wrong answer rather than a missing one: a
    // client cannot tell "this node has no templates" from "this node cannot
    // tell you", and the second is what is true here.
    agentenv::testing::Fixture orch;
    ApiImpl impl(orch.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(),
                 std::shared_ptr<SnapshotRepository>(),
                 std::shared_ptr<SnapshotRuntimeResolver>());
    const core::Expected<std::vector<TemplateModel>, ApiError> listed = impl.ListTemplates();
    MT_EXPECT_TRUE(!listed.ok());
    MT_EXPECT_TRUE(listed.error().code >= 500);
}

// ---- the Template model ----------------------------------------------------

MT_TEST(a_template_entry_serialises_every_required_key) {
    SnapshotRecord record;
    record.id                  = core::SnapshotId::Fresh();
    record.source              = SnapshotSource::Template(TemplateBuildInfo());
    record.resources.cpu_count = 2;
    record.resources.memory_mib = 512;
    record.resources.disk_size_mib = 1024;
    record.created_at_unix_ms  = 1700000000000ll;
    record.updated_at_unix_ms  = 1700000001000ll;

    const core::Json j = TemplateModel::FromRecord(record).ToJson();
    MT_EXPECT_EQ(j["templateID"].as_string(), record.id.ToString());
    MT_EXPECT_EQ(j["buildID"].as_string(), record.id.ToString());
    MT_EXPECT_EQ(j["cpuCount"].as_int(), static_cast<int64_t>(2));
    MT_EXPECT_EQ(j["memoryMB"].as_int(), static_cast<int64_t>(512));
    MT_EXPECT_EQ(j["diskSizeMB"].as_int(), static_cast<int64_t>(1024));
    MT_EXPECT_TRUE(j["public"].as_bool());
    // RFC3339, as upstream's `DateTime<Utc>` serialises -- not the raw
    // milliseconds the record stores.
    MT_EXPECT_TRUE(j["createdAt"].as_string().find("2023-11-14T") == 0);
    MT_EXPECT_TRUE(j["updatedAt"].kind() == core::Json::Kind::String);
    // Present and null, not omitted: the key is required and nullable, and an
    // epoch timestamp would claim the template was last used in 1970.
    //
    // Asserted on the object's keys, not through `operator[]`: a const lookup
    // of an absent key yields a null `Json`, so `kind() == Null` is satisfied
    // by a key that was never written -- which is exactly the bug this line
    // is here to catch.
    MT_EXPECT_EQ(j.as_object().count("lastSpawnedAt"), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(j["lastSpawnedAt"].kind() == core::Json::Kind::Null);
    MT_EXPECT_EQ(j["spawnCount"].as_int(), static_cast<int64_t>(0));
    MT_EXPECT_EQ(j["buildCount"].as_int(), static_cast<int64_t>(1));
    MT_EXPECT_EQ(j["envdVersion"].as_string(), std::string("unknown"));
    MT_EXPECT_EQ(j["buildStatus"].as_string(), std::string("waiting"));
}

MT_TEST(a_build_status_is_written_in_the_wire_spelling) {
    // The record's own serialisation capitalises these (`"Waiting"`); the
    // generated API model renames every variant to lowercase. Reusing
    // `TemplateBuildStatusToString` here would put `"Waiting"` in a body
    // whose schema declares `"waiting"`, which a generated client rejects as
    // an unknown variant -- and the two spellings are easy to confuse because
    // both types are called `TemplateBuildStatus`.
    MT_EXPECT_EQ(std::string(TemplateBuildStatusWireName(TemplateBuildStatus::Waiting)),
                 std::string("waiting"));
    MT_EXPECT_EQ(std::string(TemplateBuildStatusWireName(TemplateBuildStatus::Building)),
                 std::string("building"));
    MT_EXPECT_EQ(std::string(TemplateBuildStatusWireName(TemplateBuildStatus::Ready)),
                 std::string("ready"));
    MT_EXPECT_EQ(std::string(TemplateBuildStatusWireName(TemplateBuildStatus::Error)),
                 std::string("error"));
    // And the record spelling really does differ, so this is not a
    // distinction without a difference.
    MT_EXPECT_TRUE(std::string(snapshot::TemplateBuildStatusToString(
                       TemplateBuildStatus::Waiting)) != std::string("waiting"));
}

MT_TEST(a_sandbox_sourced_record_reports_ready_and_its_envd_version) {
    // It has no build to report on, and it exists only because a capture
    // already succeeded. A default-constructed `TemplateBuildInfo` would say
    // `Waiting`, which would describe a finished snapshot as pending.
    SnapshotRecord record;
    record.id     = core::SnapshotId::Fresh();
    record.source = SnapshotSource::Sandbox("sb-1");
    CommittedSnapshot committed;
    committed.runtime_versions.envd_version = "1.2.3";
    record.committed = core::Optional<CommittedSnapshot>(committed);

    const TemplateModel model = TemplateModel::FromRecord(record);
    MT_EXPECT_TRUE(model.build_status == TemplateBuildStatus::Ready);
    MT_EXPECT_EQ(model.envd_version, std::string("1.2.3"));
}

MT_TEST(an_unaliased_record_is_named_by_its_id) {
    // Rust `build_record_names`. `names` is how a client addresses the
    // template, so an unaliased one must still be addressable -- an empty
    // list would make it invisible to anything that reads `names[0]`.
    SnapshotRecord record;
    record.id     = core::SnapshotId::Fresh();
    record.source = SnapshotSource::Template(TemplateBuildInfo());

    const TemplateModel model = TemplateModel::FromRecord(record);
    MT_EXPECT_EQ(model.names.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(model.names[0], record.id.ToString());
}

// ---- POST /v2/templates/{templateID}/builds/{buildID} ----------------------
//
// The second step. The build itself cannot run here -- the production
// `TemplateBuildExecuteFn` boots a Firecracker sandbox and drives
// start/capture/stop, and that half is not ported -- so the handler validates
// everything and refuses. What these tests pin is that the refusal is placed
// correctly: *before* `TryStartBuild`, so the reservation survives it.

namespace {

TemplateBuildStartRequest StartBody() { return TemplateBuildStartRequest(); }

/// The build status currently stored for a record, by id.
TemplateBuildStatus StatusOf(Fixture& f, const std::string& id) {
    const core::Optional<SnapshotRecord> stored = f.Stored(id);
    MT_EXPECT_TRUE(stored.has_value());
    if (!stored.has_value()) return TemplateBuildStatus::Error;
    return stored->source.build.status;
}

}  // namespace

MT_TEST(starting_a_build_leaves_the_reservation_waiting) {
    // The rule this whole handler is arranged around. `TryStartBuild` moves
    // the record `waiting` -> `building`, and only a `waiting` record can be
    // claimed -- so a refusal that claimed first would leave the template
    // permanently unstartable, dead even once the executor lands. Refusing
    // first keeps the reservation usable by the identical request later.
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> reserved =
        f.impl->CreateTemplateBuild(Request("my-template"));
    MT_EXPECT_TRUE(reserved.ok());
    if (!reserved.ok()) return;
    const std::string id = reserved.value().template_id;
    MT_EXPECT_TRUE(StatusOf(f, id) == TemplateBuildStatus::Waiting);

    const core::Expected<core::Unit, ApiError> started =
        f.impl->StartTemplateBuild(id, id, StartBody());
    MT_EXPECT_TRUE(!started.ok());
    // 501: the request was in order, the node simply cannot do this.
    MT_EXPECT_EQ(started.error().code, 501);
    MT_EXPECT_TRUE(started.error().message.find("executor") != std::string::npos);

    // Still waiting -- not building, and not errored.
    MT_EXPECT_TRUE(StatusOf(f, id) == TemplateBuildStatus::Waiting);
    // And still listed as waiting, which is what a client polls.
    const core::Expected<std::vector<TemplateModel>, ApiError> listed = f.impl->ListTemplates();
    MT_EXPECT_TRUE(listed.ok());
    if (!listed.ok() || listed.value().empty()) return;
    MT_EXPECT_TRUE(listed.value()[0].build_status == TemplateBuildStatus::Waiting);
}

MT_TEST(a_repeated_start_is_refused_the_same_way_every_time) {
    // The consequence of leaving the record alone: the refusal is idempotent.
    // Had the first call claimed the record, the second would report a
    // *different* error about a build already in progress -- and the caller
    // would have no way back to `waiting`.
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> reserved =
        f.impl->CreateTemplateBuild(Request("my-template"));
    MT_EXPECT_TRUE(reserved.ok());
    if (!reserved.ok()) return;
    const std::string id = reserved.value().template_id;

    const core::Expected<core::Unit, ApiError> first =
        f.impl->StartTemplateBuild(id, id, StartBody());
    const core::Expected<core::Unit, ApiError> second =
        f.impl->StartTemplateBuild(id, id, StartBody());
    MT_EXPECT_TRUE(!first.ok() && !second.ok());
    MT_EXPECT_EQ(first.error().code, second.error().code);
    MT_EXPECT_EQ(first.error().message, second.error().message);
    MT_EXPECT_TRUE(StatusOf(f, id) == TemplateBuildStatus::Waiting);
}

MT_TEST(the_two_path_ids_must_agree) {
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> reserved =
        f.impl->CreateTemplateBuild(Request("my-template"));
    MT_EXPECT_TRUE(reserved.ok());
    if (!reserved.ok()) return;

    const core::Expected<core::Unit, ApiError> mismatched = f.impl->StartTemplateBuild(
        reserved.value().template_id, core::SnapshotId::Fresh().ToString(), StartBody());
    MT_EXPECT_TRUE(!mismatched.ok());
    MT_EXPECT_EQ(mismatched.error().code, 400);
    MT_EXPECT_TRUE(mismatched.error().message.find("must match") != std::string::npos);
}

MT_TEST(a_malformed_build_id_is_a_bad_request) {
    // Upstream takes the id through a typed path parameter; here it arrives
    // as text, so the parse is the handler's job. 400 rather than 404: the
    // request is malformed, and no template could ever have this id.
    Fixture f;
    const core::Expected<core::Unit, ApiError> started =
        f.impl->StartTemplateBuild("not-a-uuid", "not-a-uuid", StartBody());
    MT_EXPECT_TRUE(!started.ok());
    MT_EXPECT_EQ(started.error().code, 400);
    MT_EXPECT_TRUE(started.error().message.find("invalid buildID") != std::string::npos);
}

MT_TEST(starting_a_build_for_an_unknown_template_is_a_not_found) {
    Fixture f;
    const std::string absent = core::SnapshotId::Fresh().ToString();
    const core::Expected<core::Unit, ApiError> started =
        f.impl->StartTemplateBuild(absent, absent, StartBody());
    MT_EXPECT_TRUE(!started.ok());
    MT_EXPECT_EQ(started.error().code, 404);
    MT_EXPECT_TRUE(started.error().message.find("not found") != std::string::npos);
}

MT_TEST(an_alias_that_names_another_record_is_refused) {
    // The record lookup accepts an id *or* an alias, so `templateID` can name
    // a record whose id is not `buildID`. Running the build anyway would
    // execute it against someone else's reservation -- and publish it under
    // their name.
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> reserved =
        f.impl->CreateTemplateBuild(Request("my-template"));
    MT_EXPECT_TRUE(reserved.ok());
    if (!reserved.ok()) return;

    // Both segments agree, so the first check passes; the alias resolves to a
    // record whose id is something else entirely.
    const core::Expected<core::Unit, ApiError> started =
        f.impl->StartTemplateBuild("my-template", "my-template", StartBody());
    MT_EXPECT_TRUE(!started.ok());
    MT_EXPECT_EQ(started.error().code, 400);
    MT_EXPECT_TRUE(started.error().message.find("invalid buildID") != std::string::npos);

    // And with a *real* id in `buildID`, the ids disagree instead.
    const core::Expected<core::Unit, ApiError> crossed =
        f.impl->StartTemplateBuild("my-template", reserved.value().template_id, StartBody());
    MT_EXPECT_TRUE(!crossed.ok());
    MT_EXPECT_EQ(crossed.error().code, 400);
    MT_EXPECT_TRUE(StatusOf(f, reserved.value().template_id) == TemplateBuildStatus::Waiting);

    // Neither case above actually reaches the id comparison -- the first dies
    // at the parse, the second at the "both segments agree" check -- and the
    // comparison is only load-bearing for the case that gets past both.
    //
    // That case exists because an alias may legitimately be *shaped* like a
    // build id: `SnapshotAlias::Parse` allows letters, digits, hyphens and
    // underscores, so a template can be named after a uuid. One string then
    // passes the agreement check, parses as a `SnapshotId`, finds no record
    // under that id, and resolves by alias to a record whose id is something
    // else -- which is precisely the reservation that must not be built.
    const std::string decoy = core::SnapshotId::Fresh().ToString();
    const core::Expected<TemplateRequestResponseV3, ApiError> named =
        f.impl->CreateTemplateBuild(Request(decoy));
    MT_EXPECT_TRUE(named.ok());
    if (!named.ok()) return;
    // The reservation minted its own id, so the name is a uuid that belongs
    // to no record.
    MT_EXPECT_TRUE(named.value().template_id != decoy);

    const core::Expected<core::Unit, ApiError> impostor =
        f.impl->StartTemplateBuild(decoy, decoy, StartBody());
    MT_EXPECT_TRUE(!impostor.ok());
    MT_EXPECT_EQ(impostor.error().code, 400);
    MT_EXPECT_TRUE(impostor.error().message.find("same build record") != std::string::npos);
    MT_EXPECT_TRUE(StatusOf(f, named.value().template_id) == TemplateBuildStatus::Waiting);
}

MT_TEST(a_build_cannot_name_both_an_image_and_a_template_base) {
    // `TemplateBuildStartBaseSourceFrom`'s exclusivity rule, reached through
    // the route for the first time.
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> reserved =
        f.impl->CreateTemplateBuild(Request("my-template"));
    MT_EXPECT_TRUE(reserved.ok());
    if (!reserved.ok()) return;
    const std::string id = reserved.value().template_id;

    TemplateBuildStartRequest body;
    body.from_image    = core::Optional<std::string>("ubuntu:24.04");
    body.from_template = core::Optional<std::string>("base");
    const core::Expected<core::Unit, ApiError> started =
        f.impl->StartTemplateBuild(id, id, body);
    MT_EXPECT_TRUE(!started.ok());
    MT_EXPECT_EQ(started.error().code, 400);
    MT_EXPECT_TRUE(StatusOf(f, id) == TemplateBuildStatus::Waiting);
}

MT_TEST(a_malformed_step_is_refused_before_anything_is_claimed) {
    // The spec translation runs before the capability refusal, so a bad step
    // is reported as the 400 it is rather than hidden behind the 501.
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> reserved =
        f.impl->CreateTemplateBuild(Request("my-template"));
    MT_EXPECT_TRUE(reserved.ok());
    if (!reserved.ok()) return;
    const std::string id = reserved.value().template_id;

    TemplateStepRequest step;
    step.type = "NOT_A_REAL_STEP";
    std::vector<TemplateStepRequest> steps;
    steps.push_back(step);
    TemplateBuildStartRequest body;
    body.steps = core::Optional<std::vector<TemplateStepRequest> >(steps);

    const core::Expected<core::Unit, ApiError> started =
        f.impl->StartTemplateBuild(id, id, body);
    MT_EXPECT_TRUE(!started.ok());
    MT_EXPECT_EQ(started.error().code, 400);
    MT_EXPECT_TRUE(StatusOf(f, id) == TemplateBuildStatus::Waiting);
}

MT_TEST(a_registry_block_is_refused_rather_than_pulling_anonymously) {
    // Ignoring it would pull without the credentials the caller supplied:
    // either an authentication failure they already solved, or -- worse -- a
    // *different, public* image of the same name, published as their
    // template.
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> reserved =
        f.impl->CreateTemplateBuild(Request("my-template"));
    MT_EXPECT_TRUE(reserved.ok());
    if (!reserved.ok()) return;
    const std::string id = reserved.value().template_id;

    core::Json registry;
    registry["username"] = core::Json(std::string("u"));
    registry["password"] = core::Json(std::string("p"));

    TemplateBuildStartRequest body;
    body.from_image          = core::Optional<std::string>("private.io/app:1");
    body.from_image_registry = core::Optional<core::Json>(registry);
    const core::Expected<core::Unit, ApiError> started =
        f.impl->StartTemplateBuild(id, id, body);
    MT_EXPECT_TRUE(!started.ok());
    MT_EXPECT_EQ(started.error().code, 501);
    MT_EXPECT_TRUE(started.error().message.find("fromImageRegistry") != std::string::npos);
    MT_EXPECT_TRUE(StatusOf(f, id) == TemplateBuildStatus::Waiting);
}

MT_TEST(a_registry_block_without_an_image_is_a_bad_request_first) {
    // Upstream's own 400. Checked before the capability refusal so a
    // malformed request is reported as malformed rather than as an
    // unsupported feature -- the two tell the caller to do different things.
    Fixture f;
    const core::Expected<TemplateRequestResponseV3, ApiError> reserved =
        f.impl->CreateTemplateBuild(Request("my-template"));
    MT_EXPECT_TRUE(reserved.ok());
    if (!reserved.ok()) return;
    const std::string id = reserved.value().template_id;

    core::Json registry;
    registry["username"] = core::Json(std::string("u"));

    TemplateBuildStartRequest body;
    body.from_image_registry = core::Optional<core::Json>(registry);
    const core::Expected<core::Unit, ApiError> no_image =
        f.impl->StartTemplateBuild(id, id, body);
    MT_EXPECT_TRUE(!no_image.ok());
    MT_EXPECT_EQ(no_image.error().code, 400);
    MT_EXPECT_TRUE(no_image.error().message.find("requires fromImage") != std::string::npos);

    // And with a template base instead of an image, which is the other half
    // of the same rule.
    body.from_image    = core::Optional<std::string>("private.io/app:1");
    body.from_template = core::Optional<std::string>("base");
    const core::Expected<core::Unit, ApiError> with_template =
        f.impl->StartTemplateBuild(id, id, body);
    MT_EXPECT_TRUE(!with_template.ok());
    MT_EXPECT_EQ(with_template.error().code, 400);
    MT_EXPECT_TRUE(StatusOf(f, id) == TemplateBuildStatus::Waiting);
}

MT_TEST(the_build_spec_is_sized_from_the_reservation_not_the_start_body) {
    // `POST /v3/templates` is where the machine shape was chosen, and this
    // body has no fields for it. A spec built from defaults instead would
    // boot the build VM at the wrong size and publish a template whose
    // recorded resources no sandbox was ever run with.
    //
    // Asserted through the helper the handler calls, because the handler
    // itself cannot get far enough to return the spec.
    TemplateBuildRequestV3 reservation;
    reservation.cpu_count = core::Optional<uint32_t>(4);
    reservation.memory_mb = core::Optional<uint32_t>(2048);
    const core::Expected<SnapshotRecord, ApiError> record =
        TemplateBuildRecordFromV3Request(reservation, core::SnapshotId::Fresh(), "sized");
    MT_EXPECT_TRUE(record.ok());
    if (!record.ok()) return;

    const core::Expected<tpl::TemplateBuildSpec, ApiError> spec =
        TemplateBuildSpecFromStartRequest(StartBody(), record.value().alias,
                                          record.value().resources);
    MT_EXPECT_TRUE(spec.ok());
    if (!spec.ok()) return;
    MT_EXPECT_TRUE(spec.value().ResourcesRef().has_value());
    if (!spec.value().ResourcesRef().has_value()) return;
    MT_EXPECT_EQ(spec.value().ResourcesRef()->cpu_count, static_cast<uint32_t>(4));
    MT_EXPECT_EQ(spec.value().ResourcesRef()->memory_mib, static_cast<uint32_t>(2048));
    // And the reservation's name travels with it, so the published template
    // keeps the alias the caller reserved.
    const tpl::TemplateBuildResult<core::Optional<SnapshotAlias> > alias =
        spec.value().ParsedAlias();
    MT_EXPECT_TRUE(alias.ok());
    if (!alias.ok()) return;
    MT_EXPECT_TRUE(alias.value().has_value());
    MT_EXPECT_EQ(alias.value()->ToString(), std::string("sized"));
}

MT_TEST(starting_a_build_without_a_repository_refuses) {
    agentenv::testing::Fixture orch;
    ApiImpl impl(orch.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(),
                 std::shared_ptr<SnapshotRepository>(),
                 std::shared_ptr<SnapshotRuntimeResolver>());
    const std::string id = core::SnapshotId::Fresh().ToString();
    const core::Expected<core::Unit, ApiError> started =
        impl.StartTemplateBuild(id, id, StartBody());
    MT_EXPECT_TRUE(!started.ok());
    MT_EXPECT_TRUE(started.error().code >= 500);
}

// ---- the V2 start request DTO ----------------------------------------------

MT_TEST(the_v2_start_body_parses_every_field) {
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(
        "{\"fromImage\":\"ubuntu:24.04\",\"startCmd\":\"./run\",\"readyCmd\":\"true\","
        "\"force\":true,\"steps\":[{\"type\":\"RUN\",\"args\":[\"echo\",\"hi\"],"
        "\"filesHash\":\"abc\",\"force\":false}]}");
    MT_EXPECT_TRUE(json.ok());
    const core::Expected<TemplateBuildStartRequest, core::AnyError> body =
        TemplateBuildStartRequest::FromJson(json.value());
    MT_EXPECT_TRUE(body.ok());
    if (!body.ok()) return;
    MT_EXPECT_EQ(*body.value().from_image, std::string("ubuntu:24.04"));
    MT_EXPECT_EQ(*body.value().start_cmd, std::string("./run"));
    MT_EXPECT_EQ(*body.value().ready_cmd, std::string("true"));
    MT_EXPECT_TRUE(*body.value().force);
    MT_EXPECT_TRUE(body.value().steps.has_value());
    MT_EXPECT_EQ(body.value().steps->size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ((*body.value().steps)[0].type, std::string("RUN"));
    MT_EXPECT_EQ((*body.value().steps)[0].args->size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(*(*body.value().steps)[0].files_hash, std::string("abc"));
    MT_EXPECT_TRUE(!*(*body.value().steps)[0].force);
}

MT_TEST(an_empty_v2_start_body_is_a_valid_request) {
    // No steps and no base is a build of the configured default image, which
    // is why the route parses an absent body as `{}` instead of refusing it.
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse("{}");
    MT_EXPECT_TRUE(json.ok());
    const core::Expected<TemplateBuildStartRequest, core::AnyError> body =
        TemplateBuildStartRequest::FromJson(json.value());
    MT_EXPECT_TRUE(body.ok());
    if (!body.ok()) return;
    MT_EXPECT_TRUE(!body.value().from_image.has_value());
    MT_EXPECT_TRUE(!body.value().steps.has_value());
    // And it really does select the default image rather than failing.
    const core::Expected<TemplateBuildStartBaseSource, ApiError> source =
        TemplateBuildStartBaseSourceFrom(body.value());
    MT_EXPECT_TRUE(source.ok());
    MT_EXPECT_TRUE(source.value().kind() == TemplateBuildStartBaseSource::DefaultImage);
}

MT_TEST(a_step_without_a_type_is_refused) {
    // `type` selects the step kind. Defaulting it to an empty string would
    // have `ApplyE2bTemplateStep` report an unknown step type -- blaming a
    // value the caller never sent.
    const core::Expected<core::Json, core::AnyError> json =
        core::Json::Parse("{\"steps\":[{\"args\":[\"x\"]}]}");
    MT_EXPECT_TRUE(json.ok());
    const core::Expected<TemplateBuildStartRequest, core::AnyError> body =
        TemplateBuildStartRequest::FromJson(json.value());
    MT_EXPECT_TRUE(!body.ok());
    MT_EXPECT_TRUE(body.error().chain().find("type") != std::string::npos);
}

MT_TEST(a_wrong_typed_v2_start_field_is_a_bad_request_not_a_default) {
    const char* bodies[] = {
        "{\"fromImage\":42}",
        "{\"fromTemplate\":[]}",
        "{\"startCmd\":false}",
        "{\"readyCmd\":1.5}",
        "{\"force\":\"yes\"}",
        "{\"steps\":\"RUN\"}",
        "{\"steps\":[\"RUN\"]}",
        "{\"steps\":[{\"type\":\"RUN\",\"args\":\"echo\"}]}",
        "{\"steps\":[{\"type\":\"RUN\",\"args\":[1]}]}",
        "{\"steps\":[{\"type\":\"RUN\",\"force\":\"no\"}]}",
        "{\"steps\":[{\"type\":\"RUN\",\"filesHash\":7}]}",
        "{\"fromImageRegistry\":\"creds\"}",
    };
    for (std::size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); ++i) {
        const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(bodies[i]);
        MT_EXPECT_TRUE(json.ok());
        MT_EXPECT_TRUE(!TemplateBuildStartRequest::FromJson(json.value()).ok());
    }
}

MT_TEST(an_explicit_empty_step_arg_list_is_kept) {
    // `ApplyE2bTemplateStep` reads the arity to decide whether a step is
    // well-formed, so "no args" and "args omitted" must not collapse.
    const core::Expected<core::Json, core::AnyError> json =
        core::Json::Parse("{\"steps\":[{\"type\":\"RUN\",\"args\":[]}]}");
    MT_EXPECT_TRUE(json.ok());
    const core::Expected<TemplateBuildStartRequest, core::AnyError> body =
        TemplateBuildStartRequest::FromJson(json.value());
    MT_EXPECT_TRUE(body.ok());
    if (!body.ok()) return;
    MT_EXPECT_TRUE((*body.value().steps)[0].args.has_value());
    MT_EXPECT_TRUE((*body.value().steps)[0].args->empty());
}

// ---- the request DTO -------------------------------------------------------

MT_TEST(the_v3_request_body_parses_every_field) {
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(
        "{\"name\":\"t\",\"cpuCount\":2,\"memoryMB\":512,\"tags\":[],\"imageOnly\":true}");
    MT_EXPECT_TRUE(json.ok());
    const core::Expected<TemplateBuildRequestV3, core::AnyError> body =
        TemplateBuildRequestV3::FromJson(json.value());
    MT_EXPECT_TRUE(body.ok());
    if (!body.ok()) return;
    MT_EXPECT_EQ(*body.value().name, std::string("t"));
    MT_EXPECT_EQ(*body.value().cpu_count, static_cast<uint32_t>(2));
    MT_EXPECT_EQ(*body.value().memory_mb, static_cast<uint32_t>(512));
    MT_EXPECT_TRUE(body.value().tags.has_value());
    MT_EXPECT_TRUE(body.value().tags->empty());
    MT_EXPECT_TRUE(*body.value().image_only);
}

MT_TEST(an_absent_v3_field_stays_absent) {
    // Not defaulted: an omitted `cpuCount` must take the node's configured
    // machine default, which `ResolveTemplateResources` supplies and a zero
    // here would be rejected for.
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse("{\"name\":\"t\"}");
    MT_EXPECT_TRUE(json.ok());
    const core::Expected<TemplateBuildRequestV3, core::AnyError> body =
        TemplateBuildRequestV3::FromJson(json.value());
    MT_EXPECT_TRUE(body.ok());
    if (!body.ok()) return;
    MT_EXPECT_TRUE(!body.value().cpu_count.has_value());
    MT_EXPECT_TRUE(!body.value().memory_mb.has_value());
    MT_EXPECT_TRUE(!body.value().tags.has_value());
    MT_EXPECT_TRUE(!body.value().image_only.has_value());
}

MT_TEST(a_wrong_typed_v3_field_is_a_bad_request_not_a_default) {
    // Upstream's generated deserializer rejects these before the handler runs.
    // There is no generator here, so the DTO has to: a negative `cpuCount`
    // read as a huge unsigned one would pass the only check there is
    // (non-zero) and size a build nobody asked for, and a non-boolean
    // `imageOnly` read as `false` would build the wrong artifact.
    const char* bodies[] = {
        "{\"name\":\"t\",\"cpuCount\":-1}",
        "{\"name\":\"t\",\"cpuCount\":\"2\"}",
        "{\"name\":\"t\",\"memoryMB\":-1}",
        "{\"name\":\"t\",\"imageOnly\":\"yes\"}",
        "{\"name\":\"t\",\"tags\":\"v1\"}",
        "{\"name\":\"t\",\"tags\":[1]}",
        "{\"name\":42}",
    };
    for (std::size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); ++i) {
        const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(bodies[i]);
        MT_EXPECT_TRUE(json.ok());
        MT_EXPECT_TRUE(!TemplateBuildRequestV3::FromJson(json.value()).ok());
    }
}

MT_TEST(the_v3_response_serialises_every_required_key) {
    // All three lists are required by the schema, so a client reading
    // `names[0]` must not have to handle the key being absent.
    TemplateRequestResponseV3 response;
    response.template_id = "id";
    response.build_id    = "id";
    response.names.push_back("t");
    response.aliases.push_back("t");

    const core::Json json = response.ToJson();
    MT_EXPECT_EQ(json["templateID"].as_string(), std::string("id"));
    MT_EXPECT_EQ(json["buildID"].as_string(), std::string("id"));
    MT_EXPECT_TRUE(json["public"].as_bool());
    MT_EXPECT_EQ(json["names"].as_array().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(json["aliases"].as_array().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(json["tags"].kind() == core::Json::Kind::Array);
    MT_EXPECT_TRUE(json["tags"].as_array().empty());
}

// ---- GET /templates/{templateID}/builds/{buildID}/status -------------------

namespace {

/// One stored log line. The timestamp is explicit because `cursor` pages on
/// it, so a test that let it default could not tell a bound from a no-op.
tpl::BuildLogEntry Entry(int64_t unix_ms, core::LogRecordLevel level,
                         const std::string& message, const char* step) {
    tpl::BuildLogEntry entry;
    entry.timestamp = core::SystemTime(unix_ms * 1000000);
    entry.level     = level;
    entry.message   = message;
    if (step != NULL) entry.step = core::Optional<std::string>(step);
    return entry;
}

/// Writes a build's log document, as a build would.
void SeedLogs(Fixture& f, const std::string& id_text,
              const std::vector<tpl::BuildLogEntry>& entries) {
    const core::Expected<core::SnapshotId, std::string> id = core::SnapshotId::Parse(id_text);
    MT_EXPECT_TRUE(id.ok());
    if (!id.ok()) return;
    MT_EXPECT_TRUE(tpl::WriteBuildLogs(*f.repository, id.value(), entries).ok());
}

/// The status route's query with everything at its documented default.
BuildLogQuery StatusQuery() {
    const core::Expected<BuildLogQuery, core::AnyError> query =
        BuildLogQuery::ForStatus(std::string(), std::string(), std::string());
    MT_EXPECT_TRUE(query.ok());
    return query.ok() ? query.value() : BuildLogQuery();
}

/// A reserved template, returning its id.
std::string Reserve(Fixture& f, const std::string& name) {
    const core::Expected<TemplateRequestResponseV3, ApiError> reserved =
        f.impl->CreateTemplateBuild(Request(name));
    MT_EXPECT_TRUE(reserved.ok());
    return reserved.ok() ? reserved.value().template_id : std::string();
}

}  // namespace

MT_TEST(a_reserved_build_reports_its_status_by_id) {
    // Before this route existed, `GET /templates` was the only way to observe
    // a reservation: it returns every template on the node, so a client had to
    // scan the list for its own id and still learned nothing about why a build
    // had failed.
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;

    const core::Expected<TemplateBuildInfoModel, ApiError> status =
        f.impl->GetTemplateBuildStatus(id, id, StatusQuery());
    MT_EXPECT_TRUE(status.ok());
    if (!status.ok()) return;
    MT_EXPECT_EQ(status.value().template_id, id);
    MT_EXPECT_EQ(status.value().build_id, id);
    MT_EXPECT_TRUE(status.value().status == TemplateBuildStatus::Waiting);
    // Nothing has run, so there is no reason and no log -- and an absent
    // reason must stay absent rather than become an empty one, which a client
    // reads as a failure with no message.
    MT_EXPECT_TRUE(!status.value().reason.has_value());
    MT_EXPECT_TRUE(status.value().log_entries.empty());
}

MT_TEST(the_status_route_follows_the_record_through_its_states) {
    // The whole point of polling: `waiting` -> `building` must be visible
    // here, because this is what a client waits on.
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;
    const core::Expected<core::SnapshotId, std::string> parsed = core::SnapshotId::Parse(id);
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;

    MT_EXPECT_TRUE(record_catalog::TryStartBuild(*f.repository, parsed.value()).ok());
    const core::Expected<TemplateBuildInfoModel, ApiError> building =
        f.impl->GetTemplateBuildStatus(id, id, StatusQuery());
    MT_EXPECT_TRUE(building.ok());
    if (!building.ok()) return;
    MT_EXPECT_TRUE(building.value().status == TemplateBuildStatus::Building);
}

MT_TEST(the_status_route_reports_the_failure_reason_and_the_failing_steps_output) {
    // This is what the route is for. `GET /templates` reports *that* a build
    // failed; without the reason and the failing step's output a client has
    // no way to find out what failed, and the log document that holds the
    // answer was unreadable over HTTP.
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;
    const core::Expected<core::SnapshotId, std::string> parsed = core::SnapshotId::Parse(id);
    if (!parsed.ok()) return;
    MT_EXPECT_TRUE(record_catalog::TryStartBuild(*f.repository, parsed.value()).ok());
    MT_EXPECT_TRUE(record_catalog::MarkBuildError(*f.repository, parsed.value(),
                                  TemplateBuildErrorReason::WithStep("step 2 failed", "2"))
                       .ok());

    std::vector<tpl::BuildLogEntry> entries;
    entries.push_back(Entry(1000, core::LogRecordLevel::Info, "step 1 ok", "1"));
    entries.push_back(Entry(2000, core::LogRecordLevel::Error, "step 1 noise", "1"));
    entries.push_back(Entry(3000, core::LogRecordLevel::Info, "running step 2", "2"));
    entries.push_back(Entry(4000, core::LogRecordLevel::Error, "step 2: no such file", "2"));
    entries.push_back(Entry(5000, core::LogRecordLevel::Warn, "build aborted", NULL));
    SeedLogs(f, id, entries);

    const core::Expected<TemplateBuildInfoModel, ApiError> status =
        f.impl->GetTemplateBuildStatus(id, id, StatusQuery());
    MT_EXPECT_TRUE(status.ok());
    if (!status.ok()) return;
    MT_EXPECT_TRUE(status.value().status == TemplateBuildStatus::Error);
    MT_EXPECT_TRUE(status.value().reason.has_value());
    if (!status.value().reason.has_value()) return;
    MT_EXPECT_EQ(status.value().reason->message, std::string("step 2 failed"));
    MT_EXPECT_TRUE(status.value().reason->step.has_value());
    MT_EXPECT_EQ(*status.value().reason->step, std::string("2"));

    // The paged log is the whole log, unfiltered.
    MT_EXPECT_EQ(status.value().log_entries.size(), static_cast<std::size_t>(5));

    // The reason's log is bound twice over: to the failing step, and to
    // `warn`-and-above. Both bounds matter -- without the step filter this
    // would be every line of a build that mostly worked, and without the
    // level filter step 2's own progress output would bury its error.
    MT_EXPECT_TRUE(status.value().reason->log_entries.has_value());
    if (!status.value().reason->log_entries.has_value()) return;
    const std::vector<tpl::BuildLogEntry>& reason_logs = *status.value().reason->log_entries;
    MT_EXPECT_EQ(reason_logs.size(), static_cast<std::size_t>(1));
    if (reason_logs.empty()) return;
    MT_EXPECT_EQ(reason_logs[0].message, std::string("step 2: no such file"));
}

MT_TEST(a_reason_without_a_step_is_reported_without_logs) {
    // Absent and empty are different: an absent `logEntries` says nobody
    // looked, where an empty one says the step logged nothing. A reason with
    // no step has no step to look at, so the key must stay absent.
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;
    const core::Expected<core::SnapshotId, std::string> parsed = core::SnapshotId::Parse(id);
    if (!parsed.ok()) return;
    MT_EXPECT_TRUE(record_catalog::TryStartBuild(*f.repository, parsed.value()).ok());
    MT_EXPECT_TRUE(record_catalog::MarkBuildError(*f.repository, parsed.value(),
                                  TemplateBuildErrorReason::New("the sandbox would not boot"))
                       .ok());
    SeedLogs(f, id, std::vector<tpl::BuildLogEntry>(
                        1, Entry(1000, core::LogRecordLevel::Error, "boot failed", NULL)));

    const core::Expected<TemplateBuildInfoModel, ApiError> status =
        f.impl->GetTemplateBuildStatus(id, id, StatusQuery());
    MT_EXPECT_TRUE(status.ok());
    if (!status.ok() || !status.value().reason.has_value()) return;
    MT_EXPECT_TRUE(!status.value().reason->step.has_value());
    MT_EXPECT_TRUE(!status.value().reason->log_entries.has_value());
}

MT_TEST(the_status_log_page_starts_at_the_offset) {
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;
    std::vector<tpl::BuildLogEntry> entries;
    for (int i = 0; i < 5; ++i) {
        entries.push_back(Entry(1000 * (i + 1), core::LogRecordLevel::Info,
                                "line " + std::string(1, static_cast<char>('0' + i)), NULL));
    }
    SeedLogs(f, id, entries);

    BuildLogQuery query = StatusQuery();
    query.offset = 2;
    query.limit  = 2;
    const core::Expected<TemplateBuildInfoModel, ApiError> status =
        f.impl->GetTemplateBuildStatus(id, id, query);
    MT_EXPECT_TRUE(status.ok());
    if (!status.ok()) return;
    MT_EXPECT_EQ(status.value().log_entries.size(), static_cast<std::size_t>(2));
    if (status.value().log_entries.size() != 2) return;
    MT_EXPECT_EQ(status.value().log_entries[0].message, std::string("line 2"));
    MT_EXPECT_EQ(status.value().log_entries[1].message, std::string("line 3"));
}

MT_TEST(the_offset_indexes_the_filtered_list_not_the_stored_one) {
    // `logsOffset` is documented as an index *after* level filtering, which is
    // what lets a client page with a filter: applying it to the stored list
    // instead would skip entries the filter had already removed, so
    // consecutive pages would have gaps.
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;
    std::vector<tpl::BuildLogEntry> entries;
    entries.push_back(Entry(1000, core::LogRecordLevel::Info, "chatter", NULL));
    entries.push_back(Entry(2000, core::LogRecordLevel::Warn, "first warning", NULL));
    entries.push_back(Entry(3000, core::LogRecordLevel::Debug, "more chatter", NULL));
    entries.push_back(Entry(4000, core::LogRecordLevel::Error, "the failure", NULL));
    SeedLogs(f, id, entries);

    BuildLogQuery query = StatusQuery();
    query.level  = core::Optional<core::LogRecordLevel>(core::LogRecordLevel::Warn);
    query.offset = 1;
    const core::Expected<TemplateBuildInfoModel, ApiError> status =
        f.impl->GetTemplateBuildStatus(id, id, query);
    MT_EXPECT_TRUE(status.ok());
    if (!status.ok()) return;
    MT_EXPECT_EQ(status.value().log_entries.size(), static_cast<std::size_t>(1));
    if (status.value().log_entries.empty()) return;
    MT_EXPECT_EQ(status.value().log_entries[0].message, std::string("the failure"));
}

MT_TEST(a_level_filter_is_a_minimum_not_an_exact_match) {
    // `level=warn` must return errors too, or a client watching for problems
    // at `warn` would miss every one that was worse.
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;
    std::vector<tpl::BuildLogEntry> entries;
    entries.push_back(Entry(1000, core::LogRecordLevel::Debug, "d", NULL));
    entries.push_back(Entry(2000, core::LogRecordLevel::Info, "i", NULL));
    entries.push_back(Entry(3000, core::LogRecordLevel::Warn, "w", NULL));
    entries.push_back(Entry(4000, core::LogRecordLevel::Error, "e", NULL));
    SeedLogs(f, id, entries);

    BuildLogQuery query = StatusQuery();
    query.level = core::Optional<core::LogRecordLevel>(core::LogRecordLevel::Warn);
    const core::Expected<TemplateBuildInfoModel, ApiError> status =
        f.impl->GetTemplateBuildStatus(id, id, query);
    MT_EXPECT_TRUE(status.ok());
    if (!status.ok()) return;
    MT_EXPECT_EQ(status.value().log_entries.size(), static_cast<std::size_t>(2));
    if (status.value().log_entries.size() != 2) return;
    MT_EXPECT_EQ(status.value().log_entries[0].message, std::string("w"));
    MT_EXPECT_EQ(status.value().log_entries[1].message, std::string("e"));
}

MT_TEST(a_limit_past_the_ceiling_is_refused_rather_than_clamped) {
    // Silently reducing it would answer 200 with a short page, which a client
    // cannot tell from a build whose log really is that short -- so it would
    // stop paging early.
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;

    BuildLogQuery query = StatusQuery();
    query.limit = 101;
    const core::Expected<TemplateBuildInfoModel, ApiError> status =
        f.impl->GetTemplateBuildStatus(id, id, query);
    MT_EXPECT_TRUE(!status.ok());
    MT_EXPECT_EQ(status.error().code, 400);
    MT_EXPECT_TRUE(status.error().message.find("limit") != std::string::npos);
}

MT_TEST(a_zero_limit_asks_for_nothing_and_is_not_an_error) {
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;
    SeedLogs(f, id, std::vector<tpl::BuildLogEntry>(
                        1, Entry(1000, core::LogRecordLevel::Info, "a line", NULL)));

    BuildLogQuery query = StatusQuery();
    query.limit = 0;
    const core::Expected<TemplateBuildInfoModel, ApiError> status =
        f.impl->GetTemplateBuildStatus(id, id, query);
    MT_EXPECT_TRUE(status.ok());
    if (!status.ok()) return;
    MT_EXPECT_TRUE(status.value().log_entries.empty());
    // And the rest of the body is still there: a zero limit asks for no log
    // lines, not for no status.
    MT_EXPECT_EQ(status.value().build_id, id);
}

MT_TEST(the_status_route_ignores_the_parameters_it_does_not_declare) {
    // `cursor`, `direction` and `source` have no spelling in this route's
    // schema, and upstream passes all three literally rather than forwarding
    // whatever it was given. Pinned here so the status body's log window
    // cannot be changed by a parameter the route does not carry -- a client
    // reading the status would otherwise get a page bounded by something it
    // never sent.
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;
    std::vector<tpl::BuildLogEntry> entries;
    entries.push_back(Entry(1000, core::LogRecordLevel::Info, "first", NULL));
    entries.push_back(Entry(2000, core::LogRecordLevel::Info, "second", NULL));
    SeedLogs(f, id, entries);

    BuildLogQuery smuggled = StatusQuery();
    smuggled.backward = true;
    smuggled.cursor   = core::Optional<int64_t>(1000);
    smuggled.source   = core::Optional<LogsSource>(LogsSource::Temporary);

    const core::Expected<TemplateBuildInfoModel, ApiError> status =
        f.impl->GetTemplateBuildStatus(id, id, smuggled);
    MT_EXPECT_TRUE(status.ok());
    if (!status.ok()) return;
    // Forward, unbounded, and from the persisted document: all three
    // overrides dropped. Honouring any one of them would have given a
    // different list -- `source=temporary` alone would have emptied it.
    MT_EXPECT_EQ(status.value().log_entries.size(), static_cast<std::size_t>(2));
    if (status.value().log_entries.size() != 2) return;
    MT_EXPECT_EQ(status.value().log_entries[0].message, std::string("first"));
    MT_EXPECT_EQ(status.value().log_entries[1].message, std::string("second"));
}

MT_TEST(a_mismatched_pair_is_not_found_rather_than_a_bad_request) {
    // Deliberately *different* from the V2 start route, which answers 400 for
    // the same mismatch. Upstream splits them: the start route is asked to
    // create something from two ids and reports them as a bad request, while
    // this one is addressing `builds/{buildID}` under `{templateID}` -- and a
    // build that is not under that template is simply not there.
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;
    const std::string other = core::SnapshotId::Fresh().ToString();

    const core::Expected<TemplateBuildInfoModel, ApiError> status =
        f.impl->GetTemplateBuildStatus(id, other, StatusQuery());
    MT_EXPECT_TRUE(!status.ok());
    MT_EXPECT_EQ(status.error().code, 404);

    // The other route, same mismatch, different code.
    const core::Expected<core::Unit, ApiError> started =
        f.impl->StartTemplateBuild(id, other, StartBody());
    MT_EXPECT_TRUE(!started.ok());
    MT_EXPECT_EQ(started.error().code, 400);
}

MT_TEST(a_malformed_build_id_is_a_bad_request_on_the_status_route) {
    // 400 before anything is read, as upstream orders it. The consequence is
    // upstream's too and worth having a test say out loud: a template cannot
    // be addressed by *name* here, because a name is not a `SnapshotId` and is
    // refused before the alias lookup that `GET /templates` would have used.
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;

    const core::Expected<TemplateBuildInfoModel, ApiError> by_name =
        f.impl->GetTemplateBuildStatus("my-template", "my-template", StatusQuery());
    MT_EXPECT_TRUE(!by_name.ok());
    MT_EXPECT_EQ(by_name.error().code, 400);
    MT_EXPECT_TRUE(by_name.error().message.find("invalid buildID") != std::string::npos);
}

MT_TEST(an_unreserved_build_is_not_found) {
    Fixture f;
    const std::string absent = core::SnapshotId::Fresh().ToString();
    const core::Expected<TemplateBuildInfoModel, ApiError> status =
        f.impl->GetTemplateBuildStatus(absent, absent, StatusQuery());
    MT_EXPECT_TRUE(!status.ok());
    MT_EXPECT_EQ(status.error().code, 404);
}

MT_TEST(a_sandbox_snapshot_is_not_a_template_build) {
    // A captured sandbox is addressable by id and has no build. Reporting it
    // here would describe it as a template build that is permanently `Ready`,
    // and a client would then try to launch from a build that never ran.
    Fixture f;
    SnapshotRecord record;
    record.id                   = core::SnapshotId::Fresh();
    record.source               = SnapshotSource::Sandbox("sandbox-1");
    record.resources.cpu_count  = 1;
    record.resources.memory_mib = 256;
    f.server.Seed(DocumentKey::SnapshotRecord(record.id).Path(), record.ToJson().ToString());

    const std::string id = record.id.ToString();
    const core::Expected<TemplateBuildInfoModel, ApiError> status =
        f.impl->GetTemplateBuildStatus(id, id, StatusQuery());
    MT_EXPECT_TRUE(!status.ok());
    MT_EXPECT_EQ(status.error().code, 404);
    // And `GET /templates` does not list it either, for the same reason.
    const core::Expected<std::vector<TemplateModel>, ApiError> listed = f.impl->ListTemplates();
    MT_EXPECT_TRUE(listed.ok());
    if (!listed.ok()) return;
    MT_EXPECT_TRUE(listed.value().empty());
}

MT_TEST(a_corrupt_log_document_is_reported_rather_than_read_as_empty) {
    // An empty log and an unreadable one are different answers: the first
    // says the build has not written anything, and a client goes on waiting.
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;
    const core::Expected<core::SnapshotId, std::string> parsed = core::SnapshotId::Parse(id);
    if (!parsed.ok()) return;
    f.server.Seed(DocumentKey::BuildLog(parsed.value()).Path(), "{\"not\":\"an array\"}");

    const core::Expected<TemplateBuildInfoModel, ApiError> status =
        f.impl->GetTemplateBuildStatus(id, id, StatusQuery());
    MT_EXPECT_TRUE(!status.ok());
    MT_EXPECT_TRUE(status.error().code >= 500);
}

// ---- GET /templates/{templateID}/builds/{buildID}/logs ---------------------

MT_TEST(the_logs_route_pages_forward_from_an_inclusive_cursor) {
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;
    std::vector<tpl::BuildLogEntry> entries;
    entries.push_back(Entry(1000, core::LogRecordLevel::Info, "first", NULL));
    entries.push_back(Entry(2000, core::LogRecordLevel::Info, "second", NULL));
    entries.push_back(Entry(3000, core::LogRecordLevel::Info, "third", NULL));
    SeedLogs(f, id, entries);

    const core::Expected<BuildLogQuery, core::AnyError> query =
        BuildLogQuery::ForLogs("2000", std::string(), std::string(), std::string(),
                               std::string());
    MT_EXPECT_TRUE(query.ok());
    if (!query.ok()) return;
    const core::Expected<std::vector<tpl::BuildLogEntry>, ApiError> logs =
        f.impl->GetTemplateBuildLogs(id, id, query.value());
    MT_EXPECT_TRUE(logs.ok());
    if (!logs.ok()) return;
    // Inclusive: the entry *at* the cursor is returned, which is what lets a
    // client resume from the last timestamp it saw without having to add a
    // millisecond.
    MT_EXPECT_EQ(logs.value().size(), static_cast<std::size_t>(2));
    if (logs.value().size() != 2) return;
    MT_EXPECT_EQ(logs.value()[0].message, std::string("second"));
    MT_EXPECT_EQ(logs.value()[1].message, std::string("third"));
}

MT_TEST(a_backward_page_starts_at_the_newest_entry) {
    // Tailing a long log. Reversing has to happen before the cursor is
    // applied, or a backward page would be bounded from the wrong end and
    // return the oldest lines.
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;
    std::vector<tpl::BuildLogEntry> entries;
    entries.push_back(Entry(1000, core::LogRecordLevel::Info, "first", NULL));
    entries.push_back(Entry(2000, core::LogRecordLevel::Info, "second", NULL));
    entries.push_back(Entry(3000, core::LogRecordLevel::Info, "third", NULL));
    SeedLogs(f, id, entries);

    const core::Expected<BuildLogQuery, core::AnyError> query = BuildLogQuery::ForLogs(
        "2000", "2", std::string(), "backward", std::string());
    MT_EXPECT_TRUE(query.ok());
    if (!query.ok()) return;
    const core::Expected<std::vector<tpl::BuildLogEntry>, ApiError> logs =
        f.impl->GetTemplateBuildLogs(id, id, query.value());
    MT_EXPECT_TRUE(logs.ok());
    if (!logs.ok()) return;
    MT_EXPECT_EQ(logs.value().size(), static_cast<std::size_t>(2));
    if (logs.value().size() != 2) return;
    // Newest first, and the cursor bounds it the other way: entries at or
    // *before* 2000.
    MT_EXPECT_EQ(logs.value()[0].message, std::string("second"));
    MT_EXPECT_EQ(logs.value()[1].message, std::string("first"));
}

MT_TEST(the_temporary_source_reads_nothing_rather_than_the_persisted_log) {
    // There is no live buffer on this port, because nothing runs a build. A
    // request for the running build's log must therefore come back empty --
    // answering with the persisted document would hand the caller a
    // *previous* attempt's output and let them act on it as if it were live.
    Fixture f;
    const std::string id = Reserve(f, "my-template");
    if (id.empty()) return;
    SeedLogs(f, id, std::vector<tpl::BuildLogEntry>(
                        1, Entry(1000, core::LogRecordLevel::Info, "from an older run", NULL)));

    const core::Expected<BuildLogQuery, core::AnyError> temporary = BuildLogQuery::ForLogs(
        std::string(), std::string(), std::string(), std::string(), "temporary");
    MT_EXPECT_TRUE(temporary.ok());
    if (!temporary.ok()) return;
    const core::Expected<std::vector<tpl::BuildLogEntry>, ApiError> live =
        f.impl->GetTemplateBuildLogs(id, id, temporary.value());
    MT_EXPECT_TRUE(live.ok());
    if (!live.ok()) return;
    MT_EXPECT_TRUE(live.value().empty());

    // The persisted copy is there, so the empty answer above is the source
    // selection and not a seeding mistake.
    const core::Expected<BuildLogQuery, core::AnyError> persistent = BuildLogQuery::ForLogs(
        std::string(), std::string(), std::string(), std::string(), "persistent");
    MT_EXPECT_TRUE(persistent.ok());
    if (!persistent.ok()) return;
    const core::Expected<std::vector<tpl::BuildLogEntry>, ApiError> stored =
        f.impl->GetTemplateBuildLogs(id, id, persistent.value());
    MT_EXPECT_TRUE(stored.ok());
    if (!stored.ok()) return;
    MT_EXPECT_EQ(stored.value().size(), static_cast<std::size_t>(1));
}

MT_TEST(the_logs_route_refuses_a_build_nobody_reserved) {
    // Not an empty log: a client cannot tell that from a build that has not
    // written anything yet, and would keep polling an id that does not exist.
    Fixture f;
    const std::string absent = core::SnapshotId::Fresh().ToString();
    const core::Expected<std::vector<tpl::BuildLogEntry>, ApiError> logs =
        f.impl->GetTemplateBuildLogs(absent, absent, StatusQuery());
    MT_EXPECT_TRUE(!logs.ok());
    MT_EXPECT_EQ(logs.error().code, 404);
}

// ---- the log query parameters ----------------------------------------------

MT_TEST(the_log_query_defaults_match_the_schema) {
    const core::Expected<BuildLogQuery, core::AnyError> status =
        BuildLogQuery::ForStatus(std::string(), std::string(), std::string());
    MT_EXPECT_TRUE(status.ok());
    if (!status.ok()) return;
    MT_EXPECT_EQ(status.value().offset, static_cast<std::size_t>(0));
    // Rust `limit.unwrap_or(100)`.
    MT_EXPECT_EQ(status.value().limit, static_cast<std::size_t>(100));
    MT_EXPECT_TRUE(!status.value().level.has_value());
    MT_EXPECT_TRUE(!status.value().backward);
    MT_EXPECT_TRUE(!status.value().cursor.has_value());
    MT_EXPECT_TRUE(!status.value().source.has_value());

    const core::Expected<BuildLogQuery, core::AnyError> logs = BuildLogQuery::ForLogs(
        std::string(), std::string(), std::string(), std::string(), std::string());
    MT_EXPECT_TRUE(logs.ok());
    if (!logs.ok()) return;
    MT_EXPECT_EQ(logs.value().limit, static_cast<std::size_t>(100));
    MT_EXPECT_TRUE(!logs.value().backward);
}

MT_TEST(the_log_query_parses_every_parameter) {
    const core::Expected<BuildLogQuery, core::AnyError> status =
        BuildLogQuery::ForStatus("3", "25", "warn");
    MT_EXPECT_TRUE(status.ok());
    if (!status.ok()) return;
    MT_EXPECT_EQ(status.value().offset, static_cast<std::size_t>(3));
    MT_EXPECT_EQ(status.value().limit, static_cast<std::size_t>(25));
    MT_EXPECT_TRUE(status.value().level.has_value());
    MT_EXPECT_TRUE(*status.value().level == core::LogRecordLevel::Warn);

    const core::Expected<BuildLogQuery, core::AnyError> logs =
        BuildLogQuery::ForLogs("1700000000000", "10", "error", "backward", "persistent");
    MT_EXPECT_TRUE(logs.ok());
    if (!logs.ok()) return;
    MT_EXPECT_TRUE(logs.value().cursor.has_value());
    MT_EXPECT_EQ(*logs.value().cursor, static_cast<int64_t>(1700000000000ll));
    MT_EXPECT_EQ(logs.value().limit, static_cast<std::size_t>(10));
    MT_EXPECT_TRUE(logs.value().backward);
    MT_EXPECT_TRUE(logs.value().source.has_value());
    MT_EXPECT_TRUE(*logs.value().source == LogsSource::Persistent);

    // Not clamped at the parse. The ceiling is `QueryBuildLogs`' 400, and
    // clamping it here would make the refusal silent -- the caller would get
    // 100 entries and read that as the whole log.
    const core::Expected<BuildLogQuery, core::AnyError> over =
        BuildLogQuery::ForStatus(std::string(), "101", std::string());
    MT_EXPECT_TRUE(over.ok());
    if (!over.ok()) return;
    MT_EXPECT_EQ(over.value().limit, static_cast<std::size_t>(101));

    // `forward` is the default spelled out, so it must parse and mean the
    // same thing as an absent direction.
    const core::Expected<BuildLogQuery, core::AnyError> forward = BuildLogQuery::ForLogs(
        std::string(), std::string(), std::string(), "forward", std::string());
    MT_EXPECT_TRUE(forward.ok());
    if (!forward.ok()) return;
    MT_EXPECT_TRUE(!forward.value().backward);
}

MT_TEST(a_malformed_log_query_parameter_is_refused_not_defaulted) {
    // Each of these silently reading as "unset" would answer 200 with a page
    // the caller never asked for, and they would have no way to tell that
    // from the server honouring the request. A misspelled `direction` is the
    // sharpest: read as forward, a client asking to tail the log would get
    // the oldest page of a long build instead.
    MT_EXPECT_TRUE(!BuildLogQuery::ForStatus("abc", std::string(), std::string()).ok());
    MT_EXPECT_TRUE(!BuildLogQuery::ForStatus(std::string(), "-1", std::string()).ok());
    MT_EXPECT_TRUE(!BuildLogQuery::ForStatus(std::string(), "12x", std::string()).ok());
    MT_EXPECT_TRUE(!BuildLogQuery::ForStatus(std::string(), std::string(), "trace").ok());
    MT_EXPECT_TRUE(!BuildLogQuery::ForStatus(std::string(), std::string(), "WARN").ok());
    MT_EXPECT_TRUE(!BuildLogQuery::ForLogs("later", std::string(), std::string(), std::string(),
                                           std::string())
                        .ok());
    MT_EXPECT_TRUE(!BuildLogQuery::ForLogs(std::string(), std::string(), std::string(),
                                           "backwards", std::string())
                        .ok());
    MT_EXPECT_TRUE(!BuildLogQuery::ForLogs(std::string(), std::string(), std::string(),
                                           std::string(), "live")
                        .ok());
    // Rust's `i64::try_from` on a `u64` cursor, with its own 400: a value past
    // `i64::MAX` cannot be compared against a timestamp at all.
    MT_EXPECT_TRUE(!BuildLogQuery::ForLogs("9223372036854775808", std::string(), std::string(),
                                           std::string(), std::string())
                        .ok());
}

// ---- the build-status body --------------------------------------------------

MT_TEST(the_status_body_serialises_every_required_key) {
    SnapshotRecord record;
    record.id     = core::SnapshotId::Fresh();
    record.source = SnapshotSource::Template(TemplateBuildInfo());

    const core::Json j = TemplateBuildInfoModel::FromRecord(record).ToJson();
    MT_EXPECT_EQ(j["templateID"].as_string(), record.id.ToString());
    MT_EXPECT_EQ(j["buildID"].as_string(), record.id.ToString());
    // The lowercase wire spelling, not the record's `"Waiting"`.
    MT_EXPECT_EQ(j["status"].as_string(), std::string("waiting"));
    // Documented as always empty and still required: a client reading
    // `logs.length` must not find the key absent.
    MT_EXPECT_EQ(j.as_object().count("logs"), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(j["logs"].kind() == core::Json::Kind::Array);
    MT_EXPECT_EQ(j.as_object().count("logEntries"), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(j["logEntries"].kind() == core::Json::Kind::Array);
    // `skip_serializing_if = "Option::is_none"`: a build with no failure must
    // not carry a `reason` key at all, or a client testing for its presence
    // reads every waiting build as failed.
    MT_EXPECT_EQ(j.as_object().count("reason"), static_cast<std::size_t>(0));
}

MT_TEST(the_status_body_carries_the_reason_and_its_entries) {
    SnapshotRecord record;
    record.id = core::SnapshotId::Fresh();
    TemplateBuildInfo build;
    build.status = TemplateBuildStatus::Error;
    build.error_reason = core::Optional<TemplateBuildErrorReason>(
        TemplateBuildErrorReason::WithStep("step 2 failed", "2"));
    record.source = SnapshotSource::Template(build);

    TemplateBuildInfoModel info = TemplateBuildInfoModel::FromRecord(record);
    info.log_entries.push_back(Entry(1000, core::LogRecordLevel::Error, "boom", "2"));
    MT_EXPECT_TRUE(info.reason.has_value());
    if (!info.reason.has_value()) return;
    info.reason->log_entries = core::Optional<std::vector<tpl::BuildLogEntry> >(
        info.log_entries);

    const core::Json j = info.ToJson();
    MT_EXPECT_EQ(j["status"].as_string(), std::string("error"));
    MT_EXPECT_EQ(j["reason"]["message"].as_string(), std::string("step 2 failed"));
    // The e2b SDKs parse `step` as an index into their own recorded steps, so
    // it travels as its own field rather than being folded into the message.
    MT_EXPECT_EQ(j["reason"]["step"].as_string(), std::string("2"));
    MT_EXPECT_EQ(j["reason"]["logEntries"].as_array().size(), static_cast<std::size_t>(1));

    // One log entry, in the four keys `models::BuildLogEntry` declares. This
    // body reuses `tpl::BuildLogEntry`'s encoder because upstream's
    // `impl From<BuildLogEntry> for models::BuildLogEntry` is field-for-field;
    // the assertion is here so a change to the *document* encoder cannot
    // silently change this HTTP body.
    const core::Json& entry = j["logEntries"].as_array()[0];
    MT_EXPECT_TRUE(entry["timestamp"].as_string().find("1970-01-01T00:00:01") == 0);
    MT_EXPECT_EQ(entry["level"].as_string(), std::string("error"));
    MT_EXPECT_EQ(entry["message"].as_string(), std::string("boom"));
    MT_EXPECT_EQ(entry["step"].as_string(), std::string("2"));
}

MT_TEST(a_reason_with_no_step_omits_both_optional_keys) {
    SnapshotRecord record;
    record.id = core::SnapshotId::Fresh();
    TemplateBuildInfo build;
    build.status = TemplateBuildStatus::Error;
    build.error_reason = core::Optional<TemplateBuildErrorReason>(
        TemplateBuildErrorReason::New("the sandbox would not boot"));
    record.source = SnapshotSource::Template(build);

    const core::Json j = TemplateBuildInfoModel::FromRecord(record).ToJson();
    MT_EXPECT_EQ(j["reason"]["message"].as_string(),
                 std::string("the sandbox would not boot"));
    MT_EXPECT_EQ(j["reason"].as_object().count("step"), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(j["reason"].as_object().count("logEntries"), static_cast<std::size_t>(0));
}

MT_TEST(a_sandbox_record_carries_no_reason) {
    // A captured sandbox has no build, so it cannot have failed. Reading the
    // reason out of the default-constructed build block would be reporting a
    // failure nobody had.
    SnapshotRecord record;
    record.id     = core::SnapshotId::Fresh();
    record.source = SnapshotSource::Sandbox("sandbox-1");

    const TemplateBuildInfoModel info = TemplateBuildInfoModel::FromRecord(record);
    MT_EXPECT_TRUE(!info.reason.has_value());
    MT_EXPECT_TRUE(info.status == TemplateBuildStatus::Ready);
}

int main() { return microtest::RunAll(); }
