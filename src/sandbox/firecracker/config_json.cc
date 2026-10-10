// SPDX-License-Identifier: MIT
// Rust: the `Serialize` / `Deserialize` derives on
// `FirecrackerRuntimePolicy`, `FirecrackerCommonConfig` and
// `FirecrackerSnapshotConfig` (src/sandbox/firecracker/config.rs).
//
// This is the on-disk form of a paused sandbox: `FirecrackerPausedState`
// encodes exactly this, the persister stores it, and a restart decodes it.
// The format is therefore a compatibility contract with the Rust server, not
// an internal detail — a field written under the wrong name or shape makes a
// record that the other implementation drops, taking the sandbox's artifacts
// with it.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "agentenv/core/serde.h"
#include "agentenv/sandbox/firecracker/config.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {
namespace {

namespace sd = core::serde;

const uint64_t kNanosPerMilli = 1000000ull;
const uint64_t kMillisPerSec = 1000ull;

/// serde's representation of `std::time::Duration`.
core::Json DurationToJson(uint64_t millis) {
    core::JsonObject object;
    object["secs"] = core::Json(static_cast<int64_t>(millis / kMillisPerSec));
    object["nanos"] = core::Json(static_cast<int64_t>((millis % kMillisPerSec) * kNanosPerMilli));
    return core::Json(object);
}

core::Expected<uint64_t, std::string> DurationFromJson(const core::JsonObject& fields,
                                                       const std::string& field) {
    const core::Expected<const core::JsonObject*, std::string> object =
        sd::RequireObject(fields, field);
    if (!object.ok()) return core::make_unexpected(object.error());
    const core::Expected<uint64_t, std::string> secs = sd::RequireUint(*object.value(), "secs");
    if (!secs.ok()) return core::make_unexpected(field + ": " + secs.error());
    // serde's `Duration` keeps `nanos` below one second; anything larger is a
    // malformed record rather than a longer timeout.
    const core::Expected<uint64_t, std::string> nanos =
        sd::RequireUint(*object.value(), "nanos", 999999999ull);
    if (!nanos.ok()) return core::make_unexpected(field + ": " + nanos.error());
    return secs.value() * kMillisPerSec + nanos.value() / kNanosPerMilli;
}

/// Rust `Option<String>` for a path: serialized as null when absent (no
/// `skip_serializing_if` on these fields).
void PutOptString(core::JsonObject& object, const std::string& key,
                  const core::Optional<std::string>& value) {
    object[key] = value.has_value() ? core::Json(*value) : core::Json();
}

}  // namespace

// ---- RuntimePolicy --------------------------------------------------------

core::Json RuntimePolicy::ToJson() const {
    core::JsonObject object;
    object["socket_timeout"] = DurationToJson(socket_timeout_ms);
    object["socket_poll_interval"] = DurationToJson(socket_poll_interval_ms);
    object["envd_timeout"] = DurationToJson(envd_timeout_ms);
    object["envd_poll_interval"] = DurationToJson(envd_poll_interval_ms);
    return core::Json(object);
}

core::Expected<RuntimePolicy, std::string> RuntimePolicy::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(
            std::string("invalid type: expected struct FirecrackerRuntimePolicy"));
    }
    const core::JsonObject& fields = json.as_object();
    RuntimePolicy policy;
    const char* const kNames[4] = {"socket_timeout", "socket_poll_interval", "envd_timeout",
                                   "envd_poll_interval"};
    uint64_t* const kTargets[4] = {&policy.socket_timeout_ms, &policy.socket_poll_interval_ms,
                                   &policy.envd_timeout_ms, &policy.envd_poll_interval_ms};
    for (int i = 0; i < 4; ++i) {
        const core::Expected<uint64_t, std::string> v = DurationFromJson(fields, kNames[i]);
        if (!v.ok()) return core::make_unexpected(v.error());
        *kTargets[i] = v.value();
    }
    return policy;
}

// ---- CommonConfig ---------------------------------------------------------

