// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/config.rs — LayerConfig / UpperConfig / OverlaybdConfig.
#ifndef AGENTENV_STORAGE_OVERLAYBD_CONFIG_H_
#define AGENTENV_STORAGE_OVERLAYBD_CONFIG_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `MAX_LAYER_CNT`.
static const size_t kMaxLayerCnt = 256;

/// Rust struct `LayerConfig`.
struct LayerConfig {
    std::string gzip_index;
    std::string file;
    std::string target_file;
    std::string dir;
    std::string repo_blob_url;
    std::string digest;
    std::string uuid;
    std::string target_digest;
    uint64_t    size = 0;

    /// Rust `effective_repo_blob_url`.
  const std::string& EffectiveRepoBlobUrl(const std::string& image_repo_blob_url) const {
     return repo_blob_url.empty() ? image_repo_blob_url : repo_blob_url;
    }
};

/// Rust enum `UpperMode`.
enum class UpperMode {
    Sparse,
    LogStructured,
    HybridLogStructured,
};

/// Rust struct `UpperConfig`.
struct UpperConfig {
    bool     has_mode = false;
    UpperMode  mode = UpperMode::LogStructured;
    std::string index;
    std::string data;
    std::string target;
    std::string gzip_index;

    /// Rust `writable_mode`.
    UpperMode WritableMode() const {
        return has_mode ? mode : UpperMode::LogStructured;
}
};

/// Rust `validate_upper_config`.
core::Expected<bool, std::string> ValidateUpperConfig(const UpperConfig& upper);

/// Rust struct `DownloadConfig` — background prefetch policy, written into the
/// generated overlaybd global configs and (as `download`) into a tools image.
///
/// The defaults below are Rust's `impl Default`, which is *not* all-zero:
/// a zero-initialised struct would silently disable throttling and retries.
struct DownloadConfig {
    bool enable = false;
    int32_t delay = 300;
    int32_t delay_extra = 30;
    /// Rust `#[serde(rename = "maxMBps")]` — per-layer rate limit in MiB/s;
    /// 0 disables throttling.
    int32_t max_mbps = 100;
    int32_t try_cnt = 5;
    /// Background chunk size in bytes. Larger than the cache block size on
    /// purpose: one source request covers many cache blocks, so background
    /// throughput is high while foreground reads stay fine-grained.
    uint32_t block_size = 16u * 1024u * 1024u;
    std::size_t concurrency = 1;
    /// Only the global value takes effect; a per-image override is kept for
    /// serialization compatibility but never resizes the scheduler.
    std::size_t max_inflight_blocks = 16;
    std::size_t max_concurrent_files = 8;

    bool operator==(const DownloadConfig& o) const;
    bool operator!=(const DownloadConfig& o) const { return !(*this == o); }
};

/// Serializes to the camelCase shape serde produces, including the historical
/// `maxMBps` spelling. Returns a `Json` value rather than text so callers can
/// embed it in the larger global config they are building.
core::Json DownloadConfigToJson(const DownloadConfig& config);

/// Parses the same shape back. Missing keys keep their default, matching
/// Rust's `#[serde(default)]`.
core::Expected<DownloadConfig, std::string> ParseDownloadConfig(const core::Json& value);

/// Serde spelling of `UpperMode` (`#[serde(rename_all = "camelCase")]`).
const char* UpperModeToString(UpperMode mode);
core::Expected<UpperMode, std::string> UpperModeParse(const std::string& raw);

/// Rust struct `ImageConfig` — the per-image `image.json` that overlaybd reads.
///
/// Declared ahead of `ImageConfig` so `EffectiveDownload` can name it; the
/// definition follows below.
struct GlobalConfig;

/// Distinct from `OverlaybdConfig` above: this one describes an image's layer
/// stack, while `OverlaybdConfig` describes one attached device. All fields are
/// `#[serde(default, rename_all = "camelCase")]` upstream, so the JSON keys are
/// camelCase and every key is optional when loading.
struct ImageConfig {
    std::string repo_blob_url;
    std::vector<LayerConfig> lowers;
    UpperConfig upper;
    std::string result_file;
    /// Rust `#[serde(rename = "download", skip_serializing_if = "Option::is_none")]
    /// download_override: Option<DownloadConfig>` — a per-image override of the
    /// global download policy. Absent and present-but-default are distinct, so
    /// the presence flag is tracked separately.
    bool has_download_override = false;
    DownloadConfig download_override;
    bool acceleration_layer = false;
    std::string record_trace_path;

    /// Rust `effective_download` — the per-image override when present,
    /// otherwise the node's policy.
    ///
    /// Returns a reference into one of the two arguments, so the result must
    /// not outlive either; the ported callers copy it immediately, as Rust's
    /// `.clone()` does at the `ImageFile::open` call site.
    const DownloadConfig& EffectiveDownload(const GlobalConfig& global_config) const;

    bool operator==(const ImageConfig& other) const;
    bool operator!=(const ImageConfig& other) const { return !(*this == other); }
};

