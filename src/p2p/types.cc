// SPDX-License-Identifier: MIT
// Rust: src/p2p/types.rs
#include "agentenv/p2p/types.h"

#include <cstdio>

namespace agentenv {
namespace p2p {

std::string P2pPublishSource::ToString() const {
    if (kind == Path) return path;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "<%zu bytes>", bytes.size());
    return buf;
}

P2pPublishRequest P2pPublishRequest::File(P2pArtifactKey key, std::string path) {
    P2pPublishRequest r;
    r.key = std::move(key);
    r.source = P2pPublishSource::FromPath(std::move(path));
    r.metadata_json = "null";
    r.publish_mode = P2pPublishMode::Copy;
    return r;
}

P2pPublishRequest P2pPublishRequest::FromBytes(P2pArtifactKey key, std::vector<uint8_t> bytes) {
  P2pPublishRequest r;
    r.key = std::move(key);
    r.source = P2pPublishSource::FromBytes(std::move(bytes));
    r.metadata_json = "null";
    r.publish_mode = P2pPublishMode::Copy;
  return r;
}

P2pPublishRequest& P2pPublishRequest::WithMetadata(std::string metadata_json_value) {
    metadata_json = std::move(metadata_json_value);
    return *this;
}

P2pPublishRequest& P2pPublishRequest::WithPublishMode(P2pPublishMode mode) {
    publish_mode = mode;
    return *this;
}

}  // namespace p2p
}  // namespace agentenv
