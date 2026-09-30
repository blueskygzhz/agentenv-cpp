// SPDX-License-Identifier: MIT
// Rust: src/image/cache/{graph.rs, gc.rs}
//
// The pure-logic subset of the image-cache reference graph and GC reporting:
//   * validated identity newtypes (HardCommitId / ImageCacheConfigId /
//     ImageCacheHoldOwner) with the same trim/non-empty rules as Rust;
//   * the metadata-store key encoding (prefix + '/'-joined, hex-encoded
//     components) so the on-disk layout stays byte-compatible;
//   * source-config filename classification;
//   * the LRU capacity-eviction planner as a store-free function over in-memory
//     ref maps / sizes / last-used times (mirrors the algorithm inside
//     `ImageCacheMetadataStore::plan_capacity_eviction` after the scans);
//* the GC report/blocked/summary value types and `summary_from_report`.
//
// The async `ImageCacheMetadataStore` (LocalKvStore-backed) and the overlaybd
// config parsing stay out of this port; they are covered by the gated
// snapshot/repository + storage layers.
#ifndef AGENTENV_IMAGE_CACHE_H_
#define AGENTENV_IMAGE_CACHE_H_

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace image {
namespace cache {

// ---- graph.rs constants (exposed for key encoding + tests) ----
extern const char* const kHardCommitObjectPrefix;
extern const char* const kHoldRecordPrefix;
extern const char* const kConfigToHardPrefix;
extern const char* const kHoldToHardPrefix;
extern const char* const kHardToHoldPrefix;
extern const char* const kConfigLastUsedPrefix;
extern const char* const kImageConfigSuffix;

/// Rust `HardCommitId` — non-empty (after trim) commit digest.
class HardCommitId {
 public:
    static core::Expected<HardCommitId, std::string> New(const std::string& digest);
  const std::string& AsStr() const { return value_; }
    std::string ToString() const { return value_; }
    bool operator==(const HardCommitId& o) const { return value_ == o.value_; }
    bool operator!=(const HardCommitId& o) const { return value_ != o.value_; }
    bool operator<(const HardCommitId& o) const { return value_ < o.value_; }
 private:
    explicit HardCommitId(std::string v) : value_(std::move(v)) {}
    std::string value_;
};

/// Rust `ImageCacheConfigId` — a source-image config filename ("*-image.json").
class ImageCacheConfigId {
 public:
    static core::Expected<ImageCacheConfigId, std::string> FromFilename(
      const std::string& filename);
    /// Rust `from_config_path` — take the file name component then validate.
    static core::Expected<ImageCacheConfigId, std::string> FromConfigPath(
    const std::string& path);
    const std::string& AsStr() const { return value_; }
  std::string ToString() const { return value_; }
    bool operator==(const ImageCacheConfigId& o) const { return value_ == o.value_; }
    bool operator!=(const ImageCacheConfigId& o) const { return value_ != o.value_; }
    bool operator<(const ImageCacheConfigId& o) const { return value_ < o.value_; }
 private:
    explicit ImageCacheConfigId(std::string v) : value_(std::move(v)) {}
    std::string value_;
};

/// Rust `ImageCacheHoldOwner` — (namespace, key), both non-empty after trim.
class ImageCacheHoldOwner {
 public:
    static core::Expected<ImageCacheHoldOwner, std::string> New(
        const std::string& ns, const std::string& key);
    const std::string& Namespace() const { return namespace_; }
    const std::string& Key() const { return key_; }
    /// Rust `Display` — "namespace/key".
    std::string ToString() const { return namespace_ + "/" + key_; }
    bool operator==(const ImageCacheHoldOwner& o) const {
        return namespace_ == o.namespace_ && key_ == o.key_;
    }
    bool operator!=(const ImageCacheHoldOwner& o) const { return !(*this == o); }
 bool operator<(const ImageCacheHoldOwner& o) const {
        return namespace_ != o.namespace_ ? namespace_ < o.namespace_
: key_ < o.key_;
    }
 private:
    ImageCacheHoldOwner(std::string ns, std::string key)
        : namespace_(std::move(ns)), key_(std::move(key)) {}
    std::string namespace_;
    std::string key_;
};

/// Rust `non_empty` — trim check helper.
core::Expected<std::string, std::string> NonEmpty(const std::string& field,
        const std::string& value);

/// Rust `is_regular_config_filename`.
bool IsRegularConfigFilename(const std::string& filename);

/// Rust `hex_encode`.
std::string HexEncode(const std::string& bytes);

/// Rust `key_with_components` — prefix + hex('/'-joined components).
std::string KeyWithComponents(const std::string& prefix,
     const std::vector<std::string>& components);

// Concrete key builders (mirror the private fns in graph.rs).
std::string HardCommitObjectKey(const HardCommitId& digest);
std::string HoldRecordKey(const ImageCacheHoldOwner& owner);
std::string ConfigToHardKey(const ImageCacheConfigId& config, const HardCommitId& digest);
std::string ConfigToHardPrefixForConfig(const ImageCacheConfigId& config);
std::string ConfigLastUsedKey(const ImageCacheConfigId& config);
std::string HoldToHardKey(const ImageCacheHoldOwner& owner, const HardCommitId& digest);
std::string HardToHoldKey(const HardCommitId& digest, const ImageCacheHoldOwner& owner);
std::string HoldToHardPrefixForOwner(const ImageCacheHoldOwner& owner);
std::string HardToHoldPrefixForDigest(const HardCommitId& digest);

// ---- capacity eviction (graph.rs) ----

/// Rust `CapacityEvictionCandidate`.
struct CapacityEvictionCandidate {
    ImageCacheConfigId config_id;
    uint64_t   last_used;
    CapacityEvictionCandidate(ImageCacheConfigId id, uint64_t used)
  : config_id(std::move(id)), last_used(used) {}
    bool operator==(const CapacityEvictionCandidate& o) const {
        return config_id == o.config_id && last_used == o.last_used;
    }
};

/// Rust `CapacityEvictionPlan`.
struct CapacityEvictionPlan {
    std::vector<CapacityEvictionCandidate> candidates;
    uint64_t total_bytes = 0;
};

/// The store-free core of `plan_capacity_eviction`. `config_refs` maps each
/// source config to the hard commits it roots; `sizes` gives each commit's
/// bytes; `last_used` gives each config's recency. A commit's bytes are counted
/// as freed only once every config referring to it is being evicted.
CapacityEvictionPlan PlanCapacityEviction(
    const std::map<ImageCacheConfigId, std::set<HardCommitId> >& config_refs,
    const std::map<HardCommitId, uint64_t>& sizes,
    const std::map<ImageCacheConfigId, uint64_t>& last_used,
    uint64_t high_watermark_bytes,
    uint64_t low_watermark_bytes,
    uint64_t evictable_before);

// ---- gc.rs report types ----

/// Rust `ImageCacheGcBlockedReason`.
struct ImageCacheGcBlockedReason {
    enum Kind { RootedByConfig, Held, LiveRuntime, Unverifiable, DeleteFailed };
    Kind kind = Unverifiable;
    std::vector<std::string> configs;     // RootedByConfig
    std::vector<ImageCacheHoldOwner> owners;      // Held / LiveRuntime
    std::string detail;              // Unverifiable / DeleteFailed
};

/// Rust `ImageCacheGcBlocked`.
struct ImageCacheGcBlocked {
    HardCommitId digest;
    ImageCacheGcBlockedReason reason;
    ImageCacheGcBlocked(HardCommitId d, ImageCacheGcBlockedReason r)
        : digest(std::move(d)), reason(std::move(r)) {}
};

/// Rust `ImageCacheGcReport`.
struct ImageCacheGcReport {
    std::size_t collected = 0;
    uint64_t    freed_bytes = 0;
    std::vector<ImageCacheGcBlocked> blocked;
};

/// Rust `ImageCacheGcSummary`.
struct ImageCacheGcSummary {
    std::size_t collected = 0;
    uint64_t    freed_bytes = 0;
    std::size_t retained = 0;
  bool operator==(const ImageCacheGcSummary& o) const {
        return collected == o.collected && freed_bytes == o.freed_bytes &&
     retained == o.retained;
    }
};

/// Rust `ImageCacheGcSummary::from_report`.
ImageCacheGcSummary SummaryFromReport(const ImageCacheGcReport& report);

}  // namespace cache
}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_CACHE_H_