/// Serializes to the exact JSON shape serde produces, honouring the
/// `skip_serializing_if` attributes (`repoBlobUrl` when empty, `mode` when
/// unset). Mirrors `serde_json::to_vec_pretty(&config)`.
std::string ImageConfigToJson(const ImageConfig& config);

/// Rust `overlaybd::config::load_image_config` — parses an `image.json`.
core::Expected<ImageConfig, std::string> ParseImageConfig(const std::string& text);

/// Reads and parses the file at `path`.
core::Expected<ImageConfig, std::string> LoadImageConfig(const std::string& path);

/// Rust `lexically_normalize_path` — collapse `.` and `..` without touching
/// the filesystem.
///
/// Purely textual on purpose: the paths being resolved may not exist yet (an
/// upper's data file is created later), so `realpath` would fail on exactly
/// the inputs that need normalizing. It therefore does not resolve symlinks,
/// which means `a/symlink/..` normalizes to `a` rather than to the link's
/// parent.
std::string LexicallyNormalizePath(const std::string& path);

/// Rust `resolve_image_config_local_paths` — rebase every local path field
/// against the directory holding the config.
///
/// An `image.json` names its upper and local lowers relatively, and the daemon
/// does not run with that directory as its cwd; without this every relative
/// path would resolve against wherever the daemon happens to be. URLs are left
/// alone -- a value containing `://` is not a path.
void ResolveImageConfigLocalPaths(const std::string& image_config_path, ImageConfig* cfg);

/// Rust `validate_image_config`.
///
/// Separate from parsing because a config that round-trips is not necessarily
/// openable: these are the cross-field rules (a partial upper, a remote lower
/// with no `repoBlobUrl`, more lowers than tags exist for).
core::Expected<core::Unit, std::string> ValidateImageConfig(const ImageConfig& cfg);

/// Rust `validate_download_chunk_knobs` — the knobs a per-image override may
/// set. Exported because `ValidateImageConfig` applies it to the override.
core::Expected<core::Unit, std::string> ValidateDownloadChunkKnobs(const DownloadConfig& cfg,
                                                                     const std::string& field);

/// Writes `config` to `path`, creating parent directories as needed.
core::Expected<bool, std::string> SaveImageConfig(const std::string& path,
                                                  const ImageConfig& config);

/// Rust struct `OverlaybdConfig` — the full config.json for one device.
struct OverlaybdConfig {
    std::vector<LayerConfig> lowers;
    UpperConfig       upper;
    std::string   image_repo_blob_url;
    uint32_t  log_size_mb = 10;
    int32_t               log_rotate_num = 3;
};


// ---------------------------------------------------------------------------
// Rust: config.rs — the daemon-wide global config (`overlaybd.json`)
// ---------------------------------------------------------------------------
//
// Every struct below is `#[serde(default, rename_all = "camelCase")]` on the
// Rust side, so a missing key keeps its default rather than failing the parse.
// The three `cacheSizeGB` / `logSizeMB` spellings carry an explicit `rename`
// because camelCase would turn them into `cacheSizeGb` / `logSizeMb`.

/// Rust `env!("CARGO_PKG_VERSION")` as used for `GlobalConfig::user_agent`.
///
/// The overlaybd crate's own version, not this project's: it identifies the
/// storage implementation to a registry, and the two version independently.
extern const char* const kOverlaybdUserAgent;

/// Rust `P2PConfig`.
struct P2PConfig {
    bool enable;
    std::string address;

    P2PConfig() : enable(false), address("http://localhost:9731/accelerator") {}
};

/// Rust `GzipCacheConfig`.
struct GzipCacheConfig {
    bool enable;
    std::string cache_dir;
    /// Rust `#[serde(rename = "cacheSizeGB")]`.
    uint32_t cache_size_gb;
    uint32_t refill_size;

    GzipCacheConfig()
        : enable(false),
          cache_dir("/opt/overlaybd/gzip_cache"),
          cache_size_gb(4),
          refill_size(1024 * 1024) {}
};

/// Rust `OssConfig`.
struct OssConfig {
    bool enable;
    std::string access_key_id;
    std::string secret_access_key;
    std::string security_token;
    std::string credential_process;
    std::string default_region;
    std::string default_endpoint;
    /// `"virtual"`, `"path"`, or empty for endpoint-based auto-detection.
    std::string default_addressing_style;
    /// Per-request timeout in seconds (connect + transfer).
    uint64_t timeout_secs;
    /// Retries on transient failures.
    uint32_t retry_count;

    OssConfig() : enable(false), timeout_secs(30), retry_count(3) {}
};

/// Rust `ExporterConfig`.
struct ExporterConfig {
    bool enable;
    std::string uri_prefix;
    int32_t port;
    /// Microseconds, not milliseconds -- the default is 60 seconds.
    uint64_t update_interval;

    ExporterConfig()
        : enable(false), uri_prefix("/metrics"), port(9863),
          update_interval(60ULL * 1000ULL * 1000ULL) {}
};

/// Rust `CredentialConfig`.
struct CredentialConfig {
    /// `""`, `"file"` or `"http"`.
    std::string mode;
    std::string path;
    int32_t timeout;

