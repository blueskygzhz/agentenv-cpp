// SPDX-License-Identifier: MIT
// Rust: src/snapshot/types/snapshot.rs
#include "agentenv/snapshot/record.h"

#include "agentenv/core/time.h"

namespace agentenv {
namespace snapshot {

const char* const kSnapshotImageTagPrefix = "agentenv-snapshot-";

namespace {

core::Expected<std::string, std::string> RequireString(const core::JsonObject& fields,
                                                       const std::string& field) {
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end()) {
        return core::make_unexpected(std::string("missing field `") + field + "`");
    }
    if (it->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("field `") + field + "` must be a string");
    }
    return it->second.as_string();
}

core::Expected<int64_t, std::string> RequireInt(const core::JsonObject& fields,
                                                const std::string& field) {
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end()) {
        return core::make_unexpected(std::string("missing field `") + field + "`");
    }
    if (it->second.kind() != core::Json::Kind::Int) {
        return core::make_unexpected(std::string("field `") + field + "` must be an integer");
    }
    return it->second.as_int();
}

core::Optional<int64_t> OptionalInt(const core::JsonObject& fields,
                                    const std::string& field) {
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end() || it->second.kind() != core::Json::Kind::Int) {
        return core::Optional<int64_t>();
    }
    return core::Optional<int64_t>(it->second.as_int());
}

const core::Json* Find(const core::JsonObject& fields, const std::string& field) {
    const core::JsonObject::const_iterator it = fields.find(field);
    return it == fields.end() ? NULL : &it->second;
}

core::Json ResourcesToJson(const sandbox::SandboxResources& resources) {
    core::JsonObject object;
    object["cpu_count"] = core::Json(static_cast<int64_t>(resources.cpu_count));
    object["memory_mib"] = core::Json(static_cast<int64_t>(resources.memory_mib));
    object["disk_size_mib"] = core::Json(static_cast<int64_t>(resources.disk_size_mib));
    return core::Json(object);
}

core::Expected<sandbox::SandboxResources, std::string> ResourcesFromJson(
    const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("resources must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    sandbox::SandboxResources resources;

    const core::Json* cpu = Find(fields, "cpu_count");
    if (cpu != NULL && cpu->kind() == core::Json::Kind::Int) {
        resources.cpu_count = static_cast<uint32_t>(cpu->as_int());
    }
    const core::Json* memory = Find(fields, "memory_mib");
    if (memory != NULL && memory->kind() == core::Json::Kind::Int) {
        resources.memory_mib = static_cast<uint32_t>(memory->as_int());
    }
    const core::Json* disk = Find(fields, "disk_size_mib");
    if (disk != NULL && disk->kind() == core::Json::Kind::Int) {
        resources.disk_size_mib = static_cast<uint32_t>(disk->as_int());
    }
    return resources;
}

}  // namespace

std::string RootfsSnapshotImageTag(const core::SnapshotId& snapshot_id) {
    return std::string(kSnapshotImageTagPrefix) + snapshot_id.ToString();
}

int64_t NowUnixMs() { return core::SystemTime::Now().unix_nanos / 1000000LL; }

// ---- SnapshotVolume -------------------------------------------------------

core::Json SnapshotVolume::ToJson() const {
    // `rename_all = "camelCase"` on this struct.
    core::JsonObject object;
    object["mountPath"] = core::Json(mount_path);
    object["mode"] = core::Json(volume::VolumeModeToString(mode));
    object["sizeMb"] = core::Json(static_cast<int64_t>(size_mb));
    object["layers"] = LayerRefsToJson(layers);
    return core::Json(object);
}

