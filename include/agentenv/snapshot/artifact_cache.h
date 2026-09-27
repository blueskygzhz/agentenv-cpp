// SPDX-License-Identifier: MIT
// Rust: src/snapshot/artifact_cache.rs
#ifndef AGENTENV_SNAPSHOT_ARTIFACT_CACHE_H_
#define AGENTENV_SNAPSHOT_ARTIFACT_CACHE_H_

#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace snapshot {

/// Rust struct `ArtifactCache` — content-addressable local cache for snapshot artifacts.
class ArtifactCache {
 public:
    virtual ~ArtifactCache() {}
    virtual core::Expected<std::string, std::string>
        Fetch(const std::string& digest, const std::string& dest_path) = 0;
    virtual core::Expected<core::Unit, std::string>
        Put(const std::string& digest, const std::string& src_path) = 0;
    virtual bool Has(const std::string& digest) const = 0;
};

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_ARTIFACT_CACHE_H_
