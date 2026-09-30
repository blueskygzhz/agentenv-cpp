// SPDX-License-Identifier: MIT
// Tests for firecracker config validation + the real HTTP-over-unix-socket
// client + FirecrackerBackend pre-boot validation.
#include "microtest.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/cfg.h"
#include "agentenv/sandbox/extra_drive.h"
#include "agentenv/sandbox/manifest.h"
#include "agentenv/sandbox/ublk.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/sandbox/firecracker/config.h"
#include "agentenv/sandbox/firecracker/lifecycle.h"
#include "agentenv/sandbox/firecracker/socket.h"
#include "agentenv/sandbox/firecracker/sandbox.h"

using namespace agentenv::sandbox::firecracker;
namespace core = agentenv::core;

static agentenv::sandbox::ExtraDrive Drive(const std::string& id, const std::string& mount) {
    agentenv::sandbox::ExtraDrive d;
    d.drive_id = id;
    d.mount_path = mount;
    return d;
}

MT_TEST(default_boot_args_match_rust_byte_for_byte) {
    // Asserted exactly, not by substring: these DAMON values are reclaim
    // *policy*, and a stale copy changes how aggressively guest pagecache is
    // dropped without any test noticing. Must stay in sync with
    // config/default.toml.
    MT_EXPECT_EQ(std::string(kDefaultBootArgs),
                 std::string("console=ttyS0 reboot=k panic=1 pci=off "
                             "damon_reclaim.enabled=Y "
                             "damon_reclaim.min_age=100000 "
                             "damon_reclaim.quota_ms=20 "
                             "damon_reclaim.quota_sz=1073741824 "
                             "damon_reclaim.quota_reset_interval_ms=500 "
                             "damon_reclaim.wmarks_high=990 "
                             "damon_reclaim.wmarks_mid=990 "
                             "damon_reclaim.wmarks_low=200 "
                             "damon_reclaim.skip_anon=Y "
                             "damon_reclaim.wmarks_interval=1000000"));
}

MT_TEST(extra_drives_ok) {
    std::vector<agentenv::sandbox::ExtraDrive> ds;
    ds.push_back(Drive("data", "/mnt/data"));
    ds.push_back(Drive("logs", "/mnt/logs"));
    MT_EXPECT_TRUE(ValidateExtraDriveSet(ds, false).ok());
}

MT_TEST(extra_drives_duplicate_id) {
    std::vector<agentenv::sandbox::ExtraDrive> ds;
    ds.push_back(Drive("data", "/mnt/a"));
    ds.push_back(Drive("data", "/mnt/b"));
    auto r = ValidateExtraDriveSet(ds, false);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("duplicate extra drive id") != std::string::npos);
}

MT_TEST(extra_drives_duplicate_mount) {
    std::vector<agentenv::sandbox::ExtraDrive> ds;
    ds.push_back(Drive("a", "/mnt/x"));
    ds.push_back(Drive("b", "/mnt/x"));
    auto r = ValidateExtraDriveSet(ds, false);
    MT_EXPECT_TRUE(!r.ok());
    // An identical path is the degenerate case of the overlap rule Rust
    // applies (`starts_with` either way), so it is reported as overlapping
    // rather than merely duplicated.
    MT_EXPECT_TRUE(r.error().find("overlapping extra drive mount path") != std::string::npos);
}

MT_TEST(extra_drives_too_many) {
    std::vector<agentenv::sandbox::ExtraDrive> ds;
    for (int i = 0; i < 25; ++i) {  // MAX is 24
        char id[16]; std::snprintf(id, sizeof(id), "d%d", i);
        char mp[24]; std::snprintf(mp, sizeof(mp), "/mnt/%d", i);
        ds.push_back(Drive(id, mp));
    }
    auto r = ValidateExtraDriveSet(ds, false);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("too many extra drives") != std::string::npos);
}

MT_TEST(extra_drive_zero_virtual_size) {
    std::vector<agentenv::sandbox::ExtraDrive> ds;
    agentenv::sandbox::ExtraDrive d = Drive("data", "/mnt/data");
    // A recorded 0 is corrupt; an *absent* size is legitimate and means "let
    // the daemon read it from the image", which is why this is an optional.
    d.virtual_size = static_cast<uint64_t>(0);
    ds.push_back(d);
    auto r = ValidateExtraDriveSet(ds, false);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("must be non-zero") != std::string::npos);

    ds[0].virtual_size = core::nullopt;
    MT_EXPECT_TRUE(ValidateExtraDriveSet(ds, false).ok());
}

MT_TEST(resolve_serial_output_dir) {
    MT_EXPECT_TRUE(ResolveSerialOutputDir("").value().empty());
    MT_EXPECT_TRUE(ResolveSerialOutputDir("/abs/path").value() == "/abs/path");
    std::string rel = ResolveSerialOutputDir("rel/dir").value();
    MT_EXPECT_TRUE(!rel.empty() && rel[0] == '/');
    MT_EXPECT_TRUE(rel.find("rel/dir") != std::string::npos);
}

// ---- CommonConfig / SandboxConfig / SnapshotConfig validation ----

MT_TEST(common_config_rejects_missing_tools_drive_version) {
    // Exercised through `ResolvedToolsDrivePath` rather than `Validate`: Rust
    // always has a global config and so reaches this check first, whereas the
    // C++ accessor can return null and would report that instead. The rule
    // itself is what matters here.
    //
    // Blank-but-present must fail too, otherwise "  " would be resolved as a
    // version directory name.
    const agentenv::cfg::AppConfig app;
    const char* blanks[] = {"", "   ", "\t"};
    for (int i = 0; i < 3; ++i) {
        CommonConfig cfg = CommonConfig::New("/bin/sh", blanks[i], RuntimePolicy());
        auto r = cfg.ResolvedToolsDrivePath(app);
        MT_EXPECT_TRUE(!r.ok());
        MT_EXPECT_TRUE(r.error().find("does not record a tools drive version")
                       != std::string::npos);
    }
    // A well-formed version gets through to the path resolver and is named in
    // any resolver error, so an operator can see which version was wanted.
    CommonConfig ok = CommonConfig::New("/bin/sh", "1.2.3", RuntimePolicy());
    auto resolved = ok.ResolvedToolsDrivePath(app);
    if (!resolved.ok()) {
        MT_EXPECT_TRUE(resolved.error().find("1.2.3") != std::string::npos);
    }
}

