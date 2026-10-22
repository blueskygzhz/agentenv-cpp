// SPDX-License-Identifier: MIT
// Two HttpP2pTransport instances over real loopback sockets.
//
// Separate from test_p2p_http_transport.cc, which drives the handlers
// directly. That suite proves the protocol's shape; this one proves the two
// halves actually agree once a socket is between them. The distinction
// matters because every bug this file can find is a mismatch the first suite
// cannot see: a header the client sets and the server looks for under another
// name, a status the server returns and the client does not expect, a length
// one side counts differently.
//
// Uses the real `api::Server` through the same adapter shape `main.cc`
// installs, so the route patterns are matched by the matcher that will match
// them in production rather than by a stand-in.
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
#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/p2p/http_transport.h"
#include "microtest.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::p2p;  // NOLINT

namespace {

struct TempRoot {
    std::string path;
    TempRoot() {
        auto d = core::fs::CreateTempDir("agentenv-p2p-e2e-");
        path = d.ok() ? d.value() : std::string("/tmp/agentenv-p2p-e2e-fallback");
        core::fs::CreateDirAll(path);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
};

/// The adapter `main.cc` installs, duplicated here rather than exported.
///
/// Exporting it would put an `api::Server` type in a header `agentenv-p2p`
/// consumers include, which is the dependency the injection exists to avoid.
class ApiServerBlobServer : public P2pBlobServer {
 public:
    ApiServerBlobServer() : server_(api::MakeServer()) {}

    bool valid() const { return !!server_; }

    void Route(const std::string& method, const std::string& path_pattern,
               Handler handler) override {
        if (!server_) return;
        server_->Route(method, path_pattern,
                       [handler](const api::HttpRequest& request,
                                 api::HttpResponse* response) {
            P2pServerRequest translated;
            translated.method = request.method;
            translated.path = request.path;
            translated.body = request.body;
            for (std::unordered_map<std::string, std::string>::const_iterator it =
                     request.headers.begin();
                 it != request.headers.end(); ++it) {
                translated.headers[it->first] = it->second;
            }

            P2pServerResponse produced;
            handler(translated, &produced);

            response->status = produced.status;
            response->body = produced.body;
            for (std::map<std::string, std::string>::const_iterator it =
                     produced.headers.begin();
                 it != produced.headers.end(); ++it) {
                response->headers[it->first] = it->second;
            }
        });
    }

    core::Expected<core::Unit, std::string>
        ListenAndServe(const std::string& bind_addr) override {
        if (!server_) return core::make_unexpected(std::string("no server backend"));
        const core::Expected<core::Unit, core::AnyError> served =
            server_->ListenAndServe(bind_addr);
        if (!served.ok()) return core::make_unexpected(served.error().chain());
        return core::Unit{};
    }

    void Stop() override { if (server_) server_->Stop(); }

 private:
    std::unique_ptr<api::Server> server_;
};

/// A port nothing is bound to, obtained by binding and releasing.
///
/// Racy in principle; in practice the window is microseconds and the
/// alternative -- a fixed port -- fails whenever two runs overlap, which on a
/// build machine is routine.
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

std::string PortString(uint16_t port) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(port));
    return std::string(buf);
}

/// One node: a transport plus the store directory it owns.
struct Node {
    std::string                      store_dir;
    std::shared_ptr<HttpP2pTransport> transport;
    std::string                      address;

    ~Node() { if (transport) transport->Shutdown(); }
};

/// Starts a node and waits until its blob surface answers.
///
/// The wait is what makes the rest of the file deterministic: `Start` returns
/// as soon as the serve thread is spawned, so dialling immediately would race
/// the bind. Polling a real request is the only check that the socket is
/// actually accepting -- a sleep would be both slower and less certain.
bool StartNode(const std::string& root, const std::string& node_id,
               const std::string& token, std::vector<P2pPeer> peers, Node* out) {
    const uint16_t port = FreePort();
    if (port == 0) return false;

    out->store_dir = core::fs::Join(root, node_id);
    out->address = "127.0.0.1:" + PortString(port);

    HttpP2pTransport::Options options;
    options.config.store_dir = out->store_dir;
    options.config.has_listen_addr = true;
    options.config.listen_addr = out->address;
    options.config.lookup_timeout_ms = 3000;
    options.config.fetch_timeout_ms = 10000;
    options.node_id = node_id;
    options.advertise_addr = out->address;
    options.shared_token = token;

    std::shared_ptr<ApiServerBlobServer> adapter(new ApiServerBlobServer());
    if (!adapter->valid()) return false;
    options.server = adapter;

    std::shared_ptr<P2pPeerDiscovery> discovery(
        new StaticP2pPeerDiscovery(std::move(peers)));

    auto started = HttpP2pTransport::Start(std::move(options), discovery);
    if (!started.ok()) return false;
    out->transport = started.value();

    // Poll a lookup for a key nobody published: a clean miss proves the
    // listener is up and the token is accepted.
    for (int attempt = 0; attempt < 200; ++attempt) {
        P2pArtifactDescriptor ignored;
        const P2pResult<bool> probe = out->transport->Lookup("e2e/probe", &ignored);
        if (probe.ok()) return true;
        ::usleep(10 * 1000);
    }
    return false;
}