core::Json CommonConfig::ToJson() const {
    core::JsonObject object;
    object["firecracker_binary"] = core::Json(firecracker_binary);
    object["tools_drive_version"] = core::Json(tools_drive_version);
    PutOptString(object, "firecracker_work_base_dir", firecracker_work_base_dir);
    PutOptString(object, "serial_output_base_dir", serial_output_base_dir);
    PutOptString(object, "stdout_path", stdout_path);
    PutOptString(object, "stderr_path", stderr_path);
    PutOptString(object, "firecracker_log_level", firecracker_log_level);
    object["runtime_policy"] = runtime_policy.ToJson();
    object["track_dirty_pages"] = core::Json(track_dirty_pages);
    object["envd_version"] = core::Json(envd_version);
    object["control_plane_port"] = core::Json(static_cast<int64_t>(control_plane_port));

    if (env_vars.has_value()) {
        core::JsonObject env;
        for (std::map<std::string, std::string>::const_iterator it = env_vars->begin();
             it != env_vars->end(); ++it) {
            env[it->first] = core::Json(it->second);
        }
        object["env_vars"] = core::Json(env);
    } else {
        object["env_vars"] = core::Json();
    }
    PutOptString(object, "default_workdir", default_workdir);
    PutOptString(object, "default_user", default_user);

    object["mmds_metadata"] =
        mmds_metadata.has_value() ? mmds_metadata->ToValue() : core::Json();
    object["rootfs_image_config"] =
        rootfs_image_config.has_value() ? rootfs_image_config->ToJson() : core::Json();
    object["rootfs_virtual_size"] = rootfs_virtual_size.has_value()
                                        ? core::Json(static_cast<int64_t>(*rootfs_virtual_size))
                                        : core::Json();
    object["rootfs_allow_shrink"] = core::Json(rootfs_allow_shrink);

    core::JsonArray drives;
    for (std::size_t i = 0; i < extra_drives.size(); ++i) {
        drives.push_back(extra_drives[i].ToJson());
    }
    object["extra_drives"] = core::Json(drives);
    object["physical_extra_drive_count"] =
        core::Json(static_cast<int64_t>(physical_extra_drive_count));
    object["volume_drive_slots"] = core::Json(static_cast<int64_t>(volume_drive_slots));

    object["ublk_config"] = ublk_config.has_value() ? ublk_config->ToJson() : core::Json();
    PutOptString(object, "cpu_config_json", cpu_config_json);
    object["network_policy"] =
        network_policy.has_value() ? network_policy->ToJson() : core::Json();

    // Rust `#[serde(default, skip_serializing_if = "Option::is_none")]`, and
    // the value is a `serde_json::Map` — an embedded object, not a string
    // holding JSON. A malformed stored value is dropped rather than written
    // as a quoted string, which would not read back as params on either side.
    if (custom_extension_params_json.has_value()) {
        const core::Expected<core::Json, core::AnyError> parsed =
            core::Json::Parse(*custom_extension_params_json);
        if (parsed.ok() && parsed.value().kind() == core::Json::Kind::Object) {
            object["custom_extension_params"] = parsed.value();
        }
    }
    object["disk_rate_limit"] = disk_rate_limit.ToJson();
    // `envd_access_token` is `#[serde(skip)]`: deliberately absent.
    return core::Json(object);
}

