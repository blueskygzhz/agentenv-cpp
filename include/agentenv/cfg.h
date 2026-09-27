// SPDX-License-Identifier: MIT
// Rust: src/cfg.rs — the top-level `AppConfig`, its ~28 nested config structs,
// `normalize`/`validate`, and the `ConfigManager` global.
//
// Porting notes
// -------------
// * Rust `PathBuf` -> `std::string`. C++11 has no `<filesystem>`; every path
//   operation this module needs (join, parent, is_absolute, lexical normalize)
//   is implemented over strings in cfg.cc.
// * Rust `Option<T>` -> `core::Optional<T>`. Kept even where an empty string
//   would "work", because `src/cfg.rs` distinguishes unset from empty in
//   several places (`allowed_registries`, `overlaybd`, every dependency
//   override path).
// * `#[config(default = ...)]` -> non-static data member initializers, so a
//   default-constructed struct equals Rust's `impl_config_default!` output.
// * `#[config(env = "...")]` -> `ApplyEnvOverrides`, called from the loader
//   before `Normalize`, matching confique's `.env().file(path)` layering where
//   the environment wins.
// * Rust `Result<()>` -> `core::Expected<core::Unit, std::string>`; error
//   strings are copied verbatim from the upstream `bail!`/`anyhow!` messages so
//   operator-facing diagnostics stay identical.
#ifndef AGENTENV_CFG_H_
#define AGENTENV_CFG_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/cfg/image.h"
#include "agentenv/cfg/network.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/toml.h"
#include "agentenv/core/virtualization.h"
#include "agentenv/p2p/all.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/warm-pool/pool.h"

namespace agentenv {
namespace cfg {

/// Rust `ENV_CONFIG_PATH`.
extern const char* const kEnvConfigPath;  // "AENV_CONFIG_PATH"
/// Rust `TEST_ACCESS_TOKEN_HASH_SEED` (`#[cfg(test)]`).
extern const char* const kTestAccessTokenHashSeed;
/// Rust `HOME_PATH_PLACEHOLDER` / `RUNTIME_PATH_PLACEHOLDER`.
extern const char* const kHomePathPlaceholder;     // "$AENV_HOME"
extern const char* const kRuntimePathPlaceholder;  // "$AENV_RUNTIME"
/// Rust `crate::volume::MAX_VOLUME_MOUNTS`. Declared here (rather than pulled
/// from a volume header) because the volume module is not ported yet; keep the
/// two in sync when it lands.
const std::size_t kMaxVolumeMounts = 24;

// ---------------------------------------------------------------------------
// Bundled setup dependency manifest (Rust: config/deps_manifest.toml, embedded
// via `include_str!` and parsed once into a `LazyLock`).
// ---------------------------------------------------------------------------

/// Rust struct `ManifestDownload`.
struct ManifestDownload {
    std::string version;
};

/// Rust struct `ManifestVirtualizationDownloads`.
struct ManifestVirtualizationDownloads {
    ManifestDownload kvm;
    ManifestDownload pvm;

    /// Rust `ManifestVirtualizationDownloads::for_mode`.
    const ManifestDownload& ForMode(core::VirtualizationMode mode) const;
};

/// Rust struct `SetupDependencyManifest`.
struct SetupDependencyManifest {
    ManifestVirtualizationDownloads firecracker;
    ManifestVirtualizationDownloads kernel;
    ManifestDownload tools;
    ManifestDownload overlaybd;
    ManifestDownload regclient;

