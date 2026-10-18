// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/factory.rs :: build (config half),
//       src/sandbox/firecracker/config.rs :: from_app_config_with_user_image,
//       apply_launch_config, and the orchestrator projection into the backend
//       contracts (orchestrator/service.rs :: create_sandbox_inner, fork).
#include "microtest.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "agentenv/core/json.h"
#include "agentenv/orchestrator/service.h"
#include "agentenv/sandbox/firecracker/factory.h"
#include "orchestrator_fakes.h"

using namespace agentenv;
using namespace agentenv::sandbox;
using namespace agentenv::sandbox::firecracker;
using agentenv::storage::overlaybd::UpperMode;
using agentenv::testing::BackendScript;
using agentenv::testing::FakeFactory;
using agentenv::testing::FakeSandbox;

namespace {

/// Rust `base_app_config`.
cfg::AppConfig BaseAppConfig() {
    cfg::AppConfig config;
    config.firecracker.binary_path = std::string("firecracker");
    config.kernel.image_path = std::string("vmlinux.bin");
    config.tools.drive_path = std::string("tools.ext4");
    config.tools.version = std::string("1.2.3");
    return config;
}

ExtraDrive Drive(const std::string& id, const std::string& mount) {
    ExtraDrive d;
    d.drive_id = id;
    d.image_config_path = "/img/" + id + ".json";
    d.mount_path = mount;
    return d;
}

FreshSandboxBuildSpec Spec() {
    FreshSandboxBuildSpec spec;
    spec.image_config_path = "/img/rootfs.json";
    spec.resources.cpu_count = 4;
    spec.resources.memory_mib = 2048;
    spec.resources.disk_size_mib = 0;
    return spec;
}

SandboxLaunchConfig Launch() {
    SandboxLaunchConfig launch;
    launch.sandbox_id = core::SandboxId::Fresh();
    launch.snapshot_id = "docker.io/library/alpine:3";
    return launch;
}

SandboxConfig MustBuild(const cfg::AppConfig& app, const FreshSandboxBuildSpec& spec,
                        const SandboxLaunchConfig& launch,
                        const core::Optional<std::string>& cpu = core::nullopt) {
    core::Expected<SandboxConfig, std::string> built =
        FreshSandboxConfigForBuild(app, cpu, spec, launch);
    MT_EXPECT_TRUE(built.ok());
    return built.ok() ? built.value() : SandboxConfig();
}

}  // namespace

// ============================================================
// config.rs
// ============================================================

// config.rs :: from_app_config_does_not_bind_ublk_without_user_image
MT_TEST(from_app_config_does_not_bind_ublk_without_user_image) {
    cfg::AppConfig config = BaseAppConfig();
    config.ublk.overlaybd.read_only = true;
    core::Expected<CommonConfig, std::string> common = CommonConfig::FromAppConfig(config);
    MT_EXPECT_TRUE(common.ok());
    MT_EXPECT_TRUE(!common.value().ublk_config.has_value());
}

// config.rs :: sandbox_config_binds_overlaybd_to_user_image — the node's
// runtime upper mode wins over what the caller passed.
MT_TEST(sandbox_config_binds_overlaybd_to_user_image) {
    cfg::AppConfig config = BaseAppConfig();
    config.ublk.overlaybd.read_only = false;
    config.ublk.overlaybd.runtime_upper_mode = UpperMode::Sparse;
    ublk::OverlaybdDeviceConfig user;
    user.image_config_path = "custom-image.json";
    user.read_only = false;
    user.runtime_upper_mode = UpperMode::LogStructured;

    core::Expected<SandboxConfig, std::string> built =
        SandboxConfig::FromAppConfigWithUserImage(config, user);
    MT_EXPECT_TRUE(built.ok());
    const SandboxConfig& c = built.value();
    MT_EXPECT_TRUE(c.common.ublk_config.has_value());
    MT_EXPECT_EQ(c.common.ublk_config->overlaybd.image_config_path,
                 std::string("custom-image.json"));
    MT_EXPECT_TRUE(!c.common.ublk_config->overlaybd.read_only);
    MT_EXPECT_TRUE(c.common.ublk_config->overlaybd.runtime_upper_mode == UpperMode::Sparse);
    MT_EXPECT_TRUE(c.common.rootfs_image_config->runtime_upper_mode == UpperMode::Sparse);
    MT_EXPECT_EQ(c.kernel_image, config.ResolvedKernelImagePath());
    MT_EXPECT_EQ(c.vcpu_count, config.machine.vcpu_count);
    MT_EXPECT_EQ(c.mem_size_mib, config.machine.mem_size_mib);
}

