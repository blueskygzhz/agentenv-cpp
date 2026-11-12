#!/usr/bin/env python3
"""Mutation harness for the five served template routes: `POST /v3/templates`,
`GET /templates`, `POST /v2/templates/{templateID}/builds/{buildID}`,
`GET /templates/{templateID}/builds/{buildID}/status` and `.../logs`
(Rust `v3_templates_post`, `templates_get`,
`v2_templates_template_id_builds_build_id_post`,
`..._status_get`, `..._logs_get`).

The route was absent, and what that cost was not an endpoint but a *pipeline*.
`TemplateBuilder` and `TemplateBuildRecordFromV3Request` were both ported with
tests and reachable from nothing outside them, so `POST
/sandboxes/{id}/snapshots` was the only way to obtain a `SnapshotRecord` at
all. The record builder was the sharper case: it produced a `TemplateWaiting`
record that nobody stored, so the name it claimed was never actually reserved
and `TryStartBuild` had no record to find.

The mutations fall into two groups. The first unhooks the commit in ways that
still answer 202 -- the shape of defect that was already there, since a handler
which builds the record and drops it is indistinguishable from a correct one
if you only read the response. The second swaps the refusals for silent
success, which matters most for `imageOnly`: that flag selects a different
artifact, so ignoring it answers 202 and hands over the one thing the request
asked not to have.

Two properties of the handler are deliberately **not** in this set, because no
assertion can distinguish them:

- Where `RequireRepository` sits in the sequence. Demanding it before the id is
  minted avoids work on a node that can never serve the route, but moving it
  after the record is built produces the same error for every request -- the
  orders differ only for a request that would *also* fail to build a record,
  and then only in which of two refusals is reported.
- Reporting the ids from the stored record rather than from the local `id`.
  `record_catalog::Create` never rewrites the id, so the two are equal by
  construction; reading them from the result is a guard against a future
  backend that does, not a behaviour that can be observed now.
- Which resources and alias the start handler sizes its `TemplateBuildSpec`
  from. `TemplateBuildSpecFromStartRequest` cannot fail on either argument --
  they only reach `spec.Resources()` and `spec.Alias()`, neither of which
  validates -- and the handler builds the spec purely for the step validation
  and then drops it, because there is no executor to hand it to. Replacing
  `pending.resources` with defaults therefore changes no answer and leaves no
  trace in the store. The property is real and worth a test, so
  `the_build_spec_is_sized_from_the_reservation_not_the_start_body` asserts it
  directly against the helper; a mutation at the call site would only ever
  report as a survivor. It becomes observable when the executor lands, and
  that is the point at which a handler-level mutation belongs here.

Status is decided **by exit code only**: the suite calls `exit(1)` on the first
failing assertion, so the summary line is not printed on failure and scraping
stdout would misreport a caught mutation as a survivor.
"""

import pathlib
import shutil
import subprocess
import sys

# `tests/` is not a package and a harness runs as a script from the repo
# root, so the shared guard is imported by path rather than by package.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import mutation_residue


ROOT = pathlib.Path(__file__).resolve().parent.parent
IMPLS = ROOT / "src/api/impls.cc"
HELPERS = ROOT / "src/api/template_helpers.cc"

TARGETS = ["test_api_template_build"]

COMMIT = """    const snapshot::repository::RepositoryResult<snapshot::SnapshotRecord> created =
        snapshot::repository::record_catalog::Create(*repository.value(), record.value());"""

IMAGE_ONLY_GUARD = """    if (body.image_only.has_value() && *body.image_only) {"""