    /// Rust `SetupDependencyManifest::get` — process-wide, lazily built.
    static const SetupDependencyManifest& Get();
};

/// Rust `cfg::regctl_path`.
std::string RegctlPath(const std::string& deps_path);

// ---------------------------------------------------------------------------
// Nested config structs, in `src/cfg.rs` declaration order.
// ---------------------------------------------------------------------------

/// Rust struct `PosixFsBackendConfig`.
struct PosixFsBackendConfig {
    std::string snapshot_store = "$AENV_HOME/snapshot-store";
};

/// Rust enum `OssAddressingStyle`.
enum class OssAddressingStyle { Path, Virtual };

/// Rust struct `OssBackendConfig`. No `#[config]` defaults upstream: it is
/// plain `Deserialize`, so every field is operator-provided.
struct OssBackendConfig {
    std::string endpoint;
    std::string bucket;
    core::Optional<std::string> prefix;
    /// Rust serde aliases: `credentialProcess`, `credential-process`.
    core::Optional<std::string> credential_process;
    core::Optional<std::string> access_key_id;
    core::Optional<std::string> access_key_secret;
    core::Optional<std::string> security_token;
    core::Optional<std::string> region;
    /// Rust serde aliases: `addressingStyle`, `addressing-style`.
    core::Optional<OssAddressingStyle> addressing_style;
    core::Optional<uint64_t> cache_max_size_gb;
};

/// Rust struct `BackendConfig`.
struct BackendConfig {
    core::Optional<PosixFsBackendConfig> posix_fs;
    core::Optional<OssBackendConfig> oss;
};

/// Rust struct `FirecrackerConfig`.
struct FirecrackerConfig {
    core::Optional<std::string> binary_path;
    core::Optional<std::string> boot_args;
    core::Optional<std::vector<std::string> > allowed_extra_boot_args_prefixes;
    uint64_t socket_timeout_secs = 3;
    uint64_t socket_poll_ms = 1;
    core::Optional<std::string> version;
    core::Optional<std::string> url;
    /// Defaults to `$AENV_HOME/firecracker-work` after normalization.
    core::Optional<std::string> work_dir;
    /// Defaults to `$AENV_HOME/logs/serial` after normalization.
    core::Optional<std::string> serial_dir;
    /// When set (non-empty), enables stdout/stderr capture plus Firecracker
    /// logging to `firecracker.log`. Unset/empty disables all three.
    core::Optional<std::string> log_level;
};

/// Rust struct `PoolComponentConfig`.
struct PoolComponentConfig {
    bool enabled = true;
    bool maintenance_enabled = true;
    bool startup_prewarm = true;
};

/// Rust struct `FirecrackerProcessPoolConfig`.
struct FirecrackerProcessPoolConfig {
    bool enabled = true;
    bool maintenance_enabled = true;
    bool startup_prewarm = true;
    std::size_t fill_concurrency = 4;
};

/// Rust struct `ResolvedFirecrackerPoolConfig`.
struct ResolvedFirecrackerPoolConfig {
    warmpool::PoolConfig pool;
    std::size_t fill_concurrency = 0;
};

/// Rust struct `PoolTomlConfig`.
struct PoolTomlConfig {
    std::size_t low_watermark = 2;
    std::size_t high_watermark = 64;
    PoolComponentConfig network;
    PoolComponentConfig block;
    FirecrackerProcessPoolConfig firecracker;

    /// Rust `PoolTomlConfig::validate`.
    static core::Expected<core::Unit, std::string> Validate(const std::string& name,
                                                            const warmpool::PoolConfig& pool);
};

/// Rust struct `KernelConfig`.
struct KernelConfig {
    core::Optional<std::string> image_path;
    core::Optional<std::string> version;
    core::Optional<std::string> url;
};

/// Rust struct `OverlaybdDependencyConfig`.
struct OverlaybdDependencyConfig {
    std::string version;
    core::Optional<std::string> url;
    core::Optional<std::string> package_url;
};

/// Rust struct `ToolsConfig`.
struct ToolsConfig {
    core::Optional<std::string> drive_path;
    core::Optional<std::string> version;
    core::Optional<std::string> url;
    uint16_t control_plane_port = 49983;
};

/// Rust struct `SandboxProxyConfig`.
struct SandboxProxyConfig {
    std::vector<std::string> domains;  // default []

    /// Rust `SandboxProxyConfig::normalize` — trims, drops the trailing dot,
    /// validates as a DNS name, de-duplicates, and preserves operator order
    /// (`domains[0]` is the advertised sandbox response domain).
    core::Expected<core::Unit, std::string> Normalize();
};

/// Rust struct `EnvdConfig`.
struct EnvdConfig {
    std::string version = "0.5.15";
    uint64_t init_timeout_secs = 60;
    uint64_t poll_ms = 3;
};

/// Rust struct `SandboxConfig`. Rust hand-writes `Debug` to redact the seed;
/// `DebugString()` does the same here.
struct SandboxConfig {
    core::Optional<std::string> access_token_hash_seed;

