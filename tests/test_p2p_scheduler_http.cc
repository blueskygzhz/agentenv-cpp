// SPDX-License-Identifier: MIT
// The scheduler discovery wire over HTTP.
//
// Rust equivalent: `src/p2p/discovery/scheduler.rs`, whose four calls go over
// gRPC. The pure half of that file — caching, jittered refresh, backend
// filtering, record/forget bookkeeping — is already covered by test_p2p.cc;
// what is new here is only the transport beneath it.
//
// Two layers are tested, deliberately:
//
//   * the codecs on their own, because a field name or a selector getting the
//     wrong spelling is invisible until a real scheduler rejects it;
//   * the callbacks against a real loopback HTTP server, because that is the
//     only thing that proves the request this client sends is the request the
//     other side receives. The previous round's end-to-end suite caught a
//     HEAD-framing bug that every unit test had passed, which is the argument
//     for doing this at all.
#include "agentenv/p2p/scheduler_http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentenv/api/server.h"
#include "agentenv/core/json.h"
#include "microtest.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::p2p;  // NOLINT

namespace {

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

/// A scheduler that records what it was asked and answers what it was told to.
///
/// Running a real `api::Server` rather than invoking the callbacks' internals
/// is the point: the request body, the path and the token header all have to
/// survive an actual socket for the assertions below to mean anything.
class FakeScheduler {
 public:
    FakeScheduler() : server_(api::MakeServer()), port_(FreePort()) {}

    bool valid() const { return !!server_ && port_ != 0; }
    std::string endpoint() const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "http://127.0.0.1:%u",
                      static_cast<unsigned>(port_));
        return std::string(buf);
    }

    /// Replies to `path` with `status` and `body`, recording the request.
    void Reply(const std::string& path, int status, const std::string& body) {
        Reply_ reply;
        reply.status = status;
        reply.body = body;
        replies_[path] = reply;
    }

    void Start() {
        const char* const paths[] = {kSchedulerPeersPath, kSchedulerLookupPath,
                                     kSchedulerRecordPath, kSchedulerForgetPath};
        for (std::size_t i = 0; i < 4; ++i) {
            const std::string path = paths[i];
            server_->Route("POST", path,
                           [this, path](const api::HttpRequest& request,
                                        api::HttpResponse* response) {
                Seen seen;
                seen.body = request.body;
                // `api::Server` lowercases every header name as it parses, so
                // the canonical spelling never survives to a handler. That is
                // exactly why the blob surface looks its own token up
                // case-insensitively; a fake that only checked the canonical
                // form would be testing something the real server cannot
                // deliver.
                std::unordered_map<std::string, std::string>::const_iterator token =
                    request.headers.find("x-agentenv-p2p-token");
                if (token == request.headers.end()) {
                    token = request.headers.find("X-Agentenv-P2p-Token");
                }
                if (token != request.headers.end()) {
                    seen.has_token = true;
                    seen.token = token->second;
                }
                requests_[path].push_back(seen);

                std::map<std::string, Reply_>::const_iterator reply = replies_.find(path);
                if (reply == replies_.end()) {
                    response->status = 200;
                    response->body = "{}";
                    return;
                }
                response->status = reply->second.status;
                response->headers["Content-Type"] = "application/json";
                response->body = reply->second.body;
            });
        }
        const uint16_t port = port_;
        api::Server* server = server_.get();
        thread_.reset(new std::thread([server, port]() {
            char bind[64];
            std::snprintf(bind, sizeof(bind), "127.0.0.1:%u", static_cast<unsigned>(port));
            server->ListenAndServe(bind);
        }));
    }

    ~FakeScheduler() {
        if (server_) server_->Stop();
        if (thread_ && thread_->joinable()) thread_->join();
    }

    struct Seen {
        std::string body;
        bool        has_token = false;
        std::string token;
    };

    const std::vector<Seen>& RequestsTo(const std::string& path) {
        return requests_[path];
    }

 private:
    struct Reply_ {
        int         status = 200;
        std::string body;
    };

    std::unique_ptr<api::Server>              server_;
    uint16_t                                  port_;
    std::unique_ptr<std::thread>              thread_;
    std::map<std::string, Reply_>             replies_;
    std::map<std::string, std::vector<Seen> > requests_;
};

