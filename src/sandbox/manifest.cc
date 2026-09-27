// SPDX-License-Identifier: MIT
// Rust: src/sandbox/manifest.rs
#include "agentenv/sandbox/manifest.h"

namespace agentenv {
namespace sandbox {

const char* const kFirecrackerBackend = "firecracker";

namespace {

/// Rust `KNOWN_BACKENDS`.
const char* const kKnownBackends[] = {"firecracker"};

/// Rust `default_backend`.
std::string DefaultBackend() { return kFirecrackerBackend; }

core::Expected<uint64_t, std::string> RequireUint(const core::JsonObject& fields,
                                                  const std::string& field) {
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end()) {
        return core::make_unexpected(std::string("missing field `") + field + "`");
    }
    if (it->second.kind() != core::Json::Kind::Int) {
        return core::make_unexpected(std::string("field `") + field + "` must be an integer");
    }
    const int64_t value = it->second.as_int();
    if (value < 0) {
        return core::make_unexpected(std::string("field `") + field + "` must not be negative");
    }
    return static_cast<uint64_t>(value);
}

/// `#[serde(default)]` on an integer: absent means 0.
uint64_t OptionalUint(const core::JsonObject& fields, const std::string& field) {
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end() || it->second.kind() != core::Json::Kind::Int) return 0;
    const int64_t value = it->second.as_int();
    return value < 0 ? 0 : static_cast<uint64_t>(value);
}

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

std::string OptionalString(const core::JsonObject& fields, const std::string& field,
                           const std::string& fallback) {
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end() || it->second.kind() != core::Json::Kind::String) return fallback;
    return it->second.as_string();
}

core::Expected<bool, std::string> RequireBool(const core::JsonObject& fields,
                                              const std::string& field) {
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end()) {
        return core::make_unexpected(std::string("missing field `") + field + "`");
    }
    if (it->second.kind() != core::Json::Kind::Bool) {
        return core::make_unexpected(std::string("field `") + field + "` must be a boolean");
    }
    return it->second.as_bool();
}

/// Reads a nested object, tolerating absence the way `#[serde(skip)]` fields
/// make possible: `{"memory": {"virtualSize": 4096}}` has no path field, and a
/// manifest may omit `vmState`'s body entirely.
core::Optional<core::Json> NestedObject(const core::JsonObject& fields,
                                        const std::string& field) {
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end() || it->second.kind() != core::Json::Kind::Object) {
        return core::Optional<core::Json>();
    }
    return core::Optional<core::Json>(it->second);
}

}  // namespace

bool IsKnownBackend(const std::string& backend) {
    for (std::size_t i = 0; i < sizeof(kKnownBackends) / sizeof(kKnownBackends[0]); ++i) {
        if (backend == kKnownBackends[i]) return true;
    }
    return false;
}

// ---- SnapshotAttachedDriveArtifacts ---------------------------------------

bool SnapshotAttachedDriveArtifacts::operator==(
    const SnapshotAttachedDriveArtifacts& o) const {
    if (drive_id != o.drive_id || read_only != o.read_only || mount_path != o.mount_path ||
        virtual_size != o.virtual_size || image_config_path != o.image_config_path) {
        return false;
    }
    if (sub_path.has_value() != o.sub_path.has_value()) return false;
    if (sub_path.has_value() && *sub_path != *o.sub_path) return false;
    return true;
}

core::Json SnapshotAttachedDriveArtifacts::ToJson() const {
    core::JsonObject object;
    object["driveId"] = core::Json(drive_id);
    object["readOnly"] = core::Json(read_only);
    object["mountPath"] = core::Json(mount_path);
    if (sub_path.has_value()) {
        // `skip_serializing_if = "Option::is_none"`: absent rather than null,
        // so a record round-trips byte-identically.
        object["subPath"] = core::Json(*sub_path);
    }
    object["virtualSize"] = core::Json(static_cast<int64_t>(virtual_size));
    // `image_config_path` is `#[serde(skip)]` and intentionally not written.
    return core::Json(object);
}

core::Expected<SnapshotAttachedDriveArtifacts, std::string>
SnapshotAttachedDriveArtifacts::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("attached drive artifact must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    SnapshotAttachedDriveArtifacts drive;

    const core::Expected<std::string, std::string> drive_id = RequireString(fields, "driveId");
    if (!drive_id.ok()) return core::make_unexpected(drive_id.error());
    drive.drive_id = drive_id.value();

    const core::Expected<bool, std::string> read_only = RequireBool(fields, "readOnly");
    if (!read_only.ok()) return core::make_unexpected(read_only.error());
    drive.read_only = read_only.value();

    drive.mount_path = OptionalString(fields, "mountPath", std::string());

    const core::JsonObject::const_iterator sub_path = fields.find("subPath");
    if (sub_path != fields.end() && sub_path->second.kind() == core::Json::Kind::String) {
        drive.sub_path = core::Optional<std::string>(sub_path->second.as_string());
    }

    // Rust has no `#[serde(default)]` here, so a missing `virtualSize` is an
    // error: restoring a drive of unknown size is not possible.
    const core::Expected<uint64_t, std::string> virtual_size =
        RequireUint(fields, "virtualSize");
    if (!virtual_size.ok()) {
        return core::make_unexpected(std::string("virtualSize: ") + virtual_size.error());
    }
    drive.virtual_size = virtual_size.value();

    return drive;
}