core::Expected<SnapshotVolume, std::string> SnapshotVolume::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("snapshot volume must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    SnapshotVolume value;

    const core::Expected<std::string, std::string> mount_path =
        RequireString(fields, "mountPath");
    if (!mount_path.ok()) return core::make_unexpected(mount_path.error());
    value.mount_path = mount_path.value();

    // `#[serde(default)]` → Exclusive.
    const core::Json* mode = Find(fields, "mode");
    if (mode != NULL && mode->kind() == core::Json::Kind::String) {
        const core::Expected<volume::VolumeMode, std::string> parsed =
            volume::VolumeModeParse(mode->as_string());
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        value.mode = parsed.value();
    }

    const core::Json* size = Find(fields, "sizeMb");
    if (size == NULL || size->kind() != core::Json::Kind::Int || size->as_int() < 0) {
        return core::make_unexpected(std::string("missing field `sizeMb`"));
    }
    value.size_mb = static_cast<uint64_t>(size->as_int());

    const core::Json* layers = Find(fields, "layers");
    if (layers == NULL) {
        return core::make_unexpected(std::string("missing field `layers`"));
    }
    const core::Expected<std::vector<OverlaybdLayerRef>, std::string> parsed_layers =
        LayerRefsFromJson(*layers);
    if (!parsed_layers.ok()) return core::make_unexpected(parsed_layers.error());
    value.layers = parsed_layers.value();

    return value;
}

bool SnapshotVolume::operator==(const SnapshotVolume& o) const {
    return mount_path == o.mount_path && mode == o.mode && size_mb == o.size_mb &&
           LayerRefsEqual(layers, o.layers);
}

// ---- StartupCommand -------------------------------------------------------

void StartupCommand::ShellCommand(std::string* program, std::string* flag) const {
    if (shell.has_value()) {
        *program = *shell;
        *flag = "-c";
        return;
    }
    // Legacy templates get a login shell, so their start command can rely on
    // profile-sourced PATH entries.
    *program = "/bin/bash";
    *flag = "-lc";
}

core::Json StartupCommand::ToJson() const {
    core::JsonObject object;
    object["start_cmd"] = core::Json(start_cmd);
    object["ready_cmd"] = core::Json(ready_cmd);
    object["context"] = context.ToJson();
    if (shell.has_value()) object["shell"] = core::Json(*shell);
    return core::Json(object);
}

core::Expected<StartupCommand, std::string> StartupCommand::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("startup command must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    StartupCommand command;

    const core::Expected<std::string, std::string> start = RequireString(fields, "start_cmd");
    if (!start.ok()) return core::make_unexpected(start.error());
    command.start_cmd = start.value();

    const core::Expected<std::string, std::string> ready = RequireString(fields, "ready_cmd");
    if (!ready.ok()) return core::make_unexpected(ready.error());
    command.ready_cmd = ready.value();

    const core::Json* context = Find(fields, "context");
    if (context == NULL) {
        return core::make_unexpected(std::string("missing field `context`"));
    }
    const core::Expected<CommandContext, std::string> parsed =
        CommandContext::FromJson(*context);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());
    command.context = parsed.value();

    const core::Json* shell = Find(fields, "shell");
    if (shell != NULL && shell->kind() == core::Json::Kind::String) {
        command.shell = core::Optional<std::string>(shell->as_string());
    }
    return command;
}

bool StartupCommand::operator==(const StartupCommand& o) const {
    if (start_cmd != o.start_cmd || ready_cmd != o.ready_cmd || context != o.context) {
        return false;
    }
    if (shell.has_value() != o.shell.has_value()) return false;
    return !shell.has_value() || *shell == *o.shell;
}

// ---- PersistedDiskImagePublication ----------------------------------------

core::Json PersistedDiskImagePublication::ToJson() const {
    core::JsonObject object;
    object["image_ref"] = core::Json(image_ref);
    object["tag"] = core::Json(tag);
    object["manifest_digest"] = core::Json(manifest_digest);
    object["repo_blob_url"] = core::Json(repo_blob_url);
    return core::Json(object);
}

core::Expected<PersistedDiskImagePublication, std::string>
PersistedDiskImagePublication::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("disk publication must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    PersistedDiskImagePublication publication;

    const char* const names[] = {"image_ref", "tag", "manifest_digest", "repo_blob_url"};
    std::string* const targets[] = {&publication.image_ref, &publication.tag,
                                    &publication.manifest_digest,
                                    &publication.repo_blob_url};
    for (std::size_t i = 0; i < 4; ++i) {
        const core::Expected<std::string, std::string> value = RequireString(fields, names[i]);
        if (!value.ok()) return core::make_unexpected(value.error());
        *targets[i] = value.value();
    }
    return publication;
}

bool PersistedDiskImagePublication::operator==(
    const PersistedDiskImagePublication& o) const {
    return image_ref == o.image_ref && tag == o.tag &&
           manifest_digest == o.manifest_digest && repo_blob_url == o.repo_blob_url;
}

