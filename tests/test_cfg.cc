// SPDX-License-Identifier: MIT
// Ports the `#[cfg(test)] mod tests` blocks of src/cfg.rs, src/cfg/image.rs and
// src/cfg/network.rs. Test names mirror the Rust function names so the two
// suites can be diffed side by side.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

#include "agentenv/cfg.h"
#include "agentenv/core/virtualization.h"
#include "microtest.h"

namespace {

using agentenv::cfg::AppConfig;
using agentenv::cfg::ConfigManager;
using agentenv::cfg::DiskRateLimitConfig;
using agentenv::cfg::FirecrackerProcessPoolConfig;
using agentenv::cfg::ImageCacheGcConfig;
using agentenv::cfg::NetworkConfig;
using agentenv::cfg::NetworkEgressConfig;
using agentenv::cfg::NetworkInternalConfig;
using agentenv::cfg::OverlaybdDependencyConfig;
using agentenv::cfg::PathJoin;
using agentenv::cfg::PoolComponentConfig;
using agentenv::cfg::PosixFsBackendConfig;
using agentenv::cfg::ResolvePath;
using agentenv::cfg::SandboxConfig;
using agentenv::cfg::SandboxProxyConfig;
using agentenv::cfg::SetupDependencyManifest;
using agentenv::core::VirtualizationMode;

/// Rust `tempfile::tempdir()`.
class TempDir {
 public:
    TempDir() {
        char pattern[] = "/tmp/agentenv-cfg-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        MT_EXPECT_TRUE(created != nullptr);
        path_ = created;
    }
    ~TempDir() {
        // Best-effort recursive cleanup; a leftover temp dir must not fail a test.
        const std::string command = "rm -rf '" + path_ + "'";
        if (::system(command.c_str()) != 0) { /* ignore */ }
    }
    const std::string& path() const { return path_; }

 private:
    std::string path_;
};

bool WriteFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path.c_str());
    if (!out.is_open()) return false;
    out << contents;
    return true;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// Defaults: the C++ NSDMI values must equal Rust `impl_config_default!`.
// ---------------------------------------------------------------------------

MT_TEST(app_config_defaults_match_rust) {
    AppConfig config;
    MT_EXPECT_EQ(config.home_path, std::string("/var/lib/aenv"));
    MT_EXPECT_EQ(config.runtime_path, std::string("/run/aenv"));
    MT_EXPECT_EQ(config.deps_path, std::string("$AENV_HOME/deps"));
    MT_EXPECT_TRUE(config.virtualization_mode == VirtualizationMode::Kvm);

    MT_EXPECT_EQ(config.firecracker.socket_timeout_secs, 3u);
    MT_EXPECT_EQ(config.firecracker.socket_poll_ms, 1u);
    MT_EXPECT_EQ(config.envd.version, std::string("0.5.15"));
    MT_EXPECT_EQ(config.envd.init_timeout_secs, 60u);
    MT_EXPECT_EQ(config.envd.poll_ms, 3u);
    MT_EXPECT_EQ(config.tools.control_plane_port, 49983);
    MT_EXPECT_EQ(config.machine.vcpu_count, 2u);
    MT_EXPECT_EQ(config.machine.mem_size_mib, 1024u);
    MT_EXPECT_EQ(config.volume.max_size_mb, 262144u);
    MT_EXPECT_EQ(config.volume.max_volume_count, 4u);
    MT_EXPECT_EQ(config.pool.low_watermark, 2u);
    MT_EXPECT_EQ(config.pool.high_watermark, 64u);
    MT_EXPECT_EQ(config.pool.firecracker.fill_concurrency, 4u);
    MT_EXPECT_EQ(config.orchestrator.metrics_interval_secs, 15u);
    MT_EXPECT_EQ(config.orchestrator.metrics_retention_secs, 3600u);
    MT_EXPECT_EQ(config.orchestrator.auto_evict_interval_ms, 1000u);
    MT_EXPECT_EQ(config.orchestrator.default_sandbox_timeout_secs, 15u);
    MT_EXPECT_EQ(config.orchestrator.auto_resume_min_sandbox_timeout_secs, 300u);
    MT_EXPECT_EQ(config.ublk.overlaybd.resize_timeout_secs, 120u);
    MT_EXPECT_EQ(config.ublk.overlaybd.remote_io_workers, 4u);
    MT_EXPECT_EQ(config.ublk.overlaybd.p2p_lookup_timeout_ms, 300u);
    MT_EXPECT_EQ(config.ublk.overlaybd.p2p_fetch_range_timeout_ms, 2000u);
    MT_EXPECT_EQ(config.ublk.daemon_metrics_listen_addr, std::string("0.0.0.0:9103"));
    MT_EXPECT_TRUE(config.memory_snapshot.track_dirty_pages);
    MT_EXPECT_EQ(config.memory_snapshot.background_download.block_size, 16777216u);
    MT_EXPECT_EQ(config.memory_snapshot.background_download.concurrency, 4u);
    MT_EXPECT_EQ(config.memory_snapshot.background_download.max_inflight_blocks, 16u);
    MT_EXPECT_EQ(config.memory_snapshot.background_download.try_cnt, 5);
    MT_EXPECT_EQ(config.memory_snapshot.background_download.delay_extra, 1);
    MT_EXPECT_TRUE(config.observability.enabled);
    MT_EXPECT_TRUE(!config.observability.scheduler_report.enabled);
    MT_EXPECT_EQ(config.observability.scheduler_report.interval_secs, 5u);
    MT_EXPECT_TRUE(config.snapshot.p2p_enabled);
    MT_EXPECT_TRUE(config.snapshot.publish_compression.enabled);
    MT_EXPECT_EQ(config.snapshot.publish_compression.workers, 1u);
    MT_EXPECT_EQ(config.snapshot.memory_startup_pack.max_pack_bytes, 1073741824ull);
    MT_EXPECT_TRUE(!config.p2p.enabled);
    MT_EXPECT_EQ(config.p2p.listen_addr, std::string("0.0.0.0:0"));
    MT_EXPECT_EQ(config.p2p.lookup_timeout_ms, 5000u);
    MT_EXPECT_EQ(config.p2p.fetch_timeout_ms, 30000u);
    MT_EXPECT_EQ(config.custom_extension.timeout_ms, 5000u);

    // cfg/image.rs defaults
    MT_EXPECT_EQ(config.image.resolver.default_image, std::string("ubuntu:24.04"));
    MT_EXPECT_EQ(config.image.resolver.search_registries.size(), 2u);
    MT_EXPECT_EQ(config.image.resolver.search_registries[0], std::string("docker.io"));
    MT_EXPECT_EQ(config.image.resolver.search_registries[1], std::string("ghcr.io"));
    MT_EXPECT_TRUE(config.image.resolver.convert_standard_oci);
    MT_EXPECT_TRUE(!config.image.resolver.allowed_registries.has_value());
    MT_EXPECT_EQ(config.image.cache.remote_blocks.max_size_gb, 10u);
    MT_EXPECT_EQ(config.image.cache.gc.interval_secs, 1800u);
    MT_EXPECT_EQ(config.image.cache.gc.min_age_secs, 600u);

    // cfg/network.rs defaults
    MT_EXPECT_EQ(config.network.egress.always_denied_cidrs.size(), 6u);
    MT_EXPECT_EQ(config.network.internal.host_interaction_cidr, std::string("10.11.0.0/16"));
    MT_EXPECT_EQ(config.network.internal.veth_cidr, std::string("10.12.0.0/16"));

    // A default config must pass its own validation.
    MT_EXPECT_TRUE(config.Validate().has_value());
}