core::Expected<CommonConfig, std::string> CommonConfig::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(
            std::string("invalid type: expected struct FirecrackerCommonConfig"));
    }
    const core::JsonObject& fields = json.as_object();
    CommonConfig config;

    const core::Expected<std::string, std::string> binary =
        sd::RequireString(fields, "firecracker_binary");
    if (!binary.ok()) return core::make_unexpected(binary.error());
    config.firecracker_binary = binary.value();

    const core::Expected<std::string, std::string> tools =
        sd::DefaultString(fields, "tools_drive_version");
    if (!tools.ok()) return core::make_unexpected(tools.error());
    config.tools_drive_version = tools.value();

    const char* const kOptPaths[5] = {"firecracker_work_base_dir", "serial_output_base_dir",
                                      "stdout_path", "stderr_path", "firecracker_log_level"};
    core::Optional<std::string>* const kOptTargets[5] = {
        &config.firecracker_work_base_dir, &config.serial_output_base_dir, &config.stdout_path,
        &config.stderr_path, &config.firecracker_log_level};
    for (int i = 0; i < 5; ++i) {
        const core::Expected<core::Optional<std::string>, std::string> v =
            sd::OptString(fields, kOptPaths[i]);
        if (!v.ok()) return core::make_unexpected(v.error());
        *kOptTargets[i] = v.value();
    }

    const core::Json* policy = sd::OptValue(fields, "runtime_policy");
    if (policy == NULL) return core::make_unexpected(sd::MissingField("runtime_policy"));
    const core::Expected<RuntimePolicy, std::string> parsed_policy =
        RuntimePolicy::FromJson(*policy);
    if (!parsed_policy.ok()) return core::make_unexpected(parsed_policy.error());
    config.runtime_policy = parsed_policy.value();

    // `#[serde(default)]` on a `bool` is false — *not* the `true` that
    // `CommonConfig::New` sets. A record written before the field existed
    // described a VM without dirty-page tracking, and resuming it as if it had
    // tracking would mean capturing a memory snapshot the guest never backed.
    const core::Expected<bool, std::string> track = sd::DefaultBool(fields, "track_dirty_pages");
    if (!track.ok()) return core::make_unexpected(track.error());
    config.track_dirty_pages = track.value();

    const core::Expected<std::string, std::string> envd =
        sd::RequireString(fields, "envd_version");
    if (!envd.ok()) return core::make_unexpected(envd.error());
    config.envd_version = envd.value();

    const core::Expected<uint64_t, std::string> port =
        sd::RequireUint(fields, "control_plane_port", 0xffffull);
    if (!port.ok()) return core::make_unexpected(port.error());
    config.control_plane_port = static_cast<uint16_t>(port.value());

    if (const core::Json* env = sd::OptValue(fields, "env_vars")) {
        if (env->kind() != core::Json::Kind::Object) {
            return core::make_unexpected(sd::InvalidType("env_vars", "a map"));
        }
        std::map<std::string, std::string> parsed_env;
        const core::JsonObject& entries = env->as_object();
        for (core::JsonObject::const_iterator it = entries.begin(); it != entries.end(); ++it) {
            if (it->second.kind() != core::Json::Kind::String) {
                return core::make_unexpected(sd::InvalidType("env_vars", "string values"));
            }
            parsed_env[it->first] = it->second.as_string();
        }
        config.env_vars = parsed_env;
    }

    const core::Expected<core::Optional<std::string>, std::string> workdir =
        sd::OptString(fields, "default_workdir");
    if (!workdir.ok()) return core::make_unexpected(workdir.error());
    config.default_workdir = workdir.value();

    const core::Expected<core::Optional<std::string>, std::string> user =
        sd::OptString(fields, "default_user");
    if (!user.ok()) return core::make_unexpected(user.error());
    config.default_user = user.value();

    if (const core::Json* mmds = sd::OptValue(fields, "mmds_metadata")) {
        const core::Expected<MmdsMetadata, std::string> parsed_mmds =
            MmdsMetadata::FromValue(*mmds);
        if (!parsed_mmds.ok()) return core::make_unexpected(parsed_mmds.error());
        config.mmds_metadata = parsed_mmds.value();
    }

    if (const core::Json* rootfs = sd::OptValue(fields, "rootfs_image_config")) {
        const core::Expected<ublk::OverlaybdDeviceConfig, std::string> parsed_rootfs =
            ublk::OverlaybdDeviceConfig::FromJson(*rootfs);
        if (!parsed_rootfs.ok()) return core::make_unexpected(parsed_rootfs.error());
        config.rootfs_image_config = parsed_rootfs.value();
    }

    const core::Expected<core::Optional<uint64_t>, std::string> rootfs_size =
        sd::OptUint(fields, "rootfs_virtual_size");
    if (!rootfs_size.ok()) return core::make_unexpected(rootfs_size.error());
    config.rootfs_virtual_size = rootfs_size.value();

    const core::Expected<bool, std::string> shrink =
        sd::DefaultBool(fields, "rootfs_allow_shrink");
    if (!shrink.ok()) return core::make_unexpected(shrink.error());
    config.rootfs_allow_shrink = shrink.value();

    // `extra_drives` has no serde default: a record that cannot say which
    // drives the guest had must not resume as if it had none.
    const core::Expected<const core::JsonArray*, std::string> drives =
        sd::RequireArray(fields, "extra_drives");
    if (!drives.ok()) return core::make_unexpected(drives.error());
    for (std::size_t i = 0; i < drives.value()->size(); ++i) {
        const core::Expected<ExtraDrive, std::string> drive =
            ExtraDrive::FromJson((*drives.value())[i]);
        if (!drive.ok()) return core::make_unexpected(drive.error());
        config.extra_drives.push_back(drive.value());
    }

    const core::Expected<uint64_t, std::string> physical =
        sd::DefaultUint(fields, "physical_extra_drive_count");
    if (!physical.ok()) return core::make_unexpected(physical.error());
    config.physical_extra_drive_count = static_cast<std::size_t>(physical.value());

    const core::Expected<uint64_t, std::string> slots =
        sd::DefaultUint(fields, "volume_drive_slots");
    if (!slots.ok()) return core::make_unexpected(slots.error());
    config.volume_drive_slots = static_cast<std::size_t>(slots.value());

    if (const core::Json* ublk = sd::OptValue(fields, "ublk_config")) {
        const core::Expected<ublk::UblkConfig, std::string> parsed_ublk =
            ublk::UblkConfig::FromJson(*ublk);
        if (!parsed_ublk.ok()) return core::make_unexpected(parsed_ublk.error());
        config.ublk_config = parsed_ublk.value();
    }

    const core::Expected<core::Optional<std::string>, std::string> cpu =
        sd::OptString(fields, "cpu_config_json");
    if (!cpu.ok()) return core::make_unexpected(cpu.error());
    config.cpu_config_json = cpu.value();

    if (const core::Json* net = sd::OptValue(fields, "network_policy")) {
        const core::Expected<network::SandboxNetworkPolicy, std::string> parsed_net =
            network::SandboxNetworkPolicy::FromJson(*net);
        if (!parsed_net.ok()) return core::make_unexpected(parsed_net.error());
        config.network_policy = parsed_net.value();
    }

    if (const core::Json* params = sd::OptValue(fields, "custom_extension_params")) {
        if (params->kind() != core::Json::Kind::Object) {
            return core::make_unexpected(sd::InvalidType("custom_extension_params", "a map"));
        }
        config.custom_extension_params_json = params->ToString();
    }

    // `#[serde(default)]` on the container field: a missing object means the
    // default (no limiting), but a present one must be complete.
    if (const core::Json* limit = sd::OptValue(fields, "disk_rate_limit")) {
        const core::Expected<cfg::DiskRateLimitConfig, std::string> parsed_limit =
            cfg::DiskRateLimitConfig::FromJson(*limit);
        if (!parsed_limit.ok()) return core::make_unexpected(parsed_limit.error());
        config.disk_rate_limit = parsed_limit.value();
    } else {
        config.disk_rate_limit = cfg::DiskRateLimitConfig();
    }

    // `envd_access_token` stays unset: `#[serde(skip)]` on both sides.
    return config;
}

