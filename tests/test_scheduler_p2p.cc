// SPDX-License-Identifier: MIT
// The scheduler's P2P surface: the artifact index, the handlers, and the two
// halves of the protocol meeting over a real socket.
//
// Go upstream: services/scheduler/internal/{store,service}_test.go.
//
// The last test here is the one that matters most. Every other suite in this
// area tests one side of a contract against a stand-in for the other; this one
// points the real node-side wire (`p2p::MakeHttpSchedulerWire`) at the real
// scheduler handlers over loopback. That is the only arrangement that can
// catch the two halves disagreeing — which has already happened twice in this
// work, once on HEAD framing and once on header case.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agentenv/core/json.h"
#include "agentenv/p2p/discovery.h"
#include "agentenv/p2p/scheduler_http.h"
#include "microtest.h"
#include "services/scheduler/artifact_index.h"
#include "services/scheduler/p2p_routes.h"

using namespace agentenv;                       // NOLINT
using namespace agentenv::services::scheduler;  // NOLINT

namespace {

Node MakeNode(const std::string& id, const std::string& cluster,
              const std::string& backend, const std::string& address) {
    Node node;
    node.id = id;
    node.cluster_id = cluster;
    node.endpoint = id + ".control:1";
    node.p2p_backend = backend;
    node.p2p_address = address;
    return node;
}

uint16_t FreePort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return 0;
    }
    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

/// Drives the handlers without a socket, for the cases a socket adds nothing
/// to. The real listener is used by the last test.
class DirectServer : public api::Server {
 public:
    void Route(const std::string& method, const std::string& pattern,
               api::Handler handler) override {
        routes_.push_back(Entry(method, pattern, handler));
    }
    void Use(api::Middleware) override {}
    core::Expected<core::Unit, core::AnyError>
        ListenAndServe(const std::string&) override { return core::Unit{}; }
    void Stop() override {}

    bool Dispatch(const api::HttpRequest& request, api::HttpResponse* response) {
        for (std::size_t i = 0; i < routes_.size(); ++i) {
            if (routes_[i].method != request.method) continue;
            if (!Matches(routes_[i].pattern, request.path)) continue;
            routes_[i].handler(request, response);
            return true;
        }
        return false;
    }

 private:
    static std::vector<std::string> Segments(const std::string& path) {
        std::vector<std::string> out;
        std::size_t i = 0;
        while (i < path.size()) {
            while (i < path.size() && path[i] == '/') ++i;
            const std::size_t start = i;
            while (i < path.size() && path[i] != '/') ++i;
            if (i > start) out.push_back(path.substr(start, i - start));
        }
        return out;
    }
    static bool Matches(const std::string& pattern, const std::string& path) {
        const std::vector<std::string> want = Segments(pattern);
        const std::vector<std::string> got = Segments(path);
        if (want.size() != got.size()) return false;
        for (std::size_t i = 0; i < want.size(); ++i) {
            const bool placeholder = want[i].size() >= 2 && want[i][0] == '{' &&
                                     want[i][want[i].size() - 1] == '}';
            if (placeholder) continue;
            if (want[i] != got[i]) return false;
        }
        return true;
    }
    struct Entry {
        Entry(std::string m, std::string p, api::Handler h)
            : method(std::move(m)), pattern(std::move(p)), handler(std::move(h)) {}
        std::string  method;
        std::string  pattern;
        api::Handler handler;
    };
    std::vector<Entry> routes_;
};

struct Fixture {
    NodeRegistry          nodes;
    InMemoryArtifactStore artifacts;
    DirectServer          server;

    Fixture() : artifacts(0, 0) {
        RegisterSchedulerP2pRoutes(&server, &nodes, &artifacts, "secret");
        RegisterSchedulerNodeRoutes(&server, &nodes, &artifacts, "secret");
    }

    api::HttpResponse Post(const std::string& path, const std::string& body,
                           bool with_token = true) {
        api::HttpRequest request;
        request.method = "POST";
        request.path = path;
        request.body = body;
        if (with_token) request.headers["x-agentenv-p2p-token"] = "secret";
        api::HttpResponse response;
        server.Dispatch(request, &response);
        return response;
    }