MT_TEST(common_config_new_seeds_launchable_defaults) {
    // Rust's `new` fills these from defaults rather than leaving them zero, so
    // a config built outside `from_app_config` is still usable.
    CommonConfig cfg = CommonConfig::New("/bin/sh", "1.2.3", RuntimePolicy());
    MT_EXPECT_TRUE(cfg.track_dirty_pages);
    MT_EXPECT_EQ(cfg.control_plane_port, agentenv::cfg::ToolsConfig().control_plane_port);
    MT_EXPECT_EQ(cfg.envd_version, agentenv::cfg::EnvdConfig().version);
    // Absent, not empty: `apply_launch_config` only creates the map when there
    // is something to put in it.
    MT_EXPECT_TRUE(!cfg.env_vars.has_value());
    MT_EXPECT_TRUE(!cfg.rootfs_image_config.has_value());
    MT_EXPECT_EQ(cfg.physical_extra_drive_count, static_cast<std::size_t>(0));
    MT_EXPECT_EQ(cfg.volume_drive_slots, static_cast<std::size_t>(0));
}

MT_TEST(common_config_persisted_artifacts_distinguish_absent_from_zero_rootfs_size) {
    CommonConfig cfg = CommonConfig::New("/bin/sh", "1.2.3", RuntimePolicy());
    // Absent is fine on a fresh boot: the size comes from the image.
    MT_EXPECT_TRUE(cfg.ValidatePersistedArtifacts().ok());
    cfg.rootfs_virtual_size = static_cast<uint64_t>(0);
    auto r = cfg.ValidatePersistedArtifacts();
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("rootfs virtual size must be non-zero") != std::string::npos);
    cfg.rootfs_virtual_size = static_cast<uint64_t>(4096);
    MT_EXPECT_TRUE(cfg.ValidatePersistedArtifacts().ok());
}

MT_TEST(logging_enabled_treats_blank_level_as_off) {
    MT_EXPECT_TRUE(!LoggingEnabled(core::Optional<std::string>()));
    MT_EXPECT_TRUE(!LoggingEnabled(core::Optional<std::string>(std::string(""))));
    MT_EXPECT_TRUE(!LoggingEnabled(core::Optional<std::string>(std::string(" \t "))));
    MT_EXPECT_TRUE(LoggingEnabled(core::Optional<std::string>(std::string("info"))));
}

MT_TEST(snapshot_config_requires_a_recorded_rootfs_size) {
    // The asymmetry with a fresh boot is load-bearing: a restored guest expects
    // the exact block device it was captured with, so "absent" is not a valid
    // resume input even though it is valid at boot.
    SnapshotConfig snap;
    snap.common = CommonConfig::New("/bin/sh", "1.2.3", RuntimePolicy());
    auto absent = snap.ValidatePersistedArtifacts();
    MT_EXPECT_TRUE(!absent.ok());
    MT_EXPECT_TRUE(absent.error().find("rootfs virtual size must be non-zero")
                   != std::string::npos);

    snap.common.rootfs_virtual_size = static_cast<uint64_t>(4096);
    auto no_image = snap.ValidatePersistedArtifacts();
    MT_EXPECT_TRUE(!no_image.ok());
    MT_EXPECT_TRUE(no_image.error().find("base rootfs image config is missing")
                   != std::string::npos);
}

MT_TEST(snapshot_config_reports_the_missing_artifact_by_name) {
    SnapshotConfig snap;
    snap.common = CommonConfig::New("/bin/sh", "1.2.3", RuntimePolicy());
    snap.common.rootfs_virtual_size = static_cast<uint64_t>(4096);
    agentenv::sandbox::ublk::OverlaybdDeviceConfig rootfs;
    rootfs.image_config_path = "/bin/sh";  // exists and is a regular file
    snap.common.rootfs_image_config = rootfs;
    snap.mem_overlaybd_config.image_config_path = "/bin/sh";

    snap.vm_state_path = "/nonexistent/vm_state.bin";
    auto no_state = snap.ValidatePersistedArtifacts();
    MT_EXPECT_TRUE(!no_state.ok());
    MT_EXPECT_TRUE(no_state.error().find("vm state snapshot not found") != std::string::npos);

    snap.vm_state_path = "/bin/sh";
    MT_EXPECT_TRUE(snap.ValidatePersistedArtifacts().ok());

    // A directory exists but cannot be opened as a config; caught here rather
    // than much later inside the daemon.
    snap.mem_overlaybd_config.image_config_path = "/tmp";
    auto dir_config = snap.ValidatePersistedArtifacts();
    MT_EXPECT_TRUE(!dir_config.ok());
    MT_EXPECT_TRUE(dir_config.error().find("is not a file") != std::string::npos);
}

MT_TEST(sandbox_config_new_makes_the_user_rootfs_writable) {
    // A fresh boot's user rootfs is the sandbox's own disk, not a shared base
    // image, so it must not be read-only.
    SandboxConfig cfg = SandboxConfig::New("/bin/sh", "/bin/sh", "1.2.3",
                                           "/tmp/user-image.json");
    MT_EXPECT_TRUE(cfg.common.rootfs_image_config.has_value());
    if (!cfg.common.rootfs_image_config.has_value()) return;
    MT_EXPECT_TRUE(!cfg.common.rootfs_image_config->read_only);
    MT_EXPECT_EQ(cfg.common.rootfs_image_config->image_config_path,
                 std::string("/tmp/user-image.json"));
    MT_EXPECT_TRUE(cfg.common.rootfs_image_config->runtime_upper_mode ==
                   agentenv::storage::overlaybd::UpperMode::LogStructured);
    // Boot args stay absent here; `from_app_config_with_user_image` is what
    // substitutes DEFAULT_BOOT_ARGS.
    MT_EXPECT_TRUE(!cfg.boot_args.has_value());
}

// ---- SnapshotConfigFromRunnable (Rust from_runnable_snapshot) ----

static RunnableSnapshotView OkSnapshotView() {
    RunnableSnapshotView v;
    v.snapshot_id = "snap-1";
    v.virtualization_mode = agentenv::core::VirtualizationMode::Kvm;
    v.tools_drive_version = "1.2.3";
    v.envd_version = "0.5.15";
    v.workdir = "/work";
    return v;
}