// ---- TemplateBuildStatus / TemplateBuildInfo ------------------------------

const char* TemplateBuildStatusToString(TemplateBuildStatus status) {
    switch (status) {
        case TemplateBuildStatus::Waiting:
            return "Waiting";
        case TemplateBuildStatus::Building:
            return "Building";
        case TemplateBuildStatus::Ready:
            return "Ready";
        case TemplateBuildStatus::Error:
        default:
            return "Error";
    }
}

core::Optional<TemplateBuildStatus> TemplateBuildStatusParse(const std::string& raw) {
    if (raw == "Waiting") return core::Optional<TemplateBuildStatus>(
        TemplateBuildStatus::Waiting);
    if (raw == "Building") return core::Optional<TemplateBuildStatus>(
        TemplateBuildStatus::Building);
    if (raw == "Ready") return core::Optional<TemplateBuildStatus>(
        TemplateBuildStatus::Ready);
    if (raw == "Error") return core::Optional<TemplateBuildStatus>(
        TemplateBuildStatus::Error);
    return core::Optional<TemplateBuildStatus>();
}

TemplateBuildInfo TemplateBuildInfo::Waiting() {
    // All timestamps absent: the build has not started, and writing 0 would
    // read back as "started at the epoch".
    TemplateBuildInfo info;
    info.status = TemplateBuildStatus::Waiting;
    return info;
}

core::Json TemplateBuildInfo::ToJson() const {
    core::JsonObject object;
    object["status"] = core::Json(TemplateBuildStatusToString(status));
    object["started_at_unix_ms"] =
        started_at_unix_ms.has_value() ? core::Json(*started_at_unix_ms) : core::Json();
    object["finished_at_unix_ms"] =
        finished_at_unix_ms.has_value() ? core::Json(*finished_at_unix_ms) : core::Json();
    // No `skip_serializing_if` on these three in Rust, so null is written
    // rather than the key being omitted.
    object["error_reason"] =
        error_reason.has_value() ? error_reason->ToJson() : core::Json();
    return core::Json(object);
}

