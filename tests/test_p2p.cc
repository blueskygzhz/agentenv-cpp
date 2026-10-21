// SPDX-License-Identifier: MIT
// Tests for p2p: MockTransport (mock.rs), ResolvedP2pConfig (config.rs),
// and discovery hint merging (discovery/mod.rs).
#include "microtest.h"

#include <unistd.h>

#include <atomic>
#include <thread>
#include <cstdio>

#include "agentenv/p2p/all.h"
#include "agentenv/p2p/discovery.h"
#include "agentenv/p2p/node.h"
using namespace agentenv::p2p;

// ---- config.rs ----
MT_TEST(resolved_config_disabled_overrides_transport) {
    P2pRawConfig c;
    c.enabled = false;
    c.transport = P2pTransportKind::Iroh;
    ResolvedP2pConfig r = ResolvedP2pConfig::FromConfig(c);
    MT_EXPECT_TRUE(r.transport == P2pTransportKind::Disabled);
    // Rust `backend_id()` is `Option<&str>`: Disabled is None, not Some("").
    MT_EXPECT_TRUE(!BackendId(r.transport).has_value());
}

MT_TEST(resolved_config_enabled_iroh) {
    P2pRawConfig c;
    c.enabled = true;
    c.transport = P2pTransportKind::Iroh;
    c.listen_addr = "  0.0.0.0:4433  ";
    c.peer_discovery_refresh_interval_secs = 0;  // floored to 1
    ResolvedP2pConfig r = ResolvedP2pConfig::FromConfig(c);
    MT_EXPECT_TRUE(r.transport == P2pTransportKind::Iroh);
    MT_EXPECT_TRUE(BackendId(r.transport).has_value());
    MT_EXPECT_TRUE(*BackendId(r.transport) == "iroh");
    MT_EXPECT_TRUE(r.has_listen_addr);
    MT_EXPECT_TRUE(r.listen_addr == "0.0.0.0:4433");   // trimmed
    MT_EXPECT_TRUE(static_cast<int>(r.peer_discovery_refresh_interval_secs) == 1);
}

MT_TEST(resolved_config_empty_listen_addr_is_none) {
    P2pRawConfig c;
    c.enabled = true;
    c.listen_addr = "   ";
    ResolvedP2pConfig r = ResolvedP2pConfig::FromConfig(c);
    MT_EXPECT_TRUE(!r.has_listen_addr);
}

// ---- mock.rs ----
MT_TEST(mock_publish_lookup_fetch_roundtrip) {
    MockTransport t;
    std::vector<uint8_t> data;
    for (int i = 0; i < 100; ++i) data.push_back(static_cast<uint8_t>(i));
    P2pPublishRequest req = P2pPublishRequest::FromBytes("art-1", data);

    MT_EXPECT_TRUE(t.Publish(req).ok());
    MT_EXPECT_EQ(t.PublishCount(), 1);

    P2pArtifactDescriptor desc;
    auto found = t.LookupWithHints("art-1", std::vector<P2pArtifactProviderHint>(), &desc);
    MT_EXPECT_TRUE(found.ok() && found.value());
    MT_EXPECT_EQ(t.LookupCount(), 1);
    MT_EXPECT_TRUE(desc.has_backend_locator && desc.backend_locator == "mock");
    MT_EXPECT_TRUE(desc.providers.size() == 1 && desc.providers[0].IsLocal());

    auto bytes = t.FetchBytes(desc);
    MT_EXPECT_TRUE(bytes.ok());
    MT_EXPECT_EQ(static_cast<int>(bytes.value().size()), 100);
    MT_EXPECT_EQ(static_cast<int>(bytes.value()[42]), 42);
}

MT_TEST(mock_lookup_missing_returns_none) {
    MockTransport t;
    P2pArtifactDescriptor desc;
    auto found = t.LookupWithHints("nope", std::vector<P2pArtifactProviderHint>(), &desc);
    MT_EXPECT_TRUE(found.ok());
    MT_EXPECT_TRUE(!found.value());  // Ok(None)
}

MT_TEST(mock_forced_failures) {
    MockTransport t;
    t.SetFailLookup(true);
    P2pArtifactDescriptor desc;
    MT_EXPECT_TRUE(!t.LookupWithHints("k", std::vector<P2pArtifactProviderHint>(), &desc).ok());
    t.SetFailPublish(true);
  MT_EXPECT_TRUE(!t.Publish(P2pPublishRequest::FromBytes("k", std::vector<uint8_t>())).ok());
}

