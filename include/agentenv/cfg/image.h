// SPDX-License-Identifier: MIT
// Rust: src/cfg/image.rs
//
// Every `#[config(default = ...)]` is reproduced as a C++11 non-static data
// member initializer, so a default-constructed struct equals Rust's
// `impl_config_default!` output.
#ifndef AGENTENV_CFG_IMAGE_H_
#define AGENTENV_CFG_IMAGE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/toml.h"

namespace agentenv {
namespace cfg {

/// Rust `IMAGE_CACHE_COMMIT_DIR`.
extern const char* const kImageCacheCommitDir;
/// Rust `IMAGE_CACHE_REMOTE_BLOCKS_DIR`.
extern const char* const kImageCacheRemoteBlocksDir;
/// Rust `BYTES_PER_GIB`.
const uint64_t kBytesPerGib = 1024ULL * 1024ULL * 1024ULL;

/// Rust struct `ImageResolverConfig`.
struct ImageResolverConfig {
    std::string default_image = "ubuntu:24.04";
    std::vector<std::string> search_registries;  // default ["docker.io", "ghcr.io"]
    bool convert_standard_oci = true;
    /// Rust `Option<Vec<String>>`: unset = no restriction, `Some([])` = deny all.
    /// The distinction is load-bearing, hence Optional rather than an empty vector.
    core::Optional<std::vector<std::string> > allowed_registries;
    std::vector<std::string> try_referrers_overlaybd_prefixes;  // default []

    ImageResolverConfig();

    /// Rust `ImageResolverConfig::normalize`.
    void Normalize();
};

/// Rust struct `ImageRemoteBlocksCacheConfig`.
struct ImageRemoteBlocksCacheConfig {
    uint32_t max_size_gb = 10;
};

/// Rust struct `ImageCacheGcConfig`.
struct ImageCacheGcConfig {
    bool enabled = true;
    uint64_t interval_secs = 1800;
    uint64_t min_age_secs = 600;
    double high_watermark_ratio = 0.95;
    double low_watermark_ratio = 0.70;

    /// Rust `ImageCacheGcConfig::normalize` — a zero interval falls back to the
    /// default rather than spinning.
    void Normalize();
    /// Rust `ImageCacheGcConfig::validate`.
    core::Expected<core::Unit, std::string> Validate() const;
};

/// Rust struct `ResolvedImageCacheConfig`.
struct ResolvedImageCacheConfig {
    std::string root_dir;
    std::string commit_store;
    std::string remote_blocks_dir;
    uint32_t remote_blocks_size_gb = 0;
    core::Optional<uint64_t> capacity_bytes;
};

/// Rust struct `ResolvedImageCacheGcConfig`. Rust holds `Duration`s; seconds are
/// kept here because C++11 `std::chrono` adds nothing for a plain config value.
struct ResolvedImageCacheGcConfig {
    bool enabled = false;
    uint64_t interval_secs = 0;
    uint64_t min_age_secs = 0;
    double high_watermark_ratio = 0.0;
    double low_watermark_ratio = 0.0;

    /// Rust `ResolvedImageCacheGcConfig::watermark_bytes` — `None` when the
    /// cache capacity is unset (capacity-driven eviction disabled).
    bool WatermarkBytes(const core::Optional<uint64_t>& capacity_bytes, uint64_t* high,
                        uint64_t* low) const;
};

/// Rust struct `ImageCacheConfig`.
struct ImageCacheConfig {
    std::string root_dir = "$AENV_HOME/image-cache";
    core::Optional<uint64_t> capacity_gb;
    ImageRemoteBlocksCacheConfig remote_blocks;
    ImageCacheGcConfig gc;

    /// Rust `ImageCacheConfig::layout`.
    ResolvedImageCacheConfig Layout() const;
    /// Rust `ImageCacheConfig::gc_schedule`.
    ResolvedImageCacheGcConfig GcSchedule() const;
};

/// Rust struct `ImageConfig`.
struct ImageConfig {
    ImageResolverConfig resolver;
    ImageCacheConfig cache;

    /// Rust `ImageConfig::normalize`.
    static void Normalize(ImageConfig* config, const std::string& config_dir,
                          const std::string& home_path);

    /// Overlay TOML keys under the `image.` prefix onto `*this`.
    core::Expected<core::Unit, std::string> LoadFrom(const core::TomlTable& table);
};

/// Rust `overlaybd::config::lexically_normalize_path` — resolve `.`/`..`
/// textually (never touching the filesystem) and collapse duplicate slashes.
std::string LexicallyNormalizePath(const std::string& path);

}  // namespace cfg
}  // namespace agentenv
#endif  // AGENTENV_CFG_IMAGE_H_
