// SPDX-License-Identifier: MIT
// Rust: src/p2p/discovery/mod.rs — peer discovery trait + hint merging.
#ifndef AGENTENV_P2P_DISCOVERY_H_
#define AGENTENV_P2P_DISCOVERY_H_

#include <functional>
#include <string>
#include <vector>

#include "agentenv/p2p/error.h"
#include "agentenv/p2p/types.h"

namespace agentenv {
namespace p2p {

/// Rust `merge_hints_with_discovered_peers` — hints first (in order), then
/// discovered peers, deduplicated by node_id AND endpoint. A hint with an
/// endpoint but no node_id falls back to using the endpoint address as node_id;
/// a hint with only a node_id resolves against discovered peers (dropped if
/// absent).
std::vector<P2pPeer>
    MergeHintsWithDiscoveredPeers(const std::vector<P2pArtifactProviderHint>& hints,
       const std::vector<P2pPeer>& discovered);

/// Rust trait `P2pPeerDiscovery`.
class P2pPeerDiscovery {
 public:
    virtual ~P2pPeerDiscovery() {}
    virtual P2pResult<std::vector<P2pPeer> > Peers() = 0;
    virtual P2pResult<std::vector<P2pPeer> > PeersForKey(const P2pArtifactKey& key) = 0;
    virtual P2pResult<core::Unit> RecordKey(const P2pArtifactKey& key) = 0;
    virtual P2pResult<core::Unit> ForgetKey(const P2pArtifactKey& key) = 0;

/// Rust default method `peers_with_hints`.
    virtual P2pResult<std::vector<P2pPeer> >
    PeersWithHints(const std::vector<P2pArtifactProviderHint>& hints);
};

/// Rust `NoopP2pPeerDiscovery`.
class NoopP2pPeerDiscovery : public P2pPeerDiscovery {
 public:
 P2pResult<std::vector<P2pPeer> > Peers() override { return std::vector<P2pPeer>(); }
    P2pResult<std::vector<P2pPeer> > PeersForKey(const P2pArtifactKey&) override {
        return std::vector<P2pPeer>();
    }
    P2pResult<core::Unit> RecordKey(const P2pArtifactKey&) override { return core::Unit{}; }
    P2pResult<core::Unit> ForgetKey(const P2pArtifactKey&) override { return core::Unit{}; }
};

/// Rust `StaticP2pPeerDiscovery`.
class StaticP2pPeerDiscovery : public P2pPeerDiscovery {
 public:
    explicit StaticP2pPeerDiscovery(std::vector<P2pPeer> peers) : peers_(std::move(peers)) {}
    P2pResult<std::vector<P2pPeer> > Peers() override { return peers_; }
P2pResult<std::vector<P2pPeer> > PeersForKey(const P2pArtifactKey&) override { return peers_; }
    P2pResult<core::Unit> RecordKey(const P2pArtifactKey&) override { return core::Unit{}; }
    P2pResult<core::Unit> ForgetKey(const P2pArtifactKey&) override { return core::Unit{}; }
 private:
    std::vector<P2pPeer> peers_;
};

/// Rust: discovery/scheduler.rs :: scheduler::P2pPeer (wire type; endpoint opt).
struct SchedulerWirePeer {
    std::string node_id;
    bool        has_endpoint = false;
 P2pEndpoint endpoint;
};

/// Rust: discovery/scheduler.rs :: filter_scheduler_p2p_peers — keep only peers
/// whose endpoint backend matches and whose address is non-empty.
std::vector<P2pPeer>
    FilterSchedulerP2pPeers(const std::vector<SchedulerWirePeer>& peers,
               const std::string& backend);

/// Rust: discovery/scheduler.rs :: SchedulerPeerDiscovery.
///
/// The gRPC transport is gated (AGENTENV_WITH_GRPC); without it the discovery is
/// backed by injectable "wire" callbacks so the caching + filtering +
/// record/forget bookkeeping (the pure part) is fully real and testable.
class SchedulerPeerDiscovery : public P2pPeerDiscovery {
 public:
    typedef std::function<bool(const std::string& cluster_id,
       const std::string& backend,
    const std::string& exclude_node_id,
   std::vector<SchedulerWirePeer>* out)> WireFetch;
 typedef std::function<bool(const std::string& cluster_id,
   const std::string& backend,
     const std::string& key,
      const std::string& node_id,
 bool record)> WireRecord;

    SchedulerPeerDiscovery(std::string local_node_id, std::string cluster_id,
    bool has_backend, std::string backend,
         WireFetch fetch, WireRecord record);

    /// Rust `refresh_scheduler_peers` body (one iteration): pull + cache.
    void Refresh();

    P2pResult<std::vector<P2pPeer> > Peers() override;
    P2pResult<std::vector<P2pPeer> > PeersForKey(const P2pArtifactKey& key) override;
    P2pResult<core::Unit> RecordKey(const P2pArtifactKey& key) override;
  P2pResult<core::Unit> ForgetKey(const P2pArtifactKey& key) override;

 private:
    std::string local_node_id_;
    std::string cluster_id_;
    bool        has_backend_;
    std::string backend_;
    WireFetch   fetch_;
    WireRecord  record_;
    std::vector<P2pPeer> peers_;
};

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_DISCOVERY_H_
