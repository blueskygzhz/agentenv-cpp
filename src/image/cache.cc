// SPDX-License-Identifier: MIT
// Rust: src/image/cache/{graph.rs, gc.rs}
#include "agentenv/image/cache.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace agentenv {
namespace image {
namespace cache {

const char* const kHardCommitObjectPrefix = "object/hard-commit/";
const char* const kHoldRecordPrefix = "hold/";
const char* const kConfigToHardPrefix = "ref/config-to-hard/";
const char* const kHoldToHardPrefix = "ref/hold-to-hard/";
const char* const kHardToHoldPrefix = "ref/hard-to-hold/";
const char* const kConfigLastUsedPrefix = "config-last-used/";
const char* const kImageConfigSuffix = "-image.json";

namespace {

std::string Trim(const std::string& s) {
    std::string::size_type b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

}  // namespace

core::Expected<std::string, std::string> NonEmpty(const std::string& field,
        const std::string& value) {
    if (Trim(value).empty()) {
     return core::make_unexpected<std::string>(field + " is empty");
}
    return value;
}

// ---- HardCommitId ----

core::Expected<HardCommitId, std::string> HardCommitId::New(
    const std::string& digest) {
    if (Trim(digest).empty()) {
        return core::make_unexpected<std::string>("hard commit digest is empty");
    }
    return HardCommitId(digest);
}

// ---- ImageCacheConfigId ----

bool IsRegularConfigFilename(const std::string& filename) {
    const std::string suffix = kImageConfigSuffix;
    return filename.size() >= suffix.size() &&
 filename.compare(filename.size() - suffix.size(), suffix.size(),
       suffix) == 0;
}

core::Expected<ImageCacheConfigId, std::string> ImageCacheConfigId::FromFilename(
    const std::string& filename) {
    if (!IsRegularConfigFilename(filename)) {
        return core::make_unexpected<std::string>(
 "image cache config '" + filename + "' is not a source image config");
    }
    return ImageCacheConfigId(filename);
}

core::Expected<ImageCacheConfigId, std::string> ImageCacheConfigId::FromConfigPath(
    const std::string& path) {
    std::string::size_type slash = path.find_last_of('/');
    std::string filename =
     (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (filename.empty()) {
        return core::make_unexpected<std::string>(
        "image cache config path has no file name: " + path);
    }
    return FromFilename(filename);
}

// ---- ImageCacheHoldOwner ----

core::Expected<ImageCacheHoldOwner, std::string> ImageCacheHoldOwner::New(
    const std::string& ns, const std::string& key) {
    auto n = NonEmpty("image cache hold namespace", ns);
    if (!n.ok()) return core::make_unexpected(n.take_error());
    auto k = NonEmpty("image cache hold key", key);
    if (!k.ok()) return core::make_unexpected(k.take_error());
    return ImageCacheHoldOwner(ns, key);
}

// ---- key encoding ----

std::string HexEncode(const std::string& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) {
        out += kHex[b >> 4];
    out += kHex[b & 0x0f];
    }
    return out;
}

std::string KeyWithComponents(const std::string& prefix,
      const std::vector<std::string>& components) {
    std::string key = prefix;
bool first = true;
    for (const std::string& c : components) {
   if (!first) key += '/';
    first = false;
    key += HexEncode(c);
}
    return key;
}

std::string HardCommitObjectKey(const HardCommitId& digest) {
    return KeyWithComponents(kHardCommitObjectPrefix, {digest.AsStr()});
}

std::string HoldRecordKey(const ImageCacheHoldOwner& owner) {
    return KeyWithComponents(kHoldRecordPrefix, {owner.Namespace(), owner.Key()});
}

std::string ConfigToHardKey(const ImageCacheConfigId& config,
    const HardCommitId& digest) {
    return KeyWithComponents(kConfigToHardPrefix,
        {config.AsStr(), digest.AsStr()});
}

std::string ConfigToHardPrefixForConfig(const ImageCacheConfigId& config) {
    return KeyWithComponents(kConfigToHardPrefix, {config.AsStr()}) + "/";
}

std::string ConfigLastUsedKey(const ImageCacheConfigId& config) {
    return KeyWithComponents(kConfigLastUsedPrefix, {config.AsStr()});
}

std::string HoldToHardKey(const ImageCacheHoldOwner& owner,
   const HardCommitId& digest) {
    return KeyWithComponents(kHoldToHardPrefix,
    {owner.Namespace(), owner.Key(), digest.AsStr()});
}

std::string HardToHoldKey(const HardCommitId& digest,
       const ImageCacheHoldOwner& owner) {
    return KeyWithComponents(kHardToHoldPrefix,
        {digest.AsStr(), owner.Namespace(), owner.Key()});
}

std::string HoldToHardPrefixForOwner(const ImageCacheHoldOwner& owner) {
    return KeyWithComponents(kHoldToHardPrefix,
    {owner.Namespace(), owner.Key()}) + "/";
}

std::string HardToHoldPrefixForDigest(const HardCommitId& digest) {
    return KeyWithComponents(kHardToHoldPrefix, {digest.AsStr()}) + "/";
}

// ---- capacity eviction ----

CapacityEvictionPlan PlanCapacityEviction(
    const std::map<ImageCacheConfigId, std::set<HardCommitId> >& config_refs,
    const std::map<HardCommitId, uint64_t>& sizes,
    const std::map<ImageCacheConfigId, uint64_t>& last_used,
    uint64_t high_watermark_bytes,
    uint64_t low_watermark_bytes,
    uint64_t evictable_before) {
    CapacityEvictionPlan plan;

    uint64_t total_bytes = 0;
    for (std::map<HardCommitId, uint64_t>::const_iterator it = sizes.begin();
         it != sizes.end(); ++it) {
        total_bytes += it->second;
    }
    plan.total_bytes = total_bytes;
    if (total_bytes <= high_watermark_bytes) {
    return plan;
    }

    // Reverse index: commit -> set of configs referring to it.
    std::map<HardCommitId, std::set<ImageCacheConfigId> > referrers;
    for (std::map<ImageCacheConfigId, std::set<HardCommitId> >::const_iterator
    it = config_refs.begin(); it != config_refs.end(); ++it) {
    for (std::set<HardCommitId>::const_iterator c = it->second.begin();
        c != it->second.end(); ++c) {
  referrers[*c].insert(it->first);
      }
    }

    // Evictable configs whose last_used <= evictable_before, sorted by
    // (last_used, config_id) ascending — matches Rust's tuple sort.
    std::vector<std::pair<uint64_t, ImageCacheConfigId> > evictable;
    for (std::map<ImageCacheConfigId, std::set<HardCommitId> >::const_iterator
    it = config_refs.begin(); it != config_refs.end(); ++it) {
     std::map<ImageCacheConfigId, uint64_t>::const_iterator lu =
     last_used.find(it->first);
        if (lu != last_used.end() && lu->second <= evictable_before) {
     evictable.push_back(std::make_pair(lu->second, it->first));
        }
    }
    std::sort(evictable.begin(), evictable.end(),
       [](const std::pair<uint64_t, ImageCacheConfigId>& a,
      const std::pair<uint64_t, ImageCacheConfigId>& b) {
          if (a.first != b.first) return a.first < b.first;
    return a.second < b.second;
      });

    std::set<ImageCacheConfigId> evicting;
    std::set<HardCommitId> covered;
    uint64_t estimated_freed = 0;

for (std::size_t i = 0; i < evictable.size(); ++i) {
     uint64_t used = evictable[i].first;
        const ImageCacheConfigId& config_id = evictable[i].second;
  // saturating_sub then compare against low watermark.
        uint64_t remaining =
     (total_bytes > estimated_freed) ? (total_bytes - estimated_freed) : 0;
        if (remaining <= low_watermark_bytes) break;

        evicting.insert(config_id);
     std::map<ImageCacheConfigId, std::set<HardCommitId> >::const_iterator
        cr = config_refs.find(config_id);
        if (cr != config_refs.end()) {
   for (std::set<HardCommitId>::const_iterator commit =
        cr->second.begin(); commit != cr->second.end(); ++commit) {
    if (covered.count(*commit)) continue;
       std::map<HardCommitId, std::set<ImageCacheConfigId> >::
   const_iterator refs = referrers.find(*commit);
      if (refs != referrers.end() &&
       std::includes(evicting.begin(), evicting.end(),
         refs->second.begin(), refs->second.end())) {
        covered.insert(*commit);
std::map<HardCommitId, uint64_t>::const_iterator sz =
     sizes.find(*commit);
    if (sz != sizes.end()) estimated_freed += sz->second;
    }
          }
        }
     CapacityEvictionCandidate candidate(config_id, used);
        plan.candidates.push_back(candidate);
    }

    return plan;
}

// ---- gc.rs ----

ImageCacheGcSummary SummaryFromReport(const ImageCacheGcReport& report) {
    ImageCacheGcSummary summary;
    summary.collected = report.collected;
    summary.freed_bytes = report.freed_bytes;
    summary.retained = report.blocked.size();
    return summary;
}

}  // namespace cache
}  // namespace image
}  // namespace agentenv