/// Starts a scheduler and a wire pointed at it, polling until it answers.
///
/// The poll replaces a sleep: `ListenAndServe` runs on another thread, so the
/// first request would otherwise race the bind.
bool StartFake(FakeScheduler* fake, SchedulerWireTransport* wire,
               const std::string& token) {
    if (!fake->valid()) return false;
    fake->Start();
    *wire = MakeHttpSchedulerWire(fake->endpoint(), token, 2000, 2000);
    if (!wire->fetch) return false;

    for (int attempt = 0; attempt < 200; ++attempt) {
        std::vector<SchedulerWirePeer> peers;
        if (wire->fetch("c", "http", "self", &peers)) return true;
        ::usleep(10 * 1000);
    }
    return false;
}

std::string FieldOf(const std::string& json, const std::string& key) {
    auto parsed = core::Json::Parse(json);
    if (!parsed.ok() || parsed.value().kind() != core::Json::Kind::Object) return "";
    const core::JsonObject& o = parsed.value().as_object();
    core::JsonObject::const_iterator it = o.find(key);
    if (it == o.end() || it->second.kind() != core::Json::Kind::String) return "";
    return it->second.as_string();
}

bool HasField(const std::string& json, const std::string& key) {
    auto parsed = core::Json::Parse(json);
    if (!parsed.ok() || parsed.value().kind() != core::Json::Kind::Object) return false;
    return parsed.value().as_object().count(key) > 0;
}

}  // namespace

// ---- request encoding -------------------------------------------------------

MT_TEST(the_peers_request_omits_the_key_and_the_lookup_request_carries_it) {
    const std::string peers =
        EncodeSchedulerPeersRequest("cluster-1", "http", "node-self", std::string());
    MT_EXPECT_EQ(std::string("cluster-1"), FieldOf(peers, "clusterId"));
    MT_EXPECT_EQ(std::string("http"), FieldOf(peers, "backend"));
    MT_EXPECT_EQ(std::string("node-self"), FieldOf(peers, "excludeNodeId"));
    // `ListP2pPeersRequest` has no `key` field at all. Sending one would be a
    // message the scheduler never agreed to parse -- ignored by one
    // implementation, rejected by another.
    MT_EXPECT_TRUE(!HasField(peers, "key"));

    const std::string lookup =
        EncodeSchedulerPeersRequest("cluster-1", "http", "node-self", "layer/abc");
    MT_EXPECT_EQ(std::string("layer/abc"), FieldOf(lookup, "key"));
    MT_EXPECT_EQ(std::string("node-self"), FieldOf(lookup, "excludeNodeId"));
}

MT_TEST(the_artifact_request_names_the_holder_not_the_node_to_exclude) {
    const std::string body =
        EncodeSchedulerArtifactRequest("cluster-1", "http", "layer/abc", "node-self");
    MT_EXPECT_EQ(std::string("layer/abc"), FieldOf(body, "key"));
    // record/forget name the node that *holds* the artifact; list/lookup name
    // the one to leave out. Same string here, opposite meaning, which is why
    // the proto spells them differently -- and why swapping them would
    // silently register the wrong node as a provider.
    MT_EXPECT_EQ(std::string("node-self"), FieldOf(body, "nodeId"));
    MT_EXPECT_TRUE(!HasField(body, "excludeNodeId"));
}

// ---- response decoding ------------------------------------------------------