MT_TEST(mock_fetch_byte_range_chunks) {
    MockTransport t;
    std::vector<uint8_t> data(2500, 7);  // > 2 chunks of 1024
    t.Publish(P2pPublishRequest::FromBytes("big", data));
    P2pArtifactDescriptor desc;
    t.LookupWithHints("big", std::vector<P2pArtifactProviderHint>(), &desc);

    auto stream = t.FetchByteRange(desc, 0, 2500);
    MT_EXPECT_TRUE(stream.ok());
  size_t total = 0;
    int chunks = 0;
    std::vector<uint8_t> chunk;
    for (;;) {
        auto n = stream.value()->Next(&chunk);
        MT_EXPECT_TRUE(n.ok());
        if (!n.value()) break;
      total += chunk.size();
 ++chunks;
    }
    MT_EXPECT_EQ(static_cast<int>(total), 2500);
    MT_EXPECT_EQ(chunks, 3);  // 1024 + 1024 + 452
}

MT_TEST(mock_fetch_range_out_of_bounds) {
    MockTransport t;
    t.Publish(P2pPublishRequest::FromBytes("small", std::vector<uint8_t>(10, 0)));
    P2pArtifactDescriptor desc;
  t.LookupWithHints("small", std::vector<P2pArtifactProviderHint>(), &desc);
    auto s = t.FetchByteRange(desc, 5, 100);  // 5+100 > 10
    MT_EXPECT_TRUE(!s.ok());
}

MT_TEST(mock_unpublish) {
    MockTransport t;
    t.Publish(P2pPublishRequest::FromBytes("x", std::vector<uint8_t>(1, 9)));
    MT_EXPECT_TRUE(t.HasBlob("x"));
    auto r = t.Unpublish("x");
    MT_EXPECT_TRUE(r.ok() && r.value());
    MT_EXPECT_TRUE(!t.HasBlob("x"));
    auto r2 = t.Unpublish("x");  // idempotent
    MT_EXPECT_TRUE(r2.ok() && !r2.value());
}

MT_TEST(mock_local_endpoint) {
    MockTransport t;
    P2pEndpoint ep;
    MT_EXPECT_TRUE(t.LocalEndpoint(&ep));
    MT_EXPECT_TRUE(ep.backend == "mock" && ep.address == "mock");
}

// ---- discovery/mod.rs ----
static P2pPeer mkpeer(const std::string& node, const std::string& addr) {
    P2pPeer p;
    p.node_id = node;
    p.endpoint.backend = "backend";
  p.endpoint.address = addr;
    return p;
}

MT_TEST(discovery_static_returns_configured) {
    StaticP2pPeerDiscovery d(std::vector<P2pPeer>(1, mkpeer("node-a", "addr-a")));
    auto p = d.Peers();
    MT_EXPECT_TRUE(p.ok());
    MT_EXPECT_EQ(static_cast<int>(p.value().size()), 1);
    MT_EXPECT_TRUE(p.value()[0].node_id == "node-a");
}

MT_TEST(discovery_hints_prioritized_and_deduped) {
 std::vector<P2pPeer> peers;
    peers.push_back(mkpeer("node-a", "addr-a-from-discovery"));
    peers.push_back(mkpeer("node-b", "addr-b"));
    StaticP2pPeerDiscovery d(peers);

    P2pArtifactProviderHint hinted_a;
    hinted_a.has_node_id = true; hinted_a.node_id = "node-a";
    hinted_a.has_endpoint = true;
    hinted_a.endpoint.backend = "backend"; hinted_a.endpoint.address = "addr-a-from-hint";
    P2pArtifactProviderHint hinted_b_by_node;
    hinted_b_by_node.has_node_id = true; hinted_b_by_node.node_id = "node-b";

    std::vector<P2pArtifactProviderHint> hints;
    hints.push_back(hinted_a);
    hints.push_back(hinted_b_by_node);
    auto r = d.PeersWithHints(hints);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(static_cast<int>(r.value().size()), 2);
    MT_EXPECT_TRUE(r.value()[0].node_id == "node-a");
    MT_EXPECT_TRUE(r.value()[0].endpoint.address == "addr-a-from-hint");
    MT_EXPECT_TRUE(r.value()[1].node_id == "node-b");
}

