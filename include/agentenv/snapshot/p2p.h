// SPDX-License-Identifier: MIT
// Rust: src/snapshot/p2p.rs — bridges snapshot manager to the p2p transport.
#ifndef AGENTENV_SNAPSHOT_P2P_H_
#define AGENTENV_SNAPSHOT_P2P_H_

#include <memory>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/p2p/transport.h"

namespace agentenv {
namespace snapshot {

/// Rust trait bridge: publish and fetch snapshot artifacts via the p2p transport.
class P2pSnapshotBridge {
 public:
    virtual ~P2pSnapshotBridge() {}
    virtual core::Expected<std::string, std::string>
        Publish(const std::string& snapshot_id, const std::string& local_path) = 0;
    virtual core::Expected<std::string, std::string>
        Fetch(const std::string& snapshot_id, const std::string& dest_path) = 0;
};

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_P2P_H_
