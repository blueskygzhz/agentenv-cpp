// SPDX-License-Identifier: MIT
// Tests for p2p: MockTransport (mock.rs), ResolvedP2pConfig (config.rs),
// and discovery hint merging (discovery/mod.rs).
#include "microtest.h"

#include <cstdio>

#include "agentenv/p2p/all.h"
#include "agentenv/p2p/discovery.h"
using namespace agentenv::p2p;

// ---- config.rs ----
MT_TEST(resolved_config_disabled_overrides_transport) {
    P2pRawConfig c;
    c.enabled = false;
    c.transport = P2pTransportKind::Iroh;
    ResolvedP2pConfig r = ResolvedP2pConfig::FromConfig(c);
    MT_EXPECT_TRUE(r.transport == P2pTransportKind::Disabled);
    MT_EXPECT_TRUE(std::string(BackendId(r.transport)) == "");
}

MT_TEST(resolved_config_enabled_iroh) {
    P2pRawConfig c;
    c.enabled = true;
    c.transport = P2pTransportKind::Iroh;
    c.listen_addr = "  0.0.0.0:4433  ";
    c.peer_discovery_refresh_interval_secs = 0;  // floored to 1
    ResolvedP2pConfig r = ResolvedP2pConfig::FromConfig(c);
    MT_EXPECT_TRUE(r.transport == P2pTransportKind::Iroh);
    MT_EXPECT_TRUE(std::string(BackendId(r.transport)) == "iroh");
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
  auto t = IrohBlobsP2pTransport::Start("/tmp/store", "", &err);
    MT_EXPECT_TRUE(!t);  // not compiled in
  MT_EXPECT_TRUE(err.find("not compiled in") != std::string::npos);

    // A directly-constructed instance returns Disabled for every op.
    IrohBlobsP2pTransport inst;
    P2pArtifactDescriptor desc;
    auto r = inst.LookupWithHints("k", std::vector<P2pArtifactProviderHint>(), &desc);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().kind == P2pError::Disabled);
    P2pEndpoint ep;
    MT_EXPECT_TRUE(!inst.LocalEndpoint(&ep));
}

MT_MAIN
