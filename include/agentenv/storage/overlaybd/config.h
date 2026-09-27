// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/config.rs — LayerConfig / UpperConfig / OverlaybdConfig.
#ifndef AGENTENV_STORAGE_OVERLAYBD_CONFIG_H_
#define AGENTENV_STORAGE_OVERLAYBD_CONFIG_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

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