static SnapshotManifestView OkManifestView() {
    SnapshotManifestView m;
    m.backend = agentenv::sandbox::kFirecrackerBackend;
    m.vm_state_path = "/snap/vm_state.bin";
    m.memory_image_config_path = "/snap/mem.json";
    m.memory_virtual_size = 2048;
    m.rootfs_image_config_path = "/snap/rootfs.json";
    m.rootfs_virtual_size = 4096;
    return m;
}

MT_TEST(snapshot_projection_refuses_another_backends_capture) {
    // Not merely unsupported: a capture is restored by the VMM that took it,
    // so another backend's vm_state is unreadable here.
    agentenv::cfg::AppConfig app;
    CommonConfig base = CommonConfig::New("/bin/sh", "1.2.3", RuntimePolicy());
    SnapshotManifestView manifest = OkManifestView();
    manifest.backend = "cloud-hypervisor";
    auto r = SnapshotConfigFromRunnable(base, app, OkSnapshotView(), manifest);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("cloud-hypervisor") != std::string::npos);
    MT_EXPECT_TRUE(r.error().find("snap-1") != std::string::npos);
}

MT_TEST(snapshot_projection_refuses_a_different_virtualization_mode) {
    // The guest ABI differs between modes. Both are named so an operator can
    // tell which kind of node the snapshot belongs on.
    agentenv::cfg::AppConfig app;
    app.virtualization_mode = agentenv::core::VirtualizationMode::Kvm;
    CommonConfig base = CommonConfig::New("/bin/sh", "1.2.3", RuntimePolicy());
    RunnableSnapshotView snapshot = OkSnapshotView();
    snapshot.virtualization_mode = agentenv::core::VirtualizationMode::Pvm;
    auto r = SnapshotConfigFromRunnable(base, app, snapshot, OkManifestView());
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("pvm") != std::string::npos);
    MT_EXPECT_TRUE(r.error().find("kvm") != std::string::npos);
}

MT_TEST(snapshot_projection_refuses_a_record_without_a_tools_drive_version) {
    agentenv::cfg::AppConfig app;
    CommonConfig base = CommonConfig::New("/bin/sh", "1.2.3", RuntimePolicy());
    const char* blanks[] = {"", "  "};
    for (int i = 0; i < 2; ++i) {
        RunnableSnapshotView snapshot = OkSnapshotView();
        snapshot.tools_drive_version = blanks[i];
        auto r = SnapshotConfigFromRunnable(base, app, snapshot, OkManifestView());
        MT_EXPECT_TRUE(!r.ok());
        MT_EXPECT_TRUE(r.error().find("tools drive version") != std::string::npos);
    }
}

MT_TEST(snapshot_projection_legacy_records_count_every_drive_as_physical) {
    // A snapshot taken before reserved volume slots existed cannot distinguish
    // physical drives from slots, and all of its drives are physical. Trusting
    // its recorded count (0) would let the launch rebind real drives as if
    // they were free volume slots.
    agentenv::cfg::AppConfig app;
    CommonConfig base = CommonConfig::New("/bin/sh", "1.2.3", RuntimePolicy());
    auto a = agentenv::sandbox::ExtraDrive::TryNewOverlaybd("a", "/tmp/a.json", true);
    auto b = agentenv::sandbox::ExtraDrive::TryNewOverlaybd("b", "/tmp/b.json", true);
    MT_EXPECT_TRUE(a.ok() && b.ok());
    if (!a.ok() || !b.ok()) return;

    SnapshotManifestView legacy = OkManifestView();
    legacy.extra_drives.push_back(a.value());
    legacy.extra_drives.push_back(b.value());
    legacy.volume_drive_slots = 0;
    legacy.physical_extra_drive_count = 0;  // absent in the old record
    auto old_record = SnapshotConfigFromRunnable(base, app, OkSnapshotView(), legacy);
    MT_EXPECT_TRUE(old_record.ok());
    if (!old_record.ok()) return;
    MT_EXPECT_EQ(old_record.value().common.physical_extra_drive_count,
                 static_cast<std::size_t>(2));

    // With slots reserved, the recorded count is authoritative.
    SnapshotManifestView modern = legacy;
    modern.volume_drive_slots = 3;
    modern.physical_extra_drive_count = 1;
    auto new_record = SnapshotConfigFromRunnable(base, app, OkSnapshotView(), modern);
    MT_EXPECT_TRUE(new_record.ok());
    if (!new_record.ok()) return;
    MT_EXPECT_EQ(new_record.value().common.physical_extra_drive_count,
                 static_cast<std::size_t>(1));
    MT_EXPECT_EQ(new_record.value().common.volume_drive_slots,
                 static_cast<std::size_t>(3));
}

MT_TEST(snapshot_projection_applies_node_settings_to_rootfs_but_not_memory) {
    agentenv::cfg::AppConfig app;
    app.ublk.overlaybd.read_only = true;
    app.ublk.overlaybd.runtime_upper_mode =
        agentenv::storage::overlaybd::UpperMode::HybridLogStructured;
    CommonConfig base = CommonConfig::New("/bin/sh", "1.2.3", RuntimePolicy());
    auto r = SnapshotConfigFromRunnable(base, app, OkSnapshotView(), OkManifestView());
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    const SnapshotConfig& c = r.value();

    // The rootfs takes this node's settings: read-only and the upper format
    // describe how this host runs the image, not the image itself.
    MT_EXPECT_TRUE(c.common.rootfs_image_config.has_value());
    if (!c.common.rootfs_image_config.has_value()) return;
    MT_EXPECT_TRUE(c.common.rootfs_image_config->read_only);
    MT_EXPECT_TRUE(c.common.rootfs_image_config->runtime_upper_mode ==
                   agentenv::storage::overlaybd::UpperMode::HybridLogStructured);
    // The ublk device must agree with it, or the daemon would open the image
    // differently from how the config describes it.
    MT_EXPECT_TRUE(c.common.ublk_config.has_value());
    if (c.common.ublk_config.has_value()) {
        MT_EXPECT_TRUE(c.common.ublk_config->overlaybd == *c.common.rootfs_image_config);
    }

    // The memory image is a capture being replayed, never written back, so it
    // stays read-only and log-structured whatever the node prefers.
    MT_EXPECT_TRUE(c.mem_overlaybd_config.read_only);
    MT_EXPECT_TRUE(c.mem_overlaybd_config.runtime_upper_mode ==
                   agentenv::storage::overlaybd::UpperMode::LogStructured);
    MT_EXPECT_EQ(c.mem_virtual_size, static_cast<uint64_t>(2048));
    MT_EXPECT_EQ(c.vm_state_path, std::string("/snap/vm_state.bin"));
}