// ---------------------------------------------------------------------------
// Rust: `template_builder_defaults_and_validation`
// ---------------------------------------------------------------------------

MT_TEST(template_builder_defaults_and_validation) {
    AppConfig config;
    MT_EXPECT_EQ(config.template_build.builder_cpu_count, 16u);
    MT_EXPECT_EQ(config.template_build.builder_memory_mb, 32768u);
    MT_EXPECT_EQ(config.template_build.max_concurrent_builds, 4u);
    MT_EXPECT_EQ(config.template_build.cache_size_mb, 65536u);
    MT_EXPECT_EQ(config.template_build.builder_image,
                 std::string("docker.io/moby/buildkit:v0.33.0"));

    MT_EXPECT_TRUE(config.ValidateTemplateBuilder().has_value());
    config.template_build.max_concurrent_builds = 0;
    MT_EXPECT_TRUE(!config.ValidateTemplateBuilder().has_value());
    config.template_build.max_concurrent_builds = 1;
    MT_EXPECT_TRUE(config.ValidateTemplateBuilder().has_value());
    config.template_build.builder_cpu_count = 0;
    MT_EXPECT_TRUE(!config.ValidateTemplateBuilder().has_value());
    config.template_build.builder_cpu_count = 16;
    config.template_build.builder_memory_mb = 255;
    MT_EXPECT_TRUE(!config.ValidateTemplateBuilder().has_value());
    config.template_build.builder_memory_mb = 2147483647u;  // i32::MAX
    MT_EXPECT_TRUE(config.ValidateTemplateBuilder().has_value());
    config.template_build.builder_memory_mb += 1;
    MT_EXPECT_TRUE(!config.ValidateTemplateBuilder().has_value());
    config.template_build.builder_memory_mb = 32768;
    config.volume.max_size_mb = 8192;
    MT_EXPECT_TRUE(config.Validate().has_value());
    config.template_build.cache_size_mb = 1023;
    MT_EXPECT_TRUE(!config.ValidateTemplateBuilder().has_value());
    config.template_build.cache_size_mb = 65536;
    config.template_build.builder_image = " ";
    MT_EXPECT_TRUE(!config.ValidateTemplateBuilder().has_value());
}

// ---------------------------------------------------------------------------
// Rust: `validate_memory_snapshot_options_enforces_dirty_page_requirements`
// ---------------------------------------------------------------------------

MT_TEST(validate_memory_snapshot_options_enforces_dirty_page_requirements) {
    {
        AppConfig config;
        config.virtualization_mode = VirtualizationMode::Kvm;
        config.memory_snapshot.track_dirty_pages = false;
        MT_EXPECT_TRUE(config.ValidateMemorySnapshotOptions().has_value());
    }
    {
        AppConfig config;
        config.virtualization_mode = VirtualizationMode::Kvm;
        config.memory_snapshot.track_dirty_pages = true;
        MT_EXPECT_TRUE(config.ValidateMemorySnapshotOptions().has_value());
    }
    {
        AppConfig config;
        config.virtualization_mode = VirtualizationMode::Pvm;
        config.memory_snapshot.track_dirty_pages = true;
        agentenv::core::Expected<agentenv::core::Unit, std::string> result =
            config.ValidateMemorySnapshotOptions();
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "is disabled in PVM mode"));
    }
}

// ---------------------------------------------------------------------------
// Rust: `validate_rejects_zero_memory_snapshot_download_concurrency`
//       `validate_bounds_memory_snapshot_background_download`
// ---------------------------------------------------------------------------

MT_TEST(validate_rejects_zero_memory_snapshot_download_concurrency) {
    AppConfig config;
    config.memory_snapshot.background_download.concurrency = 0;
    agentenv::core::Expected<agentenv::core::Unit, std::string> result = config.Validate();
    MT_EXPECT_TRUE(!result.has_value());
    MT_EXPECT_TRUE(Contains(result.error(),
                            "memory_snapshot.background_download.concurrency must be > 0"));
}

MT_TEST(validate_bounds_memory_snapshot_background_download) {
    {
        AppConfig config;
        config.memory_snapshot.background_download.concurrency = 17;
        MT_EXPECT_TRUE(!config.Validate().has_value());
    }
    {
        AppConfig config;
        config.memory_snapshot.background_download.block_size = 65u * 1024u * 1024u;
        config.memory_snapshot.background_download.concurrency = 1;
        MT_EXPECT_TRUE(!config.Validate().has_value());
    }
    {
        AppConfig config;
        config.memory_snapshot.background_download.block_size = 64u * 1024u * 1024u;
        config.memory_snapshot.background_download.concurrency = 8;
        MT_EXPECT_TRUE(!config.Validate().has_value());
    }
    {
        AppConfig config;
        config.memory_snapshot.background_download.block_size = 32u * 1024u * 1024u;
        config.memory_snapshot.background_download.concurrency = 8;
        MT_EXPECT_TRUE(config.Validate().has_value());
    }
    {
        AppConfig config;
        config.memory_snapshot.background_download.delay = -1;
        MT_EXPECT_TRUE(!config.Validate().has_value());
    }
    {
        AppConfig config;
        config.memory_snapshot.background_download.try_cnt = 0;
        MT_EXPECT_TRUE(!config.Validate().has_value());
    }
}

// ---------------------------------------------------------------------------
// Rust: `validate_rejects_invalid_disk_rate_limit`
//       `validate_skips_disabled_disk_rate_limit`
//       `validate_accepts_consistent_disk_rate_limit`
// ---------------------------------------------------------------------------

MT_TEST(validate_rejects_invalid_disk_rate_limit) {
    {
        AppConfig config;
        config.machine.disk_rate_limit.enabled = true;
        config.machine.disk_rate_limit.bandwidth_burst_bytes = 1024;
        agentenv::core::Expected<agentenv::core::Unit, std::string> result = config.Validate();
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "bandwidth_burst_bytes is set but"));
    }
    {
        AppConfig config;
        config.machine.disk_rate_limit.enabled = true;
        config.machine.disk_rate_limit.iops_burst = 500;
        agentenv::core::Expected<agentenv::core::Unit, std::string> result = config.Validate();
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "iops_burst is set but"));
    }
    {
        AppConfig config;
        config.machine.disk_rate_limit.enabled = true;
        // i64::MAX + 1
        config.machine.disk_rate_limit.bandwidth_bytes_per_sec = 9223372036854775808ull;
        agentenv::core::Expected<agentenv::core::Unit, std::string> result = config.Validate();
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(
            Contains(result.error(), "machine.disk_rate_limit.bandwidth_bytes_per_sec"));
    }
}