MT_TEST(discovery_hint_endpoint_fallback_node_id) {
    std::vector<P2pPeer> empty;
    StaticP2pPeerDiscovery d(empty);
    P2pArtifactProviderHint hint;
    hint.has_endpoint = true;
    hint.endpoint.backend = "backend"; hint.endpoint.address = "addr-from-hint";
    auto r = d.PeersWithHints(std::vector<P2pArtifactProviderHint>(1, hint));
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(static_cast<int>(r.value().size()), 1);
    MT_EXPECT_TRUE(r.value()[0].node_id == "addr-from-hint");
}

MT_TEST(discovery_ignores_node_only_hint_missing) {
    StaticP2pPeerDiscovery d(std::vector<P2pPeer>(1, mkpeer("node-a", "addr-a")));
    P2pArtifactProviderHint missing;
 missing.has_node_id = true; missing.node_id = "missing-node";
    auto r = d.PeersWithHints(std::vector<P2pArtifactProviderHint>(1, missing));
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(static_cast<int>(r.value().size()), 1);
    MT_EXPECT_TRUE(r.value()[0].node_id == "node-a");
}

MT_TEST(discovery_dedup_by_endpoint) {
    std::vector<P2pPeer> peers;
    peers.push_back(mkpeer("node-from-discovery", "shared-addr"));
    peers.push_back(mkpeer("node-b", "addr-b"));
    StaticP2pPeerDiscovery d(peers);
    P2pArtifactProviderHint hint;
    hint.has_node_id = true; hint.node_id = "node-from-hint";
    hint.has_endpoint = true;
    hint.endpoint.backend = "backend"; hint.endpoint.address = "shared-addr";
    auto r = d.PeersWithHints(std::vector<P2pArtifactProviderHint>(1, hint));
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(static_cast<int>(r.value().size()), 2);
    MT_EXPECT_TRUE(r.value()[0].node_id == "node-from-hint");
    MT_EXPECT_TRUE(r.value()[1].node_id == "node-b");
}

// ---- scheduler.rs ----
static SchedulerWirePeer wpeer(const std::string& node, bool has_ep,
      const std::string& backend, const std::string& addr) {
    SchedulerWirePeer p;
    p.node_id = node;
    p.has_endpoint = has_ep;
    p.endpoint.backend = backend;
    p.endpoint.address = addr;
    return p;
}

MT_TEST(filter_scheduler_peers_keeps_only_matching_ready) {
    std::vector<SchedulerWirePeer> peers;
    peers.push_back(wpeer("node-a", true, "backend", "addr-a"));
    peers.push_back(wpeer("node-b", true, "other", "addr-b"));   // wrong backend
    peers.push_back(wpeer("node-c", true, "backend", ""));       // empty addr
    peers.push_back(wpeer("node-d", false, "", ""));      // no endpoint
    peers.push_back(wpeer("node-e", true, "backend", "addr-e"));
    std::vector<P2pPeer> out = FilterSchedulerP2pPeers(peers, "backend");
    MT_EXPECT_EQ(static_cast<int>(out.size()), 2);
    MT_EXPECT_TRUE(out[0].node_id == "node-a" && out[0].endpoint.address == "addr-a");
    MT_EXPECT_TRUE(out[1].node_id == "node-e" && out[1].endpoint.address == "addr-e");
}

MT_TEST(scheduler_discovery_caches_filtered_peers) {
    SchedulerPeerDiscovery d(
 "self", "cluster-1", true, "backend",
        [](const std::string& cluster, const std::string& backend,
        const std::string& exclude, std::vector<SchedulerWirePeer>* out) -> bool {
     (void)cluster; (void)backend; (void)exclude;
   out->push_back(wpeer("node-a", true, "backend", "addr-a"));
 out->push_back(wpeer("node-x", true, "other", "addr-x"));
            return true;
        },
        SchedulerPeerDiscovery::WireRecord());
    // Before refresh: empty.
    MT_EXPECT_EQ(static_cast<int>(d.Peers().value().size()), 0);
    d.Refresh();
    auto p = d.Peers();
    MT_EXPECT_TRUE(p.ok());
    MT_EXPECT_EQ(static_cast<int>(p.value().size()), 1);  // node-x filtered out
    MT_EXPECT_TRUE(p.value()[0].node_id == "node-a");
}