    api::HttpResponse Delete(const std::string& path) {
        api::HttpRequest request;
        request.method = "DELETE";
        request.path = path;
        request.headers["x-agentenv-p2p-token"] = "secret";
        api::HttpResponse response;
        server.Dispatch(request, &response);
        return response;
    }
};

std::vector<std::string> PeerIdsOf(const std::string& body) {
    std::vector<std::string> out;
    auto parsed = core::Json::Parse(body);
    if (!parsed.ok() || parsed.value().kind() != core::Json::Kind::Object) return out;
    const core::JsonObject& root = parsed.value().as_object();
    core::JsonObject::const_iterator peers = root.find("peers");
    if (peers == root.end() || peers->second.kind() != core::Json::Kind::Array) return out;
    const core::JsonArray& array = peers->second.as_array();
    for (std::size_t i = 0; i < array.size(); ++i) {
        if (array[i].kind() != core::Json::Kind::Object) continue;
        const core::JsonObject& entry = array[i].as_object();
        core::JsonObject::const_iterator id = entry.find("nodeId");
        if (id != entry.end() && id->second.kind() == core::Json::Kind::String) {
            out.push_back(id->second.as_string());
        }
    }
    return out;
}

bool Contains(const std::vector<std::string>& items, const std::string& want) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i] == want) return true;
    }
    return false;
}

}  // namespace

// ---- the artifact index -----------------------------------------------------

MT_TEST(an_index_key_needs_all_three_components) {
    ArtifactIndexKey key;
    MT_EXPECT_TRUE(NormalizeArtifactIndexKey(" c ", " b ", " k ", &key));
    MT_EXPECT_EQ(std::string("c"), key.cluster_id);
    MT_EXPECT_EQ(std::string("b"), key.backend);
    MT_EXPECT_EQ(std::string("k"), key.key);

    // A blank component would make the entry visible where it should not be:
    // an empty cluster id to every cluster, an empty backend to requesters
    // that cannot parse the address format.
    MT_EXPECT_TRUE(!NormalizeArtifactIndexKey("", "b", "k", &key));
    MT_EXPECT_TRUE(!NormalizeArtifactIndexKey("c", "  ", "k", &key));
    MT_EXPECT_TRUE(!NormalizeArtifactIndexKey("c", "b", "", &key));
}

MT_TEST(record_is_idempotent_and_lookup_returns_every_holder) {
    InMemoryArtifactStore store;
    store.Record("c", "http", "layer/a", "node-1");
    store.Record("c", "http", "layer/a", "node-1");
    store.Record("c", "http", "layer/a", "node-2");

    const std::vector<std::string> holders = store.Lookup("c", "http", "layer/a");
    // Idempotent because every fetch adopts what it pulled, so a node
    // republishing a key it already holds is the normal case, not an error.
    MT_EXPECT_EQ(static_cast<std::size_t>(2), holders.size());
    MT_EXPECT_TRUE(Contains(holders, "node-1"));
    MT_EXPECT_TRUE(Contains(holders, "node-2"));
    MT_EXPECT_EQ(static_cast<std::size_t>(1), store.size());
}

MT_TEST(the_index_is_partitioned_by_cluster_and_by_backend) {
    InMemoryArtifactStore store;
    store.Record("c1", "http", "layer/a", "node-1");
    store.Record("c2", "http", "layer/a", "node-2");
    store.Record("c1", "iroh", "layer/a", "node-3");

    // Same artifact key, three separate entries. Merging them would hand a
    // c1/http requester an address belonging to another cluster or in another
    // backend's format.
    MT_EXPECT_EQ(static_cast<std::size_t>(3), store.size());
    MT_EXPECT_EQ(static_cast<std::size_t>(1), store.Lookup("c1", "http", "layer/a").size());
    MT_EXPECT_EQ(std::string("node-1"), store.Lookup("c1", "http", "layer/a")[0]);
    MT_EXPECT_EQ(std::string("node-3"), store.Lookup("c1", "iroh", "layer/a")[0]);
}

