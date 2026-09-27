// SPDX-License-Identifier: MIT
// Go: services/scheduler/internal/node_registry.go
#include "services/scheduler/internal.h"

namespace agentenv {
namespace services {
namespace scheduler {

void NodeRegistry::Upsert(const Node& node) {
    std::lock_guard<std::mutex> g(mu_);
    std::map<std::string, RichNode>::iterator it = nodes_.find(node.id);
    if (it == nodes_.end()) {
        RichNode rn;
        rn.node = node;
        nodes_[node.id] = rn;
    } else {
        it->second.node = node;
    }
}

void NodeRegistry::RecordHeartbeat(const std::string& node_id, const NodeSnapshot& snap) {
    std::lock_guard<std::mutex> g(mu_);
    std::map<std::string, RichNode>::iterator it = nodes_.find(node_id);
    if (it == nodes_.end()) return;
    it->second.has_snapshot = true;
    it->second.snapshot = snap;
}

void NodeRegistry::Remove(const std::string& node_id) {
    std::lock_guard<std::mutex> g(mu_);
    nodes_.erase(node_id);
}

std::vector<RichNode> NodeRegistry::RichNodes() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<RichNode> out;
    out.reserve(nodes_.size());
    for (std::map<std::string, RichNode>::const_iterator it = nodes_.begin();
         it != nodes_.end(); ++it) {
        out.push_back(it->second);
    }
    return out;
}

}  // namespace scheduler
}  // namespace services
}  // namespace agentenv