    std::string DebugString() const;
};

/// Rust struct `VolumeConfig`.
struct VolumeConfig {
    uint64_t max_size_mb = 262144;
    std::size_t max_volume_count = 4;
};

/// Rust struct `DiskRateLimitConfig`.
struct DiskRateLimitConfig {
    bool enabled = false;
    uint64_t bandwidth_bytes_per_sec = 0;
    /// One-time burst granted once at VM start (Firecracker `one_time_burst`).
    /// Consumed before the sustained bucket and never replenished, so it only
    /// absorbs the initial spike and does not raise the steady-state rate.
    uint64_t bandwidth_burst_bytes = 0;
    uint64_t iops = 0;
    uint64_t iops_burst = 0;
};

/// Rust struct `MachineConfig`.
struct MachineConfig {
    uint32_t vcpu_count = 2;
    uint32_t mem_size_mib = 1024;
    DiskRateLimitConfig disk_rate_limit;
};

/// Rust enum `SnapshotRepositoryBackendKind`.
enum class SnapshotRepositoryBackendKind { PosixFs, Oss };

/// Rust enum `SnapshotImageStoragePolicy`.
enum class SnapshotImageStoragePolicy { ObjectStorage, SourceRegistry };

/// Rust enum `OverlaybdCompressionAlgorithm`.
enum class OverlaybdCompressionAlgorithm { Lz4, Zstd };

/// Rust struct `SnapshotImagePublishConfig`.
struct SnapshotImagePublishConfig {
    bool enabled = false;
};

/// Rust struct `SnapshotPublishCompressionConfig`.
struct SnapshotPublishCompressionConfig {
    bool enabled = true;
    OverlaybdCompressionAlgorithm algorithm = OverlaybdCompressionAlgorithm::Lz4;
    /// Blocking threads used to compress 4KiB blocks within a layer.
    /// 1 = sequential (output layout is identical at any value).
    std::size_t workers = 1;
};

/// Rust struct `SnapshotStartupPackConfig`.
struct SnapshotStartupPackConfig {
    bool enabled = false;
    uint64_t record_min_window_ms = 200;
    uint64_t record_quiet_ms = 300;
    uint64_t record_max_window_ms = 2000;
    uint64_t record_budget_secs = 10;
    uint64_t max_pack_bytes = 1073741824ULL;
    bool consume_enabled = false;
    uint64_t consume_timeout_secs = 30;
};

/// Rust struct `SnapshotConfig`.
struct SnapshotConfig {
    std::string local_cache_path = "$AENV_HOME/snapshot-local-cache";
    SnapshotRepositoryBackendKind repository_backend = SnapshotRepositoryBackendKind::PosixFs;
    /// No effect unless `[p2p].enabled` is also true.
    bool p2p_enabled = true;
    SnapshotImagePublishConfig image_publish;
    SnapshotPublishCompressionConfig publish_compression;
    SnapshotStartupPackConfig memory_startup_pack;
};

/// Rust struct `UblkOverlaybdTomlConfig`.
struct UblkOverlaybdTomlConfig {
    std::string global_config_path = "$AENV_HOME/overlaybd/overlaybd-global.json";
    bool read_only = false;
    /// Runtime upper format for newly materialized writable OverlayBD images.
    /// Existing source uppers keep their own mode.
    storage::overlaybd::UpperMode runtime_upper_mode = storage::overlaybd::UpperMode::HybridLogStructured;
    bool allow_shrink = false;
    uint64_t resize_timeout_secs = 120;
    /// Written as `remoteIoWorkers` into generated overlaybd global configs.
    uint64_t remote_io_workers = 4;
    bool download_enable = false;
    uint64_t p2p_lookup_timeout_ms = 300;
    uint64_t p2p_fetch_range_timeout_ms = 2000;
};

/// Rust struct `UblkTomlConfig`.
struct UblkTomlConfig {
    core::Optional<std::string> daemon_binary_path;
    std::string daemon_socket_path = "$AENV_RUNTIME/ublk-daemon.sock";
    /// Defaults to `$AENV_HOME/logs/ublk-daemon.log` after normalization.
    core::Optional<std::string> daemon_log_path;
    /// HTTP listen address for ublk daemon metrics. Empty string disables it.
    std::string daemon_metrics_listen_addr = "0.0.0.0:9103";
    UblkOverlaybdTomlConfig overlaybd;
};

/// Rust struct `MemorySnapshotBackgroundDownloadConfig`.
struct MemorySnapshotBackgroundDownloadConfig {
    bool enable = true;
    int32_t delay = 0;
    int32_t delay_extra = 1;
    int32_t try_cnt = 5;
    uint32_t block_size = 16777216;
    std::size_t concurrency = 4;
    /// Node-wide cap on in-flight background download blocks, bounding total
    /// scratch memory. Fixed when the backend is created.
    std::size_t max_inflight_blocks = 16;
};

/// Rust struct `MemorySnapshotConfig`.
struct MemorySnapshotConfig {
    std::string overlaybd_global_config_path = "$AENV_HOME/overlaybd/mem-overlaybd-global.json";
    /// Firecracker KVM dirty-page tracking. KVM-only; `Normalize` forces it off
    /// under PVM so an existing PVM config needs no new override.
    bool track_dirty_pages = true;
    MemorySnapshotBackgroundDownloadConfig background_download;
};

/// Rust struct `TemplateBuildConfig`. The Rust default for `builder_image`
/// comes from `include_str!("../config/buildkit-version")`.
struct TemplateBuildConfig {
    std::size_t max_concurrent_builds = 4;
    std::string builder_image = "docker.io/moby/buildkit:v0.33.0";
    uint32_t builder_cpu_count = 16;
    uint32_t builder_memory_mb = 32768;
    uint64_t cache_size_mb = 65536;
};

/// Rust struct `ObservabilitySchedulerReportConfig`.
struct ObservabilitySchedulerReportConfig {
    bool enabled = false;
    uint64_t interval_secs = 5;
};

/// Rust struct `ObservabilityConfig`.
struct ObservabilityConfig {
    bool enabled = true;
    ObservabilitySchedulerReportConfig scheduler_report;
};

/// Rust struct `ClusterConfig`.
struct ClusterConfig {
    core::Optional<std::string> scheduler_endpoint;