core::Expected<TemplateBuildInfo, std::string> TemplateBuildInfo::FromJson(
    const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("template build info must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    TemplateBuildInfo info;

    const core::Expected<std::string, std::string> status = RequireString(fields, "status");
    if (!status.ok()) return core::make_unexpected(status.error());
    const core::Optional<TemplateBuildStatus> parsed = TemplateBuildStatusParse(status.value());
    if (!parsed.has_value()) {
        return core::make_unexpected(std::string("unknown build status `") + status.value() +
                                     "`");
    }
    info.status = *parsed;

    info.started_at_unix_ms = OptionalInt(fields, "started_at_unix_ms");
    info.finished_at_unix_ms = OptionalInt(fields, "finished_at_unix_ms");

    const core::Json* reason = Find(fields, "error_reason");
    if (reason != NULL && reason->kind() != core::Json::Kind::Null) {
        const core::Expected<TemplateBuildErrorReason, std::string> parsed_reason =
            TemplateBuildErrorReason::FromJson(*reason);
        if (!parsed_reason.ok()) return core::make_unexpected(parsed_reason.error());
        info.error_reason = core::Optional<TemplateBuildErrorReason>(parsed_reason.value());
    }
    return info;
}

bool TemplateBuildInfo::operator==(const TemplateBuildInfo& o) const {
    if (status != o.status) return false;
    if (started_at_unix_ms.has_value() != o.started_at_unix_ms.has_value()) return false;
    if (started_at_unix_ms.has_value() && *started_at_unix_ms != *o.started_at_unix_ms) {
        return false;
    }
    if (finished_at_unix_ms.has_value() != o.finished_at_unix_ms.has_value()) return false;
    if (finished_at_unix_ms.has_value() && *finished_at_unix_ms != *o.finished_at_unix_ms) {
        return false;
    }
    if (error_reason.has_value() != o.error_reason.has_value()) return false;
    return !error_reason.has_value() || *error_reason == *o.error_reason;
}

// ---- SnapshotSource -------------------------------------------------------

SnapshotSource SnapshotSource::Template(const TemplateBuildInfo& build) {
    SnapshotSource source;
    source.kind = SnapshotSourceKind::Template;
    source.build = build;
    return source;
}

SnapshotSource SnapshotSource::Sandbox(const std::string& source_sandbox_id) {
    SnapshotSource source;
    source.kind = SnapshotSourceKind::Sandbox;
    source.source_sandbox_id = source_sandbox_id;
    return source;
}

core::Json SnapshotSource::ToJson() const {
    // Externally tagged, serde's default for a data-carrying enum.
    core::JsonObject object;
    if (kind == SnapshotSourceKind::Template) {
        core::JsonObject inner;
        inner["build"] = build.ToJson();
        object["Template"] = core::Json(inner);
    } else {
        core::JsonObject inner;
        inner["source_sandbox_id"] = core::Json(source_sandbox_id);
        object["Sandbox"] = core::Json(inner);
    }
    return core::Json(object);
}

core::Expected<SnapshotSource, std::string> SnapshotSource::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("snapshot source must be an object"));
    }
    const core::JsonObject& fields = json.as_object();

    const core::Json* template_variant = Find(fields, "Template");
    if (template_variant != NULL) {
        if (template_variant->kind() != core::Json::Kind::Object) {
            return core::make_unexpected(std::string("`Template` must be an object"));
        }
        const core::Json* build = Find(template_variant->as_object(), "build");
        if (build == NULL) {
            return core::make_unexpected(std::string("missing field `build`"));
        }
        const core::Expected<TemplateBuildInfo, std::string> parsed =
            TemplateBuildInfo::FromJson(*build);
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        return SnapshotSource::Template(parsed.value());
    }

    const core::Json* sandbox_variant = Find(fields, "Sandbox");
    if (sandbox_variant != NULL) {
        if (sandbox_variant->kind() != core::Json::Kind::Object) {
            return core::make_unexpected(std::string("`Sandbox` must be an object"));
        }
        const core::Expected<std::string, std::string> id =
            RequireString(sandbox_variant->as_object(), "source_sandbox_id");
        if (!id.ok()) return core::make_unexpected(id.error());
        return SnapshotSource::Sandbox(id.value());
    }

    // An unknown tag must not default to a Template with a Waiting build,
    // which would make a sandbox snapshot look like a stuck template.
    return core::make_unexpected(
        std::string("snapshot source must be tagged `Template` or `Sandbox`"));
}

bool SnapshotSource::operator==(const SnapshotSource& o) const {
    if (kind != o.kind) return false;
    return kind == SnapshotSourceKind::Template ? build == o.build
                                                : source_sandbox_id == o.source_sandbox_id;
}

SnapshotPublishSource SnapshotPublishSource::Template() {
    SnapshotPublishSource source;
    source.kind = SnapshotSourceKind::Template;
    return source;
}

SnapshotPublishSource SnapshotPublishSource::Sandbox(const std::string& source_sandbox_id) {
    SnapshotPublishSource source;
    source.kind = SnapshotSourceKind::Sandbox;
    source.source_sandbox_id = source_sandbox_id;
    return source;
}

// ---- CommittedSnapshot ----------------------------------------------------

core::Json CommittedSnapshot::ToJson() const {
    core::JsonObject object;
    object["context"] = context.ToJson();
    object["startup"] = startup.has_value() ? startup->ToJson() : core::Json();
    object["runtime_versions"] = runtime_versions.ToJson();
    object["virtualization_mode"] =
        core::Json(core::VirtualizationModeToString(virtualization_mode));
    if (!image_configs.IsEmpty()) object["image_configs"] = image_configs.ToJson();
    object["rootfs_layers"] = LayerRefsToJson(rootfs_layers);

    core::JsonArray drives;
    for (std::size_t i = 0; i < attached_drives.size(); ++i) {
        drives.push_back(attached_drives[i].ToJson());
    }
    object["attached_drives"] = core::Json(drives);

    core::JsonArray volumes;
    for (std::size_t i = 0; i < volume_snapshots.size(); ++i) {
        volumes.push_back(volume_snapshots[i].ToJson());
    }
    object["volume_snapshots"] = core::Json(volumes);

    core::JsonArray memory;
    for (std::size_t i = 0; i < memory_layers.size(); ++i) {
        memory.push_back(ManagedLayerToJson(memory_layers[i]));
    }
    object["memory_layers"] = core::Json(memory);

    core::JsonArray publications;
    for (std::size_t i = 0; i < disk_publications.size(); ++i) {
        publications.push_back(disk_publications[i].ToJson());
    }
    object["disk_publications"] = core::Json(publications);

    if (custom_extension_params.has_value()) {
        object["custom_extension_params"] = *custom_extension_params;
    }
    if (memory_startup.has_value()) {
        object["memory_startup"] = memory_startup->ToJson();
    }
    return core::Json(object);
}

