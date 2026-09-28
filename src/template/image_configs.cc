// SPDX-License-Identifier: MIT
// Rust: src/types/image_configs.rs — the `ImageConfigs` serialisation only.
//
// Split from `build_spec.cc` because `snapshot::CommittedSnapshot` carries an
// `ImageConfigs`, while `agentenv-template` already depends on
// `agentenv-snapshot`. Rust has one crate and no such constraint.
#include "agentenv/template/build_spec.h"

namespace agentenv {
namespace tpl {

bool ImageConfigEntry::operator==(const ImageConfigEntry& o) const {
    if (mount_path != o.mount_path) return false;
    if (drive_id.has_value() != o.drive_id.has_value()) return false;
    if (drive_id.has_value() && *drive_id != *o.drive_id) return false;
    // `Json` has no operator==, and the serialized form is what the embedding
    // site compares anyway.
    return config.ToString() == o.config.ToString();
}

void ImageConfigs::Add(const core::Optional<std::string>& drive_id, const std::string& mount_path,
                       const core::Json& config) {
    ImageConfigEntry entry;
    entry.drive_id = drive_id;
    entry.mount_path = mount_path;
    entry.config = config;
    entries_.push_back(entry);
}

core::Json ImageConfigs::ToJson() const {
    // `#[serde(transparent)]`: the wire form is the inner Vec itself.
    core::JsonArray array;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        core::JsonObject entry;
        if (entries_[i].drive_id.has_value()) {
            // Unset means the rootfs; writing null would be a third state.
            entry["drive_id"] = core::Json(*entries_[i].drive_id);
        }
        entry["mount_path"] = core::Json(entries_[i].mount_path);
        entry["config"] = entries_[i].config;
        array.push_back(core::Json(entry));
    }
    return core::Json(array);
}

core::Expected<ImageConfigs, std::string> ImageConfigs::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Array) {
        return core::make_unexpected(std::string("image configs must be an array"));
    }
    ImageConfigs configs;
    const core::JsonArray& array = json.as_array();
    for (std::size_t i = 0; i < array.size(); ++i) {
        if (array[i].kind() != core::Json::Kind::Object) {
            return core::make_unexpected(std::string("image config entry must be an object"));
        }
        const core::JsonObject& fields = array[i].as_object();

        ImageConfigEntry entry;
        const core::JsonObject::const_iterator drive_id = fields.find("drive_id");
        if (drive_id != fields.end() && drive_id->second.kind() == core::Json::Kind::String) {
            entry.drive_id = core::Optional<std::string>(drive_id->second.as_string());
        }

        const core::JsonObject::const_iterator mount_path = fields.find("mount_path");
        if (mount_path == fields.end() ||
            mount_path->second.kind() != core::Json::Kind::String) {
            return core::make_unexpected(std::string("missing field `mount_path`"));
        }
        entry.mount_path = mount_path->second.as_string();

        const core::JsonObject::const_iterator config = fields.find("config");
        if (config == fields.end()) {
            return core::make_unexpected(std::string("missing field `config`"));
        }
        entry.config = config->second;

        configs.entries_.push_back(entry);
    }
    return configs;
}

}  // namespace tpl
}  // namespace agentenv
