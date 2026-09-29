// SPDX-License-Identifier: MIT
// Rust: src/p2p/discovery/mod.rs
#include "agentenv/p2p/discovery.h"

#include <map>
#include <set>

namespace agentenv {
namespace p2p {

namespace {
// Rust `peer_from_hint`.
bool PeerFromHint(const P2pArtifactProviderHint& hint,
       const std::map<std::string, const P2pPeer*>& discovered_by_node_id,
     P2pPeer* out) {
    if (hint.has_endpoint) {
        std::string node_id = hint.has_node_id ? hint.node_id : hint.endpoint.address;
   out->node_id = node_id;
     out->endpoint = hint.endpoint;
        return true;
    }
    if (!hint.has_node_id) return false;
    std::map<std::string, const P2pPeer*>::const_iterator it =
        discovered_by_node_id.find(hint.node_id);
    if (it == discovered_by_node_id.end()) return false;
    *out = *it->second;
    return true;
}

// Rust `push_unique_peer` — dedup by node_id AND endpoint.
void PushUniquePeer(std::vector<P2pPeer>* peers,
      std::set<std::string>* seen_node_ids,
     std::set<std::pair<std::string, std::string> >* seen_endpoints,
  const P2pPeer& peer) {
    std::pair<std::string, std::string> ep(peer.endpoint.backend, peer.endpoint.address);
    if (seen_node_ids->count(peer.node_id) || seen_endpoints->count(ep)) {
        return;
    }
  seen_node_ids->insert(peer.node_id);
    seen_endpoints->insert(ep);
    peers->push_back(peer);
}
}  // namespace

std::vector<P2pPeer>
MergeHintsWithDiscoveredPeers(const std::vector<P2pArtifactProviderHint>& hints,
      const std::vector<P2pPeer>& discovered) {
    std::map<std::string, const P2pPeer*> discovered_by_node_id;
    for (size_t i = 0; i < discovered.size(); ++i) {
        // or_insert: first occurrence wins.
        if (!discovered_by_node_id.count(discovered[i].node_id)) {
     discovered_by_node_id[discovered[i].node_id] = &discovered[i];
        }
}

    std::vector<P2pPeer> peers;
 std::set<std::string> seen_node_ids;
    std::set<std::pair<std::string, std::string> > seen_endpoints;

    for (size_t i = 0; i < hints.size(); ++i) {
        P2pPeer peer;
        if (PeerFromHint(hints[i], discovered_by_node_id, &peer)) {
   PushUniquePeer(&peers, &seen_node_ids, &seen_endpoints, peer);
     }
    }
 for (size_t i = 0; i < discovered.size(); ++i) {
    PushUniquePeer(&peers, &seen_node_ids, &seen_endpoints, discovered[i]);
    }
    return peers;
}

P2pResult<std::vector<P2pPeer> >
P2pPeerDiscovery::PeersWithHints(const std::vector<P2pArtifactProviderHint>& hints) {
    P2pResult<std::vector<P2pPeer> > discovered = Peers();
  if (!discovered.ok()) return discovered;
    return MergeHintsWithDiscoveredPeers(hints, discovered.value());
}

// ---- scheduler.rs ----
std::vector<P2pPeer>
FilterSchedulerP2pPeers(const std::vector<SchedulerWirePeer>& peers,
    const std::string& backend) {
    std::vector<P2pPeer> out;
    for (size_t i = 0; i < peers.size(); ++i) {
    const SchedulerWirePeer& wp = peers[i];
        if (!wp.has_endpoint) continue;  // endpoint? => None
        if (wp.endpoint.backend != backend) continue; // backend mismatch
  if (wp.endpoint.address.empty()) continue;    // empty address
        P2pPeer p;
        p.node_id = wp.node_id;
        p.endpoint = wp.endpoint;
    out.push_back(p);
    }
    return out;
}

SchedulerPeerDiscovery::SchedulerPeerDiscovery(std::string local_node_id,
             std::string cluster_id,
  bool has_backend, std::string backend,
         WireFetch fetch, WireRecord record)
    : local_node_id_(std::move(local_node_id)),
      cluster_id_(std::move(cluster_id)),
      has_backend_(has_backend),
      backend_(std::move(backend)),
      fetch_(std::move(fetch)),
 record_(std::move(record)),
      lookup_() {}

void SchedulerPeerDiscovery::Refresh() {
    if (!fetch_) return;
 std::vector<SchedulerWirePeer> wire;
    if (fetch_(cluster_id_, has_backend_ ? backend_ : std::string(),
    local_node_id_, &wire)) {
        // On success replace the cache (Rust overwrites; on error keeps old).
 peers_ = FilterSchedulerP2pPeers(wire, has_backend_ ? backend_ : std::string());
    }
}

P2pResult<std::vector<P2pPeer> > SchedulerPeerDiscovery::Peers() {
    return peers_;
}

void SchedulerPeerDiscovery::SetWireLookup(WireLookup lookup) {
    lookup_ = std::move(lookup);
}

P2pResult<std::vector<P2pPeer> >
SchedulerPeerDiscovery::PeersForKey(const P2pArtifactKey& key) {
    // Rust: `let Some(backend) = self.backend.as_deref() else { return Ok(vec![]) }`.
    if (!has_backend_) return std::vector<P2pPeer>();
    // Rust issues `lookup_p2p_artifact` and filters the response. It never
    // falls back to the refresh cache: an unavailable scheduler is an error,
    // not "no providers", so the caller can distinguish the two.
    if (!lookup_) {
        return core::make_unexpected(
            P2pError::MakeInternal("lookup P2P artifact in scheduler"));
    }
    std::vector<SchedulerWirePeer> wire;
    if (!lookup_(cluster_id_, backend_, key, local_node_id_, &wire)) {
        return core::make_unexpected(
            P2pError::MakeInternal("lookup P2P artifact in scheduler"));
    }
    return FilterSchedulerP2pPeers(wire, backend_);
}

P2pResult<core::Unit> SchedulerPeerDiscovery::RecordKey(const P2pArtifactKey& key) {
    if (!has_backend_) return core::Unit{};
 if (record_ && !record_(cluster_id_, backend_, key, local_node_id_, true)) {
    return core::make_unexpected(P2pError::MakeInternal("record P2P artifact in scheduler"));
    }
    return core::Unit{};
}

P2pResult<core::Unit> SchedulerPeerDiscovery::ForgetKey(const P2pArtifactKey& key) {
    if (!has_backend_) return core::Unit{};
    if (record_ && !record_(cluster_id_, backend_, key, local_node_id_, false)) {
        return core::make_unexpected(P2pError::MakeInternal("forget P2P artifact in scheduler"));
    }
    return core::Unit{};
}

}  // namespace p2p
}  // namespace agentenv
