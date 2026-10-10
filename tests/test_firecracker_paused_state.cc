// SPDX-License-Identifier: MIT
// Rust: the Serialize/Deserialize derives on `FirecrackerRuntimePolicy`,
//       `FirecrackerCommonConfig`, `FirecrackerSnapshotConfig`,
//       `ExtraDrive`, `UblkConfig`, `OverlaybdConfig`, `DiskRateLimitConfig`,
//       plus `FirecrackerPausedState::{decode,encode,runtime_artifacts,
//       control_plane_port}` (src/sandbox/firecracker/sandbox.rs).
//
// This is the on-disk form of a paused sandbox, so these tests are about
// compatibility with what the Rust server writes, not about internal shape.
#include "microtest.h"

#include <string>

#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/sandbox/firecracker/factory.h"
#include "agentenv/sandbox/firecracker/paused_state.h"

using namespace agentenv;
using namespace agentenv::sandbox;
using namespace agentenv::sandbox::firecracker;
using agentenv::storage::overlaybd::UpperMode;

namespace {

core::Json J(const std::string& text) {
    core::Expected<core::Json, core::AnyError> v = core::Json::Parse(text);
    MT_EXPECT_TRUE(v.ok());
    return v.ok() ? v.value() : core::Json();
}

const core::Json& Field(const core::Json& object, const std::string& key) {
    static const core::Json kNull;
    const core::JsonObject& fields = object.as_object();
    core::JsonObject::const_iterator it = fields.find(key);
    return it == fields.end() ? kNull : it->second;
}

bool HasField(const core::Json& object, const std::string& key) {
    const core::JsonObject& fields = object.as_object();
    return fields.find(key) != fields.end();
}

struct TempDir {
    std::string path;
    TempDir() {
        core::Expected<std::string, std::string> d = core::fs::CreateTempDir("agentenv-paused-");
        path = d.ok() ? d.value() : std::string("/tmp/agentenv-paused-fallback");
    }
    ~TempDir() { (void)core::fs::RemoveDirAll(path); }
    std::string File(const std::string& name) const {
        const std::string p = path + "/" + name;
        MT_EXPECT_TRUE(core::fs::Write(p, "x").ok());
        return p;
    }
};

ExtraDrive Drive(const std::string& id) {
    ExtraDrive d;
    d.drive_id = id;
    d.image_config_path = "/img/" + id + ".json";
    d.read_only = false;
    d.mount_path = "/mnt/" + id;
    return d;
}

/// A config that passes `ValidatePersisted`: every artifact it names exists.
SnapshotConfig ValidSnapshotConfig(const TempDir& t) {
    SnapshotConfig config;
    config.common = CommonConfig::New("/usr/bin/firecracker", "1.2.3", RuntimePolicy());
    ublk::OverlaybdDeviceConfig rootfs;
    rootfs.image_config_path = t.File("rootfs.json");
    config.common.rootfs_image_config = rootfs;
    config.common.rootfs_virtual_size = 1024ull * 1024ull * 1024ull;
    config.common.ublk_config = ublk::UblkConfig::OverlaybdWithRuntimeUpperMode(
        rootfs.image_config_path, false, UpperMode::LogStructured);
    config.common.envd_version = "0.1.0";
    config.vm_state_path = t.File("vm_state.bin");
    config.mem_overlaybd_config.image_config_path = t.File("mem.json");
    config.mem_overlaybd_config.read_only = true;
    config.mem_virtual_size = 2048ull * 1024ull * 1024ull;
    return config;
}

}  // namespace

// ============================================================
// leaf types
// ============================================================

MT_TEST(overlaybd_device_config_round_trips_with_camelcase_upper_mode) {
    ublk::OverlaybdDeviceConfig config;
    config.image_config_path = "/img/a.json";
    config.read_only = true;
    config.runtime_upper_mode = UpperMode::HybridLogStructured;

    const core::Json json = config.ToJson();
    // Rust `#[serde(rename_all = "camelCase")]` on `UpperMode`.
    MT_EXPECT_EQ(Field(json, "runtime_upper_mode").as_string(),
                 std::string("hybridLogStructured"));

    core::Expected<ublk::OverlaybdDeviceConfig, std::string> back =
        ublk::OverlaybdDeviceConfig::FromJson(json);
    MT_EXPECT_TRUE(back.ok());
    MT_EXPECT_TRUE(back.value() == config);
}

