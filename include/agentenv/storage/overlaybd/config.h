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

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_CONFIG_H_
