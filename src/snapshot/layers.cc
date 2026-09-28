// SPDX-License-Identifier: MIT
// Rust: src/snapshot/types/snapshot.rs
#include "agentenv/snapshot/layers.h"

namespace agentenv {
namespace snapshot {
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

}  // namespace

bool LayerRefsEqual(const std::vector<OverlaybdLayerRef>& left,
                    const std::vector<OverlaybdLayerRef>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i] != right[i]) return false;
    }
    return true;
}

core::Json ManagedLayerToJson(const ManagedLayer& layer) {
    core::JsonObject object;
    object["digest"] = core::Json(layer.digest);
    object["size"] = core::Json(static_cast<int64_t>(layer.size));
    if (layer.uuid.has_value()) {
        // `skip_serializing_if = "Option::is_none"`: absent rather than null.
        object["uuid"] = core::Json(*layer.uuid);
    }
    return core::Json(object);
}

core::Expected<ManagedLayer, std::string> ManagedLayerFromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("managed layer must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    ManagedLayer layer;

    const core::Expected<std::string, std::string> digest = RequireString(fields, "digest");
    if (!digest.ok()) return core::make_unexpected(digest.error());
    layer.digest = digest.value();

    const core::Expected<uint64_t, std::string> size = RequireUint(fields, "size");
    if (!size.ok()) return core::make_unexpected(size.error());
    layer.size = size.value();

    const core::JsonObject::const_iterator uuid = fields.find("uuid");
    if (uuid != fields.end() && uuid->second.kind() == core::Json::Kind::String) {
        layer.uuid = core::Optional<std::string>(uuid->second.as_string());
    }
    return layer;
}

core::Json ExternalLayerToJson(const ExternalLayer& layer) {
    core::JsonObject object;
    object["digest"] = core::Json(layer.digest);
    object["repo_blob_url"] = core::Json(layer.repo_blob_url);
    object["size"] = core::Json(static_cast<int64_t>(layer.size));
    return core::Json(object);
}

core::Expected<ExternalLayer, std::string> ExternalLayerFromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("external layer must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    ExternalLayer layer;

    const core::Expected<std::string, std::string> digest = RequireString(fields, "digest");
    if (!digest.ok()) return core::make_unexpected(digest.error());
    layer.digest = digest.value();

    const core::Expected<std::string, std::string> url =
        RequireString(fields, "repo_blob_url");
    if (!url.ok()) return core::make_unexpected(url.error());
    layer.repo_blob_url = url.value();

    const core::Expected<uint64_t, std::string> size = RequireUint(fields, "size");
    if (!size.ok()) return core::make_unexpected(size.error());
    layer.size = size.value();

    return layer;
}

core::Json LayerRefToJson(const OverlaybdLayerRef& layer) {
    // Externally tagged, matching serde's default for a data-carrying enum.
    core::JsonObject object;
    if (layer.kind == OverlaybdLayerRef::Kind::Managed) {
        object["Managed"] = ManagedLayerToJson(layer.managed);
    } else {
        object["External"] = ExternalLayerToJson(layer.external);
    }
    return core::Json(object);
}

core::Expected<OverlaybdLayerRef, std::string> LayerRefFromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("layer reference must be an object"));
    }
    const core::JsonObject& fields = json.as_object();

    const core::JsonObject::const_iterator managed = fields.find("Managed");
    if (managed != fields.end()) {
        const core::Expected<ManagedLayer, std::string> layer =
            ManagedLayerFromJson(managed->second);
        if (!layer.ok()) return core::make_unexpected(layer.error());
        return OverlaybdLayerRef::Managed(layer.value());
    }

    const core::JsonObject::const_iterator external = fields.find("External");
    if (external != fields.end()) {
        const core::Expected<ExternalLayer, std::string> layer =
            ExternalLayerFromJson(external->second);
        if (!layer.ok()) return core::make_unexpected(layer.error());
        return OverlaybdLayerRef::External(layer.value());
    }

    // An unknown tag must not silently become a Managed layer with an empty
    // digest, which would later resolve to the wrong blob.
    return core::make_unexpected(
        std::string("layer reference must be tagged `Managed` or `External`"));
}

core::Json LayerRefsToJson(const std::vector<OverlaybdLayerRef>& layers) {
    core::JsonArray array;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        array.push_back(LayerRefToJson(layers[i]));
    }
    return core::Json(array);
}

core::Expected<std::vector<OverlaybdLayerRef>, std::string> LayerRefsFromJson(
    const core::Json& json) {
    if (json.kind() != core::Json::Kind::Array) {
        return core::make_unexpected(std::string("layer list must be an array"));
    }
    std::vector<OverlaybdLayerRef> layers;
    const core::JsonArray& values = json.as_array();
    for (std::size_t i = 0; i < values.size(); ++i) {
        const core::Expected<OverlaybdLayerRef, std::string> layer =
            LayerRefFromJson(values[i]);
        if (!layer.ok()) return core::make_unexpected(layer.error());
        layers.push_back(layer.value());
    }
    return layers;
}

}  // namespace snapshot
}  // namespace agentenv