    /// Rust `ClusterConfig::normalize` — trims and drops a blank endpoint.
    void Normalize();
};

/// Rust struct `NodeIdentityConfig`.
struct NodeIdentityConfig {
    core::Optional<std::string> node_id;
    core::Optional<std::string> cluster_id;
    core::Optional<std::string> service_instance_id;
};

/// Rust struct `OrchestratorConfig`.
struct OrchestratorConfig {
    uint64_t metrics_interval_secs = 15;
    uint64_t metrics_retention_secs = 3600;
    uint64_t auto_evict_interval_ms = 1000;
    uint64_t default_sandbox_timeout_secs = 15;
    uint64_t auto_resume_min_sandbox_timeout_secs = 300;
    std::string persisted_sandbox_store_path = "$AENV_HOME/persisted-sandboxes";
};

/// Rust struct `CustomExtensionConfig`.
///
/// The custom extension is an external HTTP service extending node behavior.
/// Its only current capability is sandbox lifecycle hooks (`/sandbox-hook/*`).
struct CustomExtensionConfig {
    /// Unset fully disables the custom extension integration.
    core::Optional<std::string> url;
    uint64_t timeout_ms = 5000;
};

/// Rust struct `P2pConfig`.
struct P2pConfig {
    bool enabled = false;
    p2p::P2pTransportKind transport = p2p::P2pTransportKind::Iroh;
    std::string store_dir = "$AENV_HOME/p2p/store";
    std::string listen_addr = "0.0.0.0:0";
    uint64_t lookup_timeout_ms = 5000;
    uint64_t fetch_timeout_ms = 30000;
    uint64_t peer_discovery_refresh_interval_secs = 5;
};

// ---------------------------------------------------------------------------
// AppConfig
// ---------------------------------------------------------------------------

/// Rust struct `AppConfig`.
struct AppConfig {
    std::string home_path = "/var/lib/aenv";
    std::string runtime_path = "/run/aenv";
    std::string deps_path = "$AENV_HOME/deps";
    core::VirtualizationMode virtualization_mode = core::VirtualizationMode::Kvm;
    FirecrackerConfig firecracker;
    KernelConfig kernel;
    ToolsConfig tools;
    core::Optional<OverlaybdDependencyConfig> overlaybd;
    MachineConfig machine;
    BackendConfig backend;
    EnvdConfig envd;
    SandboxConfig sandbox;
    VolumeConfig volume;
    OrchestratorConfig orchestrator;
    SnapshotConfig snapshot;
    UblkTomlConfig ublk;
    ObservabilityConfig observability;
    ClusterConfig cluster;
    NodeIdentityConfig node_identity;
    TemplateBuildConfig template_build;
    MemorySnapshotConfig memory_snapshot;
    PoolTomlConfig pool;
    P2pConfig p2p;
    ImageConfig image;
    SandboxProxyConfig sandbox_proxy;
    NetworkConfig network;
    CustomExtensionConfig custom_extension;