MT_TEST(forgetting_the_last_holder_drops_the_key_entirely) {
    InMemoryArtifactStore store;
    store.Record("c", "http", "layer/a", "node-1");
    store.Record("c", "http", "layer/a", "node-2");

    store.Forget("c", "http", "layer/a", "node-1");
    MT_EXPECT_EQ(static_cast<std::size_t>(1), store.Lookup("c", "http", "layer/a").size());
    MT_EXPECT_EQ(static_cast<std::size_t>(1), store.size());

    store.Forget("c", "http", "layer/a", "node-2");
    // An entry with an empty holder set would make `Lookup` report a hit with
    // no providers, which reads as "somebody has it" and costs a round trip.
    MT_EXPECT_TRUE(store.Lookup("c", "http", "layer/a").empty());
    MT_EXPECT_EQ(static_cast<std::size_t>(0), store.size());
}

MT_TEST(forgetting_a_node_drops_every_claim_it_made) {
    InMemoryArtifactStore store;
    store.Record("c", "http", "layer/a", "node-1");
    store.Record("c", "http", "layer/b", "node-1");
    store.Record("c", "http", "layer/b", "node-2");

    store.ForgetNode("node-1");

    // This is what the reverse index exists for: without it a node departure
    // would have to scan every key in the cluster.
    MT_EXPECT_TRUE(store.Lookup("c", "http", "layer/a").empty());
    const std::vector<std::string> b = store.Lookup("c", "http", "layer/b");
    MT_EXPECT_EQ(static_cast<std::size_t>(1), b.size());
    MT_EXPECT_EQ(std::string("node-2"), b[0]);
}

MT_TEST(the_index_evicts_the_least_recently_looked_up_key) {
    InMemoryArtifactStore store(2, 0);
    store.Record("c", "http", "k1", "node-1");
    store.Record("c", "http", "k2", "node-1");

    // Touch k1 so k2 becomes the oldest. Keying the LRU on lookups rather
// than writes is the point: a key written once and fetched constantly is
    // the one worth keeping.
    MT_EXPECT_EQ(static_cast<std::size_t>(1), store.Lookup("c", "http", "k1").size());

    store.Record("c", "http", "k3", "node-1");
    MT_EXPECT_EQ(static_cast<std::size_t>(2), store.size());
    MT_EXPECT_TRUE(!store.Lookup("c", "http", "k1").empty());
    MT_EXPECT_TRUE(store.Lookup("c", "http", "k2").empty());
    MT_EXPECT_TRUE(!store.Lookup("c", "http", "k3").empty());

    // The evicted key's reverse edge went with it: forgetting the node now
    // must not walk into a dangling entry.
    store.ForgetNode("node-1");
    MT_EXPECT_EQ(static_cast<std::size_t>(0), store.size());
}

MT_TEST(a_lookup_miss_does_not_count_as_a_use) {
    InMemoryArtifactStore store(2, 0);
    store.Record("c", "http", "k1", "node-1");
    store.Record("c", "http", "k2", "node-1");

    // Misses for keys nobody has must not be able to evict the keys that
    // exist, or a flood of them would empty the index.
    for (int i = 0; i < 10; ++i) store.Lookup("c", "http", "absent");

    MT_EXPECT_TRUE(!store.Lookup("c", "http", "k1").empty());
    MT_EXPECT_TRUE(!store.Lookup("c", "http", "k2").empty());
}

MT_TEST(the_lookup_node_limit_caps_how_many_holders_come_back) {
    InMemoryArtifactStore store(0, 2);
    store.Record("c", "http", "k", "node-1");
    store.Record("c", "http", "k", "node-2");
    store.Record("c", "http", "k", "node-3");

    // Bounded because the requester tries providers in order and stops at the
    // first that answers; handing back all of them is wasted payload.
    MT_EXPECT_EQ(static_cast<std::size_t>(2), store.Lookup("c", "http", "k").size());
}