MT_TEST(overlaybd_device_config_missing_upper_mode_defaults_to_log_structured) {
    // Rust `#[serde(default = "default_runtime_upper_mode")]` — a record
    // written before the field existed, not an error.
    core::Expected<ublk::OverlaybdDeviceConfig, std::string> back =
        ublk::OverlaybdDeviceConfig::FromJson(
            J("{\"image_config_path\":\"/img/a.json\",\"read_only\":false}"));
    MT_EXPECT_TRUE(back.ok());
    MT_EXPECT_TRUE(back.value().runtime_upper_mode == UpperMode::LogStructured);

    // The two genuinely required fields stay required.
    MT_EXPECT_TRUE(!ublk::OverlaybdDeviceConfig::FromJson(J("{\"read_only\":false}")).ok());
    MT_EXPECT_TRUE(
        !ublk::OverlaybdDeviceConfig::FromJson(J("{\"image_config_path\":\"/a\"}")).ok());
}

MT_TEST(ublk_config_is_externally_tagged) {
    const ublk::UblkConfig config = ublk::UblkConfig::OverlaybdWithRuntimeUpperMode(
        "/img/a.json", false, UpperMode::Sparse);
    const core::Json json = config.ToJson();
    // Rust writes `{"backend": {"Overlaybd": {...}}}`.
    MT_EXPECT_TRUE(Field(json, "backend").kind() == core::Json::Kind::Object);
    MT_EXPECT_TRUE(HasField(Field(json, "backend"), "Overlaybd"));

    core::Expected<ublk::UblkConfig, std::string> back = ublk::UblkConfig::FromJson(json);
    MT_EXPECT_TRUE(back.ok());
    MT_EXPECT_TRUE(back.value() == config);
}

MT_TEST(ublk_config_refuses_legacy_cow_backend_by_name) {
    // Rust's hand-written Deserialize rejects the removed backend with this
    // message. The sandbox must be rebuilt, so saying "corrupt record" would
    // send an operator looking in the wrong place.
    core::Expected<ublk::UblkConfig, std::string> back =
        ublk::UblkConfig::FromJson(J("{\"backend\":\"Cow\"}"));
    MT_EXPECT_TRUE(!back.ok());
    MT_EXPECT_TRUE(back.error().find("legacy CoW ublk backend is no longer supported") !=
                   std::string::npos);
    MT_EXPECT_TRUE(back.error().find("must be rebuilt with OverlayBD") != std::string::npos);

    MT_EXPECT_TRUE(!ublk::UblkConfig::FromJson(J("{}")).ok());
    MT_EXPECT_TRUE(!ublk::UblkConfig::FromJson(J("{\"backend\":{\"Zfs\":{}}}")).ok());
}

MT_TEST(extra_drive_omits_absent_optionals_and_defaults_the_rest) {
    ExtraDrive drive = Drive("data");
    const core::Json bare = drive.ToJson();
    const core::Json& inner = Field(bare, "Overlaybd");
    // `skip_serializing_if = "Option::is_none"`: omitted, not null.
    MT_EXPECT_TRUE(!HasField(inner, "virtual_size"));
    MT_EXPECT_TRUE(!HasField(inner, "sub_path"));
    MT_EXPECT_TRUE(!HasField(inner, "snapshot_output_dir"));
    MT_EXPECT_TRUE(HasField(inner, "mount_path"));
    MT_EXPECT_TRUE(HasField(inner, "volume"));

    drive.virtual_size = core::Optional<uint64_t>(4096);
    drive.sub_path = std::string("sub/dir");
    drive.snapshot_output_dir = std::string("/snap/out");
    drive.volume = true;
    core::Expected<ExtraDrive, std::string> back = ExtraDrive::FromJson(drive.ToJson());
    MT_EXPECT_TRUE(back.ok());
    MT_EXPECT_TRUE(back.value() == drive);
    MT_EXPECT_TRUE(back.value().volume);
    MT_EXPECT_EQ(*back.value().virtual_size, static_cast<uint64_t>(4096));
    MT_EXPECT_EQ(*back.value().snapshot_output_dir, std::string("/snap/out"));
}