    // --- resolved accessors (Rust `resolved_*`) ---------------------------

    /// Rust `AppConfig::resolved_firecracker_binary_path`.
    std::string ResolvedFirecrackerBinaryPath() const;
    /// Rust `AppConfig::resolved_kernel_image_path`.
    std::string ResolvedKernelImagePath() const;
    /// Rust `AppConfig::resolved_tools_version`.
    std::string ResolvedToolsVersion() const;
    /// Rust `AppConfig::resolved_tools_drive_path_for_version` — requires
    /// SemVer without build metadata, which also rejects `../escape`.
    core::Expected<std::string, std::string> ResolvedToolsDrivePathForVersion(
        const std::string& version) const;
    /// Rust `AppConfig::resolved_tools_drive_path`.
    core::Expected<std::string, std::string> ResolvedToolsDrivePath() const;
    /// Rust `AppConfig::resolved_overlaybd_oci_converter_id`.
    std::string ResolvedOverlaybdOciConverterId() const;
    /// Rust `AppConfig::resolved_overlaybd_convert_global_config_path`.
    std::string ResolvedOverlaybdConvertGlobalConfigPath() const;
    /// Rust `AppConfig::resolved_overlaybd_resize_global_config_path`.
    std::string ResolvedOverlaybdResizeGlobalConfigPath() const;
    /// Rust `AppConfig::resolved_cpu_template_helper` — unset when the binary
    /// is absent on disk.
    core::Optional<std::string> ResolvedCpuTemplateHelper() const;
    /// Rust `AppConfig::resolved_regctl_binary`.
    std::string ResolvedRegctlBinary() const;
    /// Rust `AppConfig::image_cache_layout`.
    ResolvedImageCacheConfig ImageCacheLayout() const;

    /// Rust `AppConfig::network_pool_config`.
    warmpool::PoolConfig NetworkPoolConfig() const;
    /// Rust `AppConfig::block_pool_config`.
    core::Optional<warmpool::PoolConfig> BlockPoolConfig() const;
    /// Rust `AppConfig::firecracker_pool_config`.
    core::Optional<ResolvedFirecrackerPoolConfig> FirecrackerPoolConfig() const;

    // --- lifecycle -------------------------------------------------------

    /// Rust `AppConfig::normalize`.
    core::Expected<core::Unit, std::string> Normalize(const std::string& config_dir);
    /// Rust `AppConfig::validate`.
    core::Expected<core::Unit, std::string> Validate() const;

    /// Rust confique `#[config(env = ...)]` layer. Applied before `Normalize`;
    /// the environment wins over file values.
    void ApplyEnvOverrides();
    /// Overlay a parsed TOML document onto `*this`.
    core::Expected<core::Unit, std::string> LoadFrom(const core::TomlTable& table);

    // Individual validators, exposed because the upstream tests call them
    // directly (`config.validate_template_builder()` etc.).
    core::Expected<core::Unit, std::string> ValidateTemplateBuilder() const;
    core::Expected<core::Unit, std::string> ValidateVolumeLimits() const;
    core::Expected<core::Unit, std::string> ValidateDiskRateLimit() const;
    core::Expected<core::Unit, std::string> ValidateMemorySnapshotOptions() const;
    core::Expected<core::Unit, std::string> ValidateMemorySnapshotBackgroundDownload() const;
    core::Expected<core::Unit, std::string> ValidateOverlaybdGlobalConfigPaths() const;
    core::Expected<core::Unit, std::string> ValidatePoolConfig() const;