core::Expected<CommittedSnapshot, std::string> CommittedSnapshot::FromJson(
    const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("committed snapshot must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    CommittedSnapshot committed;

    const core::Json* context = Find(fields, "context");
    if (context == NULL) {
        return core::make_unexpected(std::string("missing field `context`"));
    }
    const core::Expected<CommandContext, std::string> parsed_context =
        CommandContext::FromJson(*context);
    if (!parsed_context.ok()) return core::make_unexpected(parsed_context.error());
    committed.context = parsed_context.value();

    const core::Json* startup = Find(fields, "startup");
    if (startup != NULL && startup->kind() != core::Json::Kind::Null) {
        const core::Expected<StartupCommand, std::string> parsed =
            StartupCommand::FromJson(*startup);
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        committed.startup = core::Optional<StartupCommand>(parsed.value());
    }

    const core::Json* versions = Find(fields, "runtime_versions");
    if (versions == NULL) {
        return core::make_unexpected(std::string("missing field `runtime_versions`"));
    }
    const core::Expected<SnapshotRuntimeVersions, std::string> parsed_versions =
        SnapshotRuntimeVersions::FromJson(*versions);
    if (!parsed_versions.ok()) return core::make_unexpected(parsed_versions.error());
    committed.runtime_versions = parsed_versions.value();

    // `#[serde(default)]` → Kvm, which is what pre-PVM records described.
    const core::Json* mode = Find(fields, "virtualization_mode");
    if (mode != NULL && mode->kind() == core::Json::Kind::String) {
        const core::Expected<core::VirtualizationMode, std::string> parsed =
            core::VirtualizationModeParse(mode->as_string());
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        committed.virtualization_mode = parsed.value();
    }

    const core::Json* configs = Find(fields, "image_configs");
    if (configs != NULL && configs->kind() != core::Json::Kind::Null) {
        const core::Expected<tpl::ImageConfigs, std::string> parsed =
            tpl::ImageConfigs::FromJson(*configs);
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        committed.image_configs = parsed.value();
    }

    const core::Json* rootfs = Find(fields, "rootfs_layers");
    if (rootfs == NULL) {
        return core::make_unexpected(std::string("missing field `rootfs_layers`"));
    }
    const core::Expected<std::vector<OverlaybdLayerRef>, std::string> parsed_rootfs =
        LayerRefsFromJson(*rootfs);
    if (!parsed_rootfs.ok()) return core::make_unexpected(parsed_rootfs.error());
    committed.rootfs_layers = parsed_rootfs.value();

    const core::Json* drives = Find(fields, "attached_drives");
    if (drives == NULL || drives->kind() != core::Json::Kind::Array) {
        return core::make_unexpected(std::string("missing field `attached_drives`"));
    }
    for (std::size_t i = 0; i < drives->as_array().size(); ++i) {
        const core::Expected<CommittedAttachedDrive, std::string> drive =
            CommittedAttachedDrive::FromJson(drives->as_array()[i]);
        if (!drive.ok()) return core::make_unexpected(drive.error());
        committed.attached_drives.push_back(drive.value());
    }

    // `#[serde(default)]`.
    const core::Json* volumes = Find(fields, "volume_snapshots");
    if (volumes != NULL && volumes->kind() == core::Json::Kind::Array) {
        for (std::size_t i = 0; i < volumes->as_array().size(); ++i) {
            const core::Expected<SnapshotVolume, std::string> value =
                SnapshotVolume::FromJson(volumes->as_array()[i]);
            if (!value.ok()) return core::make_unexpected(value.error());
            committed.volume_snapshots.push_back(value.value());
        }
    }

    const core::Json* memory = Find(fields, "memory_layers");
    if (memory == NULL || memory->kind() != core::Json::Kind::Array) {
        return core::make_unexpected(std::string("missing field `memory_layers`"));
    }
    for (std::size_t i = 0; i < memory->as_array().size(); ++i) {
        const core::Expected<ManagedLayer, std::string> layer =
            ManagedLayerFromJson(memory->as_array()[i]);
        if (!layer.ok()) return core::make_unexpected(layer.error());
        committed.memory_layers.push_back(layer.value());
    }

    // `#[serde(default)]`.
    const core::Json* publications = Find(fields, "disk_publications");
    if (publications != NULL && publications->kind() == core::Json::Kind::Array) {
        for (std::size_t i = 0; i < publications->as_array().size(); ++i) {
            const core::Expected<PersistedDiskImagePublication, std::string> publication =
                PersistedDiskImagePublication::FromJson(publications->as_array()[i]);
            if (!publication.ok()) return core::make_unexpected(publication.error());
            committed.disk_publications.push_back(publication.value());
        }
    }

    const core::Json* params = Find(fields, "custom_extension_params");
    if (params != NULL && params->kind() != core::Json::Kind::Null) {
        // Opaque: stored and forwarded verbatim, never interpreted here.
        committed.custom_extension_params = core::Optional<core::Json>(*params);
    }

    const core::Json* startup_pack = Find(fields, "memory_startup");
    if (startup_pack != NULL && startup_pack->kind() != core::Json::Kind::Null) {
        const core::Expected<MemoryStartupPackInfo, std::string> parsed =
            MemoryStartupPackInfo::FromJson(*startup_pack);
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        committed.memory_startup = core::Optional<MemoryStartupPackInfo>(parsed.value());
    }

    return committed;
}

bool CommittedSnapshot::operator==(const CommittedSnapshot& o) const {
    if (context != o.context || runtime_versions != o.runtime_versions ||
        virtualization_mode != o.virtualization_mode || image_configs != o.image_configs) {
        return false;
    }
    if (startup.has_value() != o.startup.has_value()) return false;
    if (startup.has_value() && *startup != *o.startup) return false;
    if (!LayerRefsEqual(rootfs_layers, o.rootfs_layers)) return false;
    if (attached_drives != o.attached_drives) return false;
    if (volume_snapshots != o.volume_snapshots) return false;
    if (memory_layers != o.memory_layers) return false;
    if (disk_publications != o.disk_publications) return false;
    if (custom_extension_params.has_value() != o.custom_extension_params.has_value()) {
        return false;
    }
    if (custom_extension_params.has_value() &&
        custom_extension_params->ToString() != o.custom_extension_params->ToString()) {
        return false;
    }
    if (memory_startup.has_value() != o.memory_startup.has_value()) return false;
    return !memory_startup.has_value() || *memory_startup == *o.memory_startup;
}

// ---- SnapshotRecord -------------------------------------------------------

SnapshotRecord SnapshotRecord::TemplateWaiting(const core::SnapshotId& id,
                                               const core::Optional<SnapshotAlias>& alias,
                                               const sandbox::SandboxResources& resources) {
    const int64_t now = NowUnixMs();
    SnapshotRecord record;
    record.id = id;
    record.alias = alias;
    record.source = SnapshotSource::Template(TemplateBuildInfo::Waiting());
    record.resources = resources;
    record.created_at_unix_ms = now;
    record.updated_at_unix_ms = now;
    return record;
}

void SnapshotRecord::MarkCommitted(const core::Optional<SnapshotAlias>& new_alias,
                                   const sandbox::SandboxResources& new_resources,
                                   const CommittedSnapshot& new_committed,
                                   const SnapshotPublishSource& publish_source,
                                   int64_t now_unix_ms) {
    // A sandbox publish rewrites the source; a template publish leaves it a
    // Template and only advances the build state below.
    if (publish_source.kind == SnapshotSourceKind::Sandbox) {
        source = SnapshotSource::Sandbox(publish_source.source_sandbox_id);
    }
    if (source.kind == SnapshotSourceKind::Template) {
        source.build.status = TemplateBuildStatus::Ready;
        source.build.finished_at_unix_ms = core::Optional<int64_t>(now_unix_ms);
        // A previous failed attempt must not leave its reason on a record
        // that is now ready.
        source.build.error_reason = core::Optional<TemplateBuildErrorReason>();
    }
    alias = new_alias;
    resources = new_resources;
    updated_at_unix_ms = now_unix_ms;
    committed = core::Optional<CommittedSnapshot>(new_committed);
}

core::Optional<std::string> SnapshotRecord::PublishedRootfsImageRef() const {
    if (!committed.has_value()) return core::Optional<std::string>();
    const std::string expected_tag = RootfsSnapshotImageTag(id);
    for (std::size_t i = 0; i < committed->disk_publications.size(); ++i) {
        if (committed->disk_publications[i].tag == expected_tag) {
            return core::Optional<std::string>(committed->disk_publications[i].image_ref);
        }
    }
    return core::Optional<std::string>();
}

core::Json SnapshotRecord::ToJson() const {
    core::JsonObject object;
    object["id"] = core::Json(id.ToString());
    object["alias"] = alias.has_value() ? core::Json(alias->ToString()) : core::Json();
    object["source"] = source.ToJson();
    object["resources"] = ResourcesToJson(resources);
    object["created_at_unix_ms"] = core::Json(created_at_unix_ms);
    object["updated_at_unix_ms"] = core::Json(updated_at_unix_ms);
    object["committed"] = committed.has_value() ? committed->ToJson() : core::Json();
    return core::Json(object);
}

core::Expected<SnapshotRecord, std::string> SnapshotRecord::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("snapshot record must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    SnapshotRecord record;

    const core::Expected<std::string, std::string> id = RequireString(fields, "id");
    if (!id.ok()) return core::make_unexpected(id.error());
    core::Uuid uuid;
    if (!core::Uuid::Parse(id.value(), &uuid)) {
        return core::make_unexpected(std::string("invalid snapshot id `") + id.value() + "`");
    }
    record.id = core::SnapshotId(uuid);

    const core::Json* alias = Find(fields, "alias");
    if (alias != NULL && alias->kind() == core::Json::Kind::String) {
        const core::Expected<SnapshotAlias, std::string> parsed =
            SnapshotAlias::Parse(alias->as_string());
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        record.alias = core::Optional<SnapshotAlias>(parsed.value());
    }

    const core::Json* source = Find(fields, "source");
    if (source == NULL) {
        return core::make_unexpected(std::string("missing field `source`"));
    }
    const core::Expected<SnapshotSource, std::string> parsed_source =
        SnapshotSource::FromJson(*source);
    if (!parsed_source.ok()) return core::make_unexpected(parsed_source.error());
    record.source = parsed_source.value();

    const core::Json* resources = Find(fields, "resources");
    if (resources == NULL) {
        return core::make_unexpected(std::string("missing field `resources`"));
    }
    const core::Expected<sandbox::SandboxResources, std::string> parsed_resources =
        ResourcesFromJson(*resources);
    if (!parsed_resources.ok()) return core::make_unexpected(parsed_resources.error());
    record.resources = parsed_resources.value();

    const core::Expected<int64_t, std::string> created =
        RequireInt(fields, "created_at_unix_ms");
    if (!created.ok()) return core::make_unexpected(created.error());
    record.created_at_unix_ms = created.value();

    const core::Expected<int64_t, std::string> updated =
        RequireInt(fields, "updated_at_unix_ms");
    if (!updated.ok()) return core::make_unexpected(updated.error());
    record.updated_at_unix_ms = updated.value();

    const core::Json* committed = Find(fields, "committed");
    if (committed != NULL && committed->kind() != core::Json::Kind::Null) {
        const core::Expected<CommittedSnapshot, std::string> parsed =
            CommittedSnapshot::FromJson(*committed);
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        record.committed = core::Optional<CommittedSnapshot>(parsed.value());
    }

    return record;
}

bool SnapshotRecord::operator==(const SnapshotRecord& o) const {
    if (id != o.id || source != o.source || resources != o.resources ||
        created_at_unix_ms != o.created_at_unix_ms ||
        updated_at_unix_ms != o.updated_at_unix_ms) {
        return false;
    }
    if (alias.has_value() != o.alias.has_value()) return false;
    if (alias.has_value() && *alias != *o.alias) return false;
    if (committed.has_value() != o.committed.has_value()) return false;
    return !committed.has_value() || *committed == *o.committed;
}

}  // namespace snapshot
}  // namespace agentenv