P2pPeer PeerFor(const Node& node, const std::string& node_id) {
    P2pEndpoint endpoint;
    endpoint.backend = kHttpBackendId;
    endpoint.address = node.address;
    P2pPeer peer;
    peer.node_id = node_id;
    peer.endpoint = endpoint;
    return peer;
}

}  // namespace

MT_TEST(a_second_node_discovers_and_fetches_what_the_first_published) {
    TempRoot root;

    Node provider;
    MT_EXPECT_TRUE(StartNode(root.path, "node-provider", "cluster-token",
                             std::vector<P2pPeer>(), &provider));

    // Big enough to be written and read back in several chunks by the range
    // stream, so the reassembly is exercised rather than assumed.
    std::string payload;
    payload.reserve(300000);
    for (int i = 0; i < 300000; ++i) {
        payload.push_back(static_cast<char>('a' + (i % 26)));
    }
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    MT_EXPECT_TRUE(provider.transport->Publish(
        P2pPublishRequest::FromBytes("e2e/layer", bytes)).ok());

    std::vector<P2pPeer> peers;
    peers.push_back(PeerFor(provider, "node-provider"));

    Node consumer;
    MT_EXPECT_TRUE(StartNode(root.path, "node-consumer", "cluster-token",
                             peers, &consumer));

    // The consumer holds nothing locally, so this has to cross the socket.
    P2pArtifactDescriptor descriptor;
    const P2pResult<bool> found = consumer.transport->Lookup("e2e/layer", &descriptor);
    MT_EXPECT_TRUE(found.ok());
    MT_EXPECT_TRUE(found.value());
    MT_EXPECT_TRUE(descriptor.has_backend_locator);
    MT_EXPECT_EQ(core::Sha256Digest(payload), descriptor.backend_locator);
    // The served descriptor names the provider, not the asking node.
    MT_EXPECT_EQ(static_cast<std::size_t>(1), descriptor.providers.size());
    MT_EXPECT_TRUE(!descriptor.providers[0].IsLocal());
    MT_EXPECT_EQ(std::string("node-provider"), descriptor.providers[0].peer.node_id);

    const std::string destination = core::fs::Join(root.path, "fetched.bin");
    const P2pResult<uint64_t> fetched =
        consumer.transport->Fetch(descriptor, destination);
    MT_EXPECT_TRUE(fetched.ok());
    MT_EXPECT_EQ(static_cast<uint64_t>(payload.size()), fetched.value());
    MT_EXPECT_EQ(payload, core::fs::ReadToString(destination).value());
}

MT_TEST(a_fetch_adopts_the_artifact_so_the_consumer_becomes_a_provider) {
    TempRoot root;

    Node provider;
    MT_EXPECT_TRUE(StartNode(root.path, "node-provider", "t",
                             std::vector<P2pPeer>(), &provider));

    const std::string payload = "adopt me";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    MT_EXPECT_TRUE(provider.transport->Publish(
        P2pPublishRequest::FromBytes("e2e/adopt", bytes)).ok());

    std::vector<P2pPeer> peers;
    peers.push_back(PeerFor(provider, "node-provider"));
    Node consumer;
    MT_EXPECT_TRUE(StartNode(root.path, "node-consumer", "t", peers, &consumer));

    P2pArtifactDescriptor descriptor;
    MT_EXPECT_TRUE(consumer.transport->Lookup("e2e/adopt", &descriptor).value());
    MT_EXPECT_TRUE(consumer.transport->Fetch(
        descriptor, core::fs::Join(root.path, "adopted.bin")).ok());

    // `P2pFetchOptions::advertise` defaults to true -- deliberately, it is an
    // `impl Default` and not a derive -- so a node that has pulled an
    // artifact starts serving it. Without this a cluster never spreads load
    // past whichever node published first.
    P2pArtifactDescriptor local;
    const P2pResult<bool> now_local = consumer.transport->Lookup("e2e/adopt", &local);
    MT_EXPECT_TRUE(now_local.ok());
    MT_EXPECT_TRUE(now_local.value());
    MT_EXPECT_TRUE(local.providers.size() == 1 && local.providers[0].IsLocal());
}