MT_TEST(scheduler_discovery_record_forget) {
    int record_calls = 0, forget_calls = 0;
    SchedulerPeerDiscovery d(
        "self", "cluster-1", true, "backend",
        SchedulerPeerDiscovery::WireFetch(),
        [&](const std::string&, const std::string&, const std::string& key,
     const std::string& node, bool record) -> bool {
   (void)key; (void)node;
          if (record) ++record_calls; else ++forget_calls;
  return true;
      });
MT_EXPECT_TRUE(d.RecordKey("art-1").ok());
    MT_EXPECT_TRUE(d.ForgetKey("art-1").ok());
    MT_EXPECT_EQ(record_calls, 1);
    MT_EXPECT_EQ(forget_calls, 1);
}

MT_TEST(scheduler_discovery_no_backend_is_noop) {
    SchedulerPeerDiscovery d(
        "self", "cluster-1", false, "",
SchedulerPeerDiscovery::WireFetch(),
  [](const std::string&, const std::string&, const std::string&,
      const std::string&, bool) -> bool { return false; });  // would fail
    // No backend => record/forget are no-ops that succeed, peers_for_key empty.
    MT_EXPECT_TRUE(d.RecordKey("k").ok());
    MT_EXPECT_TRUE(d.ForgetKey("k").ok());
    MT_EXPECT_EQ(static_cast<int>(d.PeersForKey("k").value().size()), 0);
}

// ---- iroh (gated) ----
MT_TEST(iroh_disabled_without_feature) {
    std::string err;
    ResolvedP2pConfig cfg;
    cfg.transport = P2pTransportKind::Iroh;
    cfg.store_dir = "/tmp/store";
  auto t = IrohBlobsP2pTransport::Start(
      cfg, "node-a", std::shared_ptr<P2pPeerDiscovery>(new NoopP2pPeerDiscovery()), &err);
    MT_EXPECT_TRUE(!t);  // not compiled in
  MT_EXPECT_TRUE(err.find("not compiled in") != std::string::npos);

    // A directly-constructed instance returns Disabled for every op.
    IrohBlobsP2pTransport inst;
    P2pArtifactDescriptor desc;
    auto r = inst.LookupWithHints("k", std::vector<P2pArtifactProviderHint>(), &desc);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().kind == P2pError::TransportDisabled);
    P2pEndpoint ep;
    MT_EXPECT_TRUE(!inst.LocalEndpoint(&ep));
}

// ---- error.rs: Display strings must match thiserror verbatim ----
MT_TEST(p2p_error_display_matches_rust) {
    MT_EXPECT_TRUE(P2pError::MakeDisabled().ToString() ==
                   "P2P artifact transport is disabled");
    MT_EXPECT_TRUE(P2pError::Invalid("bad").ToString() ==
                   "invalid P2P artifact descriptor: bad");
    MT_EXPECT_TRUE(P2pError::MakeInvalidCatalog("/var/cat", "torn").ToString() ==
                   "invalid P2P artifact catalog at \"/var/cat\": torn");
    MT_EXPECT_TRUE(P2pError::MakeTimeout("fetch_byte_range").ToString() ==
                   "P2P operation timed out: fetch_byte_range");
    // #[error(transparent)] forwards the inner message with no prefix.
    MT_EXPECT_TRUE(P2pError::MakeInternal("boom").ToString() == "boom");
    MT_EXPECT_TRUE(P2pError::InternalMessage("publish", "io error").ToString() ==
                   "publish: io error");
}

// ---- types.rs: P2pFetchOptions has a hand-written Default (advertise=true) ----
MT_TEST(p2p_fetch_options_default_advertises) {
    MT_EXPECT_TRUE(P2pFetchOptions::Default().advertise);
    MT_EXPECT_TRUE(P2pFetchOptions() == P2pFetchOptions::Default());
}