MT_TEST(snapshot_projection_never_sets_a_cpu_template) {
    // A resume carries the full CPU state inside vm_state.bin. Re-applying a
    // template via the pre-boot PUT /cpu-config would be wrong, and Firecracker
    // rejects it outright.
    agentenv::cfg::AppConfig app;
    CommonConfig base = CommonConfig::New("/bin/sh", "1.2.3", RuntimePolicy());
    base.cpu_config_json = std::string("{\"template\":\"T2\"}");
    auto r = SnapshotConfigFromRunnable(base, app, OkSnapshotView(), OkManifestView());
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    // Rust gets this for free because its base always comes from
    // `from_global_config()`, where the field is never populated. Here `base`
    // is a parameter, so a node-level template could otherwise ride along into
    // a resume — it is cleared explicitly.
    MT_EXPECT_TRUE(!r.value().common.cpu_config_json.has_value());
}

MT_TEST(snapshot_projection_keeps_absent_env_vars_absent) {
    // Absent and empty are distinct downstream, so an empty recorded set must
    // not be projected as an empty map.
    agentenv::cfg::AppConfig app;
    CommonConfig base = CommonConfig::New("/bin/sh", "1.2.3", RuntimePolicy());
    auto empty = SnapshotConfigFromRunnable(base, app, OkSnapshotView(), OkManifestView());
    MT_EXPECT_TRUE(empty.ok());
    if (!empty.ok()) return;
    MT_EXPECT_TRUE(!empty.value().common.env_vars.has_value());

    RunnableSnapshotView with_env = OkSnapshotView();
    with_env.env_vars.push_back(std::make_pair(std::string("A"), std::string("1")));
    with_env.user = std::string("agent");
    auto populated = SnapshotConfigFromRunnable(base, app, with_env, OkManifestView());
    MT_EXPECT_TRUE(populated.ok());
    if (!populated.ok()) return;
    MT_EXPECT_TRUE(populated.value().common.env_vars.has_value());
    if (populated.value().common.env_vars.has_value()) {
        MT_EXPECT_EQ(populated.value().common.env_vars->size(),
                     static_cast<std::size_t>(1));
    }
    MT_EXPECT_TRUE(populated.value().common.default_workdir.has_value());
    if (populated.value().common.default_workdir.has_value()) {
        MT_EXPECT_EQ(*populated.value().common.default_workdir, std::string("/work"));
    }
    MT_EXPECT_TRUE(populated.value().common.default_user.has_value());
}

// ---- FilterExtraBootArgs / IsAllowedExtraBootArg (Rust factory.rs) ----

MT_TEST(filter_extra_boot_args_keeps_only_allowed_prefixes) {
    std::vector<std::string> prefixes;
    prefixes.push_back("agentenv-custom.");
    prefixes.push_back("agentenv.safe=");
    core::Optional<std::string> result = FilterExtraBootArgs(
        core::Optional<std::string>(std::string(
            "agentenv-custom.foo=bar debug agentenv.safe=1 root=/dev/vda")),
        prefixes);
    MT_EXPECT_TRUE(result.has_value());
    if (result.has_value())
        MT_EXPECT_EQ(*result, std::string("agentenv-custom.foo=bar agentenv.safe=1"));
}

MT_TEST(filter_extra_boot_args_drops_when_no_prefix_matches) {
    std::vector<std::string> prefixes;
    prefixes.push_back("agentenv-custom.");
    // Nothing survives when no token matches.
    MT_EXPECT_TRUE(!FilterExtraBootArgs(
        core::Optional<std::string>(std::string("debug panic=1")), prefixes).has_value());
    // Empty prefix list allows nothing through.
    MT_EXPECT_TRUE(!FilterExtraBootArgs(
        core::Optional<std::string>(std::string("agentenv-custom.foo=bar")),
        std::vector<std::string>()).has_value());
}

MT_TEST(filter_extra_boot_args_is_none_when_absent_or_blank) {
    std::vector<std::string> prefixes;
    prefixes.push_back("agentenv-custom.");
    MT_EXPECT_TRUE(!FilterExtraBootArgs(
        core::Optional<std::string>(), prefixes).has_value());
    MT_EXPECT_TRUE(!FilterExtraBootArgs(
        core::Optional<std::string>(std::string("")), prefixes).has_value());
    MT_EXPECT_TRUE(!FilterExtraBootArgs(
        core::Optional<std::string>(std::string("   ")), prefixes).has_value());
}

MT_TEST(filter_extra_boot_args_drops_invalid_chars) {
    std::vector<std::string> prefixes;
    prefixes.push_back("agentenv-custom.");
    core::Optional<std::string> result = FilterExtraBootArgs(
        core::Optional<std::string>(std::string(
            "agentenv-custom.flag "
            "agentenv-custom.bad_char=a:b "
            "agentenv-custom.good=azAZ09_-./+= "
            "agentenv-custom.base64=YWJjZA==/+")),
        prefixes);
    MT_EXPECT_TRUE(result.has_value());
    if (result.has_value())
        MT_EXPECT_EQ(*result, std::string(
            "agentenv-custom.flag "
            "agentenv-custom.good=azAZ09_-./+= "
            "agentenv-custom.base64=YWJjZA==/+"));
}

MT_TEST(is_allowed_extra_boot_arg_empty_prefix_is_not_a_wildcard) {
    // An empty prefix would match every non-empty string, defeating the allowlist.
    std::vector<std::string> prefixes;
    prefixes.push_back("");
    prefixes.push_back("safe.");
    MT_EXPECT_TRUE(!IsAllowedExtraBootArg("anything", prefixes));
    MT_EXPECT_TRUE(IsAllowedExtraBootArg("safe.flag", prefixes));
}

MT_TEST(is_allowed_extra_boot_arg_empty_token_is_rejected) {
    std::vector<std::string> prefixes;
    prefixes.push_back("p.");
    MT_EXPECT_TRUE(!IsAllowedExtraBootArg("", prefixes));
}

