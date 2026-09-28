// SPDX-License-Identifier: MIT
// Rust: src/snapshot/types/snapshot.rs and src/snapshot/types/drive.rs test
// modules.
#include <string>
#include <vector>

#include "agentenv/snapshot/drive.h"
#include "agentenv/snapshot/record.h"
#include "microtest.h"

namespace {

using agentenv::core::Json;
using agentenv::core::JsonObject;
using agentenv::core::Optional;
using agentenv::core::SnapshotId;
using agentenv::core::Uuid;
namespace snapshot = agentenv::snapshot;
namespace sandbox = agentenv::sandbox;

SnapshotId FixtureId() {
    Uuid uuid;
    MT_EXPECT_TRUE(Uuid::Parse("01936f8e-72f5-7000-8000-000000000001", &uuid));
    return SnapshotId(uuid);
}

snapshot::SnapshotAlias Alias(const std::string& value) {
    const agentenv::core::Expected<snapshot::SnapshotAlias, std::string> alias =
        snapshot::SnapshotAlias::Parse(value);
    MT_EXPECT_TRUE(alias.ok());
    return alias.value();
}

std::vector<std::string> Strings(const char* a = NULL, const char* b = NULL) {
    std::vector<std::string> out;
    if (a != NULL) out.push_back(a);
    if (b != NULL) out.push_back(b);
    return out;
}

snapshot::ManagedLayer Managed(const std::string& digest, uint64_t size) {
    snapshot::ManagedLayer layer;
    layer.digest = digest;
    layer.size = size;
    return layer;
}

/// Rust `CommittedSnapshot::mock`.
snapshot::CommittedSnapshot MockCommitted() {
    snapshot::CommittedSnapshot committed;
    committed.runtime_versions = snapshot::SnapshotRuntimeVersions::New(
        "kernel", "firecracker", "envd", "0.1.0");
    return committed;
}

snapshot::ResolvedAttachedDrive SampleResolved() {
    // Rust `sample_resolved`.
    snapshot::ResolvedAttachedDrive drive;
    drive.drive_id = "data";
    drive.image_config_path = "/tmp/image.json";
    drive.read_only = false;
    drive.virtual_size = 4096;
    drive.mount_path = sandbox::ExtraDrive::DefaultMountPath("data");
    return drive;
}

Json Parse(const std::string& text) {
    const agentenv::core::Expected<Json, agentenv::core::AnyError> json = Json::Parse(text);
    MT_EXPECT_TRUE(json.ok());
    return json.value();
}

}  // namespace

// ---------------------------------------------------------------------------
// CommandContext
// ---------------------------------------------------------------------------

MT_TEST(command_context_normalizes_a_blank_workdir) {
    // A blank workdir would become `cd ""` at boot, so it degrades to `/`.
    MT_EXPECT_EQ(snapshot::NormalizeWorkdir(""), std::string("/"));
    MT_EXPECT_EQ(snapshot::NormalizeWorkdir("   "), std::string("/"));
    MT_EXPECT_EQ(snapshot::NormalizeWorkdir("\t\n"), std::string("/"));
    MT_EXPECT_EQ(snapshot::NormalizeWorkdir("/srv"), std::string("/srv"));

    std::map<std::string, std::string> env;
    MT_EXPECT_EQ(snapshot::CommandContext::New(env, "").workdir, std::string("/"));
    // Rust `unwrap_or_default()` yields "", which normalises the same way.
    MT_EXPECT_EQ(
        snapshot::CommandContext::FromEnvAndWorkdir(env, Optional<std::string>()).workdir,
        std::string("/"));
    MT_EXPECT_EQ(snapshot::CommandContext::FromEnvAndWorkdir(
                     env, Optional<std::string>(std::string("/srv")))
                     .workdir,
                 std::string("/srv"));
}

MT_TEST(command_context_default_matches_rust) {
    const snapshot::CommandContext context;
    // Rust's Default sets "/" rather than an empty string.
    MT_EXPECT_EQ(context.workdir, std::string("/"));
    MT_EXPECT_TRUE(context.env_vars.empty());
    MT_EXPECT_TRUE(!context.user.has_value());
    MT_EXPECT_TRUE(!context.entrypoint.has_value());
    MT_EXPECT_TRUE(!context.cmd.has_value());
}