// ---- peer filtering ---------------------------------------------------------

MT_TEST(list_peers_filters_by_cluster_backend_endpoint_and_self) {
    NodeRegistry nodes;
    nodes.Upsert(MakeNode("node-a", "c1", "http", "10.0.0.1:7000"));
    nodes.Upsert(MakeNode("node-b", "c2", "http", "10.0.0.2:7000"));
    nodes.Upsert(MakeNode("node-c", "c1", "iroh", "whatever"));
    // No P2P endpoint: how a node running with p2p disabled stays out of
    // everyone's peer list while remaining schedulable for placement.
    nodes.Upsert(MakeNode("node-d", "c1", "", ""));
    nodes.Upsert(MakeNode("node-e", "c1", "http", "10.0.0.5:7000"));

    const std::vector<Node> peers = nodes.ListP2pPeers("c1", "http", "node-e");
    MT_EXPECT_EQ(static_cast<std::size_t>(1), peers.size());
    MT_EXPECT_EQ(std::string("node-a"), peers[0].id);
}

MT_TEST(an_empty_allow_list_yields_nothing_rather_than_everything) {
    NodeRegistry nodes;
    nodes.Upsert(MakeNode("node-a", "c1", "http", "10.0.0.1:7000"));
    nodes.Upsert(MakeNode("node-b", "c1", "http", "10.0.0.2:7000"));

    // A lookup that found no holders must not degrade into "here is the whole
    // cluster" -- that would have every node fetch from an arbitrary peer that
    // does not hold the artifact.
    MT_EXPECT_TRUE(
        nodes.FilterP2pPeers("c1", "http", std::vector<std::string>(), "").empty());

    std::vector<std::string> allowed;
    allowed.push_back("node-b");
    const std::vector<Node> peers = nodes.FilterP2pPeers("c1", "http", allowed, "");
    MT_EXPECT_EQ(static_cast<std::size_t>(1), peers.size());
    MT_EXPECT_EQ(std::string("node-b"), peers[0].id);
}

// ---- the handlers -----------------------------------------------------------

MT_TEST(every_scheduler_route_requires_the_token) {
    Fixture fixture;
    MT_EXPECT_EQ(401, fixture.Post("/scheduler/v1/p2p/peers", "{}", false).status);
    MT_EXPECT_EQ(401, fixture.Post("/scheduler/v1/p2p/lookup", "{}", false).status);
    MT_EXPECT_EQ(401, fixture.Post("/scheduler/v1/p2p/record", "{}", false).status);
    MT_EXPECT_EQ(401, fixture.Post("/scheduler/v1/p2p/forget", "{}", false).status);
    MT_EXPECT_EQ(401, fixture.Post("/scheduler/v1/nodes", "{}", false).status);
}

MT_TEST(a_malformed_body_is_four_hundred_not_a_crash) {
    Fixture fixture;
    MT_EXPECT_EQ(400, fixture.Post("/scheduler/v1/p2p/peers", "not json").status);
    MT_EXPECT_EQ(400, fixture.Post("/scheduler/v1/p2p/peers", "[]").status);
    MT_EXPECT_EQ(400, fixture.Post("/scheduler/v1/p2p/lookup",
                                   std::string(128 * 1024, 'x')).status);
}

MT_TEST(registering_a_node_makes_it_visible_as_a_peer) {
    Fixture fixture;
    MT_EXPECT_EQ(200, fixture.Post("/scheduler/v1/nodes",
        "{\"nodeId\":\"node-a\",\"clusterId\":\"c1\",\"endpoint\":\"ctl:1\","
        "\"p2pBackend\":\"http\",\"p2pAddress\":\"10.0.0.1:7000\"}").status);

    const api::HttpResponse peers = fixture.Post("/scheduler/v1/p2p/peers",
        "{\"clusterId\":\"c1\",\"backend\":\"http\",\"excludeNodeId\":\"node-self\"}");
    MT_EXPECT_EQ(200, peers.status);
    MT_EXPECT_EQ(static_cast<std::size_t>(1), PeerIdsOf(peers.body).size());
    MT_EXPECT_EQ(std::string("node-a"), PeerIdsOf(peers.body)[0]);
}

