// SPDX-License-Identifier: MIT
// Rust: src/snapshot/types/drive.rs
#include "agentenv/snapshot/drive.h"

namespace agentenv {
namespace snapshot {
namespace {

/// Both variants are `Overlaybd`, so the externally tagged wrapper is shared.
const char* const kOverlaybdTag = "Overlaybd";

core::Expected<core::Json, std::string> UnwrapOverlaybd(const core::Json& json,
                                                        const std::string& what) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(what + " must be an object");
    }
    const core::JsonObject& fields = json.as_object();
    const core::JsonObject::const_iterator tagged = fields.find(kOverlaybdTag);
    if (tagged == fields.end() || tagged->second.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(what + " must be tagged `Overlaybd`");
    }
    return tagged->second;
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

core::Expected<uint64_t, std::string> RequireUint(const core::JsonObject& fields,
                                                  const std::string& field) {
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end()) {
        return core::make_unexpected(std::string("missing field `") + field + "`");
    }
    if (it->second.kind() != core::Json::Kind::Int) {
        return core::make_unexpected(std::string("field `") + field + "` must be an integer");
    }
    if (it->second.as_int() < 0) {
        return core::make_unexpected(std::string("field `") + field + "` must not be negative");
    }
    return static_cast<uint64_t>(it->second.as_int());
}

std::string OptionalString(const core::JsonObject& fields, const std::string& field) {
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end() || it->second.kind() != core::Json::Kind::String) {
        return std::string();
    }
    return it->second.as_string();
}

core::Optional<std::string> OptionalSubPath(const core::JsonObject& fields) {
    const core::JsonObject::const_iterator it = fields.find("sub_path");
    if (it == fields.end() || it->second.kind() != core::Json::Kind::String) {
        return core::Optional<std::string>();
    }
    return core::Optional<std::string>(it->second.as_string());
}

}  // namespace

// ---- ResolvedAttachedDrive ------------------------------------------------

sandbox::ExtraDrive ResolvedAttachedDrive::ToExtraDrive() const {
    sandbox::ExtraDrive drive;
    drive.kind = sandbox::ExtraDrive::Kind::Overlaybd;
    drive.drive_id = drive_id;
    drive.image_config_path = image_config_path;
    drive.read_only = read_only;
    drive.mount_path = mount_path;
    // A resolved drive always knows its size, so this is never absent here
    // even though `ExtraDrive` allows it during a fresh launch.
    drive.virtual_size = core::Optional<uint64_t>(virtual_size);
    drive.sub_path = sub_path;
    drive.snapshot_output_dir = core::Optional<std::string>();
    drive.volume = false;
    return drive;
}

core::Json ResolvedAttachedDrive::ToJson() const {
    core::JsonObject inner;
    inner["drive_id"] = core::Json(drive_id);
    inner["image_config_path"] = core::Json(image_config_path);
    inner["read_only"] = core::Json(read_only);
    inner["virtual_size"] = core::Json(static_cast<int64_t>(virtual_size));
    inner["mount_path"] = core::Json(mount_path);
    if (sub_path.has_value()) inner["sub_path"] = core::Json(*sub_path);

    core::JsonObject object;
    object[kOverlaybdTag] = core::Json(inner);
    return core::Json(object);
}