MUTATIONS = [
    # ---- the commit that was missing ---------------------------------------
    #
    # Exactly the state before this route existed: the waiting record is
    # built and never stored. The response is unchanged, so nothing a client
    # can see says the name was not reserved -- and the build it points at
    # cannot be started, because `TryStartBuild` has no record to claim.
    (
        IMPLS,
        "the_waiting_record_is_built_but_never_stored",
        COMMIT,
        """    const snapshot::repository::RepositoryResult<snapshot::SnapshotRecord> created =
        record.value();""",
    ),
    # The commit happens and its failure is swallowed. A second request for a
    # taken name then gets 202 and ids that resolve to someone else's
    # template.
    (
        IMPLS,
        "a_failed_commit_is_reported_as_success",
        """    if (!created.ok()) {""",
        """    if (false) {""",
    ),
    # `templateID` and `buildID` diverge. The V2 start route refuses unless
    # the two match, so this hands the caller a pair guaranteed to be
    # rejected.
    (
        IMPLS,
        "the_build_id_differs_from_the_template_id",
        """    response.build_id    = created.value().id.ToString();""",
        """    response.build_id    = core::SnapshotId::Fresh().ToString();""",
    ),
    # The name is not echoed back. `names`/`aliases` are what a client uses to
    # address the template afterwards.
    (
        IMPLS,
        "the_claimed_name_is_not_reported_back",
        """    response.names.push_back(name);""",
        """    // name elided""",
    ),
    (
        IMPLS,
        "the_alias_list_is_left_empty",
        """    response.aliases.push_back(name);""",
        """    // alias elided""",
    ),
    # ---- refusals turned into silent success -------------------------------
    #
    # The worst of the set: `imageOnly` asks for an image and gets a template.
    # 202 either way, and the tags point at the wrong artifact.
    (
        IMPLS,
        "an_image_only_request_is_built_as_a_template_instead",
        IMAGE_ONLY_GUARD,
        """    if (false) {""",
    ),
    # The flag is read as "present" rather than "true", so an explicit
    # `imageOnly: false` is refused -- the mirror error, and the one that
    # breaks a client sending the field as a default.
    (
        IMPLS,
        "an_explicit_image_only_false_is_refused",
        IMAGE_ONLY_GUARD,
        """    if (body.image_only.has_value()) {""",
    ),
    # A nameless request proceeds. The record builder then refuses the empty
    # alias, so this surfaces as the wrong error rather than the wrong
    # artifact -- but it is still a 400 for a field the caller did supply
    # nothing for.
    (
        IMPLS,
        "a_nameless_request_is_not_refused_up_front",
        """    if (!body.name.has_value() || body.name->empty()) {""",
        """    if (false) {""",
    ),
    # An empty name is accepted as a name. `SnapshotAlias::Parse` is what
    # catches it next, so again the error is wrong rather than absent.
    (
        IMPLS,
        "an_empty_name_counts_as_a_name",
        """    if (!body.name.has_value() || body.name->empty()) {""",
        """    if (!body.name.has_value()) {""",
    ),
    # The record builder's own 400 is flattened into a 500. A tagged name or a
    # zero cpu count is bad input, and reporting it as a server fault tells
    # the caller to retry something that cannot succeed.
    (
        IMPLS,
        "a_record_builder_rejection_becomes_a_server_error",
        """        TemplateBuildRecordFromV3Request(body, id, name);
    if (!record.ok()) return core::make_unexpected(record.error());""",
        """        TemplateBuildRecordFromV3Request(body, id, name);
    if (!record.ok()) {
        return core::make_unexpected(ApiError::Make(500, record.error().message));
    }""",
    ),

    # ---- the request DTO ---------------------------------------------------
    #
    # A negative `cpuCount` read as a huge unsigned one. It passes
    # `ResolveTemplateResources`' only check -- non-zero -- and sizes a build
    # nobody asked for.
    (
        HELPERS,
        "a_negative_u32_field_is_accepted_as_huge",
        """    if (v.kind() != core::Json::Kind::Int || v.as_int() < 0 || v.as_int() > 0xFFFFFFFFll) {""",
        """    if (v.kind() != core::Json::Kind::Int) {""",
    ),
    # A wrong-typed field reads as absent, so it silently takes the node's
    # default instead of being reported.
    (
        HELPERS,
        "a_wrong_typed_u32_field_reads_as_absent",
        """    if (v.kind() != core::Json::Kind::Int || v.as_int() < 0 || v.as_int() > 0xFFFFFFFFll) {
        return core::make_unexpected(
            core::err("invalid field: " + key + " must be a non-negative integer"));
    }""",
        """    if (v.kind() != core::Json::Kind::Int) return core::Optional<uint32_t>();""",
    ),
    # A non-boolean `imageOnly` defaults to false: the one field here whose
    # misreading satisfies the request with the wrong artifact.
    (
        HELPERS,
        "a_non_boolean_image_only_defaults_to_false",
        """    } else if (image_only.kind() != core::Json::Kind::Null) {""",
        """    } else if (false) {""",
    ),
    # A non-array `tags` is ignored rather than refused.
    (
        HELPERS,
        "a_non_array_tags_field_is_ignored",
        """    } else if (tags.kind() != core::Json::Kind::Null) {""",
        """    } else if (false) {""",
    ),
    # A non-string `name` is ignored, which turns a wrong-typed field into the
    # "name must be provided" error for a request that did provide one.
    (
        HELPERS,
        "a_non_string_name_is_ignored",
        """    } else if (name.kind() != core::Json::Kind::Null) {""",
        """    } else if (false) {""",
    ),
    # `tags: []` collapses to absent. Equivalent downstream today, so this is
    # the one mutation here that is expected to be *behaviourally* invisible
    # at the handler -- the DTO test is what pins it, because the distinction
    # is the field's whole contract.
    (
        HELPERS,
        "an_explicit_empty_tag_list_reads_as_absent",
        """        body.tags = core::Optional<std::vector<std::string> >(parsed);""",
        """        if (!parsed.empty()) body.tags = core::Optional<std::vector<std::string> >(parsed);""",
    ),
    # Every required response key must be written even when empty, or a
    # client reading `names[0]` has to handle an absent key.
    (
        HELPERS,
        "empty_response_lists_are_omitted",
        """    j["names"]   = core::Json(StringsToJson(names));
    j["tags"]    = core::Json(StringsToJson(tags));
    j["aliases"] = core::Json(StringsToJson(aliases));""",
        """    if (!names.empty()) j["names"] = core::Json(StringsToJson(names));
    if (!tags.empty()) j["tags"] = core::Json(StringsToJson(tags));
    if (!aliases.empty()) j["aliases"] = core::Json(StringsToJson(aliases));""",
    ),
    # ---- GET /templates ----------------------------------------------------
    #
    # Back to the 501 that was there, whose message blamed a store that does
    # not exist. The list is the only way a client learns whether a reserved
    # build is ready, so refusing it strands every template the route above
    # creates.
    (
        IMPLS,
        "listing_templates_refuses_instead_of_reading_the_catalog",
        """    const snapshot::repository::RepositoryResult<std::vector<snapshot::SnapshotRecord> >
        records = snapshot::repository::record_catalog::List(
            *repository.value(), snapshot::repository::SnapshotListFilter::Templates());""",
        """    const snapshot::repository::RepositoryResult<std::vector<snapshot::SnapshotRecord> >
        records = core::make_unexpected(
            snapshot::repository::RepositoryError::Backend("not ported"));""",
    ),
    # The filter is dropped, so sandbox-sourced snapshots are advertised as
    # templates. They report `Ready` only because they have no build at all,
    # which makes them look like the most launchable entries in the list.
    (
        IMPLS,
        "the_template_list_includes_sandbox_snapshots",
        """            *repository.value(), snapshot::repository::SnapshotListFilter::Templates());""",
        """            *repository.value(), snapshot::repository::SnapshotListFilter::MatchesAll());""",
    ),
    # A read failure answers 200 with an empty list, which a client cannot
    # tell from a node that genuinely has no templates.
    (
        IMPLS,
        "a_failed_catalog_read_lists_nothing",
        """    if (!records.ok()) {
        // 500 for every failure, which is both upstream's shape and the
        // truthful one here: `TemplatesGetResponse` declares no 4xx variant
        // besides the auth one, and `List` can only fail by failing to read
        // or decode the catalog -- there is no request to be at fault.
        return core::make_unexpected(InternalError(
            "list template snapshots through repository: " + records.error().ToString()));
    }""",
        """    if (!records.ok()) return std::vector<TemplateModel>();""",
    ),
    # A node with no repository answers 200 with nothing, instead of saying it
    # cannot tell.
    (
        IMPLS,
        "listing_without_a_repository_answers_an_empty_list",
        """    const core::Expected<snapshot::repository::SnapshotRepository*, ApiError> repository =
        RequireRepository();
    if (!repository.ok()) return core::make_unexpected(repository.error());

    // Rust `SnapshotListFilter::templates()`. Not `MatchesAll()`: a""",
        """    const core::Expected<snapshot::repository::SnapshotRepository*, ApiError> repository =
        RequireRepository();
    if (!repository.ok()) return std::vector<TemplateModel>();

    // Rust `SnapshotListFilter::templates()`. Not `MatchesAll()`: a""",
    ),
    # ---- the Template model ------------------------------------------------
    #
    # The record's capitalised spelling in an HTTP body whose schema declares
    # lowercase. A generated client rejects it as an unknown variant, and the
    # confusion is easy because both enums are named `TemplateBuildStatus`.
    (
        HELPERS,
        "the_build_status_is_written_in_the_record_spelling",
        """    j["buildStatus"] = core::Json(TemplateBuildStatusWireName(build_status));""",
        """    j["buildStatus"] = core::Json(snapshot::TemplateBuildStatusToString(build_status));""",
    ),
    # A sandbox-sourced record reports its build's default status, which is
    # `Waiting` -- describing an already-captured snapshot as pending.
    (
        HELPERS,
        "a_sandbox_record_reports_its_builds_default_status",
        """    if (!record.source.is_template()) return snapshot::TemplateBuildStatus::Ready;""",
        """    if (false) return snapshot::TemplateBuildStatus::Ready;""",
    ),
    # A template record reports `Ready` regardless of its build: every
    # reserved-but-unbuilt template then looks launchable.
    (
        HELPERS,
        "every_record_reports_ready",
        """    return record.source.build.status;""",
        """    return snapshot::TemplateBuildStatus::Ready;""",
    ),
    # An unaliased record gets no name, so it is invisible to anything that
    # reads `names[0]` even though it is addressable by id.
    (
        HELPERS,
        "an_unaliased_record_is_left_nameless",
        """    } else {
        model.names.push_back(record.id.ToString());
    }""",
        """    }""",
    ),
    # The id is used even when an alias exists, so the name a caller chose
    # never comes back.
    (
        HELPERS,
        "the_alias_is_ignored_in_favour_of_the_id",
        """    if (record.alias.has_value()) {
        model.names.push_back(record.alias->ToString());""",
        """    if (false) {
        model.names.push_back(record.alias->ToString());""",
    ),
    # An uncommitted record reports an empty envd version, which a client has
    # to special-case separately from a missing field.
    (
        HELPERS,
        "an_uncommitted_record_reports_an_empty_envd_version",
        """                             : std::string("unknown");""",
        """                             : std::string();""",
    ),
    # The timestamps go out as raw milliseconds. A client parsing a timestamp
    # must not have to know which of two shapes the field is in.
    (
        HELPERS,
        "the_timestamps_are_written_as_raw_millis",
        """    j["createdAt"]   = core::Json(
        core::FormatRfc3339Nanos(core::SystemTime(created_at_unix_ms * 1000000)));""",
        """    j["createdAt"]   = core::Json(created_at_unix_ms);""",
    ),
    # `lastSpawnedAt` is omitted rather than null. The key is required and
    # nullable, so a client finds it absent where the schema promises it.
    (
        HELPERS,
        "a_null_last_spawned_at_is_omitted",
        """    j["lastSpawnedAt"] =
        last_spawned_at_unix_ms.has_value()
            ? core::Json(core::FormatRfc3339Nanos(
                  core::SystemTime(*last_spawned_at_unix_ms * 1000000)))
            : core::Json();""",
        """    if (last_spawned_at_unix_ms.has_value()) {
        j["lastSpawnedAt"] = core::Json(core::FormatRfc3339Nanos(
            core::SystemTime(*last_spawned_at_unix_ms * 1000000)));
    }""",
    ),
    # The machine shape is dropped. These are what a sandbox launched from the
    # template will be sized with, and zeros read as a template that cannot
    # run.
    (
        HELPERS,
        "the_machine_shape_is_not_carried_into_the_entry",
        """    model.cpu_count    = record.resources.cpu_count;
    model.memory_mb    = record.resources.memory_mib;""",
        """    model.cpu_count    = 0;
    model.memory_mb    = 0;""",
    ),
    # ---- POST /v2/templates/{id}/builds/{id} --------------------------------
    #
    # The ordering rule the whole handler is arranged around: the refusal must
    # land *before* `TryStartBuild`. That call moves the record `waiting` ->
    # `building`, and only a `waiting` record can be claimed -- so claiming and
    # then failing to build leaves the template permanently unstartable, dead
    # even once the executor lands. Here the claim is inserted ahead of the
    # refusal, which is the mistake a future author adding the executor is
    # most likely to make.
    (
        IMPLS,
        "the_build_is_claimed_before_the_refusal",
        """    return core::make_unexpected(ApiError::Make(
        501, "starting a template build is not available in this build """,
        """    (void)snapshot::repository::record_catalog::TryStartBuild(*repository.value(),
                                                             parsed_build_id.value());
    return core::make_unexpected(ApiError::Make(
        501, "starting a template build is not available in this build """,
    ),
    # The two path segments need not agree. AgentENV keeps one build per
    # template, so a build started under a different id runs against a
    # reservation nobody asked to use.
    (
        IMPLS,
        "the_two_path_ids_need_not_agree",
        """    if (template_id != build_id) {
        return core::make_unexpected(ApiError::Make(
            400, "templateID and buildID must match in AgentENV compatibility mode"));""",
        """    if (false) {
        return core::make_unexpected(ApiError::Make(
            400, "templateID and buildID must match in AgentENV compatibility mode"));""",
    ),
    # The record found by alias need not be the one `buildID` names. The
    # lookup accepts either, so this runs the build against someone else's
    # reservation and publishes it under their name.
    (
        IMPLS,
        "a_record_reached_by_alias_need_not_match_the_build_id",
        """    if (pending.id != parsed_build_id.value()) {""",
        """    if (false) {""",
    ),
    # A missing template reads as a server fault instead of a 404, so a
    # caller cannot tell a typo from an outage.
    (
        IMPLS,
        "an_unknown_template_is_reported_as_a_server_error",
        """    if (!found.value().has_value()) {
        return core::make_unexpected(
            ApiError::Make(404, "template " + template_id + " not found"));
    }""",
        """    if (!found.value().has_value()) {
        return core::make_unexpected(InternalError("template not found"));
    }""",
    ),
    # A malformed build id is accepted and the lookup decides, which turns a
    # 400 into a 404 for a request that could never have been valid.
    (
        IMPLS,
        "a_malformed_build_id_is_not_rejected",
        """    if (!parsed_build_id.ok()) {
        return core::make_unexpected(ApiError::Make(400, "invalid buildID: " + build_id));
    }""",
        """    if (!parsed_build_id.ok()) {
        return core::make_unexpected(ApiError::Make(404, "invalid buildID: " + build_id));
    }""",
    ),
    # The base-source exclusivity check is skipped, so a body naming both an
    # image and a template base is accepted and one of the two silently wins.
    (
        IMPLS,
        "a_bad_base_source_is_not_refused",
        """    if (!base_source.ok()) return core::make_unexpected(base_source.error());""",
        """    if (!base_source.ok() && false) return core::make_unexpected(base_source.error());""",
    ),
    # A malformed step is hidden behind the capability refusal. The caller is
    # told the feature is missing when what is actually wrong is their body --
    # so they wait for a release that will not fix it.
    (
        IMPLS,
        "a_malformed_spec_is_hidden_behind_the_501",
        """    if (!spec.ok()) return core::make_unexpected(spec.error());""",
        """    if (!spec.ok() && false) return core::make_unexpected(spec.error());""",
    ),
    # A registry block is accepted and ignored, so the pull happens
    # anonymously: either an auth failure the caller already solved, or a
    # different public image of the same name, published as their template.
    (
        IMPLS,
        "a_registry_block_is_silently_ignored",
        """    if (body.from_image_registry.has_value()) {""",
        """    if (false) {""",
    ),
    # The registry block's own 400 collapses into the capability 501, which
    # tells the caller to wait for a feature when their request is malformed.
    (
        IMPLS,
        "a_registry_block_without_an_image_reports_the_capability_error",
        """            return core::make_unexpected(ApiError::Make(
                400, "fromImageRegistry requires fromImage and cannot be used with "
                     "fromTemplate"));""",
        """            // fall through to the capability refusal""",
    ),
    # ---- the V2 start request DTO ------------------------------------------
    #
    # A step with no `type` defaults to an empty one, which
    # `ApplyE2bTemplateStep` reports as an unknown step type -- blaming a
    # value the caller never sent.
    (
        HELPERS,
        "a_step_without_a_type_defaults_to_an_empty_one",
        """    const core::Json& type = j["type"];
    if (type.kind() != core::Json::Kind::String) {
        return core::make_unexpected(core::err("invalid field: steps[].type must be a string"));
    }
    step.type = type.as_string();""",
        """    const core::Json& type = j["type"];
    if (type.kind() == core::Json::Kind::String) step.type = type.as_string();""",
    ),
    # A wrong-typed string field reads as absent, so `fromImage` of `42`
    # silently builds the configured default image instead.
    (
        HELPERS,
        "a_wrong_typed_start_string_reads_as_absent",
        """    if (v.kind() != core::Json::Kind::String) {
        return core::make_unexpected(core::err("invalid field: " + key + " must be a string"));
    }
    return core::Optional<std::string>(v.as_string());""",
        """    if (v.kind() != core::Json::Kind::String) return core::Optional<std::string>();
    return core::Optional<std::string>(v.as_string());""",
    ),
    # Likewise for `force`.
    (
        HELPERS,
        "a_wrong_typed_start_bool_reads_as_absent",
        """    if (v.kind() != core::Json::Kind::Bool) {
        return core::make_unexpected(core::err("invalid field: " + key + " must be a boolean"));
    }
    return core::Optional<bool>(v.as_bool());""",
        """    if (v.kind() != core::Json::Kind::Bool) return core::Optional<bool>();
    return core::Optional<bool>(v.as_bool());""",
    ),
    # A non-array `steps` is ignored, so a build that asked for steps runs
    # with none and publishes an unmodified base image as the template.
    (
        HELPERS,
        "a_non_array_steps_field_is_ignored",
        """    } else if (steps.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(core::err("invalid field: steps must be an array"));
    }""",
        """    }""",
    ),
    # A non-object entry inside `steps` is skipped rather than refused.
    (
        HELPERS,
        "a_non_object_step_entry_is_accepted",
        """    if (j.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(core::err("invalid field: steps must contain objects"));
    }""",
        """    if (j.kind() != core::Json::Kind::Object) return TemplateStepRequest();""",
    ),
    # `args: []` collapses to absent. `ApplyE2bTemplateStep` reads the arity
    # to decide whether a step is well-formed, so the two must stay distinct.
    (
        HELPERS,
        "an_explicit_empty_step_arg_list_reads_as_absent",
        """        step.args = core::Optional<std::vector<std::string> >(parsed);""",
        """        if (!parsed.empty()) step.args = core::Optional<std::vector<std::string> >(parsed);""",
    ),
    # A non-object `fromImageRegistry` is accepted as absent, so a malformed
    # credential block reads as "no credentials" and the pull goes anonymous.
    (
        HELPERS,
        "a_non_object_registry_block_reads_as_absent",
        """    } else if (registry.kind() != core::Json::Kind::Null) {
        return core::make_unexpected(
            core::err("invalid field: fromImageRegistry must be an object"));
    }""",
        """    }""",
    ),

    # ---- GET .../builds/{id}/status and .../logs ----------------------------
    #
    # Both routes were unrouted, and what that cost was the only answer to
    # "why did my build fail". `GET /templates` reports *that* a build failed
    # and carries neither a reason nor a log line, and `ReadBuildLogs` was
    # ported with no caller -- so the document holding the answer could be
    # written and never read.
    #
    # The record lookup first. A disagreeing pair is a 404 here and a 400 on
    # the V2 start route; collapsing the two would be easy and wrong, because
    # a client distinguishes "you asked badly" from "that is not here".
    (
        IMPLS,
        "a_mismatched_pair_is_accepted_on_the_status_route",
        """    if (template_id != build_id) {
        return core::make_unexpected(ApiError::Make(404, "build not found for template"));
    }""",
        """    if (false) {
        return core::make_unexpected(ApiError::Make(404, "build not found for template"));
    }""",
    ),
    (
        IMPLS,
        "a_mismatched_pair_is_reported_as_a_bad_request",
        """        return core::make_unexpected(ApiError::Make(404, "build not found for template"));""",
        """        return core::make_unexpected(ApiError::Make(400, "build not found for template"));""",
    ),
    # A malformed id becomes a 404, which tells the caller the build might
    # exist under a different name when the id could never have been valid.
    (
        IMPLS,
        "a_malformed_build_id_is_a_not_found_on_the_status_route",
        """        return core::make_unexpected(ApiError::Make(400, "invalid buildID"));""",
        """        return core::make_unexpected(ApiError::Make(404, "invalid buildID"));""",
    ),
    # A captured sandbox is reported as a template build. It has no build at
    # all, so it reads as permanently `Ready` -- the most launchable-looking
    # answer this route can give, for something that never ran.
    (
        IMPLS,
        "a_sandbox_snapshot_is_served_as_a_template_build",
        """    if (!found.value()->source.is_template()) {""",
        """    if (false) {""",
    ),
    # The log ceiling is clamped instead of refused: a caller asking for 500
    # gets 100 and cannot tell that from a build whose log is 100 lines long,
    # so it stops paging.
    (
        IMPLS,
        "a_limit_past_the_ceiling_is_accepted",
        """    if (query.limit > 100) {
        return core::make_unexpected(ApiError::Make(400, "limit must be between 0 and 100"));
    }""",
        """    // ceiling unchecked""",
    ),
    # `limit=0` stops being a request for nothing.
    (
        IMPLS,
        "a_zero_limit_still_returns_an_entry",
        """    if (query.limit == 0) return std::vector<tpl::BuildLogEntry>();""",
        """    if (false) return std::vector<tpl::BuildLogEntry>();""",
    ),
    # The worst of this set: a request for the *running* build's log is
    # answered with the persisted document. There is no live buffer here, so
    # what comes back is a previous attempt's output -- presented as the
    # current one's, which is how a client decides a build is failing.
    (
        IMPLS,
        "the_temporary_source_is_served_the_persisted_log",
        """    if (!(query.source.has_value() && *query.source == LogsSource::Temporary)) {""",
        """    if (true) {""",
    ),
    # A log that cannot be read is reported as a log with nothing in it, so a
    # client goes on waiting for lines that will never be parsed.
    (
        IMPLS,
        "an_unreadable_log_reads_as_an_empty_one",
        """        if (!read.ok()) {
            return core::make_unexpected(InternalError(
                "read build logs for '" + id.ToString() + "': " + read.error().ToString()));
        }
        entries = read.value();""",
        """        if (read.ok()) entries = read.value();""",
    ),
    # `level` as an exact match. `level=warn` then hides every error, which is
    # precisely the line a client filtering at `warn` is looking for.
    (
        IMPLS,
        "a_level_filter_becomes_an_exact_match",
        """        if (query.level.has_value() && entry.level < *query.level) continue;""",
        """        if (query.level.has_value() && entry.level != *query.level) continue;""",
    ),
    # The cursor turns exclusive. A client resuming from the last timestamp it
    # saw then skips an entry per page.
    (
        IMPLS,
        "the_cursor_bound_becomes_exclusive",
        """            if (query.backward ? entry_ms > *query.cursor : entry_ms < *query.cursor) {""",
        """            if (query.backward ? entry_ms >= *query.cursor : entry_ms <= *query.cursor) {""",
    ),
    # The cursor is bounded from the wrong end on a backward page, so tailing
    # a log returns the oldest lines.
    (
        IMPLS,
        "a_backward_cursor_is_bounded_forwards",
        """            if (query.backward ? entry_ms > *query.cursor : entry_ms < *query.cursor) {""",
        """            if (entry_ms < *query.cursor) {""",
    ),
    # `direction=backward` stops reversing, so the newest-first page is
    # oldest-first and a tail shows the start of the build.
    (
        IMPLS,
        "a_backward_page_is_not_reversed",
        """    if (query.backward) std::reverse(entries.begin(), entries.end());""",
        """    // not reversed""",
    ),
    # `logsOffset` is ignored: every page is the first one, so a client paging
    # through a long log loops on the same entries forever.
    (
        IMPLS,
        "the_log_offset_is_ignored",
        """        if (skipped < query.offset) {
            ++skipped;
            continue;
        }""",
        """        if (false) {
            ++skipped;
            continue;
        }""",
    ),
    # The offset is applied to the stored list rather than the filtered one,
    # so paging with a `level` filter leaves gaps between pages.
    (
        IMPLS,
        "the_offset_indexes_the_stored_list",
        """        if (query.level.has_value() && entry.level < *query.level) continue;""",
        """        if (skipped < query.offset) {
            ++skipped;
            continue;
        }
        if (query.level.has_value() && entry.level < *query.level) continue;""",
    ),
    # `limit` is not applied, so one request answers with the whole log --
    # which is bounded at 8 MiB, and the point of the ceiling is that a client
    # does not have to receive it.
    (
        IMPLS,
        "the_log_limit_is_not_applied",
        """        if (out.size() >= query.limit) break;""",
        """        // no limit""",
    ),
    # The reason's logs stop being bound to the failing step, so the "why did
    # it fail" field becomes the tail of a build that mostly worked.
    (
        IMPLS,
        "the_reason_logs_are_not_bound_to_the_failing_step",
        """        if (!entry.step.has_value() || *entry.step != step) continue;""",
        """        if (false) continue;""",
    ),
    # ...and stop being bound to `warn`, so the failing step's own progress
    # output buries the error inside it.
    (
        IMPLS,
        "the_reason_logs_include_the_steps_informational_output",
        """        if (entry.level < core::LogRecordLevel::Warn) continue;""",
        """        if (false) continue;""",
    ),
    # The reason is reported with no logs at all: the status says a build
    # failed and the output that says why is left unread, which is the state
    # this route was added to end.
    (
        IMPLS,
        "the_failure_reason_is_reported_without_its_logs",
        """    if (info.reason.has_value() && info.reason->step.has_value()) {""",
        """    if (false) {""",
    ),
    # The status route honours `cursor`/`direction`/`source` from its caller.
    # Its schema has no spelling for any of them, so the body's log window
    # would be bounded by something no client sent.
    (
        IMPLS,
        "the_status_route_forwards_parameters_it_cannot_carry",
        """    BuildLogQuery page = logs;
    page.cursor   = core::Optional<int64_t>();
    page.backward = false;
    page.source   = core::Optional<LogsSource>();""",
        """    const BuildLogQuery& page = logs;""",
    ),
    # The logs route skips the record lookup, so a build nobody reserved
    # answers 200 with an empty list instead of 404 -- and a client polls an
    # id that does not exist.
    (
        IMPLS,
        "the_logs_route_serves_a_build_that_does_not_exist",
        """    const core::Expected<snapshot::SnapshotRecord, ApiError> record =
        BuildRecordFor(template_id, build_id);
    if (!record.ok()) return core::make_unexpected(record.error());
    return QueryBuildLogs(record.value().id, logs);""",
        """    const core::Expected<snapshot::SnapshotRecord, ApiError> record =
        BuildRecordFor(template_id, build_id);
    if (!record.ok()) return std::vector<tpl::BuildLogEntry>();
    return QueryBuildLogs(record.value().id, logs);""",
    ),

    # ---- the status body ---------------------------------------------------
    #
    # A sandbox record's default build block is read as a failure, so a
    # captured snapshot reports an error reason nobody recorded.
    (
        HELPERS,
        "a_sandbox_record_reports_a_failure_reason",
        """    if (record.source.is_template() && record.source.build.error_reason.has_value()) {""",
        """    if (record.source.build.error_reason.has_value()) {""",
    ),
    # `reason` is written for every build. A client testing for the key's
    # presence then reads every waiting build as failed.
    (
        HELPERS,
        "the_reason_key_is_always_written",
        """    if (reason.has_value()) j["reason"] = reason->ToJson();""",
        """    j["reason"] = reason.has_value() ? reason->ToJson() : core::Json();""",
    ),
    # `step` goes out as null. The e2b SDKs parse it as an integer index into
    # their own recorded steps, and a null crashes that path before it can
    # show the build error.
    (
        HELPERS,
        "a_reason_without_a_step_writes_null",
        """    if (step.has_value()) j["step"] = core::Json(*step);""",
        """    j["step"] = step.has_value() ? core::Json(*step) : core::Json();""",
    ),
    # `logEntries` on the reason becomes an empty array when nobody looked.
    # Absent means "not fetched" and empty means "the step logged nothing";
    # merging them tells a client the failing step was silent.
    (
        HELPERS,
        "an_unfetched_reason_log_reads_as_an_empty_one",
        """    if (log_entries.has_value()) {
        core::JsonArray entries;
        for (std::size_t i = 0; i < log_entries->size(); ++i) {
            entries.emplace_back((*log_entries)[i].ToJson());
        }
        j["logEntries"] = core::Json(entries);
    }""",
        """    core::JsonArray entries;
    if (log_entries.has_value()) {
        for (std::size_t i = 0; i < log_entries->size(); ++i) {
            entries.emplace_back((*log_entries)[i].ToJson());
        }
    }
    j["logEntries"] = core::Json(entries);""",
    ),
    # The deprecated-but-required `logs` key is dropped, so a client reading
    # `logs.length` finds nothing where the schema promises an array.
    (
        HELPERS,
        "the_required_empty_logs_key_is_omitted",
        """    j["logs"] = core::Json(StringsToJson(logs));""",
        """    if (!logs.empty()) j["logs"] = core::Json(StringsToJson(logs));""",
    ),
    # The status goes out in the record's capitalised spelling -- the same
    # trap as on `GET /templates`, and worth pinning twice because the two
    # bodies are written by different code.
    (
        HELPERS,
        "the_status_body_uses_the_record_spelling",
        """    j["status"]     = core::Json(TemplateBuildStatusWireName(status));""",
        """    j["status"]     = core::Json(snapshot::TemplateBuildStatusToString(status));""",
    ),

    # ---- the log query parameters ------------------------------------------
    #
    # A malformed `limit` reads as unset, so the answer is a 200 with a page
    # size the caller never asked for and no way to tell.
    (
        HELPERS,
        "a_malformed_query_int_reads_as_absent",
        """            return core::make_unexpected(
                core::err("invalid query parameter: " + std::string(name) +
                          " must be a non-negative integer"));""",
        """            return core::Optional<int64_t>();""",
    ),
    # A cursor past `i64::MAX` wraps instead of being refused, so it lands on
    # a negative timestamp and the bound stops filtering anything.
    (
        HELPERS,
        "an_overflowing_cursor_wraps",
        """        if (value > (0x7FFFFFFFFFFFFFFFull - digit) / 10) {
            return core::make_unexpected(core::err(
                "invalid query parameter: " + std::string(name) + " exceeds int64 range"));
        }""",
        """        // overflow unchecked""",
    ),
    # An unknown `level` is ignored, so `level=trace` quietly returns debug
    # lines too.
    (
        HELPERS,
        "an_unknown_level_is_ignored",
        """        if (!decoded.has_value()) {
            return core::make_unexpected(
                core::err("invalid query parameter: unknown level '" + level + "'"));
        }""",
        """        if (!decoded.has_value()) return query;""",
    ),
    # A misspelled `direction` reads as forward. A client asking to tail a
    # long log gets the oldest page instead, and nothing says why.
    (
        HELPERS,
        "a_misspelled_direction_reads_as_forward",
        """        } else if (direction != "forward") {
            return core::make_unexpected(
                core::err("invalid query parameter: unknown direction '" + direction + "'"));
        }""",
        """        }""",
    ),
    # A misspelled `source` reads as unset, which selects the persisted
    # document -- the opposite of what a client asking for `live` wanted.
    (
        HELPERS,
        "a_misspelled_source_reads_as_absent",
        """        } else {
            return core::make_unexpected(
                core::err("invalid query parameter: unknown source '" + source + "'"));
        }""",
        """        }""",
    ),
    # `limit` is clamped at the parse, which moves the refusal out of
    # `QueryBuildLogs` and makes the ceiling silent again.
    (
        HELPERS,
        "the_limit_is_clamped_at_the_parse",
        """    if (parsed_limit.value().has_value()) {
        query.limit = static_cast<std::size_t>(*parsed_limit.value());
    }""",
        """    if (parsed_limit.value().has_value()) {
        query.limit = static_cast<std::size_t>(*parsed_limit.value());
        if (query.limit > 100) query.limit = 100;
    }""",
    ),
]