MT_TEST(a_node_id_is_required_and_a_half_given_p2p_endpoint_is_refused) {
    Fixture fixture;
    MT_EXPECT_EQ(400, fixture.Post("/scheduler/v1/nodes", "{\"clusterId\":\"c1\"}").status);
    // A backend with no address cannot be dialled and an address with no
    // backend cannot be interpreted; either would be filtered out later,
    // silently, leaving the operator with a node that never becomes a peer
    // and no reason why.
    MT_EXPECT_EQ(400, fixture.Post("/scheduler/v1/nodes",
        "{\"nodeId\":\"n\",\"p2pBackend\":\"http\"}").status);
    MT_EXPECT_EQ(400, fixture.Post("/scheduler/v1/nodes",
        "{\"nodeId\":\"n\",\"p2pAddress\":\"1.1.1.1:1\"}").status);
    // Neither is fine: schedulable, never a peer.
    MT_EXPECT_EQ(200, fixture.Post("/scheduler/v1/nodes", "{\"nodeId\":\"n\"}").status);
}

MT_TEST(recording_an_unknown_node_is_refused) {
    Fixture fixture;
    // Indexing it would send peers to an address the scheduler has never heard
    // of, and each would pay a failed dial to find out.
    const api::HttpResponse refused = fixture.Post("/scheduler/v1/p2p/record",
        "{\"clusterId\":\"c1\",\"backend\":\"http\",\"key\":\"layer/a\","
        "\"nodeId\":\"ghost\"}");
    MT_EXPECT_EQ(400, refused.status);

    fixture.Post("/scheduler/v1/nodes",
        "{\"nodeId\":\"ghost\",\"clusterId\":\"c1\","
        "\"p2pBackend\":\"http\",\"p2pAddress\":\"10.0.0.9:7000\"}");
    MT_EXPECT_EQ(200, fixture.Post("/scheduler/v1/p2p/record",
        "{\"clusterId\":\"c1\",\"backend\":\"http\",\"key\":\"layer/a\","
        "\"nodeId\":\"ghost\"}").status);
}

MT_TEST(forgetting_does_not_require_the_node_to_still_be_registered) {
    Fixture fixture;
    fixture.Post("/scheduler/v1/nodes",
        "{\"nodeId\":\"node-a\",\"clusterId\":\"c1\","
        "\"p2pBackend\":\"http\",\"p2pAddress\":\"10.0.0.1:7000\"}");
    fixture.Post("/scheduler/v1/p2p/record",
        "{\"clusterId\":\"c1\",\"backend\":\"http\",\"key\":\"layer/a\","
        "\"nodeId\":\"node-a\"}");
    fixture.Delete("/scheduler/v1/nodes/node-a");

    // A node that has already left must still be able to retract claims;
    // refusing would strand them until the LRU got round to it.
    MT_EXPECT_EQ(200, fixture.Post("/scheduler/v1/p2p/forget",
        "{\"clusterId\":\"c1\",\"backend\":\"http\",\"key\":\"layer/a\","
        "\"nodeId\":\"node-a\"}").status);
}

MT_TEST(lookup_requires_its_three_fields) {
    Fixture fixture;
    // Answering with an empty peer list would be indistinguishable from
    // "nobody has it", and the caller would fall back to the origin rather
    // than fix the request.
    MT_EXPECT_EQ(400, fixture.Post("/scheduler/v1/p2p/lookup",
        "{\"backend\":\"http\",\"key\":\"k\"}").status);
    MT_EXPECT_EQ(400, fixture.Post("/scheduler/v1/p2p/lookup",
        "{\"clusterId\":\"c\",\"key\":\"k\"}").status);
    MT_EXPECT_EQ(400, fixture.Post("/scheduler/v1/p2p/lookup",
        "{\"clusterId\":\"c\",\"backend\":\"http\"}").status);
}

