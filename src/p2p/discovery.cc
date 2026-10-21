// SPDX-License-Identifier: MIT
// Rust: src/p2p/discovery/{mod,scheduler}.rs
#include "agentenv/p2p/discovery.h"

#include <chrono>
#include <map>
#include <random>
#include <set>

#include "agentenv/core/logging.h"
#include "agentenv/core/url.h"

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
      lookup_(),
      peers_lock_(),
      peers_(),
      refresh_thread_(),
      stop_mutex_(),
      stop_cv_(),
      stop_(false),
      join_mutex_() {}

SchedulerPeerDiscovery::~SchedulerPeerDiscovery() {
    Stop();
}

void SchedulerPeerDiscovery::Stop() {
    {
        std::lock_guard<std::mutex> guard(stop_mutex_);
        stop_ = true;
    }
    stop_cv_.notify_all();
    // Join exactly once. Rust gets this for free because `Drop` runs once;
    // here `Stop()` is public, and joining the same thread from two callers is
    // undefined behaviour, so the join is claimed under its own lock.
    std::lock_guard<std::mutex> join_guard(join_mutex_);
    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }
}

std::shared_ptr<P2pPeerDiscovery> SchedulerPeerDiscovery::Start(
    const std::string& scheduler_endpoint,
    std::string local_node_id,
    std::string cluster_id,
    uint64_t refresh_interval_secs,
    core::Optional<std::string> backend,
    WireFetch fetch, WireRecord record, WireLookup lookup) {
    // Rust `GrpcEndpoint::from_shared(scheduler_endpoint)`: a malformed endpoint
    // disables discovery rather than failing the node, and it warns while doing
    // so. `core::Url::Parse` stands in for tonic's URI parse; it requires an
    // absolute URL with a non-empty host, which is the same class of check.
    core::Expected<core::Url, std::string> parsed = core::Url::Parse(scheduler_endpoint);
    if (!parsed.ok()) {
        AGENTENV_WARN("P2P scheduler discovery disabled because scheduler endpoint is invalid"
                      << " scheduler_endpoint=" << scheduler_endpoint
                      << " error=" << parsed.error());
        return std::shared_ptr<P2pPeerDiscovery>(new NoopP2pPeerDiscovery());
    }

    const bool has_backend = backend.has_value();
    // Rust floors the interval in `ResolvedP2pConfig::from_config`, so the loop
    // below always receives >= 1s and never re-floors it. Doing it once, where
    // the config is resolved, keeps the logged value and the behaviour in
    // agreement; a second floor here would let a 0 reach the log while the loop
    // silently used 1s.
    const uint64_t interval_secs = refresh_interval_secs < 1 ? 1 : refresh_interval_secs;
    AGENTENV_INFO("starting scheduler P2P peer discovery"
                  << " scheduler_endpoint=" << scheduler_endpoint
                  << " local_node_id=" << local_node_id
                  << " cluster_id=" << cluster_id
                  << " backend=" << (has_backend ? *backend : std::string())
                  << " refresh_interval_secs=" << interval_secs);

    std::shared_ptr<SchedulerPeerDiscovery> discovery(new SchedulerPeerDiscovery(
        std::move(local_node_id), std::move(cluster_id), has_backend,
        has_backend ? *backend : std::string(), std::move(fetch), std::move(record)));
    discovery->SetWireLookup(std::move(lookup));
    discovery->StartRefreshLoop(interval_secs);
    return discovery;
}

void SchedulerPeerDiscovery::StartRefreshLoop(uint64_t refresh_interval_secs) {
    if (refresh_thread_.joinable()) return;
    refresh_thread_ = std::thread(&SchedulerPeerDiscovery::RunRefreshLoop, this,
                                  refresh_interval_secs);
}