MT_TEST(extra_drive_defaults_match_serde_not_the_constructors) {
    // `mount_path` is `#[serde(default)]` on a PathBuf: the empty path, *not*
    // `DefaultMountPath(drive_id)`. A record that predates mount paths
    // describes a drive the guest never mounted, so inventing one here would
    // mount a drive the capture does not expect.
    core::Expected<ExtraDrive, std::string> back = ExtraDrive::FromJson(
        J("{\"Overlaybd\":{\"drive_id\":\"data\",\"image_config_path\":\"/img/a.json\","
          "\"read_only\":false}}"));
    MT_EXPECT_TRUE(back.ok());
    MT_EXPECT_EQ(back.value().mount_path, std::string(""));
    MT_EXPECT_TRUE(!back.value().volume);
    MT_EXPECT_TRUE(!back.value().virtual_size.has_value());

    // Required fields and the variant tag stay enforced.
    MT_EXPECT_TRUE(!ExtraDrive::FromJson(J("{\"Overlaybd\":{\"drive_id\":\"d\"}}")).ok());
    MT_EXPECT_TRUE(!ExtraDrive::FromJson(J("{\"Cow\":{}}")).ok());
    MT_EXPECT_TRUE(!ExtraDrive::FromJson(J("[]")).ok());
}

MT_TEST(disk_rate_limit_requires_every_field_when_present) {
    cfg::DiskRateLimitConfig limit;
    limit.enabled = true;
    limit.bandwidth_bytes_per_sec = 1000;
    limit.bandwidth_burst_bytes = 2000;
    limit.iops = 300;
    limit.iops_burst = 400;
    core::Expected<cfg::DiskRateLimitConfig, std::string> back =
        cfg::DiskRateLimitConfig::FromJson(limit.ToJson());
    MT_EXPECT_TRUE(back.ok());
    MT_EXPECT_TRUE(back.value() == limit);

    // No per-field serde default on this struct.
    MT_EXPECT_TRUE(!cfg::DiskRateLimitConfig::FromJson(J("{\"enabled\":true}")).ok());
}

// ============================================================
// RuntimePolicy — serde's Duration shape
// ============================================================

MT_TEST(runtime_policy_uses_serde_duration_secs_nanos) {
    RuntimePolicy policy;
    policy.socket_timeout_ms = 3000;
    policy.socket_poll_interval_ms = 1;
    policy.envd_timeout_ms = 60000;
    policy.envd_poll_interval_ms = 3;

    const core::Json json = policy.ToJson();
    const core::Json& socket = Field(json, "socket_timeout");
    MT_EXPECT_EQ(Field(socket, "secs").as_int(), static_cast<int64_t>(3));
    MT_EXPECT_EQ(Field(socket, "nanos").as_int(), static_cast<int64_t>(0));
    // A sub-second value lands entirely in `nanos`.
    const core::Json& poll = Field(json, "socket_poll_interval");
    MT_EXPECT_EQ(Field(poll, "secs").as_int(), static_cast<int64_t>(0));
    MT_EXPECT_EQ(Field(poll, "nanos").as_int(), static_cast<int64_t>(1000000));

    core::Expected<RuntimePolicy, std::string> back = RuntimePolicy::FromJson(json);
    MT_EXPECT_TRUE(back.ok());
    MT_EXPECT_TRUE(back.value() == policy);
}

MT_TEST(runtime_policy_rejects_malformed_durations) {
    // `nanos` is below one second in a well-formed Duration; a larger value is
    // a malformed record, not a longer timeout.
    MT_EXPECT_TRUE(!RuntimePolicy::FromJson(
                        J("{\"socket_timeout\":{\"secs\":1,\"nanos\":2000000000},"
                          "\"socket_poll_interval\":{\"secs\":0,\"nanos\":0},"
                          "\"envd_timeout\":{\"secs\":0,\"nanos\":0},"
                          "\"envd_poll_interval\":{\"secs\":0,\"nanos\":0}}"))
                        .ok());
    // Every one of the four is required.
    MT_EXPECT_TRUE(!RuntimePolicy::FromJson(J("{\"socket_timeout\":{\"secs\":1,\"nanos\":0}}")).ok());
    // A plain number is not a Duration.
    MT_EXPECT_TRUE(!RuntimePolicy::FromJson(J("{\"socket_timeout\":3000}")).ok());
}

// ============================================================
// CommonConfig / SnapshotConfig
// ============================================================