MT_TEST(lookup_returns_only_holders_that_are_still_usable_peers) {
    Fixture fixture;
    const char* const nodes[] = {"node-a", "node-b"};
    for (std::size_t i = 0; i < 2; ++i) {
        char body[256];
        std::snprintf(body, sizeof(body),
                      "{\"nodeId\":\"%s\",\"clusterId\":\"c1\","
                      "\"p2pBackend\":\"http\",\"p2pAddress\":\"10.0.0.%u:7000\"}",
                      nodes[i], static_cast<unsigned>(i + 1));
        fixture.Post("/scheduler/v1/nodes", body);
        char record[256];
        std::snprintf(record, sizeof(record),
                      "{\"clusterId\":\"c1\",\"backend\":\"http\",\"key\":\"layer/a\","
                      "\"nodeId\":\"%s\"}", nodes[i]);
        fixture.Post("/scheduler/v1/p2p/record", record);
    }

    const api::HttpResponse both = fixture.Post("/scheduler/v1/p2p/lookup",
        "{\"clusterId\":\"c1\",\"backend\":\"http\",\"key\":\"layer/a\","
        "\"excludeNodeId\":\"node-self\"}");
    MT_EXPECT_EQ(static_cast<std::size_t>(2), PeerIdsOf(both.body).size());

    // Drop one node. Its claim is still in the index for an instant, but the
    // registry filter is what keeps it out of the answer -- the two-step
    // lookup exists exactly so a departed node cannot be handed out.
    fixture.Delete("/scheduler/v1/nodes/node-a");
    const api::HttpResponse after = fixture.Post("/scheduler/v1/p2p/lookup",
        "{\"clusterId\":\"c1\",\"backend\":\"http\",\"key\":\"layer/a\","
        "\"excludeNodeId\":\"node-self\"}");
    MT_EXPECT_EQ(static_cast<std::size_t>(1), PeerIdsOf(after.body).size());
    MT_EXPECT_EQ(std::string("node-b"), PeerIdsOf(after.body)[0]);
}

MT_TEST(lookup_excludes_the_asking_node) {
    Fixture fixture;
    fixture.Post("/scheduler/v1/nodes",
        "{\"nodeId\":\"node-a\",\"clusterId\":\"c1\","
        "\"p2pBackend\":\"http\",\"p2pAddress\":\"10.0.0.1:7000\"}");
    fixture.Post("/scheduler/v1/p2p/record",
        "{\"clusterId\":\"c1\",\"backend\":\"http\",\"key\":\"layer/a\","
        "\"nodeId\":\"node-a\"}");

    // Returning the asker would have it fetch from itself over the network,
    // which is slower than reading its own store and a loop waiting to happen.
    const api::HttpResponse self = fixture.Post("/scheduler/v1/p2p/lookup",
        "{\"clusterId\":\"c1\",\"backend\":\"http\",\"key\":\"layer/a\","
        "\"excludeNodeId\":\"node-a\"}");
    MT_EXPECT_EQ(200, self.status);
    MT_EXPECT_TRUE(PeerIdsOf(self.body).empty());
}

MT_TEST(removing_a_node_also_drops_its_artifact_claims) {
    Fixture fixture;
    fixture.Post("/scheduler/v1/nodes",
        "{\"nodeId\":\"node-a\",\"clusterId\":\"c1\","
        "\"p2pBackend\":\"http\",\"p2pAddress\":\"10.0.0.1:7000\"}");
    fixture.Post("/scheduler/v1/p2p/record",
        "{\"clusterId\":\"c1\",\"backend\":\"http\",\"key\":\"layer/a\","
        "\"nodeId\":\"node-a\"}");
    MT_EXPECT_EQ(static_cast<std::size_t>(1), fixture.artifacts.size());

    MT_EXPECT_EQ(200, fixture.Delete("/scheduler/v1/nodes/node-a").status);
    // Leaving the claims behind would keep the index pointing at something
    // that is gone, and every peer would pay a dial to learn that.
    MT_EXPECT_EQ(static_cast<std::size_t>(0), fixture.artifacts.size());
}

