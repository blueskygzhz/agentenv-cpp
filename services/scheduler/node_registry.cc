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

bool NodeRegistry::Resolve(const std::string& node_id, Node* out) const {
    std::lock_guard<std::mutex> g(mu_);
    std::map<std::string, RichNode>::const_iterator it = nodes_.find(node_id);
    if (it == nodes_.end()) return false;
    if (out) *out = it->second.node;
    return true;
}

std::vector<Node> NodeRegistry::ListP2pPeers(const std::string& cluster_id,
                                             const std::string& backend,
                                             const std::string& exclude_node_id) const {
    std::lock_guard<std::mutex> g(mu_);
    return FilterP2pPeersLocked(cluster_id, backend, NULL, exclude_node_id);
}

std::vector<Node> NodeRegistry::FilterP2pPeers(
    const std::string& cluster_id, const std::string& backend,
    const std::vector<std::string>& node_ids,
    const std::string& exclude_node_id) const {
    // Go returns nil for an empty allow-list, before taking the lock. The
    // distinction matters: a lookup that found no holders must yield nothing,
    // not fall through to "no restriction" and return the whole cluster.
    if (node_ids.empty()) return std::vector<Node>();

    std::set<std::string> allowed(node_ids.begin(), node_ids.end());
    std::lock_guard<std::mutex> g(mu_);
    return FilterP2pPeersLocked(cluster_id, backend, &allowed, exclude_node_id);
}

std::vector<Node> NodeRegistry::FilterP2pPeersLocked(
    const std::string& cluster_id, const std::string& backend,
    const std::set<std::string>* allowed,
    const std::string& exclude_node_id) const {
    std::vector<Node> out;
    out.reserve(nodes_.size());

    for (std::map<std::string, RichNode>::const_iterator it = nodes_.begin();
         it != nodes_.end(); ++it) {
        const Node& node = it->second.node;

        // Go: an empty requested cluster id matches every node, rather than
        // only nodes with no cluster. A caller that omits it is asking a
        // question about the whole scheduler.
        if (!cluster_id.empty() && node.cluster_id != cluster_id) continue;
        if (allowed != NULL && allowed->find(node.id) == allowed->end()) continue;
        // The requester excludes itself. Returning it would have a node fetch
        // from itself over the network, which is both slower than reading its
        // own store and a loop waiting to be entered.
        if (node.id == exclude_node_id) continue;

        // A node with no P2P endpoint is not a peer. This is how a node that
        // runs with p2p disabled stays out of everyone's peer list while
        // remaining schedulable for placement.
        if (node.p2p_backend.empty() || node.p2p_address.empty()) continue;
        if (!backend.empty() && node.p2p_backend != backend) continue;

        out.push_back(node);
    }
    return out;
}

}  // namespace scheduler
}  // namespace services
}  // namespace agentenv