 private:
    const ManifestDownload& ManifestFirecracker() const;
    const ManifestDownload& ManifestKernel() const;
    void NormalizeDependencyOverridePaths(const std::string& config_dir);
};

// ---------------------------------------------------------------------------
// ConfigManager
// ---------------------------------------------------------------------------

/// Rust struct `ConfigManager` plus its `OnceLock<ConfigManager>` global.
class ConfigManager {
 public:
    /// Rust `ConfigManager::new`.
    static core::Expected<ConfigManager, std::string> New();
    /// Rust `ConfigManager::new_from_path`.
    static core::Expected<ConfigManager, std::string> NewFromPath(const std::string& path);

    /// Rust `ConfigManager::init_global`. Idempotent: a second call returns the
    /// already-installed manager.
    static core::Expected<const ConfigManager*, std::string> InitGlobal();
    /// Rust `ConfigManager::init_global_from_path`.
    static core::Expected<const ConfigManager*, std::string> InitGlobalFromPath(
        const std::string& path);
    /// Rust `ConfigManager::global` — panics upstream when uninitialized; here
    /// it returns nullptr so callers can decide.
    static const ConfigManager* Global();
    /// Rust `ConfigManager::global_config`.
    static const AppConfig* GlobalConfig();
    /// Test-only: drop the global so a test can install a different config.
    /// Rust relies on `#[cfg(test)]` lazy init instead.
    static void ResetGlobalForTesting();

    /// Rust `ConfigManager::config`.
    const AppConfig& config() const { return config_; }
    /// Rust `ConfigManager::config_path`.
    const core::Optional<std::string>& config_path() const { return config_path_; }

 private:
    ConfigManager() {}
    /// Rust `ConfigManager::load_config_file` — env + file layering, then
    /// `normalize(config_dir)` then `validate()`.
    static core::Expected<AppConfig, std::string> LoadConfigFile(const std::string& path);
    /// Rust `ConfigManager::default_config_path`.
    static std::string DefaultConfigPath();
    /// Rust `ConfigManager::env_path` — trims, treats empty as unset.
    static core::Optional<std::string> EnvPath(const char* name);

    AppConfig config_;
    core::Optional<std::string> config_path_;
};

// ---------------------------------------------------------------------------
// Path helpers (Rust: free functions at the bottom of src/cfg.rs)
// ---------------------------------------------------------------------------

/// Rust `resolve_relative_to`.
std::string ResolveRelativeTo(const std::string& base_dir, const std::string& path);
/// Rust `resolve_path` — expands `$AENV_HOME`, then resolves against config_dir.
std::string ResolvePath(const std::string& home_path, const std::string& config_dir,
                        const std::string& raw);
/// Rust `resolve_runtime_path` — also expands `$AENV_RUNTIME`.
std::string ResolveRuntimePath(const std::string& home_path, const std::string& runtime_path,
                               const std::string& config_dir, const std::string& raw);
/// Rust `resolve_optional_config_path_or`.
void ResolveOptionalConfigPathOr(core::Optional<std::string>* path, const std::string& home_path,
                                 const std::string& config_dir,
                                 const std::string& default_relative_path);

/// Drop `.` segments and repeated separators, mirroring how Rust compares
/// `Path`s via `components()`. `..` is preserved (Rust yields it as a real
/// `ParentDir` component); resolving `..` is `LexicallyNormalizePath`'s job.
std::string CollapseCurDirComponents(const std::string& path);

/// Textual `Path::join`.
std::string PathJoin(const std::string& base, const std::string& leaf);
/// Textual `Path::parent`, falling back to "." like the Rust call sites do.
std::string PathParent(const std::string& path);
/// Textual `Path::is_absolute`.
bool PathIsAbsolute(const std::string& path);

}  // namespace cfg
}  // namespace agentenv
#endif  // AGENTENV_CFG_H_