// ---- SnapshotConfig -------------------------------------------------------

core::Json SnapshotConfig::ToJson() const {
    core::JsonObject object;
    object["common"] = common.ToJson();
    object["vm_state_path"] = core::Json(vm_state_path);
    object["mem_overlaybd_config"] = mem_overlaybd_config.ToJson();
    object["mem_virtual_size"] = core::Json(static_cast<int64_t>(mem_virtual_size));
    return core::Json(object);
}

core::Expected<SnapshotConfig, std::string> SnapshotConfig::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(
            std::string("invalid type: expected struct FirecrackerSnapshotConfig"));
    }
    const core::JsonObject& fields = json.as_object();
    SnapshotConfig config;

    const core::Json* common_value = sd::OptValue(fields, "common");
    if (common_value == NULL) return core::make_unexpected(sd::MissingField("common"));
    const core::Expected<CommonConfig, std::string> common =
        CommonConfig::FromJson(*common_value);
    if (!common.ok()) return core::make_unexpected(common.error());
    config.common = common.value();

    const core::Expected<std::string, std::string> vm_state =
        sd::RequireString(fields, "vm_state_path");
    if (!vm_state.ok()) return core::make_unexpected(vm_state.error());
    config.vm_state_path = vm_state.value();

    const core::Json* mem = sd::OptValue(fields, "mem_overlaybd_config");
    if (mem == NULL) return core::make_unexpected(sd::MissingField("mem_overlaybd_config"));
    const core::Expected<ublk::OverlaybdDeviceConfig, std::string> mem_config =
        ublk::OverlaybdDeviceConfig::FromJson(*mem);
    if (!mem_config.ok()) return core::make_unexpected(mem_config.error());
    config.mem_overlaybd_config = mem_config.value();

    const core::Expected<uint64_t, std::string> mem_size =
        sd::RequireUint(fields, "mem_virtual_size");
    if (!mem_size.ok()) return core::make_unexpected(mem_size.error());
    config.mem_virtual_size = mem_size.value();

    // `pack_recording` / `memory_startup_pack` / `managed_snapshot_root` are
    // `#[serde(skip)]` and stay at their defaults.
    return config;
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