def build(target):
    return subprocess.run(
        ["cmake", "--build", "build", "-j", "8", "--target", target],
        cwd=ROOT, capture_output=True, text=True,
    ).returncode == 0


def run(target):
    try:
        return subprocess.run([str(ROOT / "build/bin" / target)],
                              cwd=ROOT, capture_output=True, text=True,
                              timeout=180).returncode
    except subprocess.TimeoutExpired:
        return 124


def build_all():
    return all(build(t) for t in TARGETS)


def failing_targets():
    """Targets whose suite reported a failure, i.e. noticed the mutation."""
    return [t for t in TARGETS if run(t) != 0]


def main():
    # Refuses if a previous run was killed mid-mutation: taking a baseline
    # from a tree that still holds one would copy the mutation into the
    # backup and lose the original. See tests/mutation_residue.py.
    mutation_residue.refuse_if_residue()
    # A full pass rebuilds once per mutation and routinely outlives a
    # command timeout; without this, SIGTERM skips the restore below.
    mutation_residue.restore_on_signal()

    sources = sorted({path for path, _, _, _ in MUTATIONS})
    original = {path: path.read_text() for path in sources}
    backups = {}
    for path in sources:
        backup = path.with_suffix(path.suffix + ".mutation-backup")
        shutil.copy(path, backup)
        backups[path] = backup

    if not build_all():
        print("BASELINE BUILD FAILED")
        return 2
    red = failing_targets()
    if red:
        print(f"BASELINE TESTS FAILED: {', '.join(red)}")
        return 2
    print("baseline: build ok, tests pass\n")

    caught, survived, broken, noop = [], [], [], []
    try:
        for path, name, old, new in MUTATIONS:
            for p in sources:
                p.write_text(original[p])
            assert original[path].count(old) == 1, \
                f"anchor for {name} matched {original[path].count(old)} times"
            if old == new:
                noop.append(name)
                print(f"  NO-OP    {name} (mutation is identity)")
                continue
            path.write_text(original[path].replace(old, new, 1))

            if not build_all():
                broken.append(name)
                print(f"  BROKEN   {name} (did not compile)")
                continue
            red = failing_targets()
            if red:
                caught.append(name)
                print(f"  caught   {name}")
            else:
                survived.append(name)
                print(f"  SURVIVED {name}")
    finally:
        for path, backup in backups.items():
            shutil.copy(backup, path)
            backup.unlink()
        build_all()

    print(f"\n{len(caught)}/{len(MUTATIONS)} caught, "
          f"{len(survived)} survived, {len(broken)} did not compile, "
          f"{len(noop)} were no-ops")
    for label, group in (("survivors", survived),
                         ("did not compile", broken),
                         ("no-ops", noop)):
        if group:
            print(f"{label}:")
            for name in group:
                print(f"  - {name}")
    return 1 if (survived or broken or noop) else 0


if __name__ == "__main__":
    raise SystemExit(main())