void SchedulerPeerDiscovery::RunRefreshLoop(uint64_t refresh_interval_secs) {
    // `Start()` already applied Rust's >= 1s floor, which lives in
    // `ResolvedP2pConfig::from_config`. This is a last-resort guard so a direct
    // constructor call cannot produce a zero interval (which would spin).
    const uint64_t interval_secs = refresh_interval_secs < 1 ? 1 : refresh_interval_secs;
    const uint64_t interval_ms = interval_secs * 1000;

    // Rust: "Spread initial polls across up to one full refresh interval to
    // avoid a thundering herd when many nodes start simultaneously."
    // `rand::rng().random_range(Duration::ZERO..refresh_interval)` — half-open,
    // so the jitter never equals a full interval.
    uint64_t jitter_ms = 0;
    {
        std::random_device seed;
        std::mt19937_64 rng(seed());
        std::uniform_int_distribution<uint64_t> dist(0, interval_ms - 1);
        jitter_ms = dist(rng);
    }

    std::unique_lock<std::mutex> guard(stop_mutex_);
    if (stop_cv_.wait_for(guard, std::chrono::milliseconds(jitter_ms),
                          [this] { return stop_; })) {
        return;
    }

    for (;;) {
        guard.unlock();
        Refresh();
        guard.lock();
        // Rust `sleep(refresh_interval)`; here the sleep is interruptible so
        // shutdown does not wait out a full interval. Rust aborts the task
        // instead, which has the same effect.
        if (stop_cv_.wait_for(guard, std::chrono::milliseconds(interval_ms),
                              [this] { return stop_; })) {
            return;
        }
    }
}

void SchedulerPeerDiscovery::Refresh() {
    if (!fetch_) {
        // No wire transport compiled in / injected. Rust cannot reach this
        // state: without a scheduler endpoint it builds a Noop discovery
        // instead. Keep the cache and say so, rather than clearing it.
        AGENTENV_DEBUG("failed to refresh P2P peers from scheduler"
                       << " error=no scheduler wire transport");
        return;
    }
    const std::string backend = has_backend_ ? backend_ : std::string();
    std::vector<SchedulerWirePeer> wire;
    if (!fetch_(cluster_id_, backend, local_node_id_, &wire)) {
        // Rust: `debug!(error, "failed to refresh P2P peers from scheduler")`
        // and the previous cache stays in place.
        AGENTENV_DEBUG("failed to refresh P2P peers from scheduler"
                       << " error=list P2P peers from scheduler");
        return;
    }
    std::vector<P2pPeer> filtered = FilterSchedulerP2pPeers(wire, backend);
    AGENTENV_TRACE("refreshed P2P peers from scheduler"
                   << " peer_count=" << filtered.size());
    // On success replace the cache (Rust overwrites; on error keeps old).
    core::WriteGuard write(peers_lock_);
    peers_.swap(filtered);
}

P2pResult<std::vector<P2pPeer> > SchedulerPeerDiscovery::Peers() {
    core::ReadGuard read(peers_lock_);
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
    // A missing wire transport is a failed RPC, not a silent success. Rust has
    // no "callback absent" state — it always holds a client — so reporting the
    // operation as having happened would invent a result. This matches the
    // `PeersForKey` treatment of an absent `lookup_`.
    if (!record_) {
        return core::make_unexpected(P2pError::MakeInternal("record P2P artifact in scheduler"));
    }
    if (!record_(cluster_id_, backend_, key, local_node_id_, true)) {
        return core::make_unexpected(P2pError::MakeInternal("record P2P artifact in scheduler"));
    }
    return core::Unit{};
}

P2pResult<core::Unit> SchedulerPeerDiscovery::ForgetKey(const P2pArtifactKey& key) {
    if (!has_backend_) return core::Unit{};
    if (!record_) {
        return core::make_unexpected(P2pError::MakeInternal("forget P2P artifact in scheduler"));
    }
    if (!record_(cluster_id_, backend_, key, local_node_id_, false)) {
        return core::make_unexpected(P2pError::MakeInternal("forget P2P artifact in scheduler"));
    }
    return core::Unit{};
}

}  // namespace p2p
}  // namespace agentenv