MT_TEST(validate_skips_disabled_disk_rate_limit) {
    // A disabled section is ignored at runtime, so even internally inconsistent
    // or out-of-range values must not block startup.
    AppConfig config;
    config.machine.disk_rate_limit.enabled = false;
    config.machine.disk_rate_limit.bandwidth_bytes_per_sec = 0;
    config.machine.disk_rate_limit.bandwidth_burst_bytes = 1024;
    config.machine.disk_rate_limit.iops = 18446744073709551615ull;
    MT_EXPECT_TRUE(config.Validate().has_value());
}

MT_TEST(validate_accepts_consistent_disk_rate_limit) {
    AppConfig config;
    config.machine.disk_rate_limit.enabled = true;
    config.machine.disk_rate_limit.bandwidth_bytes_per_sec = 104857600;
    config.machine.disk_rate_limit.bandwidth_burst_bytes = 10485760;
    config.machine.disk_rate_limit.iops = 3000;
    config.machine.disk_rate_limit.iops_burst = 500;
    MT_EXPECT_TRUE(config.Validate().has_value());
}

// ---------------------------------------------------------------------------
// Rust: `validate_rejects_invalid_volume_limits`
// ---------------------------------------------------------------------------

MT_TEST(validate_rejects_invalid_volume_limits) {
    {
        AppConfig config;
        config.volume.max_size_mb = 0;
        agentenv::core::Expected<agentenv::core::Unit, std::string> result = config.Validate();
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "volume.max_size_mb"));
    }
    {
        AppConfig config;
        config.volume.max_volume_count = 0;
        agentenv::core::Expected<agentenv::core::Unit, std::string> result = config.Validate();
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "volume.max_volume_count"));
    }
    {
        AppConfig config;
        config.volume.max_size_mb = 18446744073709551615ull;
        agentenv::core::Expected<agentenv::core::Unit, std::string> result = config.Validate();
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "volume.max_size_mb"));
    }
    {
        AppConfig config;
        config.volume.max_volume_count = agentenv::cfg::kMaxVolumeMounts + 1;
        agentenv::core::Expected<agentenv::core::Unit, std::string> result = config.Validate();
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "volume.max_volume_count"));
    }
}

// ---------------------------------------------------------------------------
// Rust: `validate_rejects_colliding_overlaybd_global_config_paths`
// ---------------------------------------------------------------------------

MT_TEST(validate_rejects_colliding_overlaybd_global_config_paths) {
    struct Case {
        const char* runtime_path;
        const char* memory_path;
        const char* left_name;
        const char* right_name;
    };
    const Case cases[4] = {
        {"/tmp/shared-overlaybd-global.json", "/tmp/shared-overlaybd-global.json",
         "ublk.overlaybd.global_config_path", "memory_snapshot.overlaybd_global_config_path"},
        {"/tmp/aenv/x/../global.json", "/tmp/aenv/global.json",
         "ublk.overlaybd.global_config_path", "memory_snapshot.overlaybd_global_config_path"},
        {"/tmp/overlaybd/resize-overlaybd-global.json",
         "/var/lib/aenv/overlaybd/mem-overlaybd-global.json",
         "ublk.overlaybd.global_config_path", "derived resize"},
        {"/tmp/overlaybd/overlaybd-global.json", "/tmp/overlaybd/convert-overlaybd-global.json",
         "memory_snapshot.overlaybd_global_config_path", "derived convert"},
    };

    for (int i = 0; i < 4; ++i) {
        AppConfig config;
        config.ublk.overlaybd.global_config_path = cases[i].runtime_path;
        config.memory_snapshot.overlaybd_global_config_path = cases[i].memory_path;

        agentenv::core::Expected<agentenv::core::Unit, std::string> result = config.Validate();
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), cases[i].left_name));
        MT_EXPECT_TRUE(Contains(result.error(), cases[i].right_name));
        MT_EXPECT_TRUE(Contains(result.error(), "must be different"));
    }
}

// ---------------------------------------------------------------------------
// Rust: `validate_rejects_zero_overlaybd_resize_timeout`
// ---------------------------------------------------------------------------

MT_TEST(validate_rejects_zero_overlaybd_resize_timeout) {
    AppConfig config;
    config.ublk.overlaybd.resize_timeout_secs = 0;
    agentenv::core::Expected<agentenv::core::Unit, std::string> result = config.Validate();
    MT_EXPECT_TRUE(!result.has_value());
    MT_EXPECT_TRUE(Contains(result.error(), "resize_timeout_secs must be > 0"));
}

// ---------------------------------------------------------------------------
// Rust: `validate_pool_config_rejects_invalid_values`
//       `firecracker_pool_fill_concurrency_rejects_zero`
// ---------------------------------------------------------------------------

MT_TEST(validate_pool_config_rejects_invalid_values) {
    {
        AppConfig config;
        config.pool.low_watermark = 64;
        config.pool.high_watermark = 32;
        agentenv::core::Expected<agentenv::core::Unit, std::string> result =
            config.ValidatePoolConfig();
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "low_watermark"));
    }
    {
        // Every pool disabled -> nothing to validate, so the inverted
        // watermarks are accepted.
        AppConfig config;
        config.pool.low_watermark = 64;
        config.pool.high_watermark = 32;
        config.pool.network.maintenance_enabled = false;
        config.pool.block.enabled = false;
        config.pool.firecracker.enabled = false;
        MT_EXPECT_TRUE(config.ValidatePoolConfig().has_value());
    }
}

MT_TEST(firecracker_pool_fill_concurrency_rejects_zero) {
    AppConfig config;
    config.pool.firecracker.enabled = true;
    config.pool.firecracker.fill_concurrency = 0;
    agentenv::core::Expected<agentenv::core::Unit, std::string> result =
        config.ValidatePoolConfig();
    MT_EXPECT_TRUE(!result.has_value());
    MT_EXPECT_TRUE(Contains(result.error(), "fill_concurrency"));
}

// ---------------------------------------------------------------------------
// Rust: `overlaybd_converter_cache_id_includes_configured_tool_version`
// ---------------------------------------------------------------------------

MT_TEST(overlaybd_converter_cache_id_includes_configured_tool_version) {
    AppConfig config;
    OverlaybdDependencyConfig dep;
    dep.version = "test-version";
    config.overlaybd = dep;
    MT_EXPECT_EQ(config.ResolvedOverlaybdOciConverterId(),
                 std::string("overlaybd-oci:test-version:agentenv-cache-v1"));
}

// ---------------------------------------------------------------------------
// Rust: `resolve_path_joins_relative_paths_against_config_dir`
//       `resolve_config_path_keeps_absolute_paths_unchanged`
// ---------------------------------------------------------------------------