// ---- the two halves, over a socket -----------------------------------------

MT_TEST(the_node_side_wire_and_the_scheduler_agree_over_a_real_socket) {
    NodeRegistry nodes;
    InMemoryArtifactStore artifacts(0, 0);

    std::unique_ptr<api::Server> server = api::MakeServer();
    MT_EXPECT_TRUE(!!server);
    RegisterSchedulerP2pRoutes(server.get(), &nodes, &artifacts, "cluster-token");
    RegisterSchedulerNodeRoutes(server.get(), &nodes, &artifacts, "cluster-token");

    const uint16_t port = FreePort();
    MT_EXPECT_TRUE(port != 0);
    char bind[64];
    std::snprintf(bind, sizeof(bind), "127.0.0.1:%u", static_cast<unsigned>(port));
    api::Server* raw = server.get();
    const std::string bind_addr = bind;
    std::thread serving([raw, bind_addr]() { raw->ListenAndServe(bind_addr); });

    char endpoint[64];
    std::snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u",
                  static_cast<unsigned>(port));
    const p2p::SchedulerWireTransport wire =
        p2p::MakeHttpSchedulerWire(endpoint, "cluster-token", 2000, 2000);
    MT_EXPECT_TRUE(!!wire.fetch);

    // Poll until the listener is up; `ListenAndServe` binds on another thread.
    bool reachable = false;
    for (int attempt = 0; attempt < 200 && !reachable; ++attempt) {
        std::vector<p2p::SchedulerWirePeer> peers;
        reachable = wire.fetch("c1", "http", "node-self", &peers);
        if (!reachable) ::usleep(10 * 1000);
    }
    MT_EXPECT_TRUE(reachable);

    // Register two peers directly, which is what a node's own startup would
    // do over this same surface.
    nodes.Upsert(MakeNode("node-a", "c1", "http", "10.0.0.1:7000"));
    nodes.Upsert(MakeNode("node-b", "c1", "http", "10.0.0.2:7000"));

    // fetch -> ListP2PPeers
    std::vector<p2p::SchedulerWirePeer> listed;
    MT_EXPECT_TRUE(wire.fetch("c1", "http", "node-b", &listed));
    MT_EXPECT_EQ(static_cast<std::size_t>(1), listed.size());
    MT_EXPECT_EQ(std::string("node-a"), listed[0].node_id);
    MT_EXPECT_TRUE(listed[0].has_endpoint);
    MT_EXPECT_EQ(std::string("10.0.0.1:7000"), listed[0].endpoint.address);

    // record -> RecordP2PArtifact, then lookup -> LookupP2PArtifact.
    MT_EXPECT_TRUE(wire.record("c1", "http", "layer/a", "node-a", true));
    std::vector<p2p::SchedulerWirePeer> found;
    MT_EXPECT_TRUE(wire.lookup("c1", "http", "layer/a", "node-b", &found));
    MT_EXPECT_EQ(static_cast<std::size_t>(1), found.size());
    MT_EXPECT_EQ(std::string("node-a"), found[0].node_id);

    // The filter the discovery applies on top must keep it.
    const std::vector<p2p::P2pPeer> filtered =
        p2p::FilterSchedulerP2pPeers(found, "http");
    MT_EXPECT_EQ(static_cast<std::size_t>(1), filtered.size());
    MT_EXPECT_EQ(std::string("10.0.0.1:7000"), filtered[0].endpoint.address);

    // forget -> ForgetP2PArtifact.
    MT_EXPECT_TRUE(wire.record("c1", "http", "layer/a", "node-a", false));
    std::vector<p2p::SchedulerWirePeer> gone;
    MT_EXPECT_TRUE(wire.lookup("c1", "http", "layer/a", "node-b", &gone));
    MT_EXPECT_TRUE(gone.empty());

    server->Stop();
    if (serving.joinable()) serving.join();
}

int main() { return microtest::RunAll(); }