MT_TEST(a_peers_response_decodes_endpoints_and_keeps_endpointless_peers) {
    const std::string body =
        "{\"peers\":["
        "{\"nodeId\":\"node-a\",\"endpoint\":{\"backend\":\"http\",\"address\":\"1.1.1.1:1\"}},"
        "{\"nodeId\":\"node-b\"},"
        "{\"nodeId\":\"node-c\",\"endpoint\":{\"backend\":\"http\",\"address\":\"\"}}"
        "]}";
    const auto decoded = DecodeSchedulerPeersResponse(body);
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_EQ(static_cast<std::size_t>(3), decoded.value().size());

    MT_EXPECT_EQ(std::string("node-a"), decoded.value()[0].node_id);
    MT_EXPECT_TRUE(decoded.value()[0].has_endpoint);
    MT_EXPECT_EQ(std::string("1.1.1.1:1"), decoded.value()[0].endpoint.address);

    // Kept, not dropped. Deciding which peers survive is
    // `FilterSchedulerP2pPeers`'s job, matching Rust's
    // `filter_scheduler_p2p_peers`; filtering in two places is how the two
    // come to disagree.
    MT_EXPECT_TRUE(!decoded.value()[1].has_endpoint);
    MT_EXPECT_TRUE(decoded.value()[2].has_endpoint);
    MT_EXPECT_EQ(std::string(""), decoded.value()[2].endpoint.address);
}

MT_TEST(the_decoder_and_the_filter_together_reproduce_the_rust_outcome) {
    // The same input Rust's `filter_scheduler_p2p_peers_keeps_only_matching_ready_peers`
    // uses: a match, a wrong backend, an empty address, no endpoint, a match.
    const std::string body =
        "{\"peers\":["
        "{\"nodeId\":\"node-a\",\"endpoint\":{\"backend\":\"backend\",\"address\":\"addr-a\"}},"
        "{\"nodeId\":\"node-b\",\"endpoint\":{\"backend\":\"other\",\"address\":\"addr-b\"}},"
        "{\"nodeId\":\"node-c\",\"endpoint\":{\"backend\":\"backend\",\"address\":\"\"}},"
        "{\"nodeId\":\"node-d\"},"
        "{\"nodeId\":\"node-e\",\"endpoint\":{\"backend\":\"backend\",\"address\":\"addr-e\"}}"
        "]}";
    const auto decoded = DecodeSchedulerPeersResponse(body);
    MT_EXPECT_TRUE(decoded.ok());

    const std::vector<P2pPeer> filtered =
        FilterSchedulerP2pPeers(decoded.value(), "backend");
    MT_EXPECT_EQ(static_cast<std::size_t>(2), filtered.size());
    MT_EXPECT_EQ(std::string("node-a"), filtered[0].node_id);
    MT_EXPECT_EQ(std::string("addr-a"), filtered[0].endpoint.address);
    MT_EXPECT_EQ(std::string("node-e"), filtered[1].node_id);
    MT_EXPECT_EQ(std::string("addr-e"), filtered[1].endpoint.address);
}

MT_TEST(an_absent_peers_field_is_an_empty_list_not_an_error) {
    // proto3 does not distinguish an empty repeated field from a missing one,
    // so a scheduler with nothing to report may legitimately omit it. Treating
    // that as a failure would keep a stale cache alive forever.
    const auto decoded = DecodeSchedulerPeersResponse("{}");
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_TRUE(decoded.value().empty());
}

MT_TEST(a_malformed_peers_response_is_refused_rather_than_partially_accepted) {
    MT_EXPECT_TRUE(!DecodeSchedulerPeersResponse("not json").ok());
    MT_EXPECT_TRUE(!DecodeSchedulerPeersResponse("[]").ok());
    MT_EXPECT_TRUE(!DecodeSchedulerPeersResponse("{\"peers\":\"nope\"}").ok());
    MT_EXPECT_TRUE(!DecodeSchedulerPeersResponse("{\"peers\":[42]}").ok());
    // A peer with no id cannot be dialled, recorded or logged about. Skipping
    // the entry instead would hide a broken scheduler while quietly shrinking
    // the peer set on every refresh.
    MT_EXPECT_TRUE(!DecodeSchedulerPeersResponse(
        "{\"peers\":[{\"endpoint\":{\"backend\":\"http\",\"address\":\"a\"}}]}").ok());
}

// ---- endpoint validation ----------------------------------------------------