MT_TEST(resolve_path_joins_relative_paths_against_config_dir) {
    TempDir temp;
    const std::string config_dir = temp.path();
    const std::string home_path = PathJoin(temp.path(), "home");

    MT_EXPECT_EQ(ResolvePath(home_path, config_dir, "bin/firecracker"),
                 PathJoin(config_dir, "bin/firecracker"));
    MT_EXPECT_EQ(ResolvePath(home_path, config_dir, "$AENV_HOME/image-cache"),
                 PathJoin(home_path, "image-cache"));
}

MT_TEST(resolve_config_path_keeps_absolute_paths_unchanged) {
    const std::string absolute = "/tmp/agentenv-absolute";
    MT_EXPECT_EQ(ResolvePath("/ignored-home", "/ignored", absolute), absolute);
}

// ---------------------------------------------------------------------------
// Rust: `normalize_paths_relative_to_config_dir`
// ---------------------------------------------------------------------------

MT_TEST(normalize_paths_relative_to_config_dir) {
    TempDir temp;
    const std::string config_dir = PathJoin(temp.path(), "configs");

    AppConfig config;
    config.deps_path = "./env";
    config.firecracker.binary_path = std::string("./bin/firecracker");
    config.firecracker.work_dir = std::string("./firecracker-work");
    config.firecracker.serial_dir = std::string("./firecracker-serial");
    config.kernel.image_path = std::string("./kernel/vmlinux.bin");
    config.tools.drive_path = std::string("./tools.ext4");
    config.image.cache.root_dir = "./custom-image-cache";
    config.snapshot.local_cache_path = "./snapshot-local-cache";
    PosixFsBackendConfig posix;
    posix.snapshot_store = "./snapshot-store";
    config.backend.posix_fs = posix;
    config.p2p.store_dir = "./p2p-store";
    config.ublk.daemon_binary_path = std::string("./bin/uvm-ublk-daemon");
    config.ublk.daemon_socket_path = "./run/uvm-ublk-daemon.sock";
    config.ublk.daemon_log_path = std::string("./logs/uvm-ublk-daemon.log");
    config.ublk.overlaybd.global_config_path = "./overlaybd-global.json";
    config.memory_snapshot.overlaybd_global_config_path = "./mem-overlaybd-global.json";
    config.orchestrator.persisted_sandbox_store_path = "./persisted-sandboxes";

    MT_EXPECT_TRUE(config.Normalize(config_dir).has_value());

    MT_EXPECT_EQ(config.deps_path, PathJoin(config_dir, "env"));
    MT_EXPECT_EQ(*config.firecracker.binary_path, PathJoin(config_dir, "bin/firecracker"));
    MT_EXPECT_EQ(*config.firecracker.work_dir, PathJoin(config_dir, "firecracker-work"));
    MT_EXPECT_EQ(*config.firecracker.serial_dir, PathJoin(config_dir, "firecracker-serial"));
    MT_EXPECT_EQ(*config.kernel.image_path, PathJoin(config_dir, "kernel/vmlinux.bin"));
    MT_EXPECT_EQ(*config.tools.drive_path, PathJoin(config_dir, "tools.ext4"));
    MT_EXPECT_EQ(config.image.cache.root_dir, PathJoin(config_dir, "custom-image-cache"));
    MT_EXPECT_EQ(config.snapshot.local_cache_path, PathJoin(config_dir, "snapshot-local-cache"));
    MT_EXPECT_EQ(config.p2p.store_dir, PathJoin(config_dir, "p2p-store"));
    MT_EXPECT_EQ(config.backend.posix_fs->snapshot_store, PathJoin(config_dir, "snapshot-store"));
    MT_EXPECT_EQ(*config.ublk.daemon_binary_path, PathJoin(config_dir, "bin/uvm-ublk-daemon"));
    MT_EXPECT_EQ(config.ublk.daemon_socket_path,
                 PathJoin(config_dir, "run/uvm-ublk-daemon.sock"));
    MT_EXPECT_EQ(*config.ublk.daemon_log_path, PathJoin(config_dir, "logs/uvm-ublk-daemon.log"));
    MT_EXPECT_EQ(config.ublk.overlaybd.global_config_path,
                 PathJoin(config_dir, "overlaybd-global.json"));
    MT_EXPECT_EQ(config.memory_snapshot.overlaybd_global_config_path,
                 PathJoin(config_dir, "mem-overlaybd-global.json"));
    MT_EXPECT_EQ(config.orchestrator.persisted_sandbox_store_path,
                 PathJoin(config_dir, "persisted-sandboxes"));
}

// ---------------------------------------------------------------------------
// Rust: `managed_dependency_paths_remain_implicit_and_resolve_from_deps_path`
// ---------------------------------------------------------------------------

MT_TEST(managed_dependency_paths_remain_implicit_and_resolve_from_deps_path) {
    TempDir temp;
    const std::string config_dir = PathJoin(temp.path(), "configs");

    AppConfig config;
    config.deps_path = "./deps";
    config.firecracker.version = std::string("fc-test");
    config.kernel.version = std::string("kernel-test");
    config.tools.version = std::string("1.2.3-custom.1");

    MT_EXPECT_TRUE(config.Normalize(config_dir).has_value());

    const std::string deps_path = PathJoin(config_dir, "deps");
    MT_EXPECT_EQ(config.deps_path, deps_path);
    MT_EXPECT_TRUE(!config.firecracker.binary_path.has_value());
    MT_EXPECT_TRUE(!config.kernel.image_path.has_value());
    MT_EXPECT_TRUE(!config.tools.drive_path.has_value());
    MT_EXPECT_EQ(config.ResolvedFirecrackerBinaryPath(),
                 PathJoin(deps_path, "firecracker/fc-test/firecracker"));
    MT_EXPECT_EQ(config.ResolvedKernelImagePath(),
                 PathJoin(deps_path, "kernel/kernel-test/vmlinux.bin"));

    agentenv::core::Expected<std::string, std::string> drive = config.ResolvedToolsDrivePath();
    MT_EXPECT_TRUE(drive.has_value());
    MT_EXPECT_EQ(drive.value(), PathJoin(deps_path, "tools/1.2.3-custom.1/tools.ext4"));

    // Path traversal and build metadata are both rejected.
    MT_EXPECT_TRUE(!config.ResolvedToolsDrivePathForVersion("../escape").has_value());
    MT_EXPECT_TRUE(!config.ResolvedToolsDrivePathForVersion("1.2.3+rebuilt").has_value());
}

// ---------------------------------------------------------------------------
// Rust: `managed_dependency_paths_select_the_active_mode_versions`
// ---------------------------------------------------------------------------