core::Expected<ResolvedAttachedDrive, std::string> ResolvedAttachedDrive::FromJson(
    const core::Json& json) {
    const core::Expected<core::Json, std::string> unwrapped =
        UnwrapOverlaybd(json, "resolved attached drive");
    if (!unwrapped.ok()) return core::make_unexpected(unwrapped.error());
    const core::JsonObject& fields = unwrapped.value().as_object();

    ResolvedAttachedDrive drive;
    const core::Expected<std::string, std::string> id = RequireString(fields, "drive_id");
    if (!id.ok()) return core::make_unexpected(id.error());
    drive.drive_id = id.value();

    const core::Expected<std::string, std::string> path =
        RequireString(fields, "image_config_path");
    if (!path.ok()) return core::make_unexpected(path.error());
    drive.image_config_path = path.value();

    const core::Expected<bool, std::string> read_only = RequireBool(fields, "read_only");
    if (!read_only.ok()) return core::make_unexpected(read_only.error());
    drive.read_only = read_only.value();

    // No serde default: a drive of unknown size cannot be attached, so this
    // must fail rather than boot a zero-sized device.
    const core::Expected<uint64_t, std::string> size = RequireUint(fields, "virtual_size");
    if (!size.ok()) {
        return core::make_unexpected(std::string("virtual_size: ") + size.error());
    }
    drive.virtual_size = size.value();

    drive.mount_path = OptionalString(fields, "mount_path");
    drive.sub_path = OptionalSubPath(fields);
    return drive;
}

bool ResolvedAttachedDrive::operator==(const ResolvedAttachedDrive& o) const {
    if (kind != o.kind || drive_id != o.drive_id ||
        image_config_path != o.image_config_path || read_only != o.read_only ||
        virtual_size != o.virtual_size || mount_path != o.mount_path) {
        return false;
    }
    if (sub_path.has_value() != o.sub_path.has_value()) return false;
    return !sub_path.has_value() || *sub_path == *o.sub_path;
}

// ---- CommittedAttachedDrive -----------------------------------------------

core::Json CommittedAttachedDrive::ToJson() const {
    core::JsonObject inner;
    inner["drive_id"] = core::Json(drive_id);
    inner["layers"] = LayerRefsToJson(layers);
    inner["read_only"] = core::Json(read_only);
    inner["virtual_size"] = core::Json(static_cast<int64_t>(virtual_size));
    inner["mount_path"] = core::Json(mount_path);
    if (sub_path.has_value()) inner["sub_path"] = core::Json(*sub_path);

    core::JsonObject object;
    object[kOverlaybdTag] = core::Json(inner);
    return core::Json(object);
}

core::Expected<CommittedAttachedDrive, std::string> CommittedAttachedDrive::FromJson(
    const core::Json& json) {
    const core::Expected<core::Json, std::string> unwrapped =
        UnwrapOverlaybd(json, "committed attached drive");
    if (!unwrapped.ok()) return core::make_unexpected(unwrapped.error());
    const core::JsonObject& fields = unwrapped.value().as_object();

    CommittedAttachedDrive drive;
    const core::Expected<std::string, std::string> id = RequireString(fields, "drive_id");
    if (!id.ok()) return core::make_unexpected(id.error());
    drive.drive_id = id.value();

    const core::JsonObject::const_iterator layers = fields.find("layers");
    if (layers == fields.end()) {
        return core::make_unexpected(std::string("missing field `layers`"));
    }
    const core::Expected<std::vector<OverlaybdLayerRef>, std::string> parsed =
        LayerRefsFromJson(layers->second);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());
    drive.layers = parsed.value();

    const core::Expected<bool, std::string> read_only = RequireBool(fields, "read_only");
    if (!read_only.ok()) return core::make_unexpected(read_only.error());
    drive.read_only = read_only.value();

    const core::Expected<uint64_t, std::string> size = RequireUint(fields, "virtual_size");
    if (!size.ok()) {
        return core::make_unexpected(std::string("virtual_size: ") + size.error());
    }
    drive.virtual_size = size.value();

    drive.mount_path = OptionalString(fields, "mount_path");
    drive.sub_path = OptionalSubPath(fields);
    return drive;
}

bool CommittedAttachedDrive::operator==(const CommittedAttachedDrive& o) const {
    if (kind != o.kind || drive_id != o.drive_id || read_only != o.read_only ||
        virtual_size != o.virtual_size || mount_path != o.mount_path) {
        return false;
    }
    if (!LayerRefsEqual(layers, o.layers)) return false;
    if (sub_path.has_value() != o.sub_path.has_value()) return false;
    return !sub_path.has_value() || *sub_path == *o.sub_path;
}

}  // namespace snapshot
}  // namespace agentenv