// ---- transport.rs: provided methods delegate to the *_with_options ones ----
MT_TEST(transport_default_methods_delegate_with_default_options) {
    MockTransport t;
    t.Publish(P2pPublishRequest::FromBytes("k", std::vector<uint8_t>(4, 7)));

    P2pArtifactDescriptor desc;
    // `lookup` == `lookup_with_hints(key, &[])`.
    auto found = t.Lookup("k", &desc);
    MT_EXPECT_TRUE(found.ok() && found.value());

    auto bytes = t.FetchBytes(desc);
    MT_EXPECT_TRUE(bytes.ok());
    MT_EXPECT_EQ(static_cast<int>(bytes.value().size()), 4);
    // The defaulted options must arrive with advertise=true.
    MT_EXPECT_TRUE(t.LastFetchOptions().advertise);

    P2pFetchOptions no_ad;
    no_ad.advertise = false;
    MT_EXPECT_TRUE(t.FetchBytesWithOptions(desc, no_ad).ok());
    MT_EXPECT_TRUE(!t.LastFetchOptions().advertise);

    // `shutdown` defaults to Ok(()).
    MT_EXPECT_TRUE(t.Shutdown().ok());
}

// ---- transport.rs: DisabledP2pTransport ----
MT_TEST(disabled_transport_semantics) {
    DisabledP2pTransport t;
    P2pArtifactDescriptor desc;
    desc.key = "k";

    // lookup => Ok(None), publish/unpublish => Ok, everything else => disabled.
    auto look = t.Lookup("k", &desc);
    MT_EXPECT_TRUE(look.ok() && !look.value());

    auto pub_r = t.Publish(P2pPublishRequest::FromBytes("k", std::vector<uint8_t>()));
    MT_EXPECT_TRUE(pub_r.ok());
    auto unpub = t.Unpublish("k");
    MT_EXPECT_TRUE(unpub.ok() && !unpub.value());

    MT_EXPECT_TRUE(!t.Fetch(desc, "/tmp/x").ok());
    MT_EXPECT_TRUE(t.Fetch(desc, "/tmp/x").error().kind == P2pError::TransportDisabled);
    MT_EXPECT_TRUE(!t.FetchBytes(desc).ok());
    MT_EXPECT_TRUE(!t.FetchByteRange(desc, 0, 1).ok());

    P2pEndpoint ep;
    MT_EXPECT_TRUE(!t.LocalEndpoint(&ep));  // default None
    MT_EXPECT_TRUE(t.Shutdown().ok());
}

// ---- mock.rs: unpublish bookkeeping records every key, removed or not ----
MT_TEST(mock_unpublish_records_all_keys) {
    MockTransport t;
    t.Publish(P2pPublishRequest::FromBytes("a", std::vector<uint8_t>(1, 1)));
    t.Unpublish("a");
    t.Unpublish("missing");

    MT_EXPECT_EQ(t.UnpublishCount(), 2);
    std::vector<P2pArtifactKey> keys = t.UnpublishedKeys();
    MT_EXPECT_EQ(static_cast<int>(keys.size()), 2);
    MT_EXPECT_TRUE(keys[0] == "a");
    MT_EXPECT_TRUE(keys[1] == "missing");
}

// ---- discovery/scheduler.rs: peers_for_key must issue a keyed lookup RPC ----
MT_TEST(scheduler_discovery_peers_for_key_uses_lookup_rpc) {
    std::vector<SchedulerWirePeer> cached;
    SchedulerWirePeer cached_peer;
    cached_peer.node_id = "cached-node";
    cached_peer.has_endpoint = true;
    cached_peer.endpoint.backend = "backend";
    cached_peer.endpoint.address = "cached-addr";
    cached.push_back(cached_peer);

    SchedulerPeerDiscovery d(
        "self", "cluster-1", true, "backend",
        [&cached](const std::string&, const std::string&, const std::string&,
                  std::vector<SchedulerWirePeer>* out) -> bool {
            *out = cached;
            return true;
        },
        SchedulerPeerDiscovery::WireRecord());
    d.Refresh();
    MT_EXPECT_EQ(static_cast<int>(d.Peers().value().size()), 1);

    // Without a lookup transport the call is an error, NOT the refresh cache.
    auto no_transport = d.PeersForKey("art-1");
    MT_EXPECT_TRUE(!no_transport.ok());

    std::string seen_key;
    std::string seen_exclude;
    d.SetWireLookup([&seen_key, &seen_exclude](
                        const std::string& cluster, const std::string& backend,
                        const std::string& key, const std::string& exclude,
                        std::vector<SchedulerWirePeer>* out) -> bool {
        seen_key = key;
        seen_exclude = exclude;
        MT_EXPECT_TRUE(cluster == "cluster-1");
        MT_EXPECT_TRUE(backend == "backend");
        SchedulerWirePeer keyed;
        keyed.node_id = "keyed-node";
        keyed.has_endpoint = true;
        keyed.endpoint.backend = "backend";
        keyed.endpoint.address = "keyed-addr";
        out->push_back(keyed);
        // A mismatched backend must be filtered out of the response.
        SchedulerWirePeer other;
        other.node_id = "other-node";
        other.has_endpoint = true;
        other.endpoint.backend = "other";
        other.endpoint.address = "other-addr";
        out->push_back(other);
        return true;
    });

    auto peers = d.PeersForKey("art-1");
    MT_EXPECT_TRUE(peers.ok());
    MT_EXPECT_EQ(static_cast<int>(peers.value().size()), 1);
    MT_EXPECT_TRUE(peers.value()[0].node_id == "keyed-node");
    MT_EXPECT_TRUE(seen_key == "art-1");
    MT_EXPECT_TRUE(seen_exclude == "self");
}