MT_TEST(managed_dependency_paths_select_the_active_mode_versions) {
    const SetupDependencyManifest& manifest = SetupDependencyManifest::Get();

    AppConfig config;
    config.deps_path = "/deps";
    config.virtualization_mode = VirtualizationMode::Kvm;
    MT_EXPECT_EQ(config.ResolvedFirecrackerBinaryPath(),
                 PathJoin(PathJoin("/deps/firecracker", manifest.firecracker.kvm.version),
                          "firecracker"));
    MT_EXPECT_EQ(config.ResolvedKernelImagePath(),
                 PathJoin(PathJoin("/deps/kernel", manifest.kernel.kvm.version), "vmlinux.bin"));

    config.virtualization_mode = VirtualizationMode::Pvm;
    MT_EXPECT_EQ(config.ResolvedFirecrackerBinaryPath(),
                 PathJoin(PathJoin("/deps/firecracker", manifest.firecracker.pvm.version),
                          "firecracker"));
    MT_EXPECT_EQ(config.ResolvedKernelImagePath(),
                 PathJoin(PathJoin("/deps/kernel", manifest.kernel.pvm.version), "vmlinux.bin"));
}

// ---------------------------------------------------------------------------
// Rust: `home_relative_toml_values_derive_from_home_path`
// ---------------------------------------------------------------------------

MT_TEST(home_relative_toml_values_derive_from_home_path) {
    TempDir temp;
    const std::string config_dir = PathJoin(temp.path(), "configs");

    AppConfig config;
    config.home_path = "./env";
    config.runtime_path = "$AENV_HOME/run";
    config.deps_path = "$AENV_HOME/deps";
    config.ublk.daemon_socket_path = "$AENV_RUNTIME/ublk-daemon.sock";
    config.firecracker.serial_dir = std::string("$AENV_HOME/logs/serial");
    config.image.cache.root_dir = "$AENV_HOME/image-cache";
    config.snapshot.local_cache_path = "$AENV_HOME/snapshot-local-cache";
    config.p2p.store_dir = "$AENV_HOME/p2p/store";
    config.ublk.overlaybd.global_config_path = "$AENV_HOME/overlaybd/overlaybd-global.json";
    config.memory_snapshot.overlaybd_global_config_path =
        "$AENV_HOME/overlaybd/mem-overlaybd-global.json";

    MT_EXPECT_TRUE(config.Normalize(config_dir).has_value());

    const std::string home_path = PathJoin(config_dir, "env");
    MT_EXPECT_EQ(config.runtime_path, PathJoin(home_path, "run"));
    MT_EXPECT_EQ(config.ublk.daemon_socket_path, PathJoin(home_path, "run/ublk-daemon.sock"));
    MT_EXPECT_EQ(*config.firecracker.serial_dir, PathJoin(home_path, "logs/serial"));
    MT_EXPECT_EQ(config.deps_path, PathJoin(home_path, "deps"));
    MT_EXPECT_EQ(config.image.cache.root_dir, PathJoin(home_path, "image-cache"));
    MT_EXPECT_EQ(config.snapshot.local_cache_path, PathJoin(home_path, "snapshot-local-cache"));
    MT_EXPECT_EQ(config.p2p.store_dir, PathJoin(PathJoin(home_path, "p2p"), "store"));
    MT_EXPECT_EQ(config.ublk.overlaybd.global_config_path,
                 PathJoin(PathJoin(home_path, "overlaybd"), "overlaybd-global.json"));
    MT_EXPECT_EQ(config.memory_snapshot.overlaybd_global_config_path,
                 PathJoin(PathJoin(home_path, "overlaybd"), "mem-overlaybd-global.json"));
}

// ---------------------------------------------------------------------------
// Rust: `compile_time_defaults_derive_from_home_path`
// ---------------------------------------------------------------------------

MT_TEST(compile_time_defaults_derive_from_home_path) {
    TempDir temp;
    const std::string config_dir = PathJoin(temp.path(), "configs");

    AppConfig config;
    config.home_path = "./env";
    MT_EXPECT_TRUE(config.Normalize(config_dir).has_value());

    const std::string home_path = PathJoin(config_dir, "env");
    // `runtime_path` has an absolute compile-time default, so it does not move.
    MT_EXPECT_EQ(config.runtime_path, std::string("/run/aenv"));
    MT_EXPECT_EQ(config.deps_path, PathJoin(home_path, "deps"));
    MT_EXPECT_EQ(*config.firecracker.work_dir, PathJoin(home_path, "firecracker-work"));
    MT_EXPECT_EQ(*config.firecracker.serial_dir, PathJoin(home_path, "logs/serial"));
    MT_EXPECT_EQ(config.image.cache.root_dir, PathJoin(home_path, "image-cache"));
    MT_EXPECT_EQ(config.snapshot.local_cache_path, PathJoin(home_path, "snapshot-local-cache"));
    MT_EXPECT_EQ(config.p2p.store_dir, PathJoin(home_path, "p2p/store"));
    MT_EXPECT_EQ(config.ublk.overlaybd.global_config_path,
                 PathJoin(home_path, "overlaybd/overlaybd-global.json"));
    MT_EXPECT_EQ(config.memory_snapshot.overlaybd_global_config_path,
                 PathJoin(home_path, "overlaybd/mem-overlaybd-global.json"));
    MT_EXPECT_EQ(config.orchestrator.persisted_sandbox_store_path,
                 PathJoin(home_path, "persisted-sandboxes"));
    MT_EXPECT_EQ(config.backend.posix_fs->snapshot_store, PathJoin(home_path, "snapshot-store"));
    MT_EXPECT_EQ(*config.ublk.daemon_binary_path, PathJoin(home_path, "ublk/uvm-ublk-daemon"));
    MT_EXPECT_EQ(config.ublk.daemon_socket_path, std::string("/run/aenv/ublk-daemon.sock"));
    MT_EXPECT_EQ(*config.ublk.daemon_log_path, PathJoin(home_path, "logs/ublk-daemon.log"));
}

// ---------------------------------------------------------------------------
// Rust: `sandbox_proxy_domains_are_normalized`
//       `sandbox_proxy_domains_reject_invalid_domain`
// ---------------------------------------------------------------------------

MT_TEST(sandbox_proxy_domains_are_normalized) {
    SandboxProxyConfig config;
    config.domains.push_back(" Sandbox.Example.Invalid. ");
    config.domains.push_back("sandbox.example.invalid");
    config.domains.push_back("");
    config.domains.push_back("old.example.invalid");

    MT_EXPECT_TRUE(config.Normalize().has_value());
    MT_EXPECT_EQ(config.domains.size(), 2u);
    MT_EXPECT_EQ(config.domains[0], std::string("sandbox.example.invalid"));
    MT_EXPECT_EQ(config.domains[1], std::string("old.example.invalid"));
}

MT_TEST(sandbox_proxy_domains_reject_invalid_domain) {
    SandboxProxyConfig config;
    config.domains.push_back("not a domain");
    agentenv::core::Expected<agentenv::core::Unit, std::string> result = config.Normalize();
    MT_EXPECT_TRUE(!result.has_value());
    MT_EXPECT_TRUE(Contains(result.error(), "sandbox_proxy.domains contains invalid domain"));
}