MT_TEST(is_allowed_extra_boot_arg_full_char_allowlist) {
    std::vector<std::string> prefixes;
    prefixes.push_back("p.");
    // Every character in [A-Za-z0-9_\-.\/+=] must be accepted.
    MT_EXPECT_TRUE(IsAllowedExtraBootArg("p.abcdefghijklmnopqrstuvwxyz", prefixes));
    MT_EXPECT_TRUE(IsAllowedExtraBootArg("p.ABCDEFGHIJKLMNOPQRSTUVWXYZ", prefixes));
    MT_EXPECT_TRUE(IsAllowedExtraBootArg("p.0123456789", prefixes));
    MT_EXPECT_TRUE(IsAllowedExtraBootArg("p._-./+=", prefixes));
    // Characters outside must be rejected even when the prefix matches.
    MT_EXPECT_TRUE(!IsAllowedExtraBootArg("p. space", prefixes));
    MT_EXPECT_TRUE(!IsAllowedExtraBootArg("p.semi;colon", prefixes));
    MT_EXPECT_TRUE(!IsAllowedExtraBootArg("p.colon:val", prefixes));
    MT_EXPECT_TRUE(!IsAllowedExtraBootArg("p.bang!", prefixes));
    MT_EXPECT_TRUE(!IsAllowedExtraBootArg("p.at@", prefixes));
    MT_EXPECT_TRUE(!IsAllowedExtraBootArg("p.pipe|val", prefixes));
    MT_EXPECT_TRUE(!IsAllowedExtraBootArg("p.backslash\\val", prefixes));
    MT_EXPECT_TRUE(!IsAllowedExtraBootArg("p.tilde~", prefixes));
    MT_EXPECT_TRUE(!IsAllowedExtraBootArg("p.amp&val", prefixes));
}

MT_TEST(filter_extra_boot_args_appended_to_existing_boot_args) {
    // Rust: `Some(existing) => format!("{existing} {extra_boot_args}")`.
    const std::string base = "console=ttyS0 panic=1";
    std::vector<std::string> prefixes;
    prefixes.push_back("custom.");
    core::Optional<std::string> extra = FilterExtraBootArgs(
        core::Optional<std::string>(std::string("custom.flag debug")), prefixes);
    MT_EXPECT_TRUE(extra.has_value());
    if (extra.has_value())
        MT_EXPECT_EQ(base + " " + *extra, std::string("console=ttyS0 panic=1 custom.flag"));
}

MT_TEST(runtime_policy_converts_seconds_to_milliseconds) {
    agentenv::cfg::AppConfig app;
    app.firecracker.socket_timeout_secs = 7;
    app.firecracker.socket_poll_ms = 2;
    app.envd.init_timeout_secs = 90;
    app.envd.poll_ms = 5;
    RuntimePolicy policy = RuntimePolicy::FromAppConfig(app);
    // Rust builds Durations with from_secs/from_millis; the unit is normalised
    // here so a millisecond field cannot be misread as seconds.
    MT_EXPECT_EQ(policy.socket_timeout_ms, static_cast<uint64_t>(7000));
    MT_EXPECT_EQ(policy.socket_poll_interval_ms, static_cast<uint64_t>(2));
    MT_EXPECT_EQ(policy.envd_timeout_ms, static_cast<uint64_t>(90000));
    MT_EXPECT_EQ(policy.envd_poll_interval_ms, static_cast<uint64_t>(5));
}

// ---- real HTTP-over-unix-socket round trip ----
namespace {
struct ServerArgs {
    std::string path;
    std::string reply;
};
void* http_server_thread(void* arg) {
    ServerArgs* sa = static_cast<ServerArgs*>(arg);
    int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return nullptr;
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sa->path.c_str(), sizeof(addr.sun_path) - 1);
    ::unlink(sa->path.c_str());
    if (::bind(srv, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(srv); return nullptr;
    }
    ::listen(srv, 1);
    int c = ::accept(srv, nullptr, nullptr);
    if (c >= 0) {
        char buf[2048];
        ::read(c, buf, sizeof(buf));  // drain request
        ::write(c, sa->reply.data(), sa->reply.size());
        ::close(c);
    }
    ::close(srv);
    ::unlink(sa->path.c_str());
    return nullptr;
}
}  // namespace

MT_TEST(unix_socket_http_roundtrip) {
    ServerArgs sa;
    sa.path = "/tmp/agentenv_fc_test.sock";
    sa.reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
               "Connection: close\r\n\r\n{\"state\":\"Running\"}";

    pthread_t th;
    pthread_create(&th, nullptr, http_server_thread, &sa);
    // Give the server a moment to bind/listen.
    struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 100* 1000000L; nanosleep(&ts, nullptr);

    std::shared_ptr<Connector> conn = MakeUnixConnector();
    auto sock = conn->Connect(sa.path);
    MT_EXPECT_TRUE(sock.ok());
    if (sock.ok()) {
        auto resp = sock.value()->Request("GET", "/", "");
        MT_EXPECT_TRUE(resp.ok());
        MT_EXPECT_TRUE(resp.value() == "{\"state\":\"Running\"}");
    }
    pthread_join(th, nullptr);
}

MT_TEST(unix_socket_connect_missing) {
    std::shared_ptr<Connector> conn = MakeUnixConnector();
    auto sock = conn->Connect("/tmp/agentenv_no_such.sock");
    // `Connect` is lazy, matching Rust's infallible `UnixSocketClient::new`:
    // no connection is attempted until a request needs one, so a missing
    // socket surfaces on first use rather than here.
    MT_EXPECT_TRUE(sock.ok());
    auto resp = sock.value()->Request("GET", "/", "");
    MT_EXPECT_TRUE(!resp.ok());
    MT_EXPECT_TRUE(resp.error().find("connect") != std::string::npos);
}

// ---- FirecrackerBackend pre-boot validation ----
MT_TEST(firecracker_boot_rejects_missing_binary) {
    Config cfg;
    cfg.firecracker_bin = "/nonexistent/firecracker";
    FirecrackerBackend be(cfg);
    agentenv::sandbox::LaunchPlan plan;
    plan.sandbox_id = agentenv::core::SandboxId::Fresh();
    plan.kernel_path = "/bin/sh";
    auto r = be.Boot(plan);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(std::string(r.error().chain()).find("firecracker binary not found")
                   != std::string::npos);
}

// ---- DAMON reclaim monitor region (Rust sandbox.rs) ----

static const uint64_t TMIB = 1024ULL * 1024ULL;
static const uint64_t TGIB = 1024ULL * TMIB;

static void ExpectRegion(uint32_t mem_mib, const char* arch, uint64_t start, uint64_t end) {
    core::Optional<std::pair<uint64_t, uint64_t> > r = DamonMonitorRegion(mem_mib, arch);
    MT_EXPECT_TRUE(r.has_value());
    if (!r.has_value()) return;
    MT_EXPECT_EQ(r->first, start);
    MT_EXPECT_EQ(r->second, end);
}

