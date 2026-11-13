// SPDX-License-Identifier: MIT
// Rust: the axum router in src/overlaybd/p2p/facade.rs, plus the catch-all
// route pattern and loopback classification it needs from the server.
//
// This is the half that was missing. `FacadeCore`'s three handlers were
// complete and tested; nothing put them on a socket, so block-level P2P was
// off on every node and the generated overlaybd configs said
// `p2pConfig.enable: false` because the facade had no address to name.
#include "microtest.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/api/http_server.h"
#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/overlaybd/p2p/artifact.h"
#include "agentenv/overlaybd/p2p/http_routes.h"
#include "agentenv/p2p/mock.h"

using namespace agentenv;
using namespace agentenv::overlaybd::p2p;

namespace {

/// Collects the registered routes and dispatches to them, standing in for a
/// socket. The route *patterns* are asserted through `api::MatchPath`, which
/// is the matcher the real adapter hands them to.
class RecordingFacadeServer : public FacadeServer {
 public:
    void Route(const std::string& method, const std::string& path_pattern,
               Handler handler) override {
        Entry entry;
        entry.method = method;
        entry.pattern = path_pattern;
        entry.handler = handler;
        entries_.push_back(entry);
    }
    core::Expected<core::Unit, std::string> ListenAndServe(const std::string&) override {
        return core::Unit();
    }
    void Stop() override {}

    std::vector<std::string> Patterns() const {
        std::vector<std::string> out;
        for (std::size_t i = 0; i < entries_.size(); ++i) out.push_back(entries_[i].pattern);
        return out;
    }

    /// Routes like the real server does: match on the pattern, then method.
    bool Dispatch(const FacadeServerRequest& request, FacadeServerResponse* response) const {
        const std::size_t question = request.raw_target.find('?');
        const std::string path = question == std::string::npos
                                     ? request.raw_target
                                     : request.raw_target.substr(0, question);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            std::map<std::string, std::string> params;
            if (!api::MatchPath(entries_[i].pattern, path, &params)) continue;
            if (entries_[i].method != request.method) continue;
            entries_[i].handler(request, response);
            return true;
        }
        return false;
    }

 private:
    struct Entry {
        std::string method;
        std::string pattern;
        Handler handler;
    };
    std::vector<Entry> entries_;
};

struct TempRoot {
    std::string path;
    TempRoot() {
        char tmpl[] = "/tmp/aenv-facade-XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
};

const char* const kDigest =
    "sha256:aaaaaaaabbbbbbbbccccccccddddddddeeeeeeeeffffffff0000000011111111";

/// A mock transport holding one layer under its digest key.
std::shared_ptr<p2p::MockTransport> TransportWithLayer(const std::string& payload) {
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    const CanonicalBlobIdentity canonical = CanonicalBlobIdentity::FromDigest(kDigest);
    LayerMetadata metadata = LayerMetadata::FromDigest(
        kDigest, core::Optional<uint64_t>(payload.size()), core::Optional<std::string>());
    p2p::P2pPublishRequest request = p2p::P2pPublishRequest::FromBytes(
        LayerArtifactKey(canonical),
        std::vector<uint8_t>(payload.begin(), payload.end()));
    request.WithMetadata(metadata.ToJson());
    MT_EXPECT_TRUE(transport->Publish(request).ok());
    return transport;
}

std::shared_ptr<FacadeCore> CoreOver(std::shared_ptr<p2p::P2pTransport> transport,
                                     const std::vector<std::string>& roots) {
    FacadeConfig config;
    config.allowed_publish_roots = roots;
    return std::shared_ptr<FacadeCore>(new FacadeCore(
        transport, config, std::shared_ptr<OriginRangeFetcher>()));
}

FacadeServerRequest GetWithRange(const std::string& target, const std::string& range) {
    FacadeServerRequest request;
    request.method = "GET";
    request.raw_target = target;
    request.headers.Set("Range", range);
    return request;
}

}  // namespace

// ============================================================
// the route patterns
// ============================================================