MT_TEST(an_unusable_endpoint_yields_no_callbacks_rather_than_silent_successes) {
    // Empty callbacks make `SchedulerPeerDiscovery` report its RPCs as failed,
    // which is the honest answer: it keeps a stale peer cache in place instead
    // of clearing it against a fabricated empty result.
    MT_EXPECT_TRUE(!MakeHttpSchedulerWire("", "", 1000, 1000).fetch);
    MT_EXPECT_TRUE(!MakeHttpSchedulerWire("not a url", "", 1000, 1000).fetch);
    // A `grpc://` endpoint parses as a URL and would otherwise be dialled as
    // HTTP, failing at the first request with an error that says nothing about
    // the real mistake.
    MT_EXPECT_TRUE(!MakeHttpSchedulerWire("grpc://scheduler:9090", "", 1000, 1000).fetch);
    // `core::http` has no TLS; sending plaintext at a TLS listener fails
    // obscurely at the first byte.
    MT_EXPECT_TRUE(!MakeHttpSchedulerWire("https://scheduler:443", "", 1000, 1000).fetch);

    const SchedulerWireTransport usable =
        MakeHttpSchedulerWire("http://scheduler:9090", "", 1000, 1000);
    MT_EXPECT_TRUE(!!usable.fetch);
    MT_EXPECT_TRUE(!!usable.lookup);
    MT_EXPECT_TRUE(!!usable.record);
}

// ---- over a real socket -----------------------------------------------------

MT_TEST(fetch_posts_to_the_peers_path_and_returns_what_was_served) {
    FakeScheduler fake;
    fake.Reply(kSchedulerPeersPath, 200,
               "{\"peers\":[{\"nodeId\":\"node-a\","
               "\"endpoint\":{\"backend\":\"http\",\"address\":\"10.0.0.1:7000\"}}]}");
    SchedulerWireTransport wire;
    MT_EXPECT_TRUE(StartFake(&fake, &wire, "cluster-token"));

    std::vector<SchedulerWirePeer> peers;
    MT_EXPECT_TRUE(wire.fetch("cluster-1", "http", "node-self", &peers));
    MT_EXPECT_EQ(static_cast<std::size_t>(1), peers.size());
    MT_EXPECT_EQ(std::string("node-a"), peers[0].node_id);
    MT_EXPECT_EQ(std::string("10.0.0.1:7000"), peers[0].endpoint.address);

    const std::vector<FakeScheduler::Seen>& seen = fake.RequestsTo(kSchedulerPeersPath);
    MT_EXPECT_TRUE(!seen.empty());
    const FakeScheduler::Seen& last = seen[seen.size() - 1];
    MT_EXPECT_EQ(std::string("cluster-1"), FieldOf(last.body, "clusterId"));
    MT_EXPECT_EQ(std::string("node-self"), FieldOf(last.body, "excludeNodeId"));
    // One secret per cluster: the scheduler calls carry the same header the
    // blob surface does, so there is one thing to rotate rather than two.
    MT_EXPECT_TRUE(last.has_token);
    MT_EXPECT_EQ(std::string("cluster-token"), last.token);
}

MT_TEST(lookup_posts_to_the_lookup_path_with_the_key) {
    FakeScheduler fake;
    fake.Reply(kSchedulerLookupPath, 200,
               "{\"peers\":[{\"nodeId\":\"node-b\","
               "\"endpoint\":{\"backend\":\"http\",\"address\":\"10.0.0.2:7000\"}}]}");
    SchedulerWireTransport wire;
    MT_EXPECT_TRUE(StartFake(&fake, &wire, ""));

    std::vector<SchedulerWirePeer> peers;
    MT_EXPECT_TRUE(wire.lookup("cluster-1", "http", "layer/xyz", "node-self", &peers));
    MT_EXPECT_EQ(static_cast<std::size_t>(1), peers.size());
    MT_EXPECT_EQ(std::string("node-b"), peers[0].node_id);

    const std::vector<FakeScheduler::Seen>& seen = fake.RequestsTo(kSchedulerLookupPath);
    MT_EXPECT_EQ(static_cast<std::size_t>(1), seen.size());
    MT_EXPECT_EQ(std::string("layer/xyz"), FieldOf(seen[0].body, "key"));
    // An empty token sends no header at all, rather than an empty one: a
    // server checking for presence must see it absent.
    MT_EXPECT_TRUE(!seen[0].has_token);
}