// Rust has no "client absent" state: it always holds a `SchedulerClient`, so a
// record/forget that cannot reach the scheduler is a failed RPC. Reporting
// success would claim the scheduler was told something it was not.
MT_TEST(scheduler_discovery_record_without_transport_is_an_error) {
    SchedulerPeerDiscovery d(
        "self", "cluster-1", true, "backend",
        SchedulerPeerDiscovery::WireFetch(),
        SchedulerPeerDiscovery::WireRecord());

    auto recorded = d.RecordKey("art-1");
    MT_EXPECT_TRUE(!recorded.ok());
    MT_EXPECT_TRUE(recorded.error().kind == P2pError::Internal);
    MT_EXPECT_TRUE(recorded.error().message == "record P2P artifact in scheduler");

    auto forgotten = d.ForgetKey("art-1");
    MT_EXPECT_TRUE(!forgotten.ok());
    MT_EXPECT_TRUE(forgotten.error().message == "forget P2P artifact in scheduler");
}

// Rust `refresh_scheduler_peers`: on RPC failure it logs at debug and keeps the
// previous cache rather than clearing it.
MT_TEST(scheduler_discovery_failed_refresh_keeps_previous_peers) {
    bool fail = false;
    SchedulerPeerDiscovery d(
        "self", "cluster-1", true, "backend",
        [&fail](const std::string&, const std::string&, const std::string&,
                std::vector<SchedulerWirePeer>* out) -> bool {
            if (fail) return false;
            SchedulerWirePeer p;
            p.node_id = "node-a";
            p.has_endpoint = true;
            p.endpoint.backend = "backend";
            p.endpoint.address = "addr-a";
            out->push_back(p);
            return true;
        },
        SchedulerPeerDiscovery::WireRecord());

    d.Refresh();
    MT_EXPECT_EQ(static_cast<int>(d.Peers().value().size()), 1);

    fail = true;
    d.Refresh();
    MT_EXPECT_EQ(static_cast<int>(d.Peers().value().size()), 1);
    MT_EXPECT_TRUE(d.Peers().value()[0].node_id == "node-a");
}

// Rust `SchedulerPeerDiscovery::start` degrades to a Noop discovery when the
// endpoint does not parse, instead of failing the node.
MT_TEST(scheduler_discovery_start_degrades_on_invalid_endpoint) {
    std::shared_ptr<P2pPeerDiscovery> d = SchedulerPeerDiscovery::Start(
        "not a url", "self", "cluster-1", 1, std::string("backend"),
        SchedulerPeerDiscovery::WireFetch(),
        SchedulerPeerDiscovery::WireRecord(),
        SchedulerPeerDiscovery::WireLookup());

    MT_EXPECT_TRUE(!!d);
    // A Noop discovery answers every call successfully and emptily; a real
    // SchedulerPeerDiscovery would report the missing transport as an error.
    MT_EXPECT_TRUE(d->Peers().ok());
    MT_EXPECT_EQ(static_cast<int>(d->Peers().value().size()), 0);
    MT_EXPECT_TRUE(d->RecordKey("art-1").ok());
}

