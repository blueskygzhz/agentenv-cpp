// SPDX-License-Identifier: MIT
// Rust: src/sandbox/ublk/device.rs
#include "agentenv/sandbox/ublk.h"

#include "agentenv/core/serde.h"

namespace agentenv {
namespace sandbox {
namespace ublk {

UblkConfig UblkConfig::OverlaybdWithRuntimeUpperMode(
    const std::string& image_config_path, bool read_only,
    storage::overlaybd::UpperMode runtime_upper_mode) {
    UblkConfig config;
    config.backend                     = UblkBackendKind::Overlaybd;
    config.overlaybd.image_config_path = image_config_path;
    config.overlaybd.read_only         = read_only;
    config.overlaybd.runtime_upper_mode = runtime_upper_mode;
    return config;
}

// ---- serialization --------------------------------------------------------

namespace sd = core::serde;

core::Json OverlaybdDeviceConfig::ToJson() const {
    core::JsonObject object;
    object["image_config_path"] = core::Json(image_config_path);
    object["read_only"] = core::Json(read_only);
    object["runtime_upper_mode"] =
        core::Json(std::string(storage::overlaybd::UpperModeToString(runtime_upper_mode)));
    return core::Json(object);
}

core::Expected<OverlaybdDeviceConfig, std::string> OverlaybdDeviceConfig::FromJson(
    const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("invalid type: expected struct OverlaybdConfig"));
    }
    const core::JsonObject& fields = json.as_object();
    OverlaybdDeviceConfig config;

    const core::Expected<std::string, std::string> path =
        sd::RequireString(fields, "image_config_path");
    if (!path.ok()) return core::make_unexpected(path.error());
    config.image_config_path = path.value();

    const core::Expected<bool, std::string> read_only = sd::RequireBool(fields, "read_only");
    if (!read_only.ok()) return core::make_unexpected(read_only.error());
    config.read_only = read_only.value();

    // Rust `#[serde(default = "default_runtime_upper_mode")]`.
    const core::Json* mode = sd::OptValue(fields, "runtime_upper_mode");
    if (mode == NULL) {
        config.runtime_upper_mode = storage::overlaybd::UpperMode::LogStructured;
    } else {
        if (mode->kind() != core::Json::Kind::String) {
            return core::make_unexpected(sd::InvalidType("runtime_upper_mode", "a string"));
        }
        const core::Expected<storage::overlaybd::UpperMode, std::string> parsed =
            storage::overlaybd::UpperModeParse(mode->as_string());
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        config.runtime_upper_mode = parsed.value();
    }
    return config;
}

/// Rust's externally tagged `UblkBackend::Overlaybd`.
static const char* const kOverlaybdVariant = "Overlaybd";
/// Rust `PersistedUblkBackend::LegacyCow`, `#[serde(rename = "Cow")]`. A unit
/// variant, so serde writes it as the bare string `"Cow"`.
static const char* const kLegacyCowVariant = "Cow";

core::Json UblkConfig::ToJson() const {
    core::JsonObject backend_object;
    backend_object[kOverlaybdVariant] = overlaybd.ToJson();
    core::JsonObject object;
    object["backend"] = core::Json(backend_object);
    return core::Json(object);
}

core::Expected<UblkConfig, std::string> UblkConfig::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("invalid type: expected struct UblkConfig"));
    }
    const core::Json* backend = sd::OptValue(json.as_object(), "backend");
    if (backend == NULL) return core::make_unexpected(sd::MissingField("backend"));

    // The legacy backend is a unit variant, so it arrives as a bare string.
    // Naming it is the whole point: the operator needs to know the sandbox has
    // to be rebuilt, not that "the record was corrupt".
    if (backend->kind() == core::Json::Kind::String &&
        backend->as_string() == kLegacyCowVariant) {
        return core::make_unexpected(std::string(
            "the legacy CoW ublk backend is no longer supported; this persisted sandbox "
            "cannot be resumed and must be rebuilt with OverlayBD"));
    }
    if (backend->kind() != core::Json::Kind::Object) {
        return core::make_unexpected(sd::InvalidType("backend", "enum UblkBackend"));
    }
    const core::Json* payload = sd::OptValue(backend->as_object(), kOverlaybdVariant);
    if (payload == NULL) {
        return core::make_unexpected(
            std::string("unknown variant, expected `Overlaybd`"));
    }
    const core::Expected<OverlaybdDeviceConfig, std::string> overlaybd =
        OverlaybdDeviceConfig::FromJson(*payload);
    if (!overlaybd.ok()) return core::make_unexpected(overlaybd.error());

    UblkConfig config;
    config.backend = UblkBackendKind::Overlaybd;
    config.overlaybd = overlaybd.value();
    return config;
}

}  // namespace ublk
}  // namespace sandbox
}  // namespace agentenv