// ---------------------------------------------------------------------------
// Rust: `cluster_normalize_trims_and_drops_blank_scheduler_endpoint`
// ---------------------------------------------------------------------------

MT_TEST(cluster_normalize_trims_and_drops_blank_scheduler_endpoint) {
    {
        agentenv::cfg::ClusterConfig config;
        config.scheduler_endpoint = std::string("  ");
        config.Normalize();
        MT_EXPECT_TRUE(!config.scheduler_endpoint.has_value());
    }
    {
        agentenv::cfg::ClusterConfig config;
        config.scheduler_endpoint = std::string("  http://scheduler:9090  ");
        config.Normalize();
        MT_EXPECT_TRUE(config.scheduler_endpoint.has_value());
        MT_EXPECT_EQ(*config.scheduler_endpoint, std::string("http://scheduler:9090"));
    }
}

// ---------------------------------------------------------------------------
// Rust: `sandbox_access_token_seed_is_redacted`
// ---------------------------------------------------------------------------

MT_TEST(sandbox_access_token_seed_is_redacted) {
    SandboxConfig config;
    config.access_token_hash_seed = std::string("cluster-secret");
    MT_EXPECT_TRUE(!Contains(config.DebugString(), "cluster-secret"));
    MT_EXPECT_TRUE(Contains(config.DebugString(), "redacted"));
}

// ---------------------------------------------------------------------------
// Rust: cfg/image.rs `validate_rejects_invalid_gc_watermarks`
// ---------------------------------------------------------------------------

MT_TEST(validate_rejects_invalid_gc_watermarks) {
    {
        ImageCacheGcConfig config;
        config.high_watermark_ratio = 1.2;
        agentenv::core::Expected<agentenv::core::Unit, std::string> result = config.Validate();
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "high_watermark_ratio"));
    }
    {
        ImageCacheGcConfig config;
        config.high_watermark_ratio = 0.5;
        config.low_watermark_ratio = 0.7;
        agentenv::core::Expected<agentenv::core::Unit, std::string> result = config.Validate();
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "low_watermark_ratio"));
    }
}

MT_TEST(image_cache_gc_normalize_restores_zero_interval) {
    ImageCacheGcConfig config;
    config.interval_secs = 0;
    config.Normalize();
    MT_EXPECT_EQ(config.interval_secs, 1800u);
}

MT_TEST(image_cache_layout_derives_paths_and_capacity) {
    AppConfig config;
    config.image.cache.root_dir = "/var/lib/aenv/image-cache";
    config.image.cache.capacity_gb = static_cast<uint64_t>(4);

    agentenv::cfg::ResolvedImageCacheConfig layout = config.ImageCacheLayout();
    MT_EXPECT_EQ(layout.root_dir, std::string("/var/lib/aenv/image-cache"));
    MT_EXPECT_EQ(layout.commit_store, std::string("/var/lib/aenv/image-cache/commits"));
    MT_EXPECT_EQ(layout.remote_blocks_dir,
                 std::string("/var/lib/aenv/image-cache/remote-blocks"));
    MT_EXPECT_EQ(layout.remote_blocks_size_gb, 10u);
    MT_EXPECT_TRUE(layout.capacity_bytes.has_value());
    MT_EXPECT_EQ(*layout.capacity_bytes, 4ull * 1024ull * 1024ull * 1024ull);
}

MT_TEST(image_resolver_normalize_trims_and_filters) {
    agentenv::cfg::ImageResolverConfig config;
    config.default_image = "   ";
    config.search_registries.clear();
    config.search_registries.push_back("  docker.io/  ");
    std::vector<std::string> allowed;
    allowed.push_back("  ghcr.io/ ");
    allowed.push_back("   ");
    config.allowed_registries = allowed;
    config.try_referrers_overlaybd_prefixes.push_back("  registry.example.com/  ");
    config.try_referrers_overlaybd_prefixes.push_back("");

    config.Normalize();

    // Blank falls back to the compile-time default.
    MT_EXPECT_EQ(config.default_image, std::string("ubuntu:24.04"));
    MT_EXPECT_EQ(config.search_registries[0], std::string("docker.io"));
    MT_EXPECT_TRUE(config.allowed_registries.has_value());
    MT_EXPECT_EQ(config.allowed_registries->size(), 1u);
    MT_EXPECT_EQ((*config.allowed_registries)[0], std::string("ghcr.io"));
    MT_EXPECT_EQ(config.try_referrers_overlaybd_prefixes.size(), 1u);
    MT_EXPECT_EQ(config.try_referrers_overlaybd_prefixes[0],
                 std::string("registry.example.com/"));

    // An explicit empty list must stay `Some([])` (deny all), not become unset.
    agentenv::cfg::ImageResolverConfig deny_all;
    deny_all.allowed_registries = std::vector<std::string>();
    deny_all.Normalize();
    MT_EXPECT_TRUE(deny_all.allowed_registries.has_value());
    MT_EXPECT_EQ(deny_all.allowed_registries->size(), 0u);
}

MT_TEST(lexically_normalize_path_resolves_dot_segments) {
    MT_EXPECT_EQ(agentenv::cfg::LexicallyNormalizePath("/tmp/aenv/x/../global.json"),
                 std::string("/tmp/aenv/global.json"));
    MT_EXPECT_EQ(agentenv::cfg::LexicallyNormalizePath("/tmp//a/./b"), std::string("/tmp/a/b"));
    MT_EXPECT_EQ(agentenv::cfg::LexicallyNormalizePath("a/b/../c"), std::string("a/c"));
}

// ---------------------------------------------------------------------------
// Rust: cfg/network.rs `validate_accepts_custom_network_config`
// ---------------------------------------------------------------------------

MT_TEST(validate_accepts_custom_network_config) {
    NetworkConfig config;
    config.egress.always_denied_cidrs.clear();
    config.egress.always_denied_cidrs.push_back("127.0.0.0/8");
    config.egress.always_denied_cidrs.push_back("169.254.0.0/16");
    config.internal.host_interaction_cidr = "100.64.0.0/16";
    config.internal.veth_cidr = "100.65.0.0/16";

    MT_EXPECT_TRUE(NetworkConfig::Validate(config).has_value());
}