MT_TEST(command_context_builders_chain) {
    snapshot::CommandContext context;
    context.WithEnvVar("A", "1").WithWorkdir("/srv").WithUser(
        Optional<std::string>(std::string("root")));

    MT_EXPECT_EQ(context.env_vars["A"], std::string("1"));
    MT_EXPECT_EQ(context.workdir, std::string("/srv"));
    MT_EXPECT_EQ(*context.user, std::string("root"));

    // `extend` replaces an existing key rather than keeping the old value.
    std::map<std::string, std::string> overrides;
    overrides["A"] = "2";
    overrides["B"] = "3";
    context.WithEnvOverrides(overrides);
    MT_EXPECT_EQ(context.env_vars["A"], std::string("2"));
    MT_EXPECT_EQ(context.env_vars["B"], std::string("3"));
}

MT_TEST(effective_start_cmd_joins_entrypoint_and_cmd) {
    snapshot::CommandContext context;
    MT_EXPECT_TRUE(!context.EffectiveStartCmd().has_value());

    context.entrypoint = Optional<std::vector<std::string> >(Strings("/bin/sh", "-c"));
    context.cmd = Optional<std::vector<std::string> >(Strings("echo hi"));

    // Each argument is quoted separately: the result goes to `bash -lc`, so
    // the space inside `echo hi` must not split it into two words.
    const Optional<std::string> command = context.EffectiveStartCmd();
    MT_EXPECT_TRUE(command.has_value());
    MT_EXPECT_TRUE(command->find("echo hi") != std::string::npos);
    MT_EXPECT_TRUE(command->find("/bin/sh") != std::string::npos);
}

MT_TEST(effective_start_cmd_is_absent_when_both_parts_are_empty) {
    snapshot::CommandContext context;
    // Present but empty is still "nothing to run".
    context.entrypoint = Optional<std::vector<std::string> >(std::vector<std::string>());
    context.cmd = Optional<std::vector<std::string> >(std::vector<std::string>());
    MT_EXPECT_TRUE(!context.EffectiveStartCmd().has_value());

    // Only a cmd is enough.
    context.cmd = Optional<std::vector<std::string> >(Strings("run"));
    MT_EXPECT_TRUE(context.EffectiveStartCmd().has_value());
}

MT_TEST(command_context_round_trips_and_omits_empty_fields) {
    snapshot::CommandContext context;
    context.env_vars["PATH"] = "/usr/bin";
    context.workdir = "/srv";

    const Json json = context.ToJson();
    const JsonObject& fields = json.as_object();
    // `skip_serializing_if` on every optional/collection field.
    MT_EXPECT_TRUE(fields.find("user") == fields.end());
    MT_EXPECT_TRUE(fields.find("entrypoint") == fields.end());
    MT_EXPECT_TRUE(fields.find("exposed_ports") == fields.end());
    MT_EXPECT_TRUE(fields.find("labels") == fields.end());

    MT_EXPECT_TRUE(snapshot::CommandContext::FromJson(json).value() == context);
}