MT_TEST(record_and_forget_reach_different_paths) {
    FakeScheduler fake;
    SchedulerWireTransport wire;
    MT_EXPECT_TRUE(StartFake(&fake, &wire, ""));

    MT_EXPECT_TRUE(wire.record("cluster-1", "http", "layer/one", "node-self", true));
    MT_EXPECT_TRUE(wire.record("cluster-1", "http", "layer/one", "node-self", false));

    // The selector is the only difference between the two requests, so the
    // path is the thing that has to change. Both hitting one endpoint would
    // make an unpublish look like a publish to the scheduler.
    MT_EXPECT_EQ(static_cast<std::size_t>(1),
                 fake.RequestsTo(kSchedulerRecordPath).size());
    MT_EXPECT_EQ(static_cast<std::size_t>(1),
                 fake.RequestsTo(kSchedulerForgetPath).size());
    MT_EXPECT_EQ(std::string("node-self"),
                 FieldOf(fake.RequestsTo(kSchedulerRecordPath)[0].body, "nodeId"));
}

MT_TEST(a_non_2xx_answer_is_a_failed_rpc_not_an_empty_result) {
    FakeScheduler fake;
    fake.Reply(kSchedulerPeersPath, 500, "{}");
    SchedulerWireTransport wire;
    // The startup poll uses `fetch`, which now fails, so the fake is started
    // by hand instead.
    MT_EXPECT_TRUE(fake.valid());
    fake.Start();
    wire = MakeHttpSchedulerWire(fake.endpoint(), "", 2000, 2000);
    MT_EXPECT_TRUE(!!wire.fetch);

    // Reporting false is what keeps `SchedulerPeerDiscovery` holding its
    // previous peer list. Returning true with an empty vector would clear the
    // cache every time the scheduler hiccupped, which is the difference
    // between a brief outage and a cluster that forgets all its peers.
    bool failed_at_least_once = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::vector<SchedulerWirePeer> peers;
        if (!wire.fetch("c", "http", "self", &peers)) {
            failed_at_least_once = true;
            MT_EXPECT_TRUE(peers.empty());
            break;
        }
        ::usleep(10 * 1000);
    }
    MT_EXPECT_TRUE(failed_at_least_once);
}

MT_TEST(an_unreachable_scheduler_is_a_failed_rpc) {
    // A port nothing is bound to.
    char endpoint[64];
    std::snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u", 1u);
    const SchedulerWireTransport wire = MakeHttpSchedulerWire(endpoint, "", 1000, 1000);
    MT_EXPECT_TRUE(!!wire.fetch);

    std::vector<SchedulerWirePeer> peers;
    MT_EXPECT_TRUE(!wire.fetch("c", "http", "self", &peers));
    MT_EXPECT_TRUE(!wire.lookup("c", "http", "k", "self", &peers));
    MT_EXPECT_TRUE(!wire.record("c", "http", "k", "self", true));
}

MT_TEST(an_undecodable_body_is_a_failed_rpc_rather_than_a_cleared_cache) {
    FakeScheduler fake;
    fake.Reply(kSchedulerPeersPath, 200, "{\"peers\":\"not an array\"}");
    MT_EXPECT_TRUE(fake.valid());
    fake.Start();
    const SchedulerWireTransport wire =
        MakeHttpSchedulerWire(fake.endpoint(), "", 2000, 2000);

    bool failed_at_least_once = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::vector<SchedulerWirePeer> peers;
        if (!wire.fetch("c", "http", "self", &peers)) {
            failed_at_least_once = true;
            break;
        }
        ::usleep(10 * 1000);
    }
    MT_EXPECT_TRUE(failed_at_least_once);
}

// ---- wired into the discovery ----------------------------------------------