MT_TEST(network_validate_rejects_too_small_and_overlapping_cidrs) {
    {
        // /20 holds 4096 addresses, far below NETWORK_MAX_SLOTS (32768).
        NetworkConfig config;
        config.internal.host_interaction_cidr = "10.11.0.0/20";
        agentenv::core::Expected<agentenv::core::Unit, std::string> result =
            NetworkConfig::Validate(config);
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "must contain at least"));
    }
    {
        // veth needs 2x the slot count, so /16 is fine but /17 is not.
        NetworkConfig config;
        config.internal.veth_cidr = "10.12.0.0/17";
        agentenv::core::Expected<agentenv::core::Unit, std::string> result =
            NetworkConfig::Validate(config);
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "two veth addresses per slot"));
    }
    {
        NetworkConfig config;
        config.internal.host_interaction_cidr = "10.11.0.0/16";
        config.internal.veth_cidr = "10.11.0.0/16";
        agentenv::core::Expected<agentenv::core::Unit, std::string> result =
            NetworkConfig::Validate(config);
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "must not overlap"));
    }
    {
        // Overlapping the fixed 169.254.0.20/30 VM link range.
        NetworkConfig config;
        config.internal.host_interaction_cidr = "169.254.0.0/16";
        agentenv::core::Expected<agentenv::core::Unit, std::string> result =
            NetworkConfig::Validate(config);
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(Contains(result.error(), "must not overlap"));
    }
    {
        NetworkConfig config;
        config.egress.always_denied_cidrs.push_back("not-a-cidr");
        agentenv::core::Expected<agentenv::core::Unit, std::string> result =
            NetworkConfig::Validate(config);
        MT_EXPECT_TRUE(!result.has_value());
        MT_EXPECT_TRUE(
            Contains(result.error(), "invalid network.egress.always_denied_cidrs entry"));
    }
}

MT_TEST(normalize_dns_name_matches_rust_rules) {
    agentenv::core::Optional<std::string> ok =
        agentenv::cfg::NormalizeDnsName("Sandbox.Example.Invalid");
    MT_EXPECT_TRUE(ok.has_value());
    MT_EXPECT_EQ(*ok, std::string("sandbox.example.invalid"));

    // Hyphens are allowed only in the interior of a label.
    MT_EXPECT_TRUE(agentenv::cfg::NormalizeDnsName("a-b.example").has_value());
    MT_EXPECT_TRUE(!agentenv::cfg::NormalizeDnsName("-ab.example").has_value());
    MT_EXPECT_TRUE(!agentenv::cfg::NormalizeDnsName("ab-.example").has_value());
    MT_EXPECT_TRUE(!agentenv::cfg::NormalizeDnsName("not a domain").has_value());
    MT_EXPECT_TRUE(!agentenv::cfg::NormalizeDnsName("").has_value());
    // A trailing dot yields an empty final label, which is invalid.
    MT_EXPECT_TRUE(!agentenv::cfg::NormalizeDnsName("example.").has_value());
    // Underscores are not DNS-name characters here.
    MT_EXPECT_TRUE(!agentenv::cfg::NormalizeDnsName("a_b.example").has_value());
}

// ---------------------------------------------------------------------------
// VirtualizationMode (Rust src/virtualization.rs tests)
// ---------------------------------------------------------------------------

MT_TEST(defaults_to_kvm_and_parses_supported_values) {
    MT_EXPECT_TRUE(agentenv::core::VirtualizationModeDefault() == VirtualizationMode::Kvm);

    agentenv::core::Expected<VirtualizationMode, std::string> upper =
        agentenv::core::VirtualizationModeParse("KVM");
    MT_EXPECT_TRUE(upper.has_value());
    MT_EXPECT_TRUE(upper.value() == VirtualizationMode::Kvm);

    agentenv::core::Expected<VirtualizationMode, std::string> pvm =
        agentenv::core::VirtualizationModeParse(" pvm ");
    MT_EXPECT_TRUE(pvm.has_value());
    MT_EXPECT_TRUE(pvm.value() == VirtualizationMode::Pvm);

    MT_EXPECT_TRUE(!agentenv::core::VirtualizationModeParse("nested").has_value());
    MT_EXPECT_EQ(std::string(agentenv::core::VirtualizationModeToString(VirtualizationMode::Pvm)),
                 std::string("pvm"));
}

// ---------------------------------------------------------------------------
// ConfigManager / TOML loading
// Rust: `bundled_default_config_loads`, `pvm_config_disables_dirty_page_tracking`
// ---------------------------------------------------------------------------

MT_TEST(pvm_config_disables_dirty_page_tracking) {
    TempDir temp;
    const std::string path = PathJoin(temp.path(), "pvm.toml");
    MT_EXPECT_TRUE(WriteFile(path, "virtualization_mode = \"pvm\"\n"));

    agentenv::core::Expected<ConfigManager, std::string> manager =
        ConfigManager::NewFromPath(path);
    MT_EXPECT_TRUE(manager.has_value());
    MT_EXPECT_TRUE(manager.value().config().virtualization_mode == VirtualizationMode::Pvm);
    // Rust: PVM forces dirty-page tracking off during normalize.
    MT_EXPECT_TRUE(!manager.value().config().memory_snapshot.track_dirty_pages);
}

MT_TEST(config_manager_loads_nested_tables_arrays_and_floats) {
    TempDir temp;
    const std::string path = PathJoin(temp.path(), "full.toml");
    const std::string toml =
        "# a comment\n"
        "home_path = \"/srv/aenv\"\n"
        "virtualization_mode = \"kvm\"\n"
        "\n"
        "[machine]\n"
        "vcpu_count = 8\n"
        "mem_size_mib = 4_096\n"
        "\n"
        "[machine.disk_rate_limit]\n"
        "enabled = true\n"
        "bandwidth_bytes_per_sec = 104857600\n"
        "iops = 3000\n"
        "\n"
        "[pool]\n"
        "low_watermark = 4\n"
        "high_watermark = 128\n"
        "\n"
        "[image.resolver]\n"
        "default_image = \"debian:12\"\n"
        "search_registries = [\"docker.io\", \"quay.io\"]\n"
        "\n"
        "[image.cache.gc]\n"
        "high_watermark_ratio = 0.9\n"
        "low_watermark_ratio = 0.5\n"
        "\n"
        "[sandbox_proxy]\n"
        "domains = [\n"
        "  \"a.example.invalid\",\n"
        "  \"b.example.invalid\",\n"
        "]\n"
        "\n"
        "[snapshot]\n"
        "repository_backend = \"posix_fs\"\n"
        "\n"
        "[ublk.overlaybd]\n"
        "runtime_upper_mode = \"sparse\"\n";
    MT_EXPECT_TRUE(WriteFile(path, toml));

    agentenv::core::Expected<ConfigManager, std::string> manager =
        ConfigManager::NewFromPath(path);
    if (!manager.has_value()) {
        std::fprintf(stderr, "  load error: %s\n", manager.error().c_str());
    }
    MT_EXPECT_TRUE(manager.has_value());

    const AppConfig& config = manager.value().config();
    MT_EXPECT_EQ(config.home_path, std::string("/srv/aenv"));
    MT_EXPECT_EQ(config.machine.vcpu_count, 8u);
    // `4_096` exercises the TOML digit separator.
    MT_EXPECT_EQ(config.machine.mem_size_mib, 4096u);
    MT_EXPECT_TRUE(config.machine.disk_rate_limit.enabled);
    MT_EXPECT_EQ(config.machine.disk_rate_limit.bandwidth_bytes_per_sec, 104857600u);
    MT_EXPECT_EQ(config.pool.low_watermark, 4u);
    MT_EXPECT_EQ(config.pool.high_watermark, 128u);
    MT_EXPECT_EQ(config.image.resolver.default_image, std::string("debian:12"));
    MT_EXPECT_EQ(config.image.resolver.search_registries.size(), 2u);
    MT_EXPECT_EQ(config.image.resolver.search_registries[1], std::string("quay.io"));
    MT_EXPECT_TRUE(config.image.cache.gc.high_watermark_ratio > 0.89);
    MT_EXPECT_TRUE(config.image.cache.gc.high_watermark_ratio < 0.91);
    MT_EXPECT_EQ(config.sandbox_proxy.domains.size(), 2u);
    MT_EXPECT_EQ(config.sandbox_proxy.domains[0], std::string("a.example.invalid"));
    MT_EXPECT_TRUE(config.ublk.overlaybd.runtime_upper_mode ==
                   agentenv::storage::overlaybd::UpperMode::Sparse);
    // `[snapshot] repository_backend = "posix_fs"` makes normalize materialize
    // the posix_fs backend and resolve its store path against home_path.
    MT_EXPECT_TRUE(config.backend.posix_fs.has_value());
    MT_EXPECT_EQ(config.backend.posix_fs->snapshot_store,
                 std::string("/srv/aenv/snapshot-store"));
}

