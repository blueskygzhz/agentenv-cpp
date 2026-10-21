// SPDX-License-Identifier: MIT
// Rust: src/p2p/discovery/mod.rs — peer discovery trait + hint merging.
#ifndef AGENTENV_P2P_DISCOVERY_H_
#define AGENTENV_P2P_DISCOVERY_H_

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agentenv/core/optional.h"
#include "agentenv/core/rwlock.h"
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
    /// Rust `REFRESH_RPC_TIMEOUT` — the `list_p2p_peers` deadline.
    static const uint64_t kRefreshRpcTimeoutMs = 10000;
    /// Rust `ARTIFACT_RPC_TIMEOUT` — the per-key lookup/record/forget deadline.
    static const uint64_t kArtifactRpcTimeoutMs = 5000;

    /// Rust `list_p2p_peers` RPC.
    typedef std::function<bool(const std::string& cluster_id,
      const std::string& backend,
    const std::string& exclude_node_id,
   std::vector<SchedulerWirePeer>* out)> WireFetch;
    /// Rust `record_p2p_artifact` / `forget_p2p_artifact` RPCs (`record` selects).
 typedef std::function<bool(const std::string& cluster_id,
   const std::string& backend,
     const std::string& key,
      const std::string& node_id,
 bool record)> WireRecord;
    /// Rust `lookup_p2p_artifact` RPC — per-key provider lookup. Distinct from
    /// `WireFetch`: it is keyed and is NOT served from the refresh cache.
    typedef std::function<bool(const std::string& cluster_id,
       const std::string& backend,
       const std::string& key,
       const std::string& exclude_node_id,
       std::vector<SchedulerWirePeer>* out)> WireLookup;

    SchedulerPeerDiscovery(std::string local_node_id, std::string cluster_id,
    bool has_backend, std::string backend,
         WireFetch fetch, WireRecord record);

    /// Rust `SchedulerPeerDiscovery::start` — the assembly entry point.
    ///
    /// Behavioural contract, in this order:
    ///  1. If `scheduler_endpoint` fails to parse, warn and return a
    ///     `NoopP2pPeerDiscovery` (Rust `discovery/scheduler.rs:47-57`). The node
    ///     still runs; it just never discovers peers.
    ///  2. Otherwise install the wire callbacks and start the background refresh
    ///     task, whose first poll is jittered over `[0, refresh_interval)`.
    ///
    /// `backend` is `None` for a disabled transport; the returned object then
    /// short-circuits every scheduler RPC, exactly as Rust does.
    static std::shared_ptr<P2pPeerDiscovery> Start(
        const std::string& scheduler_endpoint,
        std::string local_node_id,
        std::string cluster_id,
        uint64_t refresh_interval_secs,
        core::Optional<std::string> backend,
        WireFetch fetch, WireRecord record, WireLookup lookup);

    /// Stops and joins the refresh thread. Rust `impl Drop for
    /// SchedulerPeerDiscovery { refresh_task.abort() }`.
    virtual ~SchedulerPeerDiscovery();

    /// Installs the `lookup_p2p_artifact` transport used by `PeersForKey`.
    void SetWireLookup(WireLookup lookup);

    /// Rust `refresh_scheduler_peers` body (one iteration): pull + cache.
    /// On RPC failure the previously cached peer list is kept (Rust logs and
    /// sleeps rather than clearing).
    void Refresh();

    /// Blocks until the refresh thread has observed `stop_`. Exposed for tests
    /// and for callers that want a deterministic shutdown before destruction.
    void Stop();

    P2pResult<std::vector<P2pPeer> > Peers() override;
    P2pResult<std::vector<P2pPeer> > PeersForKey(const P2pArtifactKey& key) override;
    P2pResult<core::Unit> RecordKey(const P2pArtifactKey& key) override;
  P2pResult<core::Unit> ForgetKey(const P2pArtifactKey& key) override;

 private:
    /// Rust `refresh_scheduler_peers` — the `loop { list; sleep }` task body.
    void RunRefreshLoop(uint64_t refresh_interval_secs);
    /// Spawns `RunRefreshLoop`. Separate from the constructor so the thread never
    /// observes a half-built `this`.
    void StartRefreshLoop(uint64_t refresh_interval_secs);

    std::string local_node_id_;
    std::string cluster_id_;
    bool        has_backend_;
    std::string backend_;
    WireFetch   fetch_;
    WireRecord  record_;
    WireLookup  lookup_;

    /// Rust `Arc<RwLock<Vec<P2pPeer>>>`. The lock is not decorative: the refresh
    /// thread writes while `Peers()` reads on request paths.
    mutable core::RwLock      peers_lock_;
    std::vector<P2pPeer>      peers_;

    std::thread               refresh_thread_;
    mutable std::mutex        stop_mutex_;
    std::condition_variable   stop_cv_;
    bool                      stop_;
    /// Serialises `refresh_thread_.join()`; see `Stop()`.
    mutable std::mutex        join_mutex_;
};

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_DISCOVERY_H_