MT_TEST(damon_monitor_region_follows_firecracker_memory_layout) {
    // x86_64: <=3G identity-mapped, then a 1G hole below 256G, then 257G above.
    ExpectRegion(1024, "x86_64", 4096, TGIB);
    ExpectRegion(3072, "x86_64", 4096, 3 * TGIB);
    ExpectRegion(3073, "x86_64", 4096, 4 * TGIB + TMIB);
    ExpectRegion(4096, "x86_64", 4096, 5 * TGIB);
    ExpectRegion(8192, "x86_64", 4096, 9 * TGIB);
    ExpectRegion(255 * 1024, "x86_64", 4096, 256 * TGIB);
    ExpectRegion(255 * 1024 + 1, "x86_64", 4096, 512 * TGIB + TMIB);
    ExpectRegion(UINT32_MAX, "x86_64", 4096,
                 static_cast<uint64_t>(UINT32_MAX) * TMIB + 257 * TGIB);

    // aarch64: guest DRAM starts at 2G, so the window is shifted, not just sized.
    ExpectRegion(1024, "aarch64", 2 * TGIB, 3 * TGIB);
    ExpectRegion(254 * 1024, "aarch64", 2 * TGIB, 256 * TGIB);
    ExpectRegion(254 * 1024 + 1, "aarch64", 2 * TGIB, 512 * TGIB + TMIB);
    ExpectRegion(UINT32_MAX, "aarch64", 2 * TGIB,
                 static_cast<uint64_t>(UINT32_MAX) * TMIB + 258 * TGIB);

    MT_EXPECT_TRUE(!DamonMonitorRegion(0, "x86_64").has_value());
    MT_EXPECT_TRUE(!DamonMonitorRegion(1024, "riscv64").has_value());
}

MT_TEST(damon_monitor_region_is_added_only_when_needed) {
    const char* appended[] = {
        "console=ttyS0",
        "console=ttyS0 damon_reclaim.enabled=Y",
        "console=ttyS0 damon_reclaim.enabled=N",
    };
    for (int i = 0; i < 3; ++i) {
        // The region is appended regardless of whether reclaim is on: the args
        // stay valid if an operator flips `enabled` later.
        const std::string expected = std::string(appended[i]) +
            " damon_reclaim.monitor_region_start=4096"
            " damon_reclaim.monitor_region_end=5368709120";
        core::Optional<std::string> got = AddDamonMonitorRegion(
            core::Optional<std::string>(std::string(appended[i])), 4096, "x86_64");
        MT_EXPECT_TRUE(got.has_value());
        if (got.has_value()) MT_EXPECT_EQ(*got, expected);
    }

    // An explicit region is operator intent and must survive untouched, even
    // when only one of the two bounds is pinned.
    const char* unchanged[] = {
        "damon_reclaim.enabled=Y damon_reclaim.monitor_region_start=1234",
        "damon_reclaim.enabled=Y damon_reclaim.monitor_region_end=5678",
        "damon_reclaim.monitor_region_start=1234 damon_reclaim.monitor_region_end=5678",
    };
    for (int i = 0; i < 3; ++i) {
        core::Optional<std::string> got = AddDamonMonitorRegion(
            core::Optional<std::string>(std::string(unchanged[i])), 4096, "x86_64");
        MT_EXPECT_TRUE(got.has_value());
        if (got.has_value()) MT_EXPECT_EQ(*got, std::string(unchanged[i]));
    }

    // `start=0 end=0` is the opt-in sentinel asking for the computed region:
    // both tokens are dropped, then the real values appended.
    core::Optional<std::string> substituted = AddDamonMonitorRegion(
        core::Optional<std::string>(std::string(
            "console=ttyS0 damon_reclaim.monitor_region_start=0"
            " damon_reclaim.monitor_region_end=0")),
        4096, "x86_64");
    MT_EXPECT_TRUE(substituted.has_value());
    if (substituted.has_value()) {
        MT_EXPECT_EQ(*substituted,
                     std::string("console=ttyS0 damon_reclaim.monitor_region_start=4096"
                                 " damon_reclaim.monitor_region_end=5368709120"));
    }

    // Unknown arch: no layout to compute from, so the args pass through.
    core::Optional<std::string> unknown_arch = AddDamonMonitorRegion(
        core::Optional<std::string>(std::string("console=ttyS0")), 4096, "riscv64");
    MT_EXPECT_TRUE(unknown_arch.has_value());
    if (unknown_arch.has_value()) MT_EXPECT_EQ(*unknown_arch, std::string("console=ttyS0"));

    // No boot args at all stays absent rather than becoming a bare region.
    MT_EXPECT_TRUE(!AddDamonMonitorRegion(core::Optional<std::string>(), 4096, "x86_64")
                        .has_value());
}

// ---- disk I/O rate limiting (Rust sandbox.rs) ----

static agentenv::cfg::DiskRateLimitConfig RateLimitCfg() {
    agentenv::cfg::DiskRateLimitConfig cfg;
    cfg.enabled = true;
    cfg.bandwidth_bytes_per_sec = 0;
    cfg.bandwidth_burst_bytes = 0;
    cfg.iops = 0;
    cfg.iops_burst = 0;
    return cfg;
}

MT_TEST(rate_limiter_disabled_returns_none) {
    agentenv::cfg::DiskRateLimitConfig cfg = RateLimitCfg();
    cfg.enabled = false;
    cfg.bandwidth_bytes_per_sec = 104857600;
    auto rl = BuildDiskRateLimiter(cfg);
    MT_EXPECT_TRUE(rl.ok());
    MT_EXPECT_TRUE(!rl.value().has_value());
}

MT_TEST(rate_limiter_enabled_but_all_zero_returns_none) {
    // Enabled with nothing configured attaches no limiter at all, rather than
    // an empty one that Firecracker would have to interpret.
    auto rl = BuildDiskRateLimiter(RateLimitCfg());
    MT_EXPECT_TRUE(rl.ok());
    MT_EXPECT_TRUE(!rl.value().has_value());
}