MT_TEST(a_remote_range_stream_delivers_exactly_the_requested_window) {
    TempRoot root;

    Node provider;
    MT_EXPECT_TRUE(StartNode(root.path, "node-provider", "t",
                             std::vector<P2pPeer>(), &provider));

    std::string payload;
    for (int i = 0; i < 5000; ++i) payload.push_back(static_cast<char>('0' + (i % 10)));
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    MT_EXPECT_TRUE(provider.transport->Publish(
        P2pPublishRequest::FromBytes("e2e/range", bytes)).ok());

    std::vector<P2pPeer> peers;
    peers.push_back(PeerFor(provider, "node-provider"));
    Node consumer;
    MT_EXPECT_TRUE(StartNode(root.path, "node-consumer", "t", peers, &consumer));

    P2pArtifactDescriptor descriptor;
    MT_EXPECT_TRUE(consumer.transport->Lookup("e2e/range", &descriptor).value());

    auto stream = consumer.transport->FetchByteRange(descriptor, 1234, 777);
    MT_EXPECT_TRUE(stream.ok());

    std::string collected;
    std::vector<uint8_t> chunk;
    for (;;) {
        const P2pResult<bool> more = stream.value()->Next(&chunk);
        MT_EXPECT_TRUE(more.ok());
        if (!more.value()) break;
        collected.append(chunk.begin(), chunk.end());
    }
    // This is the invariant the whole range path exists to hold: the stream
    // ends only once the full window has arrived, so a truncated transfer can
    // never read as a clean end of stream.
    MT_EXPECT_EQ(payload.substr(1234, 777), collected);
}

MT_TEST(a_mismatched_token_makes_the_peer_invisible_rather_than_noisy) {
    TempRoot root;

    Node provider;
    MT_EXPECT_TRUE(StartNode(root.path, "node-provider", "right-token",
                             std::vector<P2pPeer>(), &provider));

    std::vector<uint8_t> bytes(16, 'x');
    MT_EXPECT_TRUE(provider.transport->Publish(
        P2pPublishRequest::FromBytes("e2e/secret", bytes)).ok());

    std::vector<P2pPeer> peers;
    peers.push_back(PeerFor(provider, "node-provider"));
    Node consumer;
    MT_EXPECT_TRUE(StartNode(root.path, "node-consumer", "wrong-token",
                             peers, &consumer));

    // The 401 is a miss, not an error: the caller's job is to find *a*
    // provider, and one node refusing us does not make the question
    // unanswerable. Surfacing it as a failure would turn a misconfigured peer
    // into a failed image pull.
    P2pArtifactDescriptor descriptor;
    const P2pResult<bool> found = consumer.transport->Lookup("e2e/secret", &descriptor);
    MT_EXPECT_TRUE(found.ok());
    MT_EXPECT_TRUE(!found.value());
}

MT_TEST(unpublishing_on_the_provider_stops_the_consumer_from_finding_it) {
    TempRoot root;

    Node provider;
    MT_EXPECT_TRUE(StartNode(root.path, "node-provider", "t",
                             std::vector<P2pPeer>(), &provider));

    std::vector<uint8_t> bytes(32, 'y');
    MT_EXPECT_TRUE(provider.transport->Publish(
        P2pPublishRequest::FromBytes("e2e/transient", bytes)).ok());

    std::vector<P2pPeer> peers;
    peers.push_back(PeerFor(provider, "node-provider"));
    Node consumer;
    MT_EXPECT_TRUE(StartNode(root.path, "node-consumer", "t", peers, &consumer));

    P2pArtifactDescriptor descriptor;
    MT_EXPECT_TRUE(consumer.transport->Lookup("e2e/transient", &descriptor).value());

    MT_EXPECT_TRUE(provider.transport->Unpublish("e2e/transient").value());

    // Retracted immediately over the wire, not merely locally.
    P2pArtifactDescriptor after;
    const P2pResult<bool> gone = consumer.transport->Lookup("e2e/transient", &after);
    MT_EXPECT_TRUE(gone.ok());
    MT_EXPECT_TRUE(!gone.value());

    // And the bytes stop being servable too, so a consumer holding the old
    // descriptor cannot still pull them.
    const P2pResult<uint64_t> fetched = consumer.transport->Fetch(
        descriptor, core::fs::Join(root.path, "should-not-arrive.bin"));
    MT_EXPECT_TRUE(!fetched.ok());
}