    CredentialConfig() : timeout(1) {}
};

/// Rust `CacheConfig`.
struct CacheConfig {
    std::string cache_type;
    std::string cache_dir;
    /// Rust `#[serde(rename = "cacheSizeGB")]`.
    uint32_t cache_size_gb;
    /// Both the I/O unit and the bitmap granularity. Must be a power of two
    /// >= 4096.
    uint32_t refill_size;
    /// Under capacity pressure, open entries idle longer than this may have
    /// their blocks reclaimed; the entry survives and refills on demand. 0
    /// disables open-entry eviction.
    uint64_t open_evict_idle_secs;

    CacheConfig()
        : cache_dir("/opt/overlaybd/registry_cache"),
          cache_size_gb(4),
          refill_size(262144),
          open_evict_idle_secs(600) {}
};

/// Rust `LogConfig`.
struct LogConfig {
    uint32_t log_level;
    std::string log_path;
    /// Rust `#[serde(rename = "logSizeMB")]`.
    uint32_t log_size_mb;
    int32_t log_rotate_num;

    LogConfig() : log_level(1), log_size_mb(10), log_rotate_num(3) {}
};

/// Rust `PrefetchConfig`.
struct PrefetchConfig {
    int32_t concurrency;

    PrefetchConfig() : concurrency(16) {}
};

/// Rust `CertConfig`.
struct CertConfig {
    std::string cert_file;
    std::string key_file;
};

/// Rust `GlobalConfig` — the daemon-wide settings, one per host.
///
/// The first seven fields are legacy: newer equivalents live in
/// `cache_config`, `log_config` and `credential_config`, and
/// `NormalizeCompatFields` folds the old spellings into the new ones. They are
/// kept because configs written against the old shape are still in the wild.
struct GlobalConfig {
    // -- legacy fields -----------------------------------------------------
    std::string registry_cache_dir;
    std::string credential_file_path;
    /// Rust `#[serde(rename = "registryCacheSizeGB")]`.
    uint32_t registry_cache_size_gb;
    uint32_t io_engine;
    std::string cache_type;
    uint32_t log_level;
    std::string log_path;

    // -- current fields ----------------------------------------------------
    DownloadConfig download;
    bool enable_audit;
    bool enable_thread;
    P2PConfig p2p_config;
    ExporterConfig exporter_config;
    std::string audit_path;
    std::string registry_fs_version;
    CacheConfig cache_config;
    GzipCacheConfig gzip_cache_config;
    OssConfig oss_config;
    LogConfig log_config;
    PrefetchConfig prefetch_config;
    CertConfig cert_config;
    std::string user_agent;
    CredentialConfig credential_config;

    /// Legacy compatibility field. Local files no longer allocate io_uring
    /// workers through the image service; ublk owns its queue-local rings.
    std::size_t nr_io_rings;

    /// Rust `remote_io_workers` — worker threads of the dedicated runtime
    /// driving remote image I/O.
    ///
    /// Carried for config fidelity only: it has no effect here. Rust needs it
    /// because its remote backends spawn per-connection tokio tasks whose
    /// lifetime would otherwise bind to whichever device runtime happened to
    /// poll them; a synchronous port has no runtimes to pin work to. Dropping
    /// the field instead would make a round-tripped config differ from its
    /// input.
    std::size_t remote_io_workers;

    GlobalConfig()
        : registry_cache_dir("/opt/overlaybd/registry_cache"),
          credential_file_path("/opt/overlaybd/cred.json"),
          registry_cache_size_gb(4),
          io_engine(0),
          cache_type("file"),
          log_level(1),
          log_path("/var/log/overlaybd.log"),
          enable_audit(true),
          enable_thread(false),
          audit_path("/var/log/overlaybd-audit.log"),
          registry_fs_version("v2"),
          nr_io_rings(4),
          remote_io_workers(0) {}

    /// Rust `normalize_compat_fields` — fold the legacy spellings into the
    /// current ones.
    ///
    /// Each fold is conditional on the *new* field being unset, so a config
    /// that sets both keeps the new value. The order matters for cache: the
    /// legacy block is applied first, then a still-empty type falls back to
    /// `"file"`.
    void NormalizeCompatFields();
};

/// Rust `validate_global_config`.
core::Expected<core::Unit, std::string> ValidateGlobalConfig(const GlobalConfig& cfg);

/// Rust `serde_json::from_str::<GlobalConfig>` — parses an `overlaybd.json`.
///
/// Does *not* normalize or validate; `LoadGlobalConfig` does both. Split so a
/// caller can inspect exactly what the file said.
core::Expected<GlobalConfig, std::string> ParseGlobalConfig(const std::string& text);

/// Rust `load_global_config` — read, parse, normalize, then validate.
core::Expected<GlobalConfig, std::string> LoadGlobalConfig(const std::string& path);

/// Serializes to the camelCase shape serde produces, honouring the three
/// explicit renames.
std::string GlobalConfigToJson(const GlobalConfig& config);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_CONFIG_H_