MT_TEST(rate_limiter_bandwidth_size_equals_per_second_rate) {
    agentenv::cfg::DiskRateLimitConfig cfg = RateLimitCfg();
    cfg.bandwidth_bytes_per_sec = 104857600;  // 100 MB/s
    cfg.bandwidth_burst_bytes = 10485760;
    auto built = BuildDiskRateLimiter(cfg);
    MT_EXPECT_TRUE(built.ok());
    MT_EXPECT_TRUE(built.value().has_value());
    if (!built.ok() || !built.value().has_value()) return;
    const RateLimiter& rl = *built.value();
    MT_EXPECT_TRUE(rl.bandwidth.has_value());
    if (!rl.bandwidth.has_value()) return;
    // With refill pinned to 1000 ms, bucket size == sustained bytes/sec.
    MT_EXPECT_EQ(rl.bandwidth->refill_time, kRateLimitRefillTimeMs);
    MT_EXPECT_EQ(rl.bandwidth->size, static_cast<int64_t>(104857600));
    MT_EXPECT_TRUE(rl.bandwidth->one_time_burst.has_value());
    if (rl.bandwidth->one_time_burst.has_value()) {
        MT_EXPECT_EQ(*rl.bandwidth->one_time_burst, static_cast<int64_t>(10485760));
    }
    MT_EXPECT_TRUE(!rl.ops.has_value());
}

MT_TEST(rate_limiter_iops_bucket_populated) {
    agentenv::cfg::DiskRateLimitConfig cfg = RateLimitCfg();
    cfg.iops = 3000;
    cfg.iops_burst = 500;
    auto built = BuildDiskRateLimiter(cfg);
    MT_EXPECT_TRUE(built.ok());
    MT_EXPECT_TRUE(built.value().has_value());
    if (!built.ok() || !built.value().has_value()) return;
    const RateLimiter& rl = *built.value();
    MT_EXPECT_TRUE(rl.ops.has_value());
    if (!rl.ops.has_value()) return;
    MT_EXPECT_EQ(rl.ops->refill_time, kRateLimitRefillTimeMs);
    MT_EXPECT_EQ(rl.ops->size, static_cast<int64_t>(3000));
    MT_EXPECT_TRUE(rl.ops->one_time_burst.has_value());
    if (rl.ops->one_time_burst.has_value()) {
        MT_EXPECT_EQ(*rl.ops->one_time_burst, static_cast<int64_t>(500));
    }
    MT_EXPECT_TRUE(!rl.bandwidth.has_value());
}

MT_TEST(rate_limiter_zero_burst_is_omitted_not_sent_as_zero) {
    // Firecracker only grants a one-time burst when the field is present, so a
    // 0 burst has to be absent rather than an explicit 0.
    agentenv::cfg::DiskRateLimitConfig cfg = RateLimitCfg();
    cfg.bandwidth_bytes_per_sec = 104857600;
    cfg.iops = 3000;
    auto built = BuildDiskRateLimiter(cfg);
    MT_EXPECT_TRUE(built.ok() && built.value().has_value());
    if (!built.ok() || !built.value().has_value()) return;
    const RateLimiter& rl = *built.value();
    MT_EXPECT_TRUE(rl.bandwidth.has_value() && !rl.bandwidth->one_time_burst.has_value());
    MT_EXPECT_TRUE(rl.ops.has_value() && !rl.ops->one_time_burst.has_value());
}

MT_TEST(rate_limiter_rejects_values_beyond_i64_range) {
    agentenv::cfg::DiskRateLimitConfig cfg = RateLimitCfg();
    cfg.bandwidth_bytes_per_sec = UINT64_MAX;
    auto rl = BuildDiskRateLimiter(cfg);
    MT_EXPECT_TRUE(!rl.ok());
    MT_EXPECT_TRUE(rl.error().find("bandwidth_bytes_per_sec") != std::string::npos);
    MT_EXPECT_TRUE(rl.error().find("i64 range") != std::string::npos);
}

MT_TEST(reconcile_disabled_makes_both_buckets_disabled) {
    // Firecracker treats an absent bucket in a PATCH as "leave unchanged", so
    // clearing an inherited limiter requires overwriting BOTH buckets with a
    // disabled (size == 0) bucket rather than sending an empty RateLimiter.
    agentenv::cfg::DiskRateLimitConfig cfg = RateLimitCfg();
    cfg.enabled = false;
    cfg.bandwidth_bytes_per_sec = 100ULL << 20;
    cfg.iops = 3000;
    auto built = ReconcileDiskRateLimiter(cfg);
    MT_EXPECT_TRUE(built.ok());
    if (!built.ok()) return;
    MT_EXPECT_TRUE(built.value().bandwidth.has_value());
    MT_EXPECT_TRUE(built.value().ops.has_value());
    if (!built.value().bandwidth.has_value() || !built.value().ops.has_value()) return;
    MT_EXPECT_EQ(built.value().bandwidth->size, static_cast<int64_t>(0));
    MT_EXPECT_EQ(built.value().ops->size, static_cast<int64_t>(0));
    // The sentinel is size==0 AND refill_time==0; a mixed bucket would be
    // rejected by Firecracker as invalid and fail the resume PATCH.
    MT_EXPECT_EQ(built.value().bandwidth->refill_time, static_cast<int64_t>(0));
    MT_EXPECT_EQ(built.value().ops->refill_time, static_cast<int64_t>(0));
}

MT_TEST(reconcile_bandwidth_only_clears_inherited_iops) {
    // Enabled with bandwidth but no iops: bandwidth gets its configured bucket,
    // while the unset iops dimension is overwritten with a disabled bucket so a
    // snapshot-inherited IOPS limit does not survive the resume.
    agentenv::cfg::DiskRateLimitConfig cfg = RateLimitCfg();
    cfg.enabled = true;
    cfg.bandwidth_bytes_per_sec = 100ULL << 20;
    cfg.iops = 0;
    auto built = ReconcileDiskRateLimiter(cfg);
    MT_EXPECT_TRUE(built.ok());
    if (!built.ok()) return;
    MT_EXPECT_TRUE(built.value().bandwidth.has_value() && built.value().ops.has_value());
    if (!built.value().bandwidth.has_value() || !built.value().ops.has_value()) return;
    MT_EXPECT_EQ(built.value().bandwidth->refill_time, kRateLimitRefillTimeMs);
    MT_EXPECT_EQ(built.value().bandwidth->size, static_cast<int64_t>(100ULL << 20));
    MT_EXPECT_EQ(built.value().ops->size, static_cast<int64_t>(0));
}