// The background refresh task must actually poll, and must stop on shutdown.
MT_TEST(scheduler_discovery_start_runs_background_refresh) {
    std::atomic<int> polls(0);
    std::shared_ptr<P2pPeerDiscovery> d = SchedulerPeerDiscovery::Start(
        "http://127.0.0.1:7000", "self", "cluster-1", 1, std::string("backend"),
        [&polls](const std::string&, const std::string&, const std::string&,
                 std::vector<SchedulerWirePeer>* out) -> bool {
            polls.fetch_add(1);
            SchedulerWirePeer p;
            p.node_id = "node-a";
            p.has_endpoint = true;
            p.endpoint.backend = "backend";
            p.endpoint.address = "addr-a";
            out->push_back(p);
            return true;
        },
        SchedulerPeerDiscovery::WireRecord(),
        SchedulerPeerDiscovery::WireLookup());

    // The first poll is jittered over [0, 1s), so wait past one full interval.
    for (int i = 0; i < 200 && polls.load() == 0; ++i) {
        ::usleep(20 * 1000);
    }
    MT_EXPECT_TRUE(polls.load() >= 1);
    MT_EXPECT_EQ(static_cast<int>(d->Peers().value().size()), 1);

    // Destruction must join the thread; a leaked thread would touch freed
    // members. Stop() is idempotent and is what the destructor calls.
    static_cast<SchedulerPeerDiscovery*>(d.get())->Stop();
    const int after_stop = polls.load();
    ::usleep(120 * 1000);
    MT_EXPECT_EQ(polls.load(), after_stop);
}

// ---- mod.rs: the assembly factories ----
MT_TEST(transport_from_config_disabled_yields_disabled_transport) {
    P2pFactoryInputs inputs;
    inputs.p2p.transport = P2pTransportKind::Disabled;
    inputs.node_id = "node-a";
    inputs.cluster_id = "cluster-1";

    auto t = TransportFromConfig(inputs, SchedulerWireTransport());
    MT_EXPECT_TRUE(t.ok());
    MT_EXPECT_TRUE(!!t.value());
    // Rust `DisabledP2pTransport`: lookup is Ok(None), fetch is an error.
    P2pArtifactDescriptor desc;
    auto looked_up = t.value()->LookupWithHints("k", std::vector<P2pArtifactProviderHint>(),
                                                &desc);
    MT_EXPECT_TRUE(looked_up.ok());
    MT_EXPECT_TRUE(!looked_up.value());
    auto fetched = t.value()->FetchWithOptions(desc, "/tmp/x", P2pFetchOptions());
    MT_EXPECT_TRUE(!fetched.ok());
    MT_EXPECT_TRUE(fetched.error().kind == P2pError::TransportDisabled);
}

// Rust `transport_from_config` uses `?` on `IrohBlobsP2pTransport::new`, so a
// node configured for iroh fails to start rather than silently losing p2p.
MT_TEST(transport_from_config_iroh_propagates_start_failure) {
    P2pFactoryInputs inputs;
    inputs.p2p.transport = P2pTransportKind::Iroh;
    inputs.p2p.store_dir = "/tmp/store";
    inputs.node_id = "node-a";
    inputs.cluster_id = "cluster-1";

    auto t = TransportFromConfig(inputs, SchedulerWireTransport());
    MT_EXPECT_TRUE(!t.ok());
    MT_EXPECT_TRUE(t.error().find("not compiled in") != std::string::npos);
}

// Rust `peer_discovery_from_config`: no scheduler endpoint => Noop.
MT_TEST(peer_discovery_from_config_without_scheduler_is_noop) {
    P2pFactoryInputs inputs;
    inputs.p2p.transport = P2pTransportKind::Iroh;
    inputs.node_id = "node-a";
    inputs.cluster_id = "cluster-1";
    // scheduler_endpoint left as None.

    std::shared_ptr<P2pPeerDiscovery> d =
        PeerDiscoveryFromConfig(inputs, SchedulerWireTransport());
    MT_EXPECT_TRUE(!!d);
    MT_EXPECT_TRUE(d->Peers().ok());
    MT_EXPECT_EQ(static_cast<int>(d->Peers().value().size()), 0);
    // Noop: record/forget succeed without a transport.
    MT_EXPECT_TRUE(d->RecordKey("art-1").ok());
    MT_EXPECT_TRUE(d->ForgetKey("art-1").ok());
}