MT_TEST(command_context_preserves_an_explicitly_empty_entrypoint) {
    // Absent and empty are distinct for OCI, so the round trip must keep the
    // optional engaged.
    snapshot::CommandContext context;
    context.entrypoint = Optional<std::vector<std::string> >(std::vector<std::string>());

    const snapshot::CommandContext parsed =
        snapshot::CommandContext::FromJson(context.ToJson()).value();
    MT_EXPECT_TRUE(parsed.entrypoint.has_value());
    MT_EXPECT_EQ(parsed.entrypoint->size(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// TemplateBuildErrorReason
// ---------------------------------------------------------------------------

MT_TEST(build_error_reason_reads_the_legacy_bare_string) {
    // Rust's `#[serde(untagged)]` falls back to a bare string, which is how
    // records written before `step` existed stay readable.
    const agentenv::core::Expected<snapshot::TemplateBuildErrorReason, std::string> legacy =
        snapshot::TemplateBuildErrorReason::FromJson(Json("boom"));
    MT_EXPECT_TRUE(legacy.ok());
    MT_EXPECT_EQ(legacy.value().message, std::string("boom"));
    MT_EXPECT_TRUE(!legacy.value().step.has_value());
}

MT_TEST(build_error_reason_round_trips_the_structured_form) {
    const snapshot::TemplateBuildErrorReason reason =
        snapshot::TemplateBuildErrorReason::WithStep("boom", "step-3");
    const agentenv::core::Expected<snapshot::TemplateBuildErrorReason, std::string> parsed =
        snapshot::TemplateBuildErrorReason::FromJson(reason.ToJson());
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value() == reason);

    // Always written structured, never as the legacy string.
    MT_EXPECT_TRUE(reason.ToJson().kind() == Json::Kind::Object);
    MT_EXPECT_TRUE(!snapshot::TemplateBuildErrorReason::FromJson(Parse("{}")).ok());
}

// ---------------------------------------------------------------------------
// attached drives
// ---------------------------------------------------------------------------

MT_TEST(to_extra_drive_preserves_fields) {
    // Rust: `to_extra_drive_preserves_fields`.
    const sandbox::ExtraDrive drive = SampleResolved().ToExtraDrive();

    MT_EXPECT_EQ(drive.drive_id, std::string("data"));
    MT_EXPECT_TRUE(!drive.read_only);
    MT_EXPECT_EQ(drive.mount_path, std::string("/mnt/data"));
    MT_EXPECT_TRUE(drive.virtual_size.has_value());
    MT_EXPECT_EQ(*drive.virtual_size, static_cast<uint64_t>(4096));
    MT_EXPECT_TRUE(!drive.sub_path.has_value());
    // A resolved drive is never a volume, and carries no snapshot output dir.
    MT_EXPECT_TRUE(!drive.volume);
    MT_EXPECT_TRUE(!drive.snapshot_output_dir.has_value());
}

MT_TEST(to_extra_drive_propagates_sub_path) {
    // Rust: `to_extra_drive_propagates_sub_path`.
    snapshot::ResolvedAttachedDrive drive = SampleResolved();
    drive.sub_path = Optional<std::string>(std::string("workspace/data"));

    MT_EXPECT_EQ(*drive.ToExtraDrive().sub_path, std::string("workspace/data"));
}

MT_TEST(attached_drive_virtual_size_is_required) {
    // Rust: `attached_drive_virtual_size_is_required`. A drive of unknown
    // size cannot be attached, so there is no serde default.
    const agentenv::core::Expected<snapshot::ResolvedAttachedDrive, std::string> resolved =
        snapshot::ResolvedAttachedDrive::FromJson(
            Parse("{\"Overlaybd\":{\"drive_id\":\"data\","
                  "\"image_config_path\":\"/tmp/image.json\",\"read_only\":true}}"));
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_TRUE(resolved.error().find("virtual_size") != std::string::npos);

    const agentenv::core::Expected<snapshot::CommittedAttachedDrive, std::string> committed =
        snapshot::CommittedAttachedDrive::FromJson(
            Parse("{\"Overlaybd\":{\"drive_id\":\"data\",\"layers\":[],"
                  "\"read_only\":true}}"));
    MT_EXPECT_TRUE(!committed.ok());
    MT_EXPECT_TRUE(committed.error().find("virtual_size") != std::string::npos);
}

MT_TEST(attached_drive_virtual_size_is_serialized) {
    // Rust: `attached_drive_virtual_size_is_serialized`.
    const Json resolved = SampleResolved().ToJson();
    MT_EXPECT_EQ(resolved.as_object()
                     .find("Overlaybd")
                     ->second.as_object()
                     .find("virtual_size")
                     ->second.as_int(),
                 static_cast<int64_t>(4096));

    snapshot::CommittedAttachedDrive committed;
    committed.drive_id = "data";
    committed.read_only = true;
    committed.virtual_size = 4096;
    committed.mount_path = sandbox::ExtraDrive::DefaultMountPath("data");

    const Json committed_json = committed.ToJson();
    MT_EXPECT_EQ(committed_json.as_object()
                     .find("Overlaybd")
                     ->second.as_object()
                     .find("virtual_size")
                     ->second.as_int(),
                 static_cast<int64_t>(4096));
    MT_EXPECT_TRUE(snapshot::CommittedAttachedDrive::FromJson(committed_json).value() ==
                   committed);
}

MT_TEST(attached_drives_reject_an_unknown_tag) {
    // Defaulting to Overlaybd would silently reinterpret a future variant's
    // payload.
    MT_EXPECT_TRUE(!snapshot::ResolvedAttachedDrive::FromJson(Parse("{\"Other\":{}}")).ok());
    MT_EXPECT_TRUE(!snapshot::CommittedAttachedDrive::FromJson(Parse("{}")).ok());
}

// ---------------------------------------------------------------------------
// layer references
// ---------------------------------------------------------------------------

MT_TEST(layer_refs_use_the_externally_tagged_form) {
    const snapshot::OverlaybdLayerRef managed =
        snapshot::OverlaybdLayerRef::Managed(Managed("sha256:a", 1));
    // Held in a named value: `as_object()` returns a reference into the Json,
    // which a temporary would not keep alive.
    const Json managed_json = snapshot::LayerRefToJson(managed);
    const JsonObject& fields = managed_json.as_object();
    MT_EXPECT_TRUE(fields.find("Managed") != fields.end());
    MT_EXPECT_TRUE(fields.find("External") == fields.end());

    snapshot::ExternalLayer external_layer;
    external_layer.digest = "sha256:b";
    external_layer.repo_blob_url = "https://registry/v2/x/blobs";
    external_layer.size = 2;
    const snapshot::OverlaybdLayerRef external =
        snapshot::OverlaybdLayerRef::External(external_layer);

    MT_EXPECT_TRUE(snapshot::LayerRefFromJson(snapshot::LayerRefToJson(managed)).value() ==
                   managed);
    MT_EXPECT_TRUE(snapshot::LayerRefFromJson(snapshot::LayerRefToJson(external)).value() ==
                   external);
}

MT_TEST(layer_refs_reject_an_untagged_object) {
    // An unknown tag must not become a Managed layer with an empty digest,
    // which would later resolve to the wrong blob.
    MT_EXPECT_TRUE(!snapshot::LayerRefFromJson(Parse("{}")).ok());
    MT_EXPECT_TRUE(!snapshot::LayerRefFromJson(Parse("{\"Other\":{}}")).ok());
    MT_EXPECT_TRUE(!snapshot::LayerRefFromJson(Parse("{\"Managed\":{}}")).ok());
}

MT_TEST(managed_layer_omits_an_absent_uuid) {
    const Json json = snapshot::ManagedLayerToJson(Managed("sha256:a", 1));
    MT_EXPECT_TRUE(json.as_object().find("uuid") == json.as_object().end());

    snapshot::ManagedLayer with_uuid = Managed("sha256:a", 1);
    with_uuid.uuid = Optional<std::string>(std::string("u-1"));
    MT_EXPECT_TRUE(
        snapshot::ManagedLayerFromJson(snapshot::ManagedLayerToJson(with_uuid)).value() ==
        with_uuid);
}

// ---------------------------------------------------------------------------
// SnapshotSource / TemplateBuildInfo
// ---------------------------------------------------------------------------

MT_TEST(template_build_info_waiting_has_no_timestamps) {
    const snapshot::TemplateBuildInfo info = snapshot::TemplateBuildInfo::Waiting();
    MT_EXPECT_TRUE(info.status == snapshot::TemplateBuildStatus::Waiting);
    // Writing 0 would read back as "started at the epoch".
    MT_EXPECT_TRUE(!info.started_at_unix_ms.has_value());
    MT_EXPECT_TRUE(!info.finished_at_unix_ms.has_value());
    MT_EXPECT_TRUE(!info.error_reason.has_value());
}

MT_TEST(template_build_status_round_trips_every_variant) {
    const snapshot::TemplateBuildStatus all[] = {
        snapshot::TemplateBuildStatus::Waiting, snapshot::TemplateBuildStatus::Building,
        snapshot::TemplateBuildStatus::Ready, snapshot::TemplateBuildStatus::Error};
    for (std::size_t i = 0; i < 4; ++i) {
        const std::string text = snapshot::TemplateBuildStatusToString(all[i]);
        MT_EXPECT_TRUE(snapshot::TemplateBuildStatusParse(text).has_value());
        MT_EXPECT_TRUE(*snapshot::TemplateBuildStatusParse(text) == all[i]);
    }
    // `Ready`, not `Committed` — the repository-boundary enum uses a
    // different spelling and the two must not be confused.
    MT_EXPECT_EQ(
        std::string(snapshot::TemplateBuildStatusToString(snapshot::TemplateBuildStatus::Ready)),
        std::string("Ready"));
    MT_EXPECT_TRUE(!snapshot::TemplateBuildStatusParse("Committed").has_value());
}

MT_TEST(snapshot_source_round_trips_both_variants) {
    const snapshot::SnapshotSource template_source =
        snapshot::SnapshotSource::Template(snapshot::TemplateBuildInfo::Waiting());
    MT_EXPECT_TRUE(
        snapshot::SnapshotSource::FromJson(template_source.ToJson()).value() ==
        template_source);

    const snapshot::SnapshotSource sandbox_source =
        snapshot::SnapshotSource::Sandbox("sbx-1");
    const snapshot::SnapshotSource parsed =
        snapshot::SnapshotSource::FromJson(sandbox_source.ToJson()).value();
    MT_EXPECT_TRUE(parsed == sandbox_source);
    MT_EXPECT_EQ(parsed.source_sandbox_id, std::string("sbx-1"));
}

MT_TEST(snapshot_source_rejects_an_unknown_tag) {
    // Defaulting to a Waiting template would make a sandbox snapshot look
    // like a stuck build.
    MT_EXPECT_TRUE(!snapshot::SnapshotSource::FromJson(Parse("{}")).ok());
    MT_EXPECT_TRUE(!snapshot::SnapshotSource::FromJson(Parse("{\"Other\":{}}")).ok());
}

// ---------------------------------------------------------------------------
// SnapshotRecord
// ---------------------------------------------------------------------------

MT_TEST(template_waiting_starts_uncommitted) {
    const snapshot::SnapshotRecord record = snapshot::SnapshotRecord::TemplateWaiting(
        FixtureId(), Optional<snapshot::SnapshotAlias>(), sandbox::SandboxResources());

    MT_EXPECT_TRUE(record.source.is_template());
    MT_EXPECT_TRUE(record.source.build.status == snapshot::TemplateBuildStatus::Waiting);
    // A record exists from the moment a build is queued; uncommitted is the
    // normal in-flight state, not a broken one.
    MT_EXPECT_TRUE(!record.committed.has_value());
    MT_EXPECT_EQ(record.created_at_unix_ms, record.updated_at_unix_ms);
    MT_EXPECT_TRUE(record.created_at_unix_ms > 0);
}

MT_TEST(mark_committed_advances_a_template_build) {
    snapshot::SnapshotRecord record = snapshot::SnapshotRecord::TemplateWaiting(
        FixtureId(), Optional<snapshot::SnapshotAlias>(), sandbox::SandboxResources());
    // A previous failed attempt must not leave its reason behind.
    record.source.build.status = snapshot::TemplateBuildStatus::Error;
    record.source.build.error_reason = Optional<snapshot::TemplateBuildErrorReason>(
        snapshot::TemplateBuildErrorReason::New("earlier failure"));

    sandbox::SandboxResources resources;
    resources.cpu_count = 4;
    record.MarkCommitted(Optional<snapshot::SnapshotAlias>(Alias("prod")), resources,
                         MockCommitted(), snapshot::SnapshotPublishSource::Template(), 1234);

    MT_EXPECT_TRUE(record.source.is_template());
    MT_EXPECT_TRUE(record.source.build.status == snapshot::TemplateBuildStatus::Ready);
    MT_EXPECT_EQ(*record.source.build.finished_at_unix_ms, static_cast<int64_t>(1234));
    MT_EXPECT_TRUE(!record.source.build.error_reason.has_value());
    MT_EXPECT_EQ(record.alias->ToString(), std::string("prod"));
    MT_EXPECT_EQ(record.resources.cpu_count, static_cast<uint32_t>(4));
    MT_EXPECT_EQ(record.updated_at_unix_ms, static_cast<int64_t>(1234));
    MT_EXPECT_TRUE(record.committed.has_value());
}

MT_TEST(mark_committed_rewrites_the_source_for_a_sandbox_publish) {
    snapshot::SnapshotRecord record = snapshot::SnapshotRecord::TemplateWaiting(
        FixtureId(), Optional<snapshot::SnapshotAlias>(), sandbox::SandboxResources());

    record.MarkCommitted(Optional<snapshot::SnapshotAlias>(), sandbox::SandboxResources(),
                         MockCommitted(),
                         snapshot::SnapshotPublishSource::Sandbox("sbx-7"), 99);

    // A sandbox publish replaces the source entirely; the build state of the
    // template it started as is no longer meaningful.
    MT_EXPECT_TRUE(!record.source.is_template());
    MT_EXPECT_EQ(record.source.source_sandbox_id, std::string("sbx-7"));
}

MT_TEST(published_rootfs_image_ref_matches_only_the_canonical_tag) {
    snapshot::SnapshotRecord record = snapshot::SnapshotRecord::TemplateWaiting(
        FixtureId(), Optional<snapshot::SnapshotAlias>(), sandbox::SandboxResources());
    MT_EXPECT_TRUE(!record.PublishedRootfsImageRef().has_value());

    snapshot::CommittedSnapshot committed = MockCommitted();
    snapshot::PersistedDiskImagePublication other;
    other.image_ref = "registry/app:latest";
    other.tag = "latest";
    committed.disk_publications.push_back(other);

    snapshot::PersistedDiskImagePublication canonical;
    canonical.image_ref = "registry/app:snap";
    canonical.tag = snapshot::RootfsSnapshotImageTag(FixtureId());
    committed.disk_publications.push_back(canonical);
    record.committed = Optional<snapshot::CommittedSnapshot>(committed);

    // Only the snapshot's own tag counts; an unrelated `latest` publication
    // must not be reported as this snapshot's rootfs image.
    MT_EXPECT_EQ(*record.PublishedRootfsImageRef(), std::string("registry/app:snap"));
}

MT_TEST(rootfs_snapshot_image_tag_is_prefixed) {
    MT_EXPECT_EQ(snapshot::RootfsSnapshotImageTag(FixtureId()),
                 std::string("agentenv-snapshot-") + FixtureId().ToString());
}

MT_TEST(snapshot_record_round_trips_through_json) {
    snapshot::SnapshotRecord record = snapshot::SnapshotRecord::TemplateWaiting(
        FixtureId(), Optional<snapshot::SnapshotAlias>(Alias("prod")),
        sandbox::SandboxResources());

    snapshot::CommittedSnapshot committed = MockCommitted();
    committed.context.env_vars["A"] = "1";
    committed.rootfs_layers.push_back(
        snapshot::OverlaybdLayerRef::Managed(Managed("sha256:a", 10)));
    committed.memory_layers.push_back(Managed("sha256:m", 20));

    snapshot::CommittedAttachedDrive drive;
    drive.drive_id = "data";
    drive.virtual_size = 4096;
    drive.mount_path = "/mnt/data";
    committed.attached_drives.push_back(drive);

    snapshot::SnapshotVolume volume;
    volume.mount_path = "/vol";
    volume.size_mb = 128;
    committed.volume_snapshots.push_back(volume);

    record.MarkCommitted(record.alias, record.resources, committed,
                         snapshot::SnapshotPublishSource::Template(), 4321);

    const snapshot::SnapshotRecord parsed =
        snapshot::SnapshotRecord::FromJson(Parse(record.ToJson().ToString())).value();
    MT_EXPECT_TRUE(parsed == record);
    MT_EXPECT_EQ(parsed.alias->ToString(), std::string("prod"));
    MT_EXPECT_EQ(parsed.committed->rootfs_layers.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(parsed.committed->attached_drives[0].virtual_size,
                 static_cast<uint64_t>(4096));
    MT_EXPECT_EQ(parsed.committed->volume_snapshots[0].size_mb, static_cast<uint64_t>(128));
}

MT_TEST(snapshot_record_round_trips_without_an_alias_or_committed_payload) {
    const snapshot::SnapshotRecord record = snapshot::SnapshotRecord::TemplateWaiting(
        FixtureId(), Optional<snapshot::SnapshotAlias>(), sandbox::SandboxResources());

    const snapshot::SnapshotRecord parsed =
        snapshot::SnapshotRecord::FromJson(Parse(record.ToJson().ToString())).value();
    MT_EXPECT_TRUE(parsed == record);
    MT_EXPECT_TRUE(!parsed.alias.has_value());
    MT_EXPECT_TRUE(!parsed.committed.has_value());
}

MT_TEST(snapshot_record_rejects_a_malformed_id_or_missing_fields) {
    MT_EXPECT_TRUE(!snapshot::SnapshotRecord::FromJson(Parse("{}")).ok());
    MT_EXPECT_TRUE(!snapshot::SnapshotRecord::FromJson(Parse("{\"id\":\"not-a-uuid\"}")).ok());
    MT_EXPECT_TRUE(!snapshot::SnapshotRecord::FromJson(Parse("[]")).ok());
}

// ---------------------------------------------------------------------------
// CommittedSnapshot
// ---------------------------------------------------------------------------

MT_TEST(committed_snapshot_defaults_the_virtualization_mode) {
    // `#[serde(default)]`: a record written before PVM existed describes a
    // KVM capture.
    snapshot::CommittedSnapshot committed = MockCommitted();
    Json json = committed.ToJson();
    JsonObject fields = json.as_object();
    fields.erase("virtualization_mode");

    const agentenv::core::Expected<snapshot::CommittedSnapshot, std::string> parsed =
        snapshot::CommittedSnapshot::FromJson(Json(fields));
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value().virtualization_mode ==
                   agentenv::core::VirtualizationMode::Kvm);
}

MT_TEST(committed_snapshot_omits_absent_optional_sections) {
    const snapshot::CommittedSnapshot committed = MockCommitted();
    const Json json = committed.ToJson();
    const JsonObject& fields = json.as_object();

    // All carry `skip_serializing_if`, so the keys must be missing rather
    // than present-and-null.
    MT_EXPECT_TRUE(fields.find("image_configs") == fields.end());
    MT_EXPECT_TRUE(fields.find("custom_extension_params") == fields.end());
    MT_EXPECT_TRUE(fields.find("memory_startup") == fields.end());
}

MT_TEST(committed_snapshot_round_trips_a_startup_pack_and_extension_params) {
    snapshot::CommittedSnapshot committed = MockCommitted();

    snapshot::MemoryStartupPackInfo pack;
    pack.pack_size = 10;
    pack.mem_virtual_size = 20;
    pack.index_sha256 = "abc";
    committed.memory_startup = Optional<snapshot::MemoryStartupPackInfo>(pack);

    // Opaque JSON: stored and forwarded verbatim, never interpreted.
    committed.custom_extension_params = Optional<Json>(Parse("{\"k\":[1,2]}"));

    const snapshot::CommittedSnapshot parsed =
        snapshot::CommittedSnapshot::FromJson(Parse(committed.ToJson().ToString())).value();
    MT_EXPECT_TRUE(parsed.memory_startup.has_value());
    MT_EXPECT_EQ(parsed.memory_startup->index_sha256, std::string("abc"));
    MT_EXPECT_TRUE(parsed.custom_extension_params.has_value());
    MT_EXPECT_TRUE(parsed.custom_extension_params->ToString().find("\"k\"") !=
                   std::string::npos);
}

MT_TEST(startup_command_picks_the_right_shell) {
    snapshot::StartupCommand command;
    command.start_cmd = "run";
    command.ready_cmd = "check";

    std::string program;
    std::string flag;
    command.ShellCommand(&program, &flag);
    // Legacy templates get a login shell so a profile-sourced PATH applies.
    MT_EXPECT_EQ(program, std::string("/bin/bash"));
    MT_EXPECT_EQ(flag, std::string("-lc"));

    command.shell = Optional<std::string>(std::string("/bin/sh"));
    command.ShellCommand(&program, &flag);
    MT_EXPECT_EQ(program, std::string("/bin/sh"));
    MT_EXPECT_EQ(flag, std::string("-c"));

    MT_EXPECT_TRUE(snapshot::StartupCommand::FromJson(command.ToJson()).value() == command);
}

MT_TEST(snapshot_volume_defaults_its_mode) {
    // `#[serde(default)]` → Exclusive.
    const agentenv::core::Expected<snapshot::SnapshotVolume, std::string> parsed =
        snapshot::SnapshotVolume::FromJson(
            Parse("{\"mountPath\":\"/vol\",\"sizeMb\":64,\"layers\":[]}"));
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value().mode == agentenv::volume::VolumeMode::Exclusive);
    MT_EXPECT_EQ(parsed.value().size_mb, static_cast<uint64_t>(64));

    // camelCase on this struct, so the snake_case spelling is not accepted.
    MT_EXPECT_TRUE(!snapshot::SnapshotVolume::FromJson(
                        Parse("{\"mount_path\":\"/vol\",\"sizeMb\":1,\"layers\":[]}"))
                        .ok());
}

int main() { return microtest::RunAll(); }