MT_TEST(common_config_round_trips_every_persisted_field) {
    TempDir t;
    SnapshotConfig source = ValidSnapshotConfig(t);
    CommonConfig& c = source.common;
    c.firecracker_work_base_dir = std::string("/var/lib/aenv/work");
    c.serial_output_base_dir = std::string("/var/log/aenv");
    c.stdout_path = std::string("/var/log/aenv/out");
    c.stderr_path = std::string("/var/log/aenv/err");
    c.firecracker_log_level = std::string("info");
    c.track_dirty_pages = true;
    c.control_plane_port = 49983;
    std::map<std::string, std::string> env;
    env["PATH"] = "/usr/bin";
    c.env_vars = env;
    c.default_workdir = std::string("/work");
    c.default_user = std::string("agent");
    c.mmds_metadata = MmdsMetadata(core::SandboxId::Fresh(), "snap-1");
    c.rootfs_allow_shrink = true;
    c.extra_drives.push_back(Drive("data"));
    c.physical_extra_drive_count = 1;
    c.volume_drive_slots = 2;
    c.cpu_config_json = std::string("{\"cpuid\":[]}");
    c.network_policy = network::SandboxNetworkPolicy();
    c.custom_extension_params_json = std::string("{\"from\":\"launch\"}");
    c.disk_rate_limit.enabled = true;
    c.disk_rate_limit.iops = 500;

    core::Expected<CommonConfig, std::string> back = CommonConfig::FromJson(c.ToJson());
    MT_EXPECT_TRUE(back.ok());
    if (!back.ok()) return;
    // Compare through the wire form: this is the property that matters.
    MT_EXPECT_EQ(back.value().ToJson().ToString(), c.ToJson().ToString());
    MT_EXPECT_EQ(back.value().extra_drives.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(back.value().control_plane_port, static_cast<uint16_t>(49983));
    MT_EXPECT_EQ((*back.value().env_vars)["PATH"], std::string("/usr/bin"));
    MT_EXPECT_EQ(back.value().volume_drive_slots, static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(back.value().disk_rate_limit == c.disk_rate_limit);
}

MT_TEST(common_config_never_persists_the_envd_token) {
    // Rust `#[serde(skip)]`. A capture that carried a live credential would
    // hand one out to anyone who can read the record or a leaked snapshot.
    CommonConfig c = CommonConfig::New("/usr/bin/firecracker", "1.2.3", RuntimePolicy());
    c.envd_access_token = EnvdAccessToken("super-secret-token");
    const std::string json = c.ToJson().ToString();
    MT_EXPECT_TRUE(json.find("super-secret-token") == std::string::npos);
    MT_EXPECT_TRUE(json.find("envd_access_token") == std::string::npos);

    core::Expected<CommonConfig, std::string> back = CommonConfig::FromJson(c.ToJson());
    MT_EXPECT_TRUE(back.ok());
    MT_EXPECT_TRUE(!back.value().envd_access_token.has_value());
}

MT_TEST(common_config_custom_params_are_embedded_json_not_a_string) {
    // The value is a `serde_json::Map` upstream. Writing it as a quoted string
    // would not read back as params on either implementation.
    CommonConfig c = CommonConfig::New("/usr/bin/firecracker", "1.2.3", RuntimePolicy());
    MT_EXPECT_TRUE(!HasField(c.ToJson(), "custom_extension_params"));

    c.custom_extension_params_json = std::string("{\"a\":1}");
    const core::Json json = c.ToJson();
    MT_EXPECT_TRUE(Field(json, "custom_extension_params").kind() == core::Json::Kind::Object);

    core::Expected<CommonConfig, std::string> back = CommonConfig::FromJson(json);
    MT_EXPECT_TRUE(back.ok());
    MT_EXPECT_TRUE(back.value().custom_extension_params_json.has_value());
    MT_EXPECT_EQ(J(*back.value().custom_extension_params_json).ToString(), J("{\"a\":1}").ToString());
}

MT_TEST(common_config_serde_defaults_differ_from_constructor_defaults) {
    // A record written by the Rust server before these fields existed.
    // `track_dirty_pages` must come back **false** (bool's default), even
    // though `CommonConfig::New` sets it true: that record described a VM
    // without dirty-page tracking, and resuming it as tracked would capture a
    // memory snapshot the guest never backed.
    const std::string legacy =
        "{\"firecracker_binary\":\"/usr/bin/firecracker\","
        "\"runtime_policy\":{\"socket_timeout\":{\"secs\":3,\"nanos\":0},"
        "\"socket_poll_interval\":{\"secs\":0,\"nanos\":1000000},"
        "\"envd_timeout\":{\"secs\":60,\"nanos\":0},"
        "\"envd_poll_interval\":{\"secs\":0,\"nanos\":3000000}},"
        "\"envd_version\":\"0.1.0\",\"control_plane_port\":49983,"
        "\"extra_drives\":[]}";
    core::Expected<CommonConfig, std::string> back = CommonConfig::FromJson(J(legacy));
    MT_EXPECT_TRUE(back.ok());
    if (!back.ok()) return;
    MT_EXPECT_TRUE(!back.value().track_dirty_pages);
    MT_EXPECT_TRUE(CommonConfig::New("/b", "1", RuntimePolicy()).track_dirty_pages);
    MT_EXPECT_EQ(back.value().tools_drive_version, std::string(""));
    MT_EXPECT_TRUE(!back.value().rootfs_allow_shrink);
    MT_EXPECT_EQ(back.value().physical_extra_drive_count, static_cast<std::size_t>(0));
    MT_EXPECT_EQ(back.value().volume_drive_slots, static_cast<std::size_t>(0));
    MT_EXPECT_TRUE(back.value().disk_rate_limit == cfg::DiskRateLimitConfig());
    MT_EXPECT_TRUE(!back.value().env_vars.has_value());
    MT_EXPECT_TRUE(!back.value().mmds_metadata.has_value());
}

MT_TEST(common_config_required_fields_are_required) {
    const char* const kRequired[4] = {"firecracker_binary", "runtime_policy", "envd_version",
                                      "extra_drives"};
    for (int i = 0; i < 4; ++i) {
        core::JsonObject fields =
            CommonConfig::New("/usr/bin/firecracker", "1.2.3", RuntimePolicy()).ToJson().as_object();
        fields.erase(kRequired[i]);
        core::Expected<CommonConfig, std::string> back =
            CommonConfig::FromJson(core::Json(fields));
        MT_EXPECT_TRUE(!back.ok());
    }
    // A port past u16 is refused rather than truncated.
    core::JsonObject fields =
        CommonConfig::New("/usr/bin/firecracker", "1.2.3", RuntimePolicy()).ToJson().as_object();
    fields["control_plane_port"] = core::Json(static_cast<int64_t>(70000));
    MT_EXPECT_TRUE(!CommonConfig::FromJson(core::Json(fields)).ok());
}

MT_TEST(snapshot_config_skips_runtime_only_fields) {
    TempDir t;
    SnapshotConfig config = ValidSnapshotConfig(t);
    config.pack_recording = true;  // Rust `#[serde(skip)]`

    const core::Json json = config.ToJson();
    MT_EXPECT_TRUE(!HasField(json, "pack_recording"));
    MT_EXPECT_TRUE(!HasField(json, "memory_startup_pack"));
    MT_EXPECT_TRUE(!HasField(json, "managed_snapshot_root"));

    core::Expected<SnapshotConfig, std::string> back = SnapshotConfig::FromJson(json);
    MT_EXPECT_TRUE(back.ok());
    if (!back.ok()) return;
    // A throwaway recording VM must not come back as one after a restart.
    MT_EXPECT_TRUE(!back.value().pack_recording);
    MT_EXPECT_EQ(back.value().vm_state_path, config.vm_state_path);
    MT_EXPECT_EQ(back.value().mem_virtual_size, config.mem_virtual_size);
    MT_EXPECT_TRUE(back.value().mem_overlaybd_config == config.mem_overlaybd_config);
}

// ============================================================
// FirecrackerPausedState
// ============================================================

MT_TEST(paused_state_round_trips_through_encode_and_decode) {
    TempDir t;
    const SnapshotConfig config = ValidSnapshotConfig(t);
    const FirecrackerPausedState state(config);

    core::Expected<FirecrackerPausedState, std::string> back =
        FirecrackerPausedState::Decode("/unused/artifact/root", J(state.Encode()));
    MT_EXPECT_TRUE(back.ok());
    if (!back.ok()) return;
    MT_EXPECT_EQ(back.value().Encode(), state.Encode());
}

MT_TEST(paused_state_decode_validates_persisted_artifacts) {
    TempDir t;
    SnapshotConfig config = ValidSnapshotConfig(t);
    const std::string encoded = FirecrackerPausedState(config).Encode();

    // Removing the captured VM state makes the record unresumable. Rust wraps
    // the failure with this context, and the orchestrator's load path drops
    // such a record together with its artifacts rather than offering a resume.
    MT_EXPECT_TRUE(core::fs::RemoveFile(config.vm_state_path).ok());
    core::Expected<FirecrackerPausedState, std::string> back =
        FirecrackerPausedState::Decode("", J(encoded));
    MT_EXPECT_TRUE(!back.ok());
    MT_EXPECT_TRUE(back.error().find("validate Firecracker paused state artifacts") !=
                   std::string::npos);
}

MT_TEST(paused_state_decode_reports_deserialize_failures_separately) {
    core::Expected<FirecrackerPausedState, std::string> back =
        FirecrackerPausedState::Decode("", J("{}"));
    MT_EXPECT_TRUE(!back.ok());
    MT_EXPECT_TRUE(back.error().find("deserialize Firecracker paused state") !=
                   std::string::npos);
}

MT_TEST(paused_state_runtime_artifacts_cover_rootfs_and_extra_drives) {
    TempDir t;
    SnapshotConfig config = ValidSnapshotConfig(t);
    config.common.extra_drives.push_back(Drive("data"));
    config.common.extra_drives.push_back(Drive("logs"));

    const RuntimeArtifactSet artifacts = FirecrackerPausedState(config).RuntimeArtifacts();
    MT_EXPECT_EQ(artifacts.overlaybd_image_config_paths.size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(artifacts.overlaybd_image_config_paths[0],
                 config.common.rootfs_image_config->image_config_path);
    MT_EXPECT_EQ(artifacts.overlaybd_image_config_paths[1], std::string("/img/data.json"));
    MT_EXPECT_EQ(artifacts.overlaybd_image_config_paths[2], std::string("/img/logs.json"));

    // No rootfs recorded: only the extra drives are protected.
    config.common.rootfs_image_config = core::nullopt;
    MT_EXPECT_EQ(FirecrackerPausedState(config).RuntimeArtifacts()
                     .overlaybd_image_config_paths.size(),
                 static_cast<std::size_t>(2));
}

MT_TEST(paused_state_control_plane_port_treats_zero_as_absent) {
    TempDir t;
    SnapshotConfig config = ValidSnapshotConfig(t);
    config.common.control_plane_port = 49983;
    MT_EXPECT_TRUE(FirecrackerPausedState(config).ControlPlanePort().has_value());
    MT_EXPECT_EQ(*FirecrackerPausedState(config).ControlPlanePort(),
                 static_cast<uint16_t>(49983));

    // Rust `(port != 0).then_some(port)` — a capture that never recorded one.
    config.common.control_plane_port = 0;
    MT_EXPECT_TRUE(!FirecrackerPausedState(config).ControlPlanePort().has_value());
}

// ============================================================
// the factory entry point the restart path calls
// ============================================================

MT_TEST(decode_firecracker_paused_state_accepts_encoded_text) {
    TempDir t;
    const SnapshotConfig config = ValidSnapshotConfig(t);
    const std::string encoded = FirecrackerPausedState(config).Encode();

    core::Expected<std::shared_ptr<PausedSandboxState>, core::AnyError> decoded =
        DecodeFirecrackerPausedState("/unused", encoded);
    MT_EXPECT_TRUE(decoded.ok());
    if (!decoded.ok()) return;
    MT_EXPECT_TRUE(decoded.value() != NULL);
    MT_EXPECT_EQ(decoded.value()->Encode(), encoded);
    MT_EXPECT_TRUE(decoded.value()->ControlPlanePort().has_value());
}

MT_TEST(decode_firecracker_paused_state_rejects_non_json_payload) {
    core::Expected<std::shared_ptr<PausedSandboxState>, core::AnyError> decoded =
        DecodeFirecrackerPausedState("/unused", "not json {");
    MT_EXPECT_TRUE(!decoded.ok());
    MT_EXPECT_TRUE(decoded.error().chain().find("deserialize Firecracker paused state") !=
                   std::string::npos);
}

MT_TEST(decode_firecracker_paused_state_refuses_legacy_cow_record) {
    // End to end: a sandbox captured on the removed backend must surface the
    // rebuild instruction, not a generic parse error.
    TempDir t;
    SnapshotConfig config = ValidSnapshotConfig(t);
    core::JsonObject common = config.common.ToJson().as_object();
    common["ublk_config"] = J("{\"backend\":\"Cow\"}");
    core::JsonObject root = config.ToJson().as_object();
    root["common"] = core::Json(common);

    core::Expected<std::shared_ptr<PausedSandboxState>, core::AnyError> decoded =
        DecodeFirecrackerPausedState("/unused", core::Json(root).ToString());
    MT_EXPECT_TRUE(!decoded.ok());
    MT_EXPECT_TRUE(decoded.error().chain().find("legacy CoW ublk backend") != std::string::npos);
}

MT_MAIN