MT_TEST(config_manager_reports_validation_and_parse_errors) {
    {
        TempDir temp;
        const std::string path = PathJoin(temp.path(), "bad-value.toml");
        MT_EXPECT_TRUE(WriteFile(path, "[volume]\nmax_volume_count = 0\n"));
        agentenv::core::Expected<ConfigManager, std::string> manager =
            ConfigManager::NewFromPath(path);
        MT_EXPECT_TRUE(!manager.has_value());
        MT_EXPECT_TRUE(Contains(manager.error(), "volume.max_volume_count"));
    }
    {
        TempDir temp;
        const std::string path = PathJoin(temp.path(), "bad-syntax.toml");
        MT_EXPECT_TRUE(WriteFile(path, "home_path\n"));
        agentenv::core::Expected<ConfigManager, std::string> manager =
            ConfigManager::NewFromPath(path);
        MT_EXPECT_TRUE(!manager.has_value());
        MT_EXPECT_TRUE(Contains(manager.error(), "expected `key = value`"));
    }
    {
        TempDir temp;
        const std::string path = PathJoin(temp.path(), "bad-enum.toml");
        MT_EXPECT_TRUE(WriteFile(path, "virtualization_mode = \"nested\"\n"));
        agentenv::core::Expected<ConfigManager, std::string> manager =
            ConfigManager::NewFromPath(path);
        MT_EXPECT_TRUE(!manager.has_value());
        MT_EXPECT_TRUE(Contains(manager.error(), "unsupported virtualization mode"));
    }
    {
        agentenv::core::Expected<ConfigManager, std::string> manager =
            ConfigManager::NewFromPath("/nonexistent/agentenv/config.toml");
        MT_EXPECT_TRUE(!manager.has_value());
        MT_EXPECT_TRUE(Contains(manager.error(), "cannot open config file"));
    }
}

MT_TEST(config_manager_global_is_idempotent) {
    ConfigManager::ResetGlobalForTesting();
    MT_EXPECT_TRUE(ConfigManager::Global() == nullptr);
    MT_EXPECT_TRUE(ConfigManager::GlobalConfig() == nullptr);

    TempDir temp;
    const std::string path = PathJoin(temp.path(), "global.toml");
    MT_EXPECT_TRUE(WriteFile(path, "home_path = \"/srv/aenv-global\"\n"));

    agentenv::core::Expected<const ConfigManager*, std::string> first =
        ConfigManager::InitGlobalFromPath(path);
    MT_EXPECT_TRUE(first.has_value());
    MT_EXPECT_EQ(first.value()->config().home_path, std::string("/srv/aenv-global"));

    // A second init must return the already-installed manager, not reload.
    const std::string other = PathJoin(temp.path(), "other.toml");
    MT_EXPECT_TRUE(WriteFile(other, "home_path = \"/srv/aenv-other\"\n"));
    agentenv::core::Expected<const ConfigManager*, std::string> second =
        ConfigManager::InitGlobalFromPath(other);
    MT_EXPECT_TRUE(second.has_value());
    MT_EXPECT_EQ(second.value()->config().home_path, std::string("/srv/aenv-global"));
    MT_EXPECT_TRUE(ConfigManager::GlobalConfig() != nullptr);

    ConfigManager::ResetGlobalForTesting();
}

// ---------------------------------------------------------------------------
// Pool resolution (Rust `network_pool_config` / `block_pool_config` /
// `firecracker_pool_config`)
// ---------------------------------------------------------------------------

MT_TEST(pool_configs_resolve_from_shared_watermarks) {
    AppConfig config;
    config.pool.low_watermark = 3;
    config.pool.high_watermark = 9;

    agentenv::warmpool::PoolConfig network = config.NetworkPoolConfig();
    MT_EXPECT_EQ(network.low_watermark, 3u);
    MT_EXPECT_EQ(network.high_watermark, 9u);
    MT_EXPECT_TRUE(network.maintenance_enabled);

    // Disabling the component clears maintenance even when the flag is on.
    config.pool.network.enabled = false;
    MT_EXPECT_TRUE(!config.NetworkPoolConfig().maintenance_enabled);

    // Block pools always disable maintenance (async request-time refill).
    agentenv::core::Optional<agentenv::warmpool::PoolConfig> block = config.BlockPoolConfig();
    MT_EXPECT_TRUE(block.has_value());
    MT_EXPECT_TRUE(!block->maintenance_enabled);
    config.pool.block.enabled = false;
    MT_EXPECT_TRUE(!config.BlockPoolConfig().has_value());

    agentenv::core::Optional<agentenv::cfg::ResolvedFirecrackerPoolConfig> fc =
        config.FirecrackerPoolConfig();
    MT_EXPECT_TRUE(fc.has_value());
    MT_EXPECT_EQ(fc->fill_concurrency, 4u);
    MT_EXPECT_EQ(fc->pool.low_watermark, 3u);
    config.pool.firecracker.enabled = false;
    MT_EXPECT_TRUE(!config.FirecrackerPoolConfig().has_value());
}

MT_TEST(resolved_overlaybd_paths_are_siblings_of_the_runtime_config) {
    AppConfig config;
    config.ublk.overlaybd.global_config_path = "/srv/aenv/overlaybd/overlaybd-global.json";
    MT_EXPECT_EQ(config.ResolvedOverlaybdConvertGlobalConfigPath(),
                 std::string("/srv/aenv/overlaybd/convert-overlaybd-global.json"));
    MT_EXPECT_EQ(config.ResolvedOverlaybdResizeGlobalConfigPath(),
                 std::string("/srv/aenv/overlaybd/resize-overlaybd-global.json"));
    MT_EXPECT_EQ(config.ResolvedRegctlBinary(),
                 agentenv::cfg::RegctlPath(config.deps_path));
}

MT_MAIN
