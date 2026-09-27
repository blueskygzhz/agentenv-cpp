// SPDX-License-Identifier: MIT
// Rust: src/cfg/image.rs
#include "agentenv/cfg/image.h"

#include <cmath>
#include <sstream>
#include <vector>

#include "agentenv/cfg.h"

namespace agentenv {
namespace cfg {

const char* const kImageCacheCommitDir = "commits";
const char* const kImageCacheRemoteBlocksDir = "remote-blocks";

namespace {

std::string Trim(const std::string& raw) {
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && (raw[begin] == ' ' || raw[begin] == '\t' || raw[begin] == '\r' ||
                           raw[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (raw[end - 1] == ' ' || raw[end - 1] == '\t' || raw[end - 1] == '\r' ||
                           raw[end - 1] == '\n')) {
        --end;
    }
    return raw.substr(begin, end - begin);
}

/// Rust `str::trim_end_matches('/')`.
std::string TrimTrailingSlashes(const std::string& raw) {
    std::size_t end = raw.size();
    while (end > 0 && raw[end - 1] == '/') --end;
    return raw.substr(0, end);
}

/// Rust `validate_ratio`.
core::Expected<core::Unit, std::string> ValidateRatio(const char* name, double value) {
    // Rust `!value.is_finite()` covers NaN and both infinities.
    const bool finite = !(value != value) && value != HUGE_VAL && value != -HUGE_VAL;
    if (!finite || value <= 0.0 || value > 1.0) {
        std::ostringstream oss;
        oss << name << " must be > 0 and <= 1, got " << value;
        return core::make_unexpected(oss.str());
    }
    return core::Unit();
}

}  // namespace

std::string LexicallyNormalizePath(const std::string& path) {
    if (path.empty()) return path;
    const bool absolute = path[0] == '/';

    std::vector<std::string> stack;
    std::size_t cursor = 0;
    while (cursor <= path.size()) {
        const std::size_t slash = path.find('/', cursor);
        const std::string component =
            path.substr(cursor, slash == std::string::npos ? std::string::npos : slash - cursor);

        if (!component.empty() && component != ".") {
            if (component == "..") {
                // `..` pops a real component; it is preserved when the path is
                // relative and there is nothing to pop (matching Rust).
                if (!stack.empty() && stack.back() != "..") {
                    stack.pop_back();
                } else if (!absolute) {
                    stack.push_back("..");
                }
            } else {
                stack.push_back(component);
            }
        }

        if (slash == std::string::npos) break;
        cursor = slash + 1;
    }

    std::string out;
    if (absolute) out.push_back('/');
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (i > 0) out.push_back('/');
        out += stack[i];
    }
    if (out.empty()) out = absolute ? "/" : ".";
    return out;
}

// ---------------------------------------------------------------------------
// ImageResolverConfig
// ---------------------------------------------------------------------------

ImageResolverConfig::ImageResolverConfig() {
    // Rust `#[config(default = ["docker.io", "ghcr.io"])]`.
    search_registries.push_back("docker.io");
    search_registries.push_back("ghcr.io");
}

void ImageResolverConfig::Normalize() {
    default_image = Trim(default_image);
    if (default_image.empty()) {
        default_image = ImageResolverConfig().default_image;
    }

    for (std::size_t i = 0; i < search_registries.size(); ++i) {
        search_registries[i] = TrimTrailingSlashes(Trim(search_registries[i]));
    }

    // Keep `None` (unset = no restriction) distinct from `Some([])` (explicit
    // empty list = deny all). Only trim and drop empty entries.
    if (allowed_registries.has_value()) {
        std::vector<std::string> filtered;
        const std::vector<std::string>& registries = *allowed_registries;
        for (std::size_t i = 0; i < registries.size(); ++i) {
            const std::string host = TrimTrailingSlashes(Trim(registries[i]));
            if (!host.empty()) filtered.push_back(host);
        }
        allowed_registries = filtered;
    }

    std::vector<std::string> prefixes;
    for (std::size_t i = 0; i < try_referrers_overlaybd_prefixes.size(); ++i) {
        const std::string prefix = Trim(try_referrers_overlaybd_prefixes[i]);
        if (!prefix.empty()) prefixes.push_back(prefix);
    }
    try_referrers_overlaybd_prefixes = prefixes;
}

// ---------------------------------------------------------------------------
// ImageCacheGcConfig
// ---------------------------------------------------------------------------

void ImageCacheGcConfig::Normalize() {
    if (interval_secs == 0) {
        interval_secs = ImageCacheGcConfig().interval_secs;
    }
}

core::Expected<core::Unit, std::string> ImageCacheGcConfig::Validate() const {
    core::Expected<core::Unit, std::string> high =
        ValidateRatio("image.cache.gc.high_watermark_ratio", high_watermark_ratio);
    if (!high.has_value()) return core::make_unexpected(high.error());

    core::Expected<core::Unit, std::string> low =
        ValidateRatio("image.cache.gc.low_watermark_ratio", low_watermark_ratio);
    if (!low.has_value()) return core::make_unexpected(low.error());

    if (low_watermark_ratio > high_watermark_ratio) {
        std::ostringstream oss;
        oss << "image.cache.gc.low_watermark_ratio (" << low_watermark_ratio
            << ") must be <= image.cache.gc.high_watermark_ratio (" << high_watermark_ratio << ")";
        return core::make_unexpected(oss.str());
    }
    return core::Unit();
}

// ---------------------------------------------------------------------------
// ResolvedImageCacheGcConfig
// ---------------------------------------------------------------------------

bool ResolvedImageCacheGcConfig::WatermarkBytes(const core::Optional<uint64_t>& capacity_bytes,
                                                uint64_t* high, uint64_t* low) const {
    if (!capacity_bytes.has_value()) return false;
    const double capacity = static_cast<double>(*capacity_bytes);
    if (high != nullptr) *high = static_cast<uint64_t>(capacity * high_watermark_ratio);
    if (low != nullptr) *low = static_cast<uint64_t>(capacity * low_watermark_ratio);
    return true;
}

// ---------------------------------------------------------------------------
// ImageCacheConfig
// ---------------------------------------------------------------------------

ResolvedImageCacheConfig ImageCacheConfig::Layout() const {
    ResolvedImageCacheConfig resolved;
    const std::string normalized_root = LexicallyNormalizePath(root_dir);
    resolved.root_dir = normalized_root;
    resolved.commit_store = PathJoin(normalized_root, kImageCacheCommitDir);
    resolved.remote_blocks_dir = PathJoin(normalized_root, kImageCacheRemoteBlocksDir);
    resolved.remote_blocks_size_gb = remote_blocks.max_size_gb;
    if (capacity_gb.has_value()) {
        // Rust `saturating_mul`.
        const uint64_t gb = *capacity_gb;
        const uint64_t limit = UINT64_MAX / kBytesPerGib;
        resolved.capacity_bytes = gb > limit ? UINT64_MAX : gb * kBytesPerGib;
    }
    return resolved;
}

ResolvedImageCacheGcConfig ImageCacheConfig::GcSchedule() const {
    ResolvedImageCacheGcConfig resolved;
    resolved.enabled = gc.enabled;
    resolved.interval_secs = gc.interval_secs;
    resolved.min_age_secs = gc.min_age_secs;
    resolved.high_watermark_ratio = gc.high_watermark_ratio;
    resolved.low_watermark_ratio = gc.low_watermark_ratio;
    return resolved;
}

// ---------------------------------------------------------------------------
// ImageConfig
// ---------------------------------------------------------------------------

void ImageConfig::Normalize(ImageConfig* config, const std::string& config_dir,
                            const std::string& home_path) {
    config->resolver.Normalize();
    config->cache.gc.Normalize();
    const std::string raw = ResolvePath(home_path, config_dir, config->cache.root_dir);
    config->cache.root_dir = LexicallyNormalizePath(raw);
}

core::Expected<core::Unit, std::string> ImageConfig::LoadFrom(const core::TomlTable& table) {
#define AENV_LOAD_STRING(KEY, FIELD)                                     \
    do {                                                                 \
        const core::TomlValue* value = table.Find(KEY);                  \
        if (value != nullptr) {                                          \
            core::Expected<std::string, std::string> parsed =            \
                value->AsString();                                       \
            if (!parsed.has_value())                                     \
                return core::make_unexpected(std::string(KEY) + ": " +   \
                                             parsed.error());            \
            (FIELD) = parsed.value();                                    \
        }                                                                \
    } while (false)

#define AENV_LOAD_BOOL(KEY, FIELD)                                       \
    do {                                                                 \
        const core::TomlValue* value = table.Find(KEY);                  \
        if (value != nullptr) {                                          \
            core::Expected<bool, std::string> parsed = value->AsBoolean();\
            if (!parsed.has_value())                                     \
                return core::make_unexpected(std::string(KEY) + ": " +   \
                                             parsed.error());            \
            (FIELD) = parsed.value();                                    \
        }                                                                \
    } while (false)

#define AENV_LOAD_UINT(KEY, FIELD, TYPE)                                 \
    do {                                                                 \
        const core::TomlValue* value = table.Find(KEY);                  \
        if (value != nullptr) {                                          \
            core::Expected<long long, std::string> parsed =              \
                value->AsInteger();                                      \
            if (!parsed.has_value())                                     \
                return core::make_unexpected(std::string(KEY) + ": " +   \
                                             parsed.error());            \
            if (parsed.value() < 0)                                      \
                return core::make_unexpected(std::string(KEY) +          \
                                             " must not be negative");   \
            (FIELD) = static_cast<TYPE>(parsed.value());                 \
        }                                                                \
    } while (false)

#define AENV_LOAD_DOUBLE(KEY, FIELD)                                     \
    do {                                                                 \
        const core::TomlValue* value = table.Find(KEY);                  \
        if (value != nullptr) {                                          \
            core::Expected<double, std::string> parsed = value->AsFloat();\
            if (!parsed.has_value())                                     \
                return core::make_unexpected(std::string(KEY) + ": " +   \
                                             parsed.error());            \
            (FIELD) = parsed.value();                                    \
        }                                                                \
    } while (false)

#define AENV_LOAD_STRING_ARRAY(KEY, FIELD)                               \
    do {                                                                 \
        const core::TomlValue* value = table.Find(KEY);                  \
        if (value != nullptr) {                                          \
            core::Expected<std::vector<std::string>, std::string>        \
                parsed = value->AsStringArray();                         \
            if (!parsed.has_value())                                     \
                return core::make_unexpected(std::string(KEY) + ": " +   \
                                             parsed.error());            \
            (FIELD) = parsed.value();                                    \
        }                                                                \
    } while (false)

    AENV_LOAD_STRING("image.resolver.default_image", resolver.default_image);
    AENV_LOAD_STRING_ARRAY("image.resolver.search_registries", resolver.search_registries);
    AENV_LOAD_BOOL("image.resolver.convert_standard_oci", resolver.convert_standard_oci);
    {
        const core::TomlValue* value = table.Find("image.resolver.allowed_registries");
        if (value != nullptr) {
            core::Expected<std::vector<std::string>, std::string> parsed = value->AsStringArray();
            if (!parsed.has_value()) {
                return core::make_unexpected(std::string("image.resolver.allowed_registries: ") +
                                             parsed.error());
            }
            resolver.allowed_registries = parsed.value();
        }
    }
    AENV_LOAD_STRING_ARRAY("image.resolver.try_referrers_overlaybd_prefixes",
                           resolver.try_referrers_overlaybd_prefixes);

    AENV_LOAD_STRING("image.cache.root_dir", cache.root_dir);
    {
        const core::TomlValue* value = table.Find("image.cache.capacity_gb");
        if (value != nullptr) {
            core::Expected<long long, std::string> parsed = value->AsInteger();
            if (!parsed.has_value()) {
                return core::make_unexpected(std::string("image.cache.capacity_gb: ") +
                                             parsed.error());
            }
            if (parsed.value() < 0) {
                return core::make_unexpected(
                    std::string("image.cache.capacity_gb must not be negative"));
            }
            cache.capacity_gb = static_cast<uint64_t>(parsed.value());
        }
    }
    AENV_LOAD_UINT("image.cache.remote_blocks.max_size_gb", cache.remote_blocks.max_size_gb,
                   uint32_t);
    AENV_LOAD_BOOL("image.cache.gc.enabled", cache.gc.enabled);
    AENV_LOAD_UINT("image.cache.gc.interval_secs", cache.gc.interval_secs, uint64_t);
    AENV_LOAD_UINT("image.cache.gc.min_age_secs", cache.gc.min_age_secs, uint64_t);
    AENV_LOAD_DOUBLE("image.cache.gc.high_watermark_ratio", cache.gc.high_watermark_ratio);
    AENV_LOAD_DOUBLE("image.cache.gc.low_watermark_ratio", cache.gc.low_watermark_ratio);

#undef AENV_LOAD_STRING
#undef AENV_LOAD_BOOL
#undef AENV_LOAD_UINT
#undef AENV_LOAD_DOUBLE
#undef AENV_LOAD_STRING_ARRAY

    return core::Unit();
}

}  // namespace cfg
}  // namespace agentenv