// ---- SandboxSnapshotManifest ---------------------------------------------

core::Expected<SandboxSnapshotManifest, std::string> SandboxSnapshotManifest::New(
    const std::string& backend, const std::string& vm_state_path,
    const std::string& mem_image_config_path, uint64_t mem_virtual_size,
    const std::string& rootfs_image_config_path, uint64_t rootfs_virtual_size,
    const std::vector<ExtraDrive>& attached_drives) {
    if (!IsKnownBackend(backend)) {
        return core::make_unexpected(std::string("backend '") + backend +
                                     "' names no VMM a restore can reach");
    }

    SandboxSnapshotManifest manifest;
    manifest.version = kManifestFormatVersion;
    manifest.backend = backend;
    manifest.vm_state.path = vm_state_path;
    manifest.memory.image_config_path = mem_image_config_path;
    manifest.memory.virtual_size = mem_virtual_size;
    manifest.rootfs.image_config_path = rootfs_image_config_path;
    manifest.rootfs.virtual_size = rootfs_virtual_size;
    manifest.volume_drive_slots = 0;
    // Counted before conversion, and from the input rather than the converted
    // list, exactly as Rust does.
    manifest.physical_extra_drive_count = attached_drives.size();

    // Rust builds the struct with an empty `attached_drives` and then defers
    // to `with_extra_drives`, so the validation lives in one place.
    return manifest.WithExtraDrives(attached_drives);
}

std::vector<ExtraDrive> SandboxSnapshotManifest::ExtraDrives() const {
    std::vector<ExtraDrive> drives;
    drives.reserve(attached_drives.size());
    for (std::size_t i = 0; i < attached_drives.size(); ++i) {
        const SnapshotAttachedDriveArtifacts& source = attached_drives[i];
        ExtraDrive drive;
        drive.kind = ExtraDrive::Kind::Overlaybd;
        drive.drive_id = source.drive_id;
        drive.image_config_path = source.image_config_path;
        drive.read_only = source.read_only;
        drive.virtual_size = core::Optional<uint64_t>(source.virtual_size);
        drive.sub_path = source.sub_path;
        drive.snapshot_output_dir = core::Optional<std::string>();
        drive.volume = false;

        // Rust `unwrap_or_else(|_| ExtraDrive::default_mount_path(..))`: a
        // stored mount path that no longer validates must not make an existing
        // snapshot unusable, so it falls back to the default.
        const core::Expected<std::string, std::string> normalized =
            NormalizeMountPathForDrive(source.drive_id, source.mount_path);
        drive.mount_path = normalized.ok() ? normalized.value()
                                           : ExtraDrive::DefaultMountPath(source.drive_id);
        drives.push_back(drive);
    }
    return drives;
}

core::Expected<SandboxSnapshotManifest, std::string>
SandboxSnapshotManifest::WithExtraDrives(const std::vector<ExtraDrive>& extra_drives) const {
    SandboxSnapshotManifest updated = *this;
    updated.attached_drives.clear();
    updated.attached_drives.reserve(extra_drives.size());

    for (std::size_t i = 0; i < extra_drives.size(); ++i) {
        const ExtraDrive& drive = extra_drives[i];
        if (!drive.virtual_size.has_value()) {
            // A capture records the size it observed; an unknown size here
            // would be written into a record no restore could size a device
            // from.
            return core::make_unexpected(std::string("snapshot attached drive '") +
                                         drive.drive_id +
                                         "' virtual size must be known");
        }
        if (*drive.virtual_size == 0) {
            return core::make_unexpected(std::string("snapshot attached drive '") +
                                         drive.drive_id +
                                         "' virtual size must be non-zero");
        }

        SnapshotAttachedDriveArtifacts artifact;
        artifact.drive_id = drive.drive_id;
        artifact.read_only = drive.read_only;
        artifact.mount_path = drive.mount_path;
        artifact.sub_path = drive.sub_path;
        artifact.virtual_size = *drive.virtual_size;
        artifact.image_config_path = drive.image_config_path;
        updated.attached_drives.push_back(artifact);
    }
    return updated;
}