// A disabled transport has no backend id, so the discovery issues no keyed
// scheduler RPCs at all (Rust: `backend: None` short-circuits).
MT_TEST(peer_discovery_from_config_disabled_transport_has_no_backend) {
    P2pFactoryInputs inputs;
    inputs.p2p.transport = P2pTransportKind::Disabled;
    inputs.p2p.peer_discovery_refresh_interval_secs = 1;
    inputs.scheduler_endpoint = std::string("http://127.0.0.1:7000");
    inputs.node_id = "node-a";
    inputs.cluster_id = "cluster-1";

    bool lookup_called = false;
    SchedulerWireTransport wire;
    wire.lookup = [&lookup_called](const std::string&, const std::string&,
                                   const std::string&, const std::string&,
                                   std::vector<SchedulerWirePeer>*) -> bool {
        lookup_called = true;
        return true;
    };

    std::shared_ptr<P2pPeerDiscovery> d = PeerDiscoveryFromConfig(inputs, std::move(wire));
    MT_EXPECT_TRUE(!!d);

    auto peers = d->PeersForKey("art-1");
    MT_EXPECT_TRUE(peers.ok());
    MT_EXPECT_EQ(static_cast<int>(peers.value().size()), 0);
    MT_EXPECT_TRUE(!lookup_called);
    // record/forget are no-ops too, not errors.
    MT_EXPECT_TRUE(d->RecordKey("art-1").ok());

    static_cast<SchedulerPeerDiscovery*>(d.get())->Stop();
}

// The refresh thread writes `peers_` while request paths read it. Rust holds an
// `Arc<RwLock<Vec<P2pPeer>>>` for exactly this reason. TSan is unavailable in
// this toolchain (its link step needs -pie), so the race is instead exercised
// directly: a writer churning the cache against concurrent readers, asserting
// every read observes a complete, self-consistent snapshot rather than a torn
// vector.
MT_TEST(scheduler_discovery_peers_cache_is_safe_under_concurrent_access) {
    std::atomic<bool> stop(false);
    std::atomic<int> generation(0);

    SchedulerPeerDiscovery d(
        "self", "cluster-1", true, "backend",
        [&generation](const std::string&, const std::string&, const std::string&,
                      std::vector<SchedulerWirePeer>* out) -> bool {
            // Alternate between 1 and 3 peers so a torn read is observable as a
            // size that is neither.
            const int n = (generation.fetch_add(1) % 2 == 0) ? 1 : 3;
            for (int i = 0; i < n; ++i) {
                SchedulerWirePeer p;
                p.node_id = "node";
                p.has_endpoint = true;
                p.endpoint.backend = "backend";
                p.endpoint.address = "addr";
                out->push_back(p);
            }
            return true;
        },
        SchedulerPeerDiscovery::WireRecord());

    std::thread writer([&d, &stop] {
        while (!stop.load()) d.Refresh();
    });

    std::atomic<int> bad_sizes(0);
    std::atomic<int> reads(0);
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.push_back(std::thread([&d, &stop, &bad_sizes, &reads] {
            while (!stop.load()) {
                std::vector<P2pPeer> snapshot = d.Peers().value();
                const size_t n = snapshot.size();
                // FilterSchedulerP2pPeers dedups nothing, so only 1 or 3 are
                // reachable. 0 is also valid before the first Refresh lands.
                if (n != 0 && n != 1 && n != 3) bad_sizes.fetch_add(1);
                for (size_t i = 0; i < n; ++i) {
                    if (snapshot[i].node_id != "node") bad_sizes.fetch_add(1);
                    if (snapshot[i].endpoint.address != "addr") bad_sizes.fetch_add(1);
                }
                reads.fetch_add(1);
            }
        }));
    }

    ::usleep(300 * 1000);
    stop.store(true);
    writer.join();
    for (size_t i = 0; i < readers.size(); ++i) readers[i].join();

    MT_EXPECT_TRUE(reads.load() > 0);
    MT_EXPECT_TRUE(generation.load() > 0);
    MT_EXPECT_EQ(bad_sizes.load(), 0);
}

MT_MAIN