MT_TEST(the_discovery_serves_peers_the_wire_fetched_and_keeps_them_on_failure) {
    FakeScheduler fake;
    fake.Reply(kSchedulerPeersPath, 200,
               "{\"peers\":["
               "{\"nodeId\":\"node-a\","
               "\"endpoint\":{\"backend\":\"http\",\"address\":\"10.0.0.1:7000\"}},"
               "{\"nodeId\":\"node-b\","
               "\"endpoint\":{\"backend\":\"iroh\",\"address\":\"irrelevant\"}}"
               "]}");
    SchedulerWireTransport wire;
    MT_EXPECT_TRUE(StartFake(&fake, &wire, ""));

    std::shared_ptr<P2pPeerDiscovery> discovery = SchedulerPeerDiscovery::Start(
        fake.endpoint(), "node-self", "cluster-1", 1,
        core::Optional<std::string>(std::string("http")),
        wire.fetch, wire.record, wire.lookup);

    // `Refresh` is driven directly rather than waiting out the jittered first
    // poll, which is up to a full interval away.
    static_cast<SchedulerPeerDiscovery*>(discovery.get())->Refresh();

    const P2pResult<std::vector<P2pPeer> > peers = discovery->Peers();
    MT_EXPECT_TRUE(peers.ok());
    // The iroh peer is filtered out: this node speaks `http`, and an endpoint
    // in another backend's address format is not dialable here.
    MT_EXPECT_EQ(static_cast<std::size_t>(1), peers.value().size());
    MT_EXPECT_EQ(std::string("node-a"), peers.value()[0].node_id);

    // Now make the scheduler fail and refresh again.
    fake.Reply(kSchedulerPeersPath, 503, "{}");
    static_cast<SchedulerPeerDiscovery*>(discovery.get())->Refresh();

    const P2pResult<std::vector<P2pPeer> > after = discovery->Peers();
    MT_EXPECT_TRUE(after.ok());
    // Rust logs the error and leaves the cache alone. Clearing it would turn a
    // momentary scheduler outage into every node losing all its peers at once.
    MT_EXPECT_EQ(static_cast<std::size_t>(1), after.value().size());
    MT_EXPECT_EQ(std::string("node-a"), after.value()[0].node_id);

    static_cast<SchedulerPeerDiscovery*>(discovery.get())->Stop();
}

MT_TEST(a_disabled_backend_issues_no_scheduler_calls_at_all) {
    FakeScheduler fake;
    SchedulerWireTransport wire;
    MT_EXPECT_TRUE(StartFake(&fake, &wire, ""));
    const std::size_t before = fake.RequestsTo(kSchedulerLookupPath).size();

    // `BackendId(Disabled)` is `None`, and that distinction is load-bearing:
    // `Some("")` would still issue the RPCs and then filter every peer out,
    // which is a silent no-provider result instead of no call.
    std::shared_ptr<P2pPeerDiscovery> discovery = SchedulerPeerDiscovery::Start(
        fake.endpoint(), "node-self", "cluster-1", 1,
        core::Optional<std::string>(), wire.fetch, wire.record, wire.lookup);

    MT_EXPECT_TRUE(discovery->PeersForKey("layer/abc").value().empty());
    MT_EXPECT_TRUE(discovery->RecordKey("layer/abc").ok());
    MT_EXPECT_TRUE(discovery->ForgetKey("layer/abc").ok());
    MT_EXPECT_EQ(before, fake.RequestsTo(kSchedulerLookupPath).size());

    static_cast<SchedulerPeerDiscovery*>(discovery.get())->Stop();
}

MT_TEST(peers_for_key_reports_an_unavailable_scheduler_as_an_error) {
    FakeScheduler fake;
    fake.Reply(kSchedulerLookupPath, 500, "{}");
    SchedulerWireTransport wire;
    MT_EXPECT_TRUE(StartFake(&fake, &wire, ""));

    std::shared_ptr<P2pPeerDiscovery> discovery = SchedulerPeerDiscovery::Start(
        fake.endpoint(), "node-self", "cluster-1", 1,
        core::Optional<std::string>(std::string("http")),
        wire.fetch, wire.record, wire.lookup);

    // An unavailable scheduler is an error, not "no providers". The caller has
    // to be able to tell the two apart: one is worth retrying, the other means
    // fall back to the origin.
    const P2pResult<std::vector<P2pPeer> > result = discovery->PeersForKey("layer/abc");
    MT_EXPECT_TRUE(!result.ok());

    static_cast<SchedulerPeerDiscovery*>(discovery.get())->Stop();
}

int main() { return microtest::RunAll(); }