core::Json SandboxSnapshotManifest::ToJson() const {
    core::JsonObject object;
    object["version"] = core::Json(static_cast<int64_t>(version));
    object["backend"] = core::Json(backend);

    // `vmState` keeps an empty body: its only field is `#[serde(skip)]`.
    object["vmState"] = core::Json(core::JsonObject());

    core::JsonObject memory;
    memory["virtualSize"] = core::Json(static_cast<int64_t>(this->memory.virtual_size));
    object["memory"] = core::Json(memory);

    core::JsonObject rootfs;
    rootfs["virtualSize"] = core::Json(static_cast<int64_t>(this->rootfs.virtual_size));
    object["rootfs"] = core::Json(rootfs);

    core::JsonArray drives;
    for (std::size_t i = 0; i < attached_drives.size(); ++i) {
        drives.push_back(attached_drives[i].ToJson());
    }
    object["attachedDrives"] = core::Json(drives);

    object["volumeDriveSlots"] = core::Json(static_cast<int64_t>(volume_drive_slots));
    object["physicalExtraDriveCount"] =
        core::Json(static_cast<int64_t>(physical_extra_drive_count));
    if (memory_startup_pack.has_value()) {
        object["memoryStartupPack"] = memory_startup_pack->ToJson();
    }
    return core::Json(object);
}

core::Expected<SandboxSnapshotManifest, std::string> SandboxSnapshotManifest::FromJson(
    const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("sandbox snapshot manifest must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    SandboxSnapshotManifest manifest;

    const core::Expected<uint64_t, std::string> version = RequireUint(fields, "version");
    if (!version.ok()) return core::make_unexpected(version.error());
    manifest.version = static_cast<uint32_t>(version.value());

    // `#[serde(default = "default_backend")]`.
    manifest.backend = OptionalString(fields, "backend", DefaultBackend());

    const core::Optional<core::Json> memory = NestedObject(fields, "memory");
    if (!memory.has_value()) {
        return core::make_unexpected(std::string("missing field `memory`"));
    }
    const core::Expected<uint64_t, std::string> mem_size =
        RequireUint(memory->as_object(), "virtualSize");
    if (!mem_size.ok()) {
        return core::make_unexpected(std::string("memory: ") + mem_size.error());
    }
    manifest.memory.virtual_size = mem_size.value();

    const core::Optional<core::Json> rootfs = NestedObject(fields, "rootfs");
    if (!rootfs.has_value()) {
        return core::make_unexpected(std::string("missing field `rootfs`"));
    }
    const core::Expected<uint64_t, std::string> rootfs_size =
        RequireUint(rootfs->as_object(), "virtualSize");
    if (!rootfs_size.ok()) {
        return core::make_unexpected(std::string("rootfs: ") + rootfs_size.error());
    }
    manifest.rootfs.virtual_size = rootfs_size.value();

    const core::JsonObject::const_iterator drives = fields.find("attachedDrives");
    if (drives == fields.end() || drives->second.kind() != core::Json::Kind::Array) {
        return core::make_unexpected(std::string("missing field `attachedDrives`"));
    }
    const core::JsonArray& drive_array = drives->second.as_array();
    for (std::size_t i = 0; i < drive_array.size(); ++i) {
        const core::Expected<SnapshotAttachedDriveArtifacts, std::string> drive =
            SnapshotAttachedDriveArtifacts::FromJson(drive_array[i]);
        if (!drive.ok()) return core::make_unexpected(drive.error());
        manifest.attached_drives.push_back(drive.value());
    }

    manifest.volume_drive_slots =
        static_cast<std::size_t>(OptionalUint(fields, "volumeDriveSlots"));
    manifest.physical_extra_drive_count =
        static_cast<std::size_t>(OptionalUint(fields, "physicalExtraDriveCount"));

    const core::JsonObject::const_iterator pack = fields.find("memoryStartupPack");
    if (pack != fields.end() && pack->second.kind() == core::Json::Kind::Object) {
        const core::Expected<snapshot::ResolvedStartupPack, std::string> resolved =
            snapshot::ResolvedStartupPack::FromJson(pack->second);
        if (!resolved.ok()) return core::make_unexpected(resolved.error());
        manifest.memory_startup_pack =
            core::Optional<snapshot::ResolvedStartupPack>(resolved.value());
    }

    return manifest;
}

bool SandboxSnapshotManifest::operator==(const SandboxSnapshotManifest& o) const {
    if (version != o.version || backend != o.backend || vm_state != o.vm_state ||
        memory != o.memory || rootfs != o.rootfs ||
        volume_drive_slots != o.volume_drive_slots ||
        physical_extra_drive_count != o.physical_extra_drive_count) {
        return false;
    }
    if (attached_drives != o.attached_drives) return false;
    if (memory_startup_pack.has_value() != o.memory_startup_pack.has_value()) return false;
    if (memory_startup_pack.has_value() && *memory_startup_pack != *o.memory_startup_pack) {
        return false;
    }
    return true;
}

}  // namespace sandbox
}  // namespace agentenv