MT_TEST(reconcile_iops_only_clears_inherited_bandwidth) {
    agentenv::cfg::DiskRateLimitConfig cfg = RateLimitCfg();
    cfg.enabled = true;
    cfg.bandwidth_bytes_per_sec = 0;
    cfg.iops = 3000;
    auto built = ReconcileDiskRateLimiter(cfg);
    MT_EXPECT_TRUE(built.ok());
    if (!built.ok()) return;
    MT_EXPECT_TRUE(built.value().bandwidth.has_value() && built.value().ops.has_value());
    if (!built.value().bandwidth.has_value() || !built.value().ops.has_value()) return;
    MT_EXPECT_EQ(built.value().bandwidth->size, static_cast<int64_t>(0));
    MT_EXPECT_EQ(built.value().ops->refill_time, kRateLimitRefillTimeMs);
    MT_EXPECT_EQ(built.value().ops->size, static_cast<int64_t>(3000));
}

MT_TEST(reconcile_both_dimensions_use_configured_buckets) {
    agentenv::cfg::DiskRateLimitConfig cfg = RateLimitCfg();
    cfg.enabled = true;
    cfg.bandwidth_bytes_per_sec = 100ULL << 20;
    cfg.iops = 3000;
    auto built = ReconcileDiskRateLimiter(cfg);
    MT_EXPECT_TRUE(built.ok());
    if (!built.ok()) return;
    MT_EXPECT_TRUE(built.value().bandwidth.has_value() && built.value().ops.has_value());
    if (!built.value().bandwidth.has_value() || !built.value().ops.has_value()) return;
    MT_EXPECT_EQ(built.value().bandwidth->refill_time, kRateLimitRefillTimeMs);
    MT_EXPECT_EQ(built.value().bandwidth->size, static_cast<int64_t>(100ULL << 20));
    MT_EXPECT_EQ(built.value().ops->refill_time, kRateLimitRefillTimeMs);
    MT_EXPECT_EQ(built.value().ops->size, static_cast<int64_t>(3000));
}

MT_TEST(reconcile_rejects_values_beyond_i64_range) {
    agentenv::cfg::DiskRateLimitConfig cfg = RateLimitCfg();
    cfg.iops = UINT64_MAX;
    auto built = ReconcileDiskRateLimiter(cfg);
    MT_EXPECT_TRUE(!built.ok());
    MT_EXPECT_TRUE(built.error().find("iops") != std::string::npos);
}

// ---- drive slots / boot args (Rust sandbox.rs) ----

MT_TEST(volume_drive_slot_ids_are_indexed) {
    MT_EXPECT_EQ(VolumeDriveSlotId(0),
                 std::string(agentenv::sandbox::kVolumeDriveSlotPrefix) + "0");
    MT_EXPECT_EQ(VolumeDriveSlotId(3),
                 std::string(agentenv::sandbox::kVolumeDriveSlotPrefix) + "3");
    MT_EXPECT_TRUE(VolumeDriveSlotId(0) != VolumeDriveSlotId(1));
}

MT_TEST(build_drives_boot_arg_is_none_without_drives) {
    std::vector<agentenv::sandbox::ExtraDrive> drives;
    MT_EXPECT_TRUE(!BuildDrivesBootArg(drives).has_value());
}

MT_TEST(build_drives_boot_arg_uses_custom_mount_path) {
    // vda is the tools drive and vdb the user image, so extra drives start at vdc.
    auto data = agentenv::sandbox::ExtraDrive::TryNewOverlaybdWithMountPath(
        "data", "/tmp/data-image.json", true, "/workspace/data",
        core::Optional<std::string>());
    auto logs = agentenv::sandbox::ExtraDrive::TryNewOverlaybd(
        "logs", "/tmp/logs-image.json", true);
    MT_EXPECT_TRUE(data.ok());
    MT_EXPECT_TRUE(logs.ok());
    if (!data.ok() || !logs.ok()) return;
    std::vector<agentenv::sandbox::ExtraDrive> drives;
    drives.push_back(data.value());
    drives.push_back(logs.value());
    core::Optional<std::string> arg = BuildDrivesBootArg(drives);
    MT_EXPECT_TRUE(arg.has_value());
    if (arg.has_value()) {
        MT_EXPECT_EQ(*arg,
                     std::string("agentenv_drives=vdc:/workspace/data,vdd:/mnt/logs"));
    }
}

MT_TEST(build_drives_boot_arg_includes_sub_path) {
    auto data = agentenv::sandbox::ExtraDrive::TryNewOverlaybdWithMountPath(
        "data", "/tmp/data-image.json", true, "/mnt/data",
        core::Optional<std::string>(std::string("sub/dir")));
    MT_EXPECT_TRUE(data.ok());
    if (!data.ok()) return;
    std::vector<agentenv::sandbox::ExtraDrive> drives;
    drives.push_back(data.value());
    core::Optional<std::string> arg = BuildDrivesBootArg(drives);
    MT_EXPECT_TRUE(arg.has_value());
    if (arg.has_value()) {
        MT_EXPECT_EQ(*arg, std::string("agentenv_drives=vdc:/mnt/data:sub/dir"));
    }
}

MT_TEST(image_config_paths_put_rootfs_first) {
    // Order matters: the rootfs image is the base layer of the runtime artifact
    // set, so it must precede the drives that are stacked on top of it.
    auto data = agentenv::sandbox::ExtraDrive::TryNewOverlaybd(
        "data", "/tmp/data-image.json", true);
    MT_EXPECT_TRUE(data.ok());
    if (!data.ok()) return;
    std::vector<agentenv::sandbox::ExtraDrive> drives;
    drives.push_back(data.value());

    std::vector<std::string> with_rootfs = RootfsAndExtraDriveImageConfigPaths(
        core::Optional<std::string>(std::string("/tmp/user-image.json")), drives);
    MT_EXPECT_EQ(with_rootfs.size(), static_cast<std::size_t>(2));
    if (with_rootfs.size() == 2) {
        MT_EXPECT_EQ(with_rootfs[0], std::string("/tmp/user-image.json"));
        MT_EXPECT_EQ(with_rootfs[1], std::string("/tmp/data-image.json"));
    }

    std::vector<std::string> without_rootfs =
        RootfsAndExtraDriveImageConfigPaths(core::Optional<std::string>(), drives);
    MT_EXPECT_EQ(without_rootfs.size(), static_cast<std::size_t>(1));
    if (without_rootfs.size() == 1) {
        MT_EXPECT_EQ(without_rootfs[0], std::string("/tmp/data-image.json"));
    }
}

MT_MAIN