MT_TEST(a_fetch_learns_the_size_from_the_first_range_without_a_head) {
    TempRoot root;

    Node provider;
    MT_EXPECT_TRUE(StartNode(root.path, "node-provider", "t",
                             std::vector<P2pPeer>(), &provider));

    // Deliberately *not* a multiple of the fetch chunk size, so the last
    // request is a partial one and the loop has to stop on the total rather
    // than on a neat boundary.
    std::string payload;
    for (int i = 0; i < 4097; ++i) payload.push_back(static_cast<char>(i % 251));
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    MT_EXPECT_TRUE(provider.transport->Publish(
        P2pPublishRequest::FromBytes("e2e/sized", bytes)).ok());

    std::vector<P2pPeer> peers;
    peers.push_back(PeerFor(provider, "node-provider"));
    Node consumer;
    MT_EXPECT_TRUE(StartNode(root.path, "node-consumer", "t", peers, &consumer));

    P2pArtifactDescriptor descriptor;
    MT_EXPECT_TRUE(consumer.transport->Lookup("e2e/sized", &descriptor).value());

    // The client asks for a whole chunk, far more than exists. The server
    // clamps and reports the real total in `Content-Range`; the client takes
    // the size from there. No HEAD is involved -- it could not be, since
    // `core::http` cannot parse a bodiless reply.
    const std::string destination = core::fs::Join(root.path, "sized.bin");
    const P2pResult<uint64_t> fetched =
        consumer.transport->Fetch(descriptor, destination);
    MT_EXPECT_TRUE(fetched.ok());
    MT_EXPECT_EQ(static_cast<uint64_t>(4097), fetched.value());
    MT_EXPECT_EQ(payload, core::fs::ReadToString(destination).value());
}

MT_TEST(an_empty_artifact_fetches_as_an_empty_file_not_as_a_failure) {
    TempRoot root;

    Node provider;
    MT_EXPECT_TRUE(StartNode(root.path, "node-provider", "t",
                             std::vector<P2pPeer>(), &provider));

    MT_EXPECT_TRUE(provider.transport->Publish(
        P2pPublishRequest::FromBytes("e2e/empty", std::vector<uint8_t>())).ok());

    std::vector<P2pPeer> peers;
    peers.push_back(PeerFor(provider, "node-provider"));
    Node consumer;
    MT_EXPECT_TRUE(StartNode(root.path, "node-consumer", "t", peers, &consumer));

    P2pArtifactDescriptor descriptor;
    MT_EXPECT_TRUE(consumer.transport->Lookup("e2e/empty", &descriptor).value());

    // Every range over an empty artifact is unsatisfiable, so the server can
    // only answer 416. The client has to read that as "zero bytes, nothing to
    // do" rather than as an error -- otherwise a legitimately empty layer
    // becomes an unfetchable one.
    const std::string destination = core::fs::Join(root.path, "empty.bin");
    const P2pResult<uint64_t> fetched =
        consumer.transport->Fetch(descriptor, destination);
    MT_EXPECT_TRUE(fetched.ok());
    MT_EXPECT_EQ(static_cast<uint64_t>(0), fetched.value());
    MT_EXPECT_TRUE(core::fs::Exists(destination));
    MT_EXPECT_EQ(std::string(""), core::fs::ReadToString(destination).value());
}

MT_TEST(a_corrupted_transfer_is_refused_by_the_digest_check) {
    TempRoot root;

    Node provider;
    MT_EXPECT_TRUE(StartNode(root.path, "node-provider", "t",
                             std::vector<P2pPeer>(), &provider));

    const std::string payload = "original content";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    MT_EXPECT_TRUE(provider.transport->Publish(
        P2pPublishRequest::FromBytes("e2e/corrupt", bytes)).ok());

    std::vector<P2pPeer> peers;
    peers.push_back(PeerFor(provider, "node-provider"));
    Node consumer;
    MT_EXPECT_TRUE(StartNode(root.path, "node-consumer", "t", peers, &consumer));

    P2pArtifactDescriptor descriptor;
    MT_EXPECT_TRUE(consumer.transport->Lookup("e2e/corrupt", &descriptor).value());

    // Rewrite the provider's stored bytes behind its back, keeping the length
    // so the per-range length check cannot catch it. This backend carries no
    // per-chunk proof the way iroh's verified streaming does, which makes the
    // whole-artifact digest the only thing standing between a consumer and
    // silently wrong content.
    std::shared_ptr<BlobLease> lease =
        provider.transport->store()->Acquire(descriptor.backend_locator);
    MT_EXPECT_TRUE(lease != nullptr);
    MT_EXPECT_TRUE(core::fs::Write(lease->entry().path, "ORIGINAL CONTENT").ok());
    lease.reset();

    const std::string destination = core::fs::Join(root.path, "corrupt.bin");
    const P2pResult<uint64_t> fetched =
        consumer.transport->Fetch(descriptor, destination);
    MT_EXPECT_TRUE(!fetched.ok());
    // Refused *and* not left behind: a rejected artifact that stayed at the
    // destination would be indistinguishable from a good one to the next
    // reader.
    MT_EXPECT_TRUE(!core::fs::Exists(destination));
}

int main() { return microtest::RunAll(); }