MT_TEST(the_three_endpoints_are_registered) {
    RecordingFacadeServer server;
    RegisterOverlaybdP2pFacadeRoutes(&server, CoreOver(
        std::shared_ptr<p2p::P2pTransport>(new p2p::MockTransport()),
        std::vector<std::string>()));

    const std::vector<std::string> patterns = server.Patterns();
    MT_EXPECT_EQ(patterns.size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(patterns[0], std::string("/p2p-http/{*origin}"));
    MT_EXPECT_EQ(patterns[1], std::string("/p2p-uuid/{uuid}"));
    MT_EXPECT_EQ(patterns[2], std::string("/p2p-control/publish-layer"));
}

MT_TEST(a_null_core_registers_nothing) {
    // A disabled runtime must not be able to serve: registering handlers over
    // a null core would dereference it on the first request.
    RecordingFacadeServer server;
    RegisterOverlaybdP2pFacadeRoutes(&server, std::shared_ptr<FacadeCore>());
    MT_EXPECT_EQ(server.Patterns().size(), static_cast<std::size_t>(0));
}

// ============================================================
// the catch-all, which is what makes the origin route expressible
// ============================================================

MT_TEST(the_catch_all_captures_an_origin_url_with_its_separators) {
    // The reason the pattern exists: the captured remainder is a URL, so the
    // `/` characters inside it must survive. A `{origin}` placeholder would
    // match only one segment and this path would 404.
    std::map<std::string, std::string> params;
    MT_EXPECT_TRUE(api::MatchPath("/p2p-http/{*origin}",
                                  "/p2p-http/https://reg.example/v2/repo/blobs/sha256:abc",
                                  &params));
    MT_EXPECT_EQ(params["origin"],
                 std::string("https:/reg.example/v2/repo/blobs/sha256:abc"));
}

MT_TEST(the_catch_all_requires_a_non_empty_remainder) {
    // axum's wildcard needs at least one segment. Dispatching `/p2p-http`
    // with an empty origin would reach `ReconstructOriginUrl` as a malformed
    // request rather than being a 404, which is what it is.
    MT_EXPECT_TRUE(!api::MatchPath("/p2p-http/{*origin}", "/p2p-http", NULL));
    MT_EXPECT_TRUE(!api::MatchPath("/p2p-http/{*origin}", "/p2p-http/", NULL));
}

MT_TEST(the_catch_all_does_not_percent_decode) {
    // An encoded separator in the origin's own path must stay encoded: the
    // capture is reassembled into a URL, and decoding here would turn `%2F`
    // into a real separator and change which object is fetched.
    std::map<std::string, std::string> params;
    MT_EXPECT_TRUE(api::MatchPath("/p2p-http/{*rest}", "/p2p-http/a%2Fb/c", &params));
    MT_EXPECT_EQ(params["rest"], std::string("a%2Fb/c"));
}

MT_TEST(an_ordinary_placeholder_still_matches_exactly_one_segment) {
    // The catch-all must not have loosened the normal case, which is what
    // keeps `/sandboxes/{id}` from swallowing `/sandboxes/abc/exec`.
    MT_EXPECT_TRUE(api::MatchPath("/sandboxes/{id}", "/sandboxes/abc", NULL));
    MT_EXPECT_TRUE(!api::MatchPath("/sandboxes/{id}", "/sandboxes/abc/exec", NULL));
    MT_EXPECT_TRUE(!api::MatchPath("/sandboxes/{id}", "/sandboxes", NULL));
}

// ============================================================
// loopback classification — the publish endpoint's gate
// ============================================================

MT_TEST(loopback_classification_covers_every_local_form) {
    MT_EXPECT_TRUE(api::IsLoopbackAddressText("127.0.0.1"));
    // The whole 127/8 block: a client bound to 127.0.0.2 is equally local.
    MT_EXPECT_TRUE(api::IsLoopbackAddressText("127.0.0.2"));
    MT_EXPECT_TRUE(api::IsLoopbackAddressText("127.255.255.254"));
    MT_EXPECT_TRUE(api::IsLoopbackAddressText("::1"));
    // What a dual-stack listener reports for an IPv4 client.
    MT_EXPECT_TRUE(api::IsLoopbackAddressText("::ffff:127.0.0.1"));
    MT_EXPECT_TRUE(api::IsLoopbackAddressText("::FFFF:127.0.0.1"));
}

MT_TEST(loopback_classification_fails_closed) {
    MT_EXPECT_TRUE(!api::IsLoopbackAddressText("10.0.0.1"));
    MT_EXPECT_TRUE(!api::IsLoopbackAddressText("128.0.0.1"));
    MT_EXPECT_TRUE(!api::IsLoopbackAddressText("::ffff:10.0.0.1"));
    MT_EXPECT_TRUE(!api::IsLoopbackAddressText("2001:db8::1"));
    // Unknown or unparseable is not loopback: the gate must fail closed.
    MT_EXPECT_TRUE(!api::IsLoopbackAddressText(""));
    MT_EXPECT_TRUE(!api::IsLoopbackAddressText("localhost"));
    MT_EXPECT_TRUE(!api::IsLoopbackAddressText("127.0.0.1.5"));
}

// ============================================================
// the publish endpoint
// ============================================================

MT_TEST(publish_refuses_a_non_loopback_peer) {
    RecordingFacadeServer server;
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    RegisterOverlaybdP2pFacadeRoutes(&server, CoreOver(transport, std::vector<std::string>()));

    FacadeServerRequest request;
    request.method = "POST";
    request.raw_target = "/p2p-control/publish-layer";
    request.body = "{}";
    request.peer_is_loopback = false;

    FacadeServerResponse response;
    MT_EXPECT_TRUE(server.Dispatch(request, &response));
    MT_EXPECT_EQ(response.status, 403);
    // Refused before the body was parsed, so nothing was hashed on behalf of
    // a caller that was never going to be allowed.
    MT_EXPECT_EQ(transport->PublishCount(), 0);
}

MT_TEST(publish_accepts_a_loopback_peer_with_an_allowlisted_layer) {
    TempRoot root;
    const std::string layer = root.path + "/layer.commit";
    const std::string payload = "overlaybd-layer-bytes";
    MT_EXPECT_TRUE(core::fs::Write(layer, payload).ok());
    const core::Expected<core::FileDigest, std::string> described = core::DescribeFile(layer);
    MT_EXPECT_TRUE(described.ok());
    if (!described.ok()) return;

    const core::Expected<std::vector<std::string>, std::string> roots =
        CanonicalizePublishRoots(std::vector<std::string>(1, root.path));
    MT_EXPECT_TRUE(roots.ok());
    if (!roots.ok()) return;

    RecordingFacadeServer server;
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    RegisterOverlaybdP2pFacadeRoutes(&server, CoreOver(transport, roots.value()));

    PublishLayerRequest payload_request;
    payload_request.path = layer;
    payload_request.digest = described.value().sha256;
    payload_request.size = described.value().size;

    FacadeServerRequest request;
    request.method = "POST";
    request.raw_target = "/p2p-control/publish-layer";
    request.body = payload_request.ToJson();
    request.peer_is_loopback = true;

    FacadeServerResponse response;
    MT_EXPECT_TRUE(server.Dispatch(request, &response));
    MT_EXPECT_EQ(response.status, 200);
    MT_EXPECT_EQ(transport->PublishCount(), 1);
}

MT_TEST(publish_refuses_a_path_outside_the_allowlist) {
    TempRoot allowed;
    TempRoot outside;
    const std::string layer = outside.path + "/layer.commit";
    MT_EXPECT_TRUE(core::fs::Write(layer, "bytes").ok());
    const core::Expected<core::FileDigest, std::string> described = core::DescribeFile(layer);
    MT_EXPECT_TRUE(described.ok());
    if (!described.ok()) return;

    const core::Expected<std::vector<std::string>, std::string> roots =
        CanonicalizePublishRoots(std::vector<std::string>(1, allowed.path));
    MT_EXPECT_TRUE(roots.ok());
    if (!roots.ok()) return;

    RecordingFacadeServer server;
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    RegisterOverlaybdP2pFacadeRoutes(&server, CoreOver(transport, roots.value()));

    PublishLayerRequest payload_request;
    payload_request.path = layer;
    payload_request.digest = described.value().sha256;
    payload_request.size = described.value().size;

    FacadeServerRequest request;
    request.method = "POST";
    request.raw_target = "/p2p-control/publish-layer";
    request.body = payload_request.ToJson();
    request.peer_is_loopback = true;

    FacadeServerResponse response;
    MT_EXPECT_TRUE(server.Dispatch(request, &response));
    MT_EXPECT_EQ(response.status, 403);
    MT_EXPECT_EQ(transport->PublishCount(), 0);
}

// ============================================================
// ranged reads
// ============================================================

MT_TEST(a_uuid_read_serves_bytes_from_a_peer) {
    const std::string payload = "0123456789abcdef";
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    const CanonicalBlobIdentity canonical = CanonicalBlobIdentity::FromUuid("layer-uuid");
    LayerMetadata metadata =
        LayerMetadata::FromUuid("layer-uuid", core::Optional<uint64_t>(payload.size()));
    p2p::P2pPublishRequest publish = p2p::P2pPublishRequest::FromBytes(
        LayerArtifactKey(canonical),
        std::vector<uint8_t>(payload.begin(), payload.end()));
    publish.WithMetadata(metadata.ToJson());
    MT_EXPECT_TRUE(transport->Publish(publish).ok());

    RecordingFacadeServer server;
    RegisterOverlaybdP2pFacadeRoutes(&server, CoreOver(transport, std::vector<std::string>()));

    FacadeServerResponse response;
    MT_EXPECT_TRUE(server.Dispatch(
        GetWithRange("/p2p-uuid/layer-uuid", "bytes=4-7"), &response));
    MT_EXPECT_EQ(response.status, 206);
    MT_EXPECT_EQ(response.body, std::string("4567"));
    MT_EXPECT_EQ(response.headers["Content-Range"], std::string("bytes 4-7/16"));
    MT_EXPECT_EQ(response.headers["Content-Length"], std::string("4"));
    MT_EXPECT_EQ(response.headers["Accept-Ranges"], std::string("bytes"));
}

MT_TEST(a_uuid_miss_is_a_404) {
    // A uuid-addressed layer has no origin, so a miss is terminal rather than
    // a fallback.
    RecordingFacadeServer server;
    RegisterOverlaybdP2pFacadeRoutes(&server, CoreOver(
        std::shared_ptr<p2p::P2pTransport>(new p2p::MockTransport()),
        std::vector<std::string>()));

    FacadeServerResponse response;
    MT_EXPECT_TRUE(server.Dispatch(
        GetWithRange("/p2p-uuid/absent-uuid", "bytes=0-7"), &response));
    MT_EXPECT_EQ(response.status, 404);
}

MT_TEST(an_origin_read_serves_from_a_peer_when_the_digest_matches) {
    // The origin URL embeds the digest, which is how the facade finds the
    // layer in the catalog without asking the registry anything.
    const std::string payload = "abcdefghijklmnop";
    std::shared_ptr<p2p::MockTransport> transport = TransportWithLayer(payload);

    RecordingFacadeServer server;
    RegisterOverlaybdP2pFacadeRoutes(&server, CoreOver(transport, std::vector<std::string>()));

    FacadeServerResponse response;
    MT_EXPECT_TRUE(server.Dispatch(
        GetWithRange(std::string("/p2p-http/https://reg.example/v2/repo/blobs/") + kDigest,
                     "bytes=0-3"),
        &response));
    MT_EXPECT_EQ(response.status, 206);
    MT_EXPECT_EQ(response.body, std::string("abcd"));
}

MT_TEST(an_origin_miss_without_a_fetcher_is_a_502) {
    // With no origin fetcher injected there is nothing to fall back to. This
    // is the shape a node had *everywhere* before the production fetcher
    // existed: every P2P miss answered 502.
    RecordingFacadeServer server;
    RegisterOverlaybdP2pFacadeRoutes(&server, CoreOver(
        std::shared_ptr<p2p::P2pTransport>(new p2p::MockTransport()),
        std::vector<std::string>()));

    FacadeServerResponse response;
    MT_EXPECT_TRUE(server.Dispatch(
        GetWithRange("/p2p-http/https://reg.example/v2/repo/blobs/sha256:absent", "bytes=0-3"),
        &response));
    MT_EXPECT_EQ(response.status, 502);
}

MT_TEST(a_missing_range_header_is_a_400) {
    RecordingFacadeServer server;
    RegisterOverlaybdP2pFacadeRoutes(&server, CoreOver(
        std::shared_ptr<p2p::P2pTransport>(new p2p::MockTransport()),
        std::vector<std::string>()));

    FacadeServerRequest request;
    request.method = "GET";
    request.raw_target = "/p2p-uuid/layer-uuid";

    FacadeServerResponse response;
    MT_EXPECT_TRUE(server.Dispatch(request, &response));
    MT_EXPECT_EQ(response.status, 400);
}

MT_TEST(a_conditional_header_is_refused) {
    // The facade has no validators to compare against, so answering an `If-*`
    // request unconditionally would be wrong rather than merely unsupported.
    RecordingFacadeServer server;
    RegisterOverlaybdP2pFacadeRoutes(&server, CoreOver(
        std::shared_ptr<p2p::P2pTransport>(new p2p::MockTransport()),
        std::vector<std::string>()));

    FacadeServerRequest request = GetWithRange("/p2p-uuid/layer-uuid", "bytes=0-3");
    request.headers.Set("If-None-Match", "\"etag\"");

    FacadeServerResponse response;
    MT_EXPECT_TRUE(server.Dispatch(request, &response));
    MT_EXPECT_EQ(response.status, 412);
}

// ============================================================
// DrainByteStream — the framing guarantee
// ============================================================

namespace {

/// Yields fixed chunks, then ends.
class ScriptedStream : public p2p::P2pByteStream {
 public:
    explicit ScriptedStream(const std::vector<std::string>& chunks)
        : chunks_(chunks), next_(0) {}
    p2p::P2pResult<bool> Next(std::vector<uint8_t>* chunk_out) override {
        if (next_ >= chunks_.size()) return false;
        const std::string& chunk = chunks_[next_++];
        chunk_out->assign(chunk.begin(), chunk.end());
        return true;
    }

 private:
    std::vector<std::string> chunks_;
    std::size_t next_;
};

}  // namespace

MT_TEST(draining_joins_every_chunk) {
    std::vector<std::string> chunks;
    chunks.push_back("abc");
    chunks.push_back("de");
    std::shared_ptr<p2p::P2pByteStream> stream(new ScriptedStream(chunks));

    std::string out;
    MT_EXPECT_TRUE(DrainByteStream(stream, 5, &out).ok());
    MT_EXPECT_EQ(out, std::string("abcde"));
}

MT_TEST(draining_refuses_a_surplus_rather_than_trimming_it) {
    // More bytes than the window means the source answered a different
    // region, so the prefix cannot be assumed to be the right one either --
    // trimming would hand overlaybd the wrong data as if it were correct.
    std::vector<std::string> chunks;
    chunks.push_back("abc");
    chunks.push_back("de");
    std::shared_ptr<p2p::P2pByteStream> stream(new ScriptedStream(chunks));

    std::string out;
    MT_EXPECT_TRUE(!DrainByteStream(stream, 4, &out).ok());
}

MT_TEST(draining_a_null_stream_is_an_error) {
    std::string out;
    MT_EXPECT_TRUE(!DrainByteStream(std::shared_ptr<p2p::P2pByteStream>(), 4, &out).ok());
}

MT_MAIN