MT_TEST(from_app_config_with_user_image_boot_args_default_and_override) {
    cfg::AppConfig config = BaseAppConfig();
    ublk::OverlaybdDeviceConfig user;
    user.image_config_path = "x.json";

    SandboxConfig defaulted = SandboxConfig::FromAppConfigWithUserImage(config, user).value();
    MT_EXPECT_EQ(*defaulted.boot_args, std::string(kDefaultBootArgs));

    config.firecracker.boot_args = std::string("console=ttyS0");
    SandboxConfig configured = SandboxConfig::FromAppConfigWithUserImage(config, user).value();
    MT_EXPECT_EQ(*configured.boot_args, std::string("console=ttyS0"));

    // `Option::or_else` keeps a configured value even when it is empty.
    config.firecracker.boot_args = std::string("");
    SandboxConfig empty = SandboxConfig::FromAppConfigWithUserImage(config, user).value();
    MT_EXPECT_EQ(*empty.boot_args, std::string(""));
}

MT_TEST(apply_launch_config_merges_env_launch_wins) {
    SandboxConfig c;
    std::map<std::string, std::string> image_env;
    image_env["FROM_IMAGE"] = "true";
    image_env["SHARED_KEY"] = "image";
    c.common.env_vars = image_env;

    SandboxLaunchConfig launch = Launch();
    std::map<std::string, std::string> overrides;
    overrides["FROM_LAUNCH"] = "true";
    overrides["SHARED_KEY"] = "launch";
    launch.env_vars = overrides;
    c.ApplyLaunchConfig(launch);

    MT_EXPECT_EQ(c.common.env_vars->size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ((*c.common.env_vars)["FROM_IMAGE"], std::string("true"));
    MT_EXPECT_EQ((*c.common.env_vars)["FROM_LAUNCH"], std::string("true"));
    MT_EXPECT_EQ((*c.common.env_vars)["SHARED_KEY"], std::string("launch"));
}

MT_TEST(apply_launch_config_empty_env_does_not_create_map) {
    SandboxConfig c;
    SandboxLaunchConfig launch = Launch();
    launch.env_vars = std::map<std::string, std::string>();
    c.ApplyLaunchConfig(launch);
    MT_EXPECT_TRUE(!c.common.env_vars.has_value());

    std::map<std::string, std::string> one;
    one["A"] = "1";
    launch.env_vars = one;
    c.ApplyLaunchConfig(launch);
    MT_EXPECT_TRUE(c.common.env_vars.has_value());
}

MT_TEST(apply_launch_config_builds_mmds_for_this_sandbox) {
    SandboxConfig c;
    SandboxLaunchConfig launch = Launch();
    launch.envd_access_token = EnvdAccessToken("tok");
    launch.extra_mmds["imageConfigs"] = core::Json(core::JsonArray());
    launch.extra_mmds["accessTokenHash"] = core::Json("forged");
    c.ApplyLaunchConfig(launch);

    MT_EXPECT_TRUE(c.common.mmds_metadata.has_value());
    const MmdsMetadata& m = *c.common.mmds_metadata;
    MT_EXPECT_EQ(m.SandboxId(), launch.sandbox_id.ToString());
    MT_EXPECT_EQ(m.SnapshotId(), launch.snapshot_id);
    MT_EXPECT_EQ(m.AccessTokenHash(), MmdsMetadata::HashAccessToken("tok"));
    MT_EXPECT_EQ(m.ExtraCount(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(m.Extra().find("imageConfigs") != m.Extra().end());
    MT_EXPECT_TRUE(c.common.envd_access_token.has_value());
    MT_EXPECT_EQ(c.common.envd_access_token->Expose(), std::string("tok"));
}

MT_TEST(apply_launch_config_clears_absent_launch_fields) {
    // Plain assignments upstream: a launch without a policy / params / token
    // must not inherit whatever the config held before.
    SandboxConfig c;
    c.common.network_policy = network::SandboxNetworkPolicy();
    c.common.custom_extension_params_json = std::string("{\"stale\":true}");
    c.common.envd_access_token = EnvdAccessToken("stale");
    c.ApplyLaunchConfig(Launch());
    MT_EXPECT_TRUE(!c.common.network_policy.has_value());
    MT_EXPECT_TRUE(!c.common.custom_extension_params_json.has_value());
    MT_EXPECT_TRUE(!c.common.envd_access_token.has_value());

    SandboxLaunchConfig launch = Launch();
    custom_extension::Params params;
    params.json_bytes = "{\"from\":\"launch\"}";
    launch.custom_extension_params = params;
    c.ApplyLaunchConfig(launch);
    MT_EXPECT_EQ(*c.common.custom_extension_params_json, std::string("{\"from\":\"launch\"}"));
}

// ============================================================
// factory.rs :: build (config half)
// ============================================================

MT_TEST(build_takes_machine_shape_from_spec) {
    cfg::AppConfig app = BaseAppConfig();
    app.machine.vcpu_count = 2;
    app.machine.mem_size_mib = 1024;
    SandboxConfig c = MustBuild(app, Spec(), Launch());
    MT_EXPECT_EQ(c.vcpu_count, 4u);
    MT_EXPECT_EQ(c.mem_size_mib, 2048u);
}

MT_TEST(build_rootfs_size_zero_is_unset_otherwise_bytes) {
    cfg::AppConfig app = BaseAppConfig();
    SandboxConfig unset = MustBuild(app, Spec(), Launch());
    MT_EXPECT_TRUE(!unset.common.rootfs_virtual_size.has_value());

    FreshSandboxBuildSpec spec = Spec();
    spec.resources.disk_size_mib = 2048;
    SandboxConfig sized = MustBuild(app, spec, Launch());
    MT_EXPECT_EQ(*sized.common.rootfs_virtual_size, 2048ull * 1024ull * 1024ull);

    // The largest u32 MiB still fits in bytes.
    spec.resources.disk_size_mib = 0xffffffffu;
    SandboxConfig max = MustBuild(app, spec, Launch());
    MT_EXPECT_EQ(*max.common.rootfs_virtual_size, 0xffffffffull * 1024ull * 1024ull);
}

MT_TEST(build_rootfs_is_writable_and_ublk_matches_it) {
    cfg::AppConfig app = BaseAppConfig();
    app.ublk.overlaybd.runtime_upper_mode = UpperMode::HybridLogStructured;
    // A node-level read_only applies to resumes of shared images, not to a
    // fresh sandbox's own disk.
    app.ublk.overlaybd.read_only = true;
    SandboxConfig c = MustBuild(app, Spec(), Launch());
    MT_EXPECT_EQ(c.common.rootfs_image_config->image_config_path, std::string("/img/rootfs.json"));
    MT_EXPECT_TRUE(!c.common.rootfs_image_config->read_only);
    MT_EXPECT_TRUE(c.common.rootfs_image_config->runtime_upper_mode ==
                   UpperMode::HybridLogStructured);
    MT_EXPECT_TRUE(c.common.ublk_config.has_value());
    MT_EXPECT_TRUE(c.common.ublk_config->overlaybd == *c.common.rootfs_image_config);
}

MT_TEST(build_appends_only_allowed_extra_boot_args) {
    cfg::AppConfig app = BaseAppConfig();
    app.firecracker.boot_args = std::string("console=ttyS0");
    std::vector<std::string> prefixes;
    prefixes.push_back("agentenv-custom.");
    app.firecracker.allowed_extra_boot_args_prefixes = prefixes;
    FreshSandboxBuildSpec spec = Spec();
    spec.extra_boot_args = std::string("agentenv-custom.foo=bar init=/bin/sh agentenv-custom.x=a:b");

    SandboxConfig c = MustBuild(app, spec, Launch());
    MT_EXPECT_EQ(*c.boot_args, std::string("console=ttyS0 agentenv-custom.foo=bar"));

    // No configured prefixes means nothing is allowed.
    app.firecracker.allowed_extra_boot_args_prefixes = core::nullopt;
    SandboxConfig none = MustBuild(app, spec, Launch());
    MT_EXPECT_EQ(*none.boot_args, std::string("console=ttyS0"));
}

MT_TEST(build_carries_drives_cpu_template_and_context) {
    cfg::AppConfig app = BaseAppConfig();
    FreshSandboxBuildSpec spec = Spec();
    spec.extra_drives.push_back(Drive("data", "/mnt/data"));
    spec.context.env_vars["PATH"] = "/usr/bin";
    spec.context.workdir = "/work";
    spec.context.user = std::string("agent");

    SandboxConfig c = MustBuild(app, spec, Launch(), std::string("{\"cpuid\":[]}"));
    MT_EXPECT_EQ(c.common.extra_drives.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(c.common.extra_drives[0] == spec.extra_drives[0]);
    MT_EXPECT_EQ(*c.common.cpu_config_json, std::string("{\"cpuid\":[]}"));
    MT_EXPECT_EQ((*c.common.env_vars)["PATH"], std::string("/usr/bin"));
    MT_EXPECT_EQ(*c.common.default_workdir, std::string("/work"));
    MT_EXPECT_EQ(*c.common.default_user, std::string("agent"));

    // An image with no env leaves it absent, not empty.
    SandboxConfig bare = MustBuild(app, Spec(), Launch());
    MT_EXPECT_TRUE(!bare.common.env_vars.has_value());
    MT_EXPECT_TRUE(!bare.common.cpu_config_json.has_value());
    MT_EXPECT_EQ(*bare.common.default_workdir, std::string("/"));
}

MT_TEST(build_applies_launch_config_last) {
    cfg::AppConfig app = BaseAppConfig();
    FreshSandboxBuildSpec spec = Spec();
    spec.context.env_vars["SHARED"] = "image";
    SandboxLaunchConfig launch = Launch();
    std::map<std::string, std::string> overrides;
    overrides["SHARED"] = "launch";
    launch.env_vars = overrides;

    SandboxConfig c = MustBuild(app, spec, launch);
    MT_EXPECT_EQ((*c.common.env_vars)["SHARED"], std::string("launch"));
    MT_EXPECT_EQ(c.common.mmds_metadata->SandboxId(), launch.sandbox_id.ToString());
}

// ============================================================
// orchestrator projection into the backend contracts
// ============================================================

namespace {

struct OrchFixture {
    BackendScript script;
    std::shared_ptr<FakeFactory> factory;
    std::shared_ptr<orchestrator::Orchestrator> orch;
    cfg::AppConfig config;

    OrchFixture() : factory(new FakeFactory(&script)) {
        config = BaseAppConfig();
        config.sandbox.access_token_hash_seed = std::string("seed");
        config.kernel.version = std::string("6.1.0");
        orch = orchestrator::Orchestrator::New(
                   std::shared_ptr<orchestrator::MetadataStore>(
                       new orchestrator::InMemoryMetadataStore()),
                   factory,
                   std::shared_ptr<orchestrator::SandboxPersister>(
                       new orchestrator::DisabledSandboxPersister()),
                   config)
                   .value();
    }
};

orchestrator::CreateSandboxRequest ImageRequest() {
    orchestrator::CreateSandboxRequest r;
    r.source.kind = orchestrator::SandboxLaunchSource::Kind::Image;
    r.source.image_ref = "alpine:3";
    r.source.overlaybd_config_path = "/img/rootfs.json";
    r.source.context.workdir = "/srv";
    r.source.context.env_vars["IMAGE"] = "1";
    r.source.extra_drives.push_back(Drive("image-drive", "/mnt/image"));
    r.extra_drives.push_back(Drive("volume", "/mnt/vol"));
    return r;
}

}  // namespace

MT_TEST(create_fresh_routes_every_drive_through_build_spec) {
    OrchFixture f;
    orchestrator::CreateSandboxRequest r = ImageRequest();
    r.env_vars["LAUNCH"] = "1";
    MT_EXPECT_TRUE(f.orch->CreateSandbox(r).ok());

    const FreshSandboxBuildSpec& spec = f.factory->last_build_spec;
    const SandboxLaunchConfig& launch = f.factory->last_launch_config;
    // Image drives first, then the launch ones — and all of them in full.
    MT_EXPECT_EQ(spec.extra_drives.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(spec.extra_drives[0].drive_id, std::string("image-drive"));
    MT_EXPECT_EQ(spec.extra_drives[1].drive_id, std::string("volume"));
    MT_EXPECT_EQ(spec.extra_drives[1].image_config_path, std::string("/img/volume.json"));
    MT_EXPECT_EQ(spec.extra_drives[1].mount_path, std::string("/mnt/vol"));
    // Rust `extra_drives: Vec::new()` on the fresh launch config.
    MT_EXPECT_TRUE(launch.extra_drives.empty());
    MT_EXPECT_TRUE(!launch.extra_drives_in_snapshot);
    // The image's command context reaches the backend.
    MT_EXPECT_EQ(spec.context.workdir, std::string("/srv"));
    MT_EXPECT_EQ(spec.context.env_vars.at("IMAGE"), std::string("1"));
    MT_EXPECT_EQ(launch.env_vars->at("LAUNCH"), std::string("1"));
}

MT_TEST(create_passes_network_policy_itself_only_when_rules_are_needed) {
    OrchFixture f;
    orchestrator::CreateSandboxRequest open = ImageRequest();
    MT_EXPECT_TRUE(f.orch->CreateSandbox(open).ok());
    MT_EXPECT_TRUE(!f.factory->last_launch_config.network.has_value());

    orchestrator::CreateSandboxRequest denied = ImageRequest();
    denied.network_policy.base_policy = network::BaseSandboxNetworkPolicy::Deny;
    denied.network_policy.allow_public_traffic = false;
    MT_EXPECT_TRUE(f.orch->CreateSandbox(denied).ok());
    MT_EXPECT_TRUE(f.factory->last_launch_config.network.has_value());
    MT_EXPECT_TRUE(f.factory->last_launch_config.network->base_policy ==
                   network::BaseSandboxNetworkPolicy::Deny);
    MT_EXPECT_TRUE(!f.factory->last_launch_config.network->allow_public_traffic);
}

MT_TEST(create_puts_image_configs_into_mmds_extras) {
    OrchFixture f;
    orchestrator::CreateSandboxRequest bare = ImageRequest();
    MT_EXPECT_TRUE(f.orch->CreateSandbox(bare).ok());
    MT_EXPECT_TRUE(f.factory->last_launch_config.extra_mmds.empty());

    orchestrator::CreateSandboxRequest with = ImageRequest();
    core::JsonObject cfg_obj;
    cfg_obj["Cmd"] = core::Json(core::JsonArray(1, core::Json("/bin/sh")));
    with.source.image_configs.Add(core::nullopt, "/", core::Json(cfg_obj));
    MT_EXPECT_TRUE(f.orch->CreateSandbox(with).ok());
    const core::JsonObject& extras = f.factory->last_launch_config.extra_mmds;
    MT_EXPECT_EQ(extras.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(extras.at("imageConfigs").ToString(), with.source.image_configs.ToJson().ToString());
}

MT_TEST(create_fresh_records_configured_runtime_versions) {
    OrchFixture f;
    core::Expected<orchestrator::SandboxMetadata, orchestrator::OrchestratorError> m =
        f.orch->CreateSandbox(ImageRequest());
    MT_EXPECT_TRUE(m.ok());
    MT_EXPECT_EQ(m.value().runtime_versions.kernel_version, std::string("6.1.0"));
    // Unset firecracker version is recorded as "unknown", as upstream.
    MT_EXPECT_EQ(m.value().runtime_versions.firecracker_version, std::string("unknown"));
    MT_EXPECT_EQ(m.value().runtime_versions.envd_version, f.config.envd.version);
    MT_EXPECT_EQ(m.value().runtime_versions.tools_drive_version, std::string("1.2.3"));
}

MT_TEST(create_from_snapshot_binds_launch_drives_and_inherits_params) {
    OrchFixture f;
    orchestrator::CreateSandboxRequest r;
    r.source.kind = orchestrator::SandboxLaunchSource::Kind::Snapshot;
    r.source.snapshot_id = "snap-1";
    r.source.snapshot_virtualization_mode = f.config.virtualization_mode;
    custom_extension::Params inherited;
    inherited.json_bytes = "{\"from\":\"snapshot\"}";
    r.source.snapshot_custom_extension_params = inherited;
    r.extra_drives.push_back(Drive("volume", "/mnt/vol"));
    r.extra_drives_in_snapshot = true;
    MT_EXPECT_TRUE(f.orch->CreateSandbox(r).ok());

    const SandboxLaunchConfig& launch = f.factory->last_launch_config;
    MT_EXPECT_EQ(launch.snapshot_id, std::string("snap-1"));
    MT_EXPECT_EQ(launch.extra_drives.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(launch.extra_drives[0].image_config_path, std::string("/img/volume.json"));
    MT_EXPECT_TRUE(launch.extra_drives_in_snapshot);
    MT_EXPECT_EQ(launch.custom_extension_params->json_bytes,
                 std::string("{\"from\":\"snapshot\"}"));
}

MT_TEST(fork_hands_backend_full_extra_drives) {
    OrchFixture f;
    orchestrator::SandboxMetadata m;
    m.id = core::SandboxId::Fresh();
    m.state = orchestrator::SandboxState::Running;
    m.timeout_ms = core::Optional<int64_t>(60 * 1000);
    // Seed a running source the same way the fork tests do.
    std::shared_ptr<orchestrator::InMemoryMetadataStore> store(
        new orchestrator::InMemoryMetadataStore());
    std::shared_ptr<orchestrator::Orchestrator> orch(new orchestrator::Orchestrator(
        store, f.factory,
        std::shared_ptr<orchestrator::SandboxPersister>(
            new orchestrator::DisabledSandboxPersister())));
    MT_EXPECT_TRUE(store->Add(m).ok());
    std::unique_ptr<SandboxBackend> backend(new FakeSandbox(&f.script));
    orch->RegisterSandboxHandle(
        m.id, orchestrator::SandboxHandlePtr(new orchestrator::SandboxHandle(std::move(backend))));
    orch->UpsertProxyRoute(m.id, orchestrator::ProxyTarget("10.0.0.1"));

    std::vector<orchestrator::SandboxForkChildSpec> children(1);
    children[0].sandbox_id = core::SandboxId::Fresh();
    children[0].extra_drives.push_back(Drive("replacement", "/mnt/data"));
    children[0].replace_drive_ids.push_back(std::make_pair(std::string("data"),
                                                           std::string("replacement")));
    (void)orch->ForkSandboxWithSpecs(m.id, children, orchestrator::NewTimeout::Set(30 * 1000));

    MT_EXPECT_EQ(f.script.fork_calls, 1);
    MT_EXPECT_EQ(f.script.last_fork_specs.size(), static_cast<std::size_t>(1));
    const ExtraDrive& d = f.script.last_fork_specs[0].extra_drives.at(0);
    MT_EXPECT_EQ(d.drive_id, std::string("replacement"));
    MT_EXPECT_EQ(d.image_config_path, std::string("/img/replacement.json"));
    MT_EXPECT_EQ(d.mount_path, std::string("/mnt/data"));
}

// ============================================================
// Rust `FirecrackerSandbox::new_with_id` — drive partitioning
// ============================================================

namespace {

ExtraDrive VolumeDrive(const std::string& id, const std::string& mount) {
    ExtraDrive d = Drive(id, mount);
    d.volume = true;
    return d;
}

std::vector<std::string> DriveIds(const std::vector<ExtraDrive>& drives) {
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < drives.size(); ++i) ids.push_back(drives[i].drive_id);
    return ids;
}

}  // namespace

MT_TEST(physical_drives_are_ordered_before_volumes) {
    // The guest names drives positionally (vdc, vdd, ...), so the list order
    // *is* the slot assignment. A volume left ahead of a physical drive is
    // mounted as the wrong filesystem -- which the API accepts without
    // complaint, because every drive on its own is valid.
    SandboxConfig config;
    config.common.extra_drives.push_back(VolumeDrive("vol-a", "/mnt/a"));
    config.common.extra_drives.push_back(Drive("phys-a", "/mnt/p1"));
    config.common.extra_drives.push_back(VolumeDrive("vol-b", "/mnt/b"));
    config.common.extra_drives.push_back(Drive("phys-b", "/mnt/p2"));

    MT_EXPECT_TRUE(PartitionExtraDrives(&config, 4).ok());

    const std::vector<std::string> ids = DriveIds(config.common.extra_drives);
    MT_EXPECT_EQ(ids.size(), static_cast<std::size_t>(4));
    if (ids.size() != 4) return;
    MT_EXPECT_EQ(ids[0], std::string("phys-a"));
    MT_EXPECT_EQ(ids[1], std::string("phys-b"));
    MT_EXPECT_EQ(ids[2], std::string("vol-a"));
    MT_EXPECT_EQ(ids[3], std::string("vol-b"));
    // The split point `BuildConfigureSequence` uses.
    MT_EXPECT_EQ(config.common.physical_extra_drive_count, static_cast<std::size_t>(2));
}

MT_TEST(more_volumes_than_slots_is_refused_not_truncated) {
    // Firecracker cannot add a virtio-block device after a snapshot restore,
    // so every volume must occupy a slot reserved on the first boot. Dropping
    // the surplus would boot a sandbox whose caller asked for a volume and
    // finds the mount point empty.
    SandboxConfig config;
    config.common.extra_drives.push_back(VolumeDrive("v1", "/mnt/1"));
    config.common.extra_drives.push_back(VolumeDrive("v2", "/mnt/2"));
    config.common.extra_drives.push_back(VolumeDrive("v3", "/mnt/3"));

    const core::Expected<core::Unit, std::string> r = PartitionExtraDrives(&config, 2);
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().find("3 volumes") != std::string::npos);
    MT_EXPECT_TRUE(r.error().find("2 reserved volume slots") != std::string::npos);
}

MT_TEST(reserved_slots_never_exceed_the_guests_drive_letters) {
    // The guest only has vdc..vdz. Reserving more slots than letters would
    // name a drive the guest cannot address.
    SandboxConfig config;
    for (std::size_t i = 0; i < kMaxExtraDrives - 1; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "phys-%02zu", i);
        config.common.extra_drives.push_back(Drive(id, "/mnt/p"));
    }
    // Asks for far more volume slots than the one remaining letter.
    MT_EXPECT_TRUE(PartitionExtraDrives(&config, 100).ok());
    MT_EXPECT_EQ(config.common.volume_drive_slots, static_cast<std::size_t>(1));
}

MT_TEST(a_physical_drive_count_past_the_letters_yields_no_slots) {
    // Saturating, not wrapping: `remaining` is computed by subtraction, and
    // an unsigned wrap here would reserve billions of slots and then accept
    // any number of volumes.
    SandboxConfig config;
    for (std::size_t i = 0; i < kMaxExtraDrives + 3; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "phys-%02zu", i);
        config.common.extra_drives.push_back(Drive(id, "/mnt/p"));
    }
    MT_EXPECT_TRUE(PartitionExtraDrives(&config, 4).ok());
    MT_EXPECT_EQ(config.common.volume_drive_slots, static_cast<std::size_t>(0));

    // And with a volume present it must now fail rather than reserve a slot
    // that does not exist.
    config.common.extra_drives.push_back(VolumeDrive("vol", "/mnt/v"));
    MT_EXPECT_TRUE(!PartitionExtraDrives(&config, 4).ok());
}


// ============================================================
// snapshot_config_for_fork
// ============================================================
// Rust `snapshot_config_for_fork`. Every child of a fork resumes the *same*
// capture, so the only thing that may differ is which volume a drive slot
// points at -- and the substitutions below are the only way a child gets its
// own volumes at all.

namespace {

SnapshotConfig ForkSource() {
    SnapshotConfig config;
    config.common.extra_drives.push_back(Drive("phys-a", "/mnt/p1"));
    config.common.extra_drives.push_back(VolumeDrive("vol-a", "/mnt/a"));
    config.common.extra_drives.push_back(VolumeDrive("vol-b", "/mnt/b"));
    // The first drive is physical; the rest are volume slots.
    config.common.physical_extra_drive_count = 1;
    return config;
}

SandboxForkSpec ChildSpec() {
    SandboxForkSpec spec;
    spec.sandbox_id = core::SandboxId::Fresh();
    return spec;
}

}  // namespace

MT_TEST(fork_config_without_substitutions_is_the_source_unchanged) {
    const SnapshotConfig source = ForkSource();
    const core::Expected<SnapshotConfig, std::string> forked =
        SnapshotConfigForFork(source, ChildSpec());
    MT_EXPECT_TRUE(forked.ok());
    if (!forked.ok()) return;
    // The common case. Children that share the parent's volumes resume the
    // capture verbatim.
    MT_EXPECT_TRUE(DriveIds(forked.value().common.extra_drives) ==
                   DriveIds(source.common.extra_drives));
}

MT_TEST(fork_config_replaces_a_volume_slot_in_place) {
    const SnapshotConfig source = ForkSource();
    SandboxForkSpec child = ChildSpec();
    ExtraDrive replacement = VolumeDrive("vol-child", "/mnt/a");
    replacement.image_config_path = "/img/child-a.json";
    child.extra_drives.push_back(replacement);
    child.replace_drive_ids.push_back(std::make_pair("vol-a", "vol-child"));

    const core::Expected<SnapshotConfig, std::string> forked =
        SnapshotConfigForFork(source, child);
    MT_EXPECT_TRUE(forked.ok());
    if (!forked.ok()) return;

    // Substituted in place: the slot ordering the guest names devices by is
    // untouched, so `vol-child` is mounted where `vol-a` was.
    const std::vector<std::string> ids = DriveIds(forked.value().common.extra_drives);
    MT_EXPECT_EQ(static_cast<int>(ids.size()), 3);
    MT_EXPECT_EQ(ids[0], std::string("phys-a"));
    MT_EXPECT_EQ(ids[1], std::string("vol-child"));
    MT_EXPECT_EQ(ids[2], std::string("vol-b"));

    // Substituted by value, not by id. Firecracker reopens every serialized
    // slot path during the load, so the image config has to be in the config
    // before the load -- carrying only the id forward would resume the child
    // on the parent's image.
    MT_EXPECT_EQ(forked.value().common.extra_drives[1].image_config_path,
                 std::string("/img/child-a.json"));
}

MT_TEST(fork_config_refuses_a_duplicate_replacement) {
    const SnapshotConfig source = ForkSource();
    SandboxForkSpec child = ChildSpec();
    child.extra_drives.push_back(VolumeDrive("vol-child", "/mnt/a"));
    child.replace_drive_ids.push_back(std::make_pair("vol-a", "vol-child"));
    child.replace_drive_ids.push_back(std::make_pair("vol-b", "vol-child"));

    // Refused rather than deduplicated. Two slots resolving to one volume
    // would mount the same image twice, and for a writable volume that is two
    // mounts racing on one device.
    const core::Expected<SnapshotConfig, std::string> forked =
        SnapshotConfigForFork(source, child);
    MT_EXPECT_TRUE(!forked.ok());
    if (!forked.ok()) {
        MT_EXPECT_TRUE(forked.error().find("duplicate") != std::string::npos);
    }
}

MT_TEST(fork_config_refuses_a_replacement_it_was_not_given) {
    const SnapshotConfig source = ForkSource();
    SandboxForkSpec child = ChildSpec();
    // Names a replacement but does not supply the drive, so there is no image
    // path to put in the slot. The id alone carries nothing.
    child.replace_drive_ids.push_back(std::make_pair("vol-a", "vol-missing"));

    const core::Expected<SnapshotConfig, std::string> forked =
        SnapshotConfigForFork(source, child);
    MT_EXPECT_TRUE(!forked.ok());
    if (!forked.ok()) {
        MT_EXPECT_TRUE(forked.error().find("missing") != std::string::npos);
    }
}

MT_TEST(fork_config_refuses_a_source_slot_that_does_not_exist) {
    const SnapshotConfig source = ForkSource();
    SandboxForkSpec child = ChildSpec();
    child.extra_drives.push_back(VolumeDrive("vol-child", "/mnt/x"));
    child.replace_drive_ids.push_back(std::make_pair("vol-nope", "vol-child"));

    const core::Expected<SnapshotConfig, std::string> forked =
        SnapshotConfigForFork(source, child);
    MT_EXPECT_TRUE(!forked.ok());
    if (!forked.ok()) {
        MT_EXPECT_TRUE(forked.error().find("does not exist") != std::string::npos);
    }
}

MT_TEST(fork_config_refuses_to_append_a_drive) {
    const SnapshotConfig source = ForkSource();
    SandboxForkSpec child = ChildSpec();
    // A drive with no slot to go into, and no replacement naming it.
    child.extra_drives.push_back(VolumeDrive("vol-extra", "/mnt/extra"));

    // Appending is refused because the guest's device names come from the
    // serialized layout: Firecracker has no slot to load this into, and
    // making room would rename every later device.
    const core::Expected<SnapshotConfig, std::string> forked =
        SnapshotConfigForFork(source, child);
    MT_EXPECT_TRUE(!forked.ok());
    if (!forked.ok()) {
        MT_EXPECT_TRUE(forked.error().find("append") != std::string::npos);
    }
}

MT_TEST(fork_config_allows_replacing_every_volume_slot_at_once) {
    const SnapshotConfig source = ForkSource();
    SandboxForkSpec child = ChildSpec();
    child.extra_drives.push_back(VolumeDrive("c-a", "/mnt/a"));
    child.extra_drives.push_back(VolumeDrive("c-b", "/mnt/b"));
    child.replace_drive_ids.push_back(std::make_pair("vol-a", "c-a"));
    child.replace_drive_ids.push_back(std::make_pair("vol-b", "c-b"));

    const core::Expected<SnapshotConfig, std::string> forked =
        SnapshotConfigForFork(source, child);
    MT_EXPECT_TRUE(forked.ok());
    if (!forked.ok()) return;
    const std::vector<std::string> ids = DriveIds(forked.value().common.extra_drives);
    MT_EXPECT_EQ(ids[0], std::string("phys-a"));
    MT_EXPECT_EQ(ids[1], std::string("c-a"));
    MT_EXPECT_EQ(ids[2], std::string("c-b"));
    // The physical count is carried through: it is what tells the resume
    // which slots are volumes.
    MT_EXPECT_EQ(forked.value().common.physical_extra_drive_count,
                 static_cast<std::size_t>(1));
}

MT_MAIN
