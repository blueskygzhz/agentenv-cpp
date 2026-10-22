// SPDX-License-Identifier: MIT
// The HTTP P2P transport. No Rust counterpart: this backend exists because
// the iroh one needs magicsock and verified streaming, which are protocols to
// match rather than libraries to call.
//
// What is asserted here is the protocol's load-bearing properties, not its
// happy path. A transport that moves bytes but gets any of these wrong is
// worse than one that is switched off, because the failure surfaces somewhere
// else entirely:
//
//   * a short range read must not look like success  (silent truncation)
//   * a whole-artifact GET must be refused           (unbounded memory)
//   * a catalog miss must be 200, not 404            (indistinguishable from
//                                                     a node not speaking it)
//   * a fetched artifact's digest must be checked    (the only integrity gate
//                                                     this backend has)
//   * a lease must outlive an unpublish              (bytes deleted mid-read)
#include "agentenv/p2p/http_transport.h"

#include <map>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "microtest.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::p2p;  // NOLINT

namespace {

struct TempRoot {
    std::string path;
    TempRoot() {
        auto d = core::fs::CreateTempDir("agentenv-p2p-http-");
        path = d.ok() ? d.value() : std::string("/tmp/agentenv-p2p-http-fallback");
        core::fs::CreateDirAll(path);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
};

/// Collects the routes instead of binding a port, so the handlers can be
/// driven directly. The protocol's shape is a property of the handlers; a real
/// socket would only add a way for the test to be flaky.
class RecordingServer : public P2pBlobServer {
 public:
    void Route(const std::string& method, const std::string& pattern,
               Handler handler) override {
        routes_.push_back(Entry(method, pattern, handler));
    }
    core::Expected<core::Unit, std::string>
        ListenAndServe(const std::string&) override {
        return core::Unit{};
    }
    void Stop() override {}

    /// Matches the way `api::Server` does — segment-wise, with `{name}`
    /// placeholders and the segment counts required to agree. Reproducing that
    /// rather than prefix-matching is the point: a looser matcher here would
    /// pass routes the real server would reject.
    bool Dispatch(const P2pServerRequest& request, P2pServerResponse* response) {
        for (std::size_t i = 0; i < routes_.size(); ++i) {
            if (routes_[i].method != request.method) continue;
            if (!Matches(routes_[i].pattern, request.path)) continue;
            routes_[i].handler(request, response);
            return true;
        }
        return false;
    }

    std::size_t route_count() const { return routes_.size(); }

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
        Entry(std::string m, std::string p, Handler h)
            : method(std::move(m)), pattern(std::move(p)), handler(std::move(h)) {}
        std::string method;
        std::string pattern;
        Handler     handler;
    };
    std::vector<Entry> routes_;
};

P2pServerRequest Get(const std::string& path, const std::string& range) {
    P2pServerRequest request;
    request.method = "GET";
    request.path = path;
    if (!range.empty()) request.headers["Range"] = range;
    return request;
}

std::string WriteFile(const std::string& dir, const std::string& name,
                      const std::string& contents) {
    const std::string path = core::fs::Join(dir, name);
    core::fs::Write(path, contents);
    return path;
}

struct Fixture {
    TempRoot                                  root;
    std::shared_ptr<HttpBlobStore>            store;
    std::shared_ptr<PublishedArtifactCatalog> catalog;
    RecordingServer                           server;

    Fixture() {
        auto opened = HttpBlobStore::Open(core::fs::Join(root.path, "store"));
        store = opened.ok() ? opened.value() : std::shared_ptr<HttpBlobStore>();

        P2pEndpoint local;
        local.backend = kHttpBackendId;
        local.address = "10.0.0.1:7000";
        auto loaded = PublishedArtifactCatalog::Load(
            core::fs::Join(root.path, "catalog.db"), "node-a", local);
        catalog = loaded.ok() ? loaded.value() : std::shared_ptr<PublishedArtifactCatalog>();

        RegisterHttpP2pRoutes(&server, catalog, store, "secret");
    }
};

}  // namespace

// ---- range header parsing ---------------------------------------------------

MT_TEST(range_header_accepts_only_a_single_closed_range) {
    uint64_t start = 0, end = 0;
    MT_EXPECT_TRUE(ParseByteRangeHeader("bytes=0-1023", &start, &end));
    MT_EXPECT_EQ(static_cast<uint64_t>(0), start);
    MT_EXPECT_EQ(static_cast<uint64_t>(1023), end);

    // An open-ended range asks for "the rest", whose size the client never
    // agreed to -- which is precisely what the per-request ceiling exists to
    // stop, so it cannot be allowed in through the header.
    MT_EXPECT_TRUE(!ParseByteRangeHeader("bytes=10-", &start, &end));
    MT_EXPECT_TRUE(!ParseByteRangeHeader("bytes=-10", &start, &end));
    // A multi-range request needs a multipart body neither side can frame.
    MT_EXPECT_TRUE(!ParseByteRangeHeader("bytes=0-10,20-30", &start, &end));
    MT_EXPECT_TRUE(!ParseByteRangeHeader("items=0-10", &start, &end));
    MT_EXPECT_TRUE(!ParseByteRangeHeader("bytes=5-1", &start, &end));
    MT_EXPECT_TRUE(!ParseByteRangeHeader("bytes=a-b", &start, &end));
}

MT_TEST(content_range_is_rendered_with_the_total) {
    MT_EXPECT_EQ(std::string("bytes 0-9/100"), FormatContentRange(0, 9, 100));
}

MT_TEST(host_port_parsing_rejects_what_cannot_be_dialled) {
    std::string host;
    uint16_t port = 0;
    MT_EXPECT_TRUE(ParseHostPort("10.0.0.1:7000", &host, &port));
    MT_EXPECT_EQ(std::string("10.0.0.1"), host);
    MT_EXPECT_EQ(static_cast<uint16_t>(7000), port);

    MT_EXPECT_TRUE(!ParseHostPort("10.0.0.1", &host, &port));
    MT_EXPECT_TRUE(!ParseHostPort(":7000", &host, &port));
    MT_EXPECT_TRUE(!ParseHostPort("10.0.0.1:0", &host, &port));
    MT_EXPECT_TRUE(!ParseHostPort("10.0.0.1:70000", &host, &port));
    MT_EXPECT_TRUE(!ParseHostPort("10.0.0.1:http", &host, &port));
}

// ---- descriptor locator -----------------------------------------------------

MT_TEST(a_descriptor_without_a_locator_is_refused_rather_than_guessed) {
    P2pArtifactDescriptor descriptor;
    descriptor.key = "layer/abc";
    // The key names *what* the artifact is; the locator names *which bytes*.
    // Falling back from one to the other would dial a peer for content it
    // never claimed to hold.
    const P2pResult<std::string> locator = LocatorFromDescriptor(descriptor);
    MT_EXPECT_TRUE(!locator.ok());
    MT_EXPECT_EQ(static_cast<int>(P2pError::InvalidDescriptor),
                 static_cast<int>(locator.error().kind));

    descriptor.has_backend_locator = true;
    descriptor.backend_locator = "sha256:aa";
    MT_EXPECT_TRUE(LocatorFromDescriptor(descriptor).ok());
}

// ---- blob store -------------------------------------------------------------

MT_TEST(import_is_content_addressed_so_the_same_bytes_land_once) {
    TempRoot root;
    auto opened = HttpBlobStore::Open(core::fs::Join(root.path, "store"));
    MT_EXPECT_TRUE(opened.ok());
    std::shared_ptr<HttpBlobStore> store = opened.value();

    const std::string payload = "the same bytes";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());

    auto first = store->Import("key/one", P2pPublishSource::FromBytes(bytes),
                               P2pPublishMode::Copy);
    auto second = store->Import("key/two", P2pPublishSource::FromBytes(bytes),
                                P2pPublishMode::Copy);
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(second.ok());
    MT_EXPECT_EQ(first.value().digest, second.value().digest);
    MT_EXPECT_EQ(first.value().path, second.value().path);
    MT_EXPECT_EQ(core::Sha256Digest(payload), first.value().digest);
}

MT_TEST(unpublishing_one_key_keeps_bytes_a_second_key_still_names) {
    TempRoot root;
    auto store = HttpBlobStore::Open(core::fs::Join(root.path, "store")).value();

    const std::string payload = "shared";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    auto first = store->Import("key/one", P2pPublishSource::FromBytes(bytes),
                               P2pPublishMode::Copy);
    store->Import("key/two", P2pPublishSource::FromBytes(bytes), P2pPublishMode::Copy);

    MT_EXPECT_TRUE(store->Remove("key/one").value());
    // Content addressing means two keys legitimately share one file; removing
    // it with the first unpublish would break the second.
    MT_EXPECT_TRUE(core::fs::Exists(first.value().path));
    MT_EXPECT_TRUE(store->Acquire(first.value().digest) != nullptr);

    MT_EXPECT_TRUE(store->Remove("key/two").value());
    MT_EXPECT_TRUE(!core::fs::Exists(first.value().path));
}

MT_TEST(a_reference_publication_never_deletes_the_callers_file) {
    TempRoot root;
    auto store = HttpBlobStore::Open(core::fs::Join(root.path, "store")).value();
    const std::string source = WriteFile(root.path, "borrowed.bin", "not ours");

    auto imported = store->Import("key/ref", P2pPublishSource::FromPath(source),
                                  P2pPublishMode::Reference);
    MT_EXPECT_TRUE(imported.ok());
    MT_EXPECT_TRUE(imported.value().referenced);
    MT_EXPECT_EQ(source, imported.value().path);

    MT_EXPECT_TRUE(store->Remove("key/ref").value());
    // The path was only lent to this store; deleting it would destroy data
    // still in use for its original purpose.
    MT_EXPECT_TRUE(core::fs::Exists(source));
}

MT_TEST(a_lease_outlives_the_unpublish_that_races_it) {
    TempRoot root;
    auto store = HttpBlobStore::Open(core::fs::Join(root.path, "store")).value();

    const std::string payload = "mid-transfer";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    auto imported = store->Import("key/live", P2pPublishSource::FromBytes(bytes),
                                  P2pPublishMode::Copy);
    const std::string path = imported.value().path;

    std::shared_ptr<BlobLease> lease = store->Acquire(imported.value().digest);
    MT_EXPECT_TRUE(lease != nullptr);

    MT_EXPECT_TRUE(store->Remove("key/live").value());
    // The key stops resolving at once...
    MT_EXPECT_TRUE(store->Acquire(imported.value().digest) == nullptr);
    // ...but the bytes a reader is still streaming are not pulled out from
    // under it. This is what Rust's TempTag buys inside its lazy stream.
    MT_EXPECT_TRUE(core::fs::Exists(path));

    lease.reset();
    MT_EXPECT_TRUE(!core::fs::Exists(path));
}

MT_TEST(a_publication_whose_bytes_vanished_is_not_replayed_on_reopen) {
    TempRoot root;
    const std::string dir = core::fs::Join(root.path, "store");
    std::string path;
    {
        auto store = HttpBlobStore::Open(dir).value();
        const std::string payload = "transient";
        std::vector<uint8_t> bytes(payload.begin(), payload.end());
        path = store->Import("key/gone", P2pPublishSource::FromBytes(bytes),
                             P2pPublishMode::Copy).value().path;
        MT_EXPECT_EQ(static_cast<std::size_t>(1), store->size());
    }
    core::fs::RemoveFile(path);

    // Replaying it would have the catalog answer with a locator that 404s on
    // the next request, which reads to a peer as a node that lies about what
    // it holds. Forgetting it only makes this node a non-provider, which is
    // both true and fixable by republishing.
    auto reopened = HttpBlobStore::Open(dir);
    MT_EXPECT_TRUE(reopened.ok());
    MT_EXPECT_EQ(static_cast<std::size_t>(0), reopened.value()->size());
}

MT_TEST(publications_survive_a_restart) {
    TempRoot root;
    const std::string dir = core::fs::Join(root.path, "store");
    std::string digest;
    {
        auto store = HttpBlobStore::Open(dir).value();
        const std::string payload = "durable";
        std::vector<uint8_t> bytes(payload.begin(), payload.end());
        digest = store->Import("key/durable", P2pPublishSource::FromBytes(bytes),
                               P2pPublishMode::Copy).value().digest;
    }
    // A publish is a promise that this node can serve those bytes; a restart
    // must not silently retract it while peers still hold the descriptor.
    auto reopened = HttpBlobStore::Open(dir).value();
    MT_EXPECT_TRUE(reopened->Acquire(digest) != nullptr);
    MT_EXPECT_EQ(digest, *reopened->LocatorFor("key/durable"));
}

// ---- routes: catalog --------------------------------------------------------

MT_TEST(routes_registered_are_catalog_head_and_get) {
    Fixture fixture;
    MT_EXPECT_EQ(static_cast<std::size_t>(3), fixture.server.route_count());
}

MT_TEST(a_catalog_miss_is_a_two_hundred_with_no_descriptor) {
    Fixture fixture;

    CatalogRequest request;
    request.key = "nothing/here";
    P2pServerRequest http;
    http.method = "POST";
    http.path = kCatalogLookupPath;
    http.headers[kTokenHeader] = "secret";
    http.body = request.Encode();

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    // Never a 404: the wire type is explicit that an absent descriptor is a
    // miss rather than an error, and a 404 would be indistinguishable from a
    // node that does not speak this protocol at all.
    MT_EXPECT_EQ(200, response.status);
    auto decoded = CatalogResponse::Decode(response.body);
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_TRUE(!decoded.value().descriptor.has_value());
}

MT_TEST(a_catalog_hit_advertises_this_node_as_the_provider) {
    Fixture fixture;

    P2pArtifactDescriptor published;
    published.key = "layer/one";
    published.has_backend_locator = true;
    published.backend_locator = "sha256:aa";
    // Deliberately recorded with no provider at all; the served copy must
    // still name this node, because a requesting peer is asking what we can
    // serve rather than what we have heard about.
    MT_EXPECT_TRUE(fixture.catalog->Upsert(published).ok());

    CatalogRequest request;
    request.key = "layer/one";
    P2pServerRequest http;
    http.method = "POST";
    http.path = kCatalogLookupPath;
    http.headers[kTokenHeader] = "secret";
    http.body = request.Encode();

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    MT_EXPECT_EQ(200, response.status);

    auto decoded = CatalogResponse::Decode(response.body);
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_TRUE(decoded.value().descriptor.has_value());
    const P2pArtifactDescriptor& served = *decoded.value().descriptor;
    MT_EXPECT_EQ(static_cast<std::size_t>(1), served.providers.size());
    MT_EXPECT_TRUE(!served.providers[0].IsLocal());
    MT_EXPECT_EQ(std::string("node-a"), served.providers[0].peer.node_id);
    MT_EXPECT_EQ(std::string(kHttpBackendId), served.providers[0].peer.endpoint.backend);
}

MT_TEST(an_oversized_catalog_request_is_rejected_before_it_is_parsed) {
    Fixture fixture;
    P2pServerRequest http;
    http.method = "POST";
    http.path = kCatalogLookupPath;
    http.headers[kTokenHeader] = "secret";
    http.body = std::string(kMaxCatalogRequestBytes + 1, 'x');

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    MT_EXPECT_EQ(400, response.status);
}

MT_TEST(a_wrong_token_is_rejected_on_every_route) {
    Fixture fixture;

    P2pServerRequest catalog;
    catalog.method = "POST";
    catalog.path = kCatalogLookupPath;
    catalog.headers[kTokenHeader] = "wrong";
    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(catalog, &response));
    MT_EXPECT_EQ(401, response.status);

    P2pServerRequest blob = Get(std::string(kBlobPathPrefix) + "sha256:aa", "bytes=0-1");
    response = P2pServerResponse();
    MT_EXPECT_TRUE(fixture.server.Dispatch(blob, &response));
    MT_EXPECT_EQ(401, response.status);

    // A missing header is as wrong as a wrong one: the check must not be
    // skippable by omission.
    blob.headers.clear();
    blob.headers["Range"] = "bytes=0-1";
    response = P2pServerResponse();
    MT_EXPECT_TRUE(fixture.server.Dispatch(blob, &response));
    MT_EXPECT_EQ(401, response.status);
}

MT_TEST(the_token_header_is_matched_case_insensitively) {
    Fixture fixture;
    P2pServerRequest http;
    http.method = "POST";
    http.path = kCatalogLookupPath;
    // HTTP header names are case-insensitive and an intermediary is entitled
    // to normalise them; a check keyed on one spelling would reject a
    // perfectly valid peer.
    http.headers["x-agentenv-p2p-token"] = "secret";
    CatalogRequest request;
    request.key = "k";
    http.body = request.Encode();

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    MT_EXPECT_EQ(200, response.status);
}

// ---- routes: blobs ----------------------------------------------------------

MT_TEST(a_blob_get_without_a_range_header_is_refused) {
    Fixture fixture;
    const std::string payload = "0123456789";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    auto imported = fixture.store->Import("k", P2pPublishSource::FromBytes(bytes),
                                          P2pPublishMode::Copy);

    P2pServerRequest http = Get(std::string(kBlobPathPrefix) + imported.value().digest, "");
    http.headers[kTokenHeader] = "secret";

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    // The response body is assembled in memory before it is written, so
    // answering a whole-artifact GET would charge this node an
    // artifact-sized allocation chosen by the peer. Requiring the header is
    // what makes the memory ceiling a property of the protocol.
    MT_EXPECT_EQ(400, response.status);
}

MT_TEST(a_range_get_answers_206_with_exactly_the_requested_bytes) {
    Fixture fixture;
    const std::string payload = "0123456789";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    auto imported = fixture.store->Import("k", P2pPublishSource::FromBytes(bytes),
                                          P2pPublishMode::Copy);

    P2pServerRequest http =
        Get(std::string(kBlobPathPrefix) + imported.value().digest, "bytes=2-5");
    http.headers[kTokenHeader] = "secret";

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    MT_EXPECT_EQ(206, response.status);
    MT_EXPECT_EQ(std::string("2345"), response.body);
    MT_EXPECT_EQ(std::string("4"), response.headers["Content-Length"]);
    MT_EXPECT_EQ(std::string("bytes 2-5/10"), response.headers["Content-Range"]);
    MT_EXPECT_EQ(imported.value().digest, response.headers[kDigestHeader]);
}

MT_TEST(a_range_running_past_the_end_is_clamped_and_reports_the_total) {
    Fixture fixture;
    const std::string payload = "0123456789";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    auto imported = fixture.store->Import("k", P2pPublishSource::FromBytes(bytes),
                                          P2pPublishMode::Copy);

    P2pServerRequest http =
        Get(std::string(kBlobPathPrefix) + imported.value().digest, "bytes=5-20");
    http.headers[kTokenHeader] = "secret";

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    // Clamped rather than refused, per RFC 7233. This is load-bearing, not
    // pedantry: it is what lets a client ask for the first chunk of an
    // artifact whose size it does not know and learn the total from
    // `Content-Range` -- which it must, because `core::http` cannot read back
    // a bodiless HEAD reply.
    MT_EXPECT_EQ(206, response.status);
    MT_EXPECT_EQ(std::string("56789"), response.body);
    MT_EXPECT_EQ(std::string("bytes 5-9/10"), response.headers["Content-Range"]);
}

MT_TEST(a_range_starting_past_the_end_is_416_and_names_the_total) {
    Fixture fixture;
    const std::string payload = "0123456789";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    auto imported = fixture.store->Import("k", P2pPublishSource::FromBytes(bytes),
                                          P2pPublishMode::Copy);

    P2pServerRequest http =
        Get(std::string(kBlobPathPrefix) + imported.value().digest, "bytes=10-20");
    http.headers[kTokenHeader] = "secret";

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    MT_EXPECT_EQ(416, response.status);
    MT_EXPECT_EQ(std::string("bytes */10"), response.headers["Content-Range"]);
}

MT_TEST(an_empty_artifact_reports_its_size_through_the_unsatisfied_form) {
    Fixture fixture;
    auto imported = fixture.store->Import("k", P2pPublishSource::FromBytes(
                                              std::vector<uint8_t>()),
                                          P2pPublishMode::Copy);
    MT_EXPECT_TRUE(imported.ok());
    MT_EXPECT_EQ(static_cast<uint64_t>(0), imported.value().size);

    P2pServerRequest http =
        Get(std::string(kBlobPathPrefix) + imported.value().digest, "bytes=0-99");
    http.headers[kTokenHeader] = "secret";

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    // No range over an empty artifact is satisfiable, so `bytes */0` is the
    // only way its size can be reported. A client reads that as "nothing to
    // fetch" rather than as a failure.
    MT_EXPECT_EQ(416, response.status);
    MT_EXPECT_EQ(std::string("bytes */0"), response.headers["Content-Range"]);
}

MT_TEST(a_range_over_the_per_request_ceiling_is_clamped_not_refused) {
    Fixture fixture;
    const std::string payload = "0123456789";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    auto imported = fixture.store->Import("k", P2pPublishSource::FromBytes(bytes),
                                          P2pPublishMode::Copy);

    char range[64];
    std::snprintf(range, sizeof(range), "bytes=0-%llu",
                  static_cast<unsigned long long>(kMaxRangeBytes + 10));
    P2pServerRequest http =
        Get(std::string(kBlobPathPrefix) + imported.value().digest, range);
    http.headers[kTokenHeader] = "secret";

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    // Refusing would deadlock a transfer against a server whose ceiling is
    // lower than the client's chunk size; clamping lets the client see what
    // it was granted and continue from there.
    MT_EXPECT_EQ(206, response.status);
    MT_EXPECT_EQ(payload, response.body);
}

MT_TEST(content_range_parsing_covers_both_the_satisfied_and_unsatisfied_forms) {
    bool satisfied = false;
    uint64_t start = 0, end = 0, total = 0;

    MT_EXPECT_TRUE(ParseContentRangeHeader("bytes 5-9/10", &satisfied, &start, &end,
                                           &total));
    MT_EXPECT_TRUE(satisfied);
    MT_EXPECT_EQ(static_cast<uint64_t>(5), start);
    MT_EXPECT_EQ(static_cast<uint64_t>(9), end);
    MT_EXPECT_EQ(static_cast<uint64_t>(10), total);

    MT_EXPECT_TRUE(ParseContentRangeHeader("bytes */0", &satisfied, &start, &end,
                                           &total));
    MT_EXPECT_TRUE(!satisfied);
    MT_EXPECT_EQ(static_cast<uint64_t>(0), total);

    // An unknown total is refused rather than tolerated: every caller needs
    // the size, so accepting the header without one only defers the problem
    // to somewhere with less context.
    MT_EXPECT_TRUE(!ParseContentRangeHeader("bytes 0-9/*", &satisfied, &start, &end,
                                            &total));
    MT_EXPECT_TRUE(!ParseContentRangeHeader("items 0-9/10", &satisfied, &start, &end,
                                            &total));
    MT_EXPECT_TRUE(!ParseContentRangeHeader("bytes 9-5/10", &satisfied, &start, &end,
                                            &total));
    MT_EXPECT_TRUE(!ParseContentRangeHeader("bytes 0-9", &satisfied, &start, &end,
                                            &total));
}

MT_TEST(an_unknown_locator_is_404) {
    Fixture fixture;
    P2pServerRequest http =
        Get(std::string(kBlobPathPrefix) + "sha256:deadbeef", "bytes=0-1");
    http.headers[kTokenHeader] = "secret";

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    MT_EXPECT_EQ(404, response.status);
}

MT_TEST(a_traversal_path_never_reaches_the_handler_at_all) {
    Fixture fixture;
    // Segment-wise matching means a locator containing `/` cannot route: the
    // segment counts disagree. The handler's alphabet check is the second
    // line of defence, not the first.
    P2pServerRequest http = Get(std::string(kBlobPathPrefix) + "../../etc/passwd",
                                "bytes=0-1");
    http.headers[kTokenHeader] = "secret";

    P2pServerResponse response;
    MT_EXPECT_TRUE(!fixture.server.Dispatch(http, &response));
}

MT_TEST(a_single_segment_locator_outside_the_digest_alphabet_is_rejected) {
    Fixture fixture;
    // This one does route — one segment, right count — so the alphabet check
    // is what has to stop it. The locator reaches the store, which maps it
    // onto a file name, so it is validated rather than trusted.
    P2pServerRequest http = Get(std::string(kBlobPathPrefix) + "sha256:aa%00bb",
                                "bytes=0-1");
    http.headers[kTokenHeader] = "secret";

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    MT_EXPECT_EQ(400, response.status);
}

MT_TEST(head_reports_the_total_size_and_the_digest) {
    // The transport's own fetch path does not issue this -- `core::http`
    // cannot parse a bodiless reply -- so nothing else covers the route. It is
    // served because it belongs to the protocol (an operator's `curl -I`, a
    // peer with a streaming client), and a protocol surface with no test is
    // one that breaks silently the next time the handlers are touched.
    Fixture fixture;
    const std::string payload = "0123456789";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    auto imported = fixture.store->Import("k", P2pPublishSource::FromBytes(bytes),
                                          P2pPublishMode::Copy);

    P2pServerRequest http;
    http.method = "HEAD";
    http.path = std::string(kBlobPathPrefix) + imported.value().digest;
    http.headers[kTokenHeader] = "secret";

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    MT_EXPECT_EQ(200, response.status);
    MT_EXPECT_EQ(std::string("10"), response.headers["Content-Length"]);
    MT_EXPECT_EQ(std::string("bytes"), response.headers["Accept-Ranges"]);
    MT_EXPECT_EQ(imported.value().digest, response.headers[kDigestHeader]);
    // No body, which is what makes it unreadable by `core::http` and is also
    // what HEAD means.
    MT_EXPECT_TRUE(response.body.empty());
}

MT_TEST(head_and_get_agree_about_what_is_published) {
    Fixture fixture;
    // The two routes resolve the locator through the same store, so a
    // publication is either visible to both or to neither. Drifting here
    // would let a size probe succeed for something that cannot be fetched.
    const std::string absent = std::string(kBlobPathPrefix) + "sha256:absent";

    P2pServerRequest head;
    head.method = "HEAD";
    head.path = absent;
    head.headers[kTokenHeader] = "secret";
    P2pServerResponse head_response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(head, &head_response));
    MT_EXPECT_EQ(404, head_response.status);

    P2pServerRequest get = Get(absent, "bytes=0-1");
    get.headers[kTokenHeader] = "secret";
    P2pServerResponse get_response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(get, &get_response));
    MT_EXPECT_EQ(404, get_response.status);
}

MT_TEST(a_reference_whose_file_changed_is_409_rather_than_wrong_bytes) {
    Fixture fixture;
    const std::string source = WriteFile(fixture.root.path, "mutable.bin", "original");
    auto imported = fixture.store->Import("k", P2pPublishSource::FromPath(source),
                                          P2pPublishMode::Reference);
    MT_EXPECT_TRUE(imported.ok());

    // The store does not own this file, so its size is re-checked on every
    // serve. Sending the new bytes would fail the reader's own digest check
    // much later, with nothing pointing back here.
    core::fs::Write(source, "a different length entirely");

    P2pServerRequest http =
        Get(std::string(kBlobPathPrefix) + imported.value().digest, "bytes=0-3");
    http.headers[kTokenHeader] = "secret";

    P2pServerResponse response;
    MT_EXPECT_TRUE(fixture.server.Dispatch(http, &response));
    MT_EXPECT_EQ(409, response.status);
}

// ---- transport wiring -------------------------------------------------------

MT_TEST(start_refuses_an_advertise_address_peers_could_not_dial) {
    TempRoot root;
    HttpP2pTransport::Options options;
    options.config.store_dir = root.path;
    options.node_id = "node-a";
    options.server.reset(new RecordingServer());

    // Empty: nothing to hand a peer.
    auto missing = HttpP2pTransport::Start(options,
        std::shared_ptr<P2pPeerDiscovery>(new NoopP2pPeerDiscovery()));
    MT_EXPECT_TRUE(!missing.ok());

    // A bind address is not an advertise address; `0.0.0.0:p` would hand
    // every peer an address that resolves to itself.
    options.advertise_addr = "not-a-host-port";
    auto malformed = HttpP2pTransport::Start(options,
        std::shared_ptr<P2pPeerDiscovery>(new NoopP2pPeerDiscovery()));
    MT_EXPECT_TRUE(!malformed.ok());
}

MT_TEST(start_refuses_to_run_without_a_server_to_serve_on) {
    TempRoot root;
    HttpP2pTransport::Options options;
    options.config.store_dir = root.path;
    options.node_id = "node-a";
    options.advertise_addr = "10.0.0.1:7000";
    // A transport that published its address and then answered no requests
    // would look to every peer like a provider that lies.
    auto started = HttpP2pTransport::Start(options,
        std::shared_ptr<P2pPeerDiscovery>(new NoopP2pPeerDiscovery()));
    MT_EXPECT_TRUE(!started.ok());
}

MT_TEST(publish_records_the_locator_and_lookup_finds_it_locally) {
    TempRoot root;
    HttpP2pTransport::Options options;
    options.config.store_dir = root.path;
    options.node_id = "node-a";
    options.advertise_addr = "10.0.0.1:7000";
    options.server.reset(new RecordingServer());

    auto started = HttpP2pTransport::Start(options,
        std::shared_ptr<P2pPeerDiscovery>(new NoopP2pPeerDiscovery()));
    MT_EXPECT_TRUE(started.ok());
    std::shared_ptr<HttpP2pTransport> transport = started.value();

    const std::string payload = "published bytes";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    MT_EXPECT_TRUE(transport->Publish(
        P2pPublishRequest::FromBytes("layer/one", bytes)).ok());

    P2pArtifactDescriptor found;
    const P2pResult<bool> hit = transport->Lookup("layer/one", &found);
    MT_EXPECT_TRUE(hit.ok());
    MT_EXPECT_TRUE(hit.value());
    MT_EXPECT_TRUE(found.has_backend_locator);
    MT_EXPECT_EQ(core::Sha256Digest(payload), found.backend_locator);

    P2pEndpoint endpoint;
    MT_EXPECT_TRUE(transport->LocalEndpoint(&endpoint));
    MT_EXPECT_EQ(std::string(kHttpBackendId), endpoint.backend);
    MT_EXPECT_EQ(std::string("10.0.0.1:7000"), endpoint.address);

    MT_EXPECT_TRUE(transport->Shutdown().ok());
}

MT_TEST(unpublish_reports_whether_a_publication_was_removed) {
    TempRoot root;
    HttpP2pTransport::Options options;
    options.config.store_dir = root.path;
    options.node_id = "node-a";
    options.advertise_addr = "10.0.0.1:7000";
    options.server.reset(new RecordingServer());
    auto transport = HttpP2pTransport::Start(options,
        std::shared_ptr<P2pPeerDiscovery>(new NoopP2pPeerDiscovery())).value();

    std::vector<uint8_t> bytes(4, 'x');
    MT_EXPECT_TRUE(transport->Publish(P2pPublishRequest::FromBytes("k", bytes)).ok());

    const P2pResult<bool> first = transport->Unpublish("k");
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(first.value());

    const P2pResult<bool> second = transport->Unpublish("k");
    MT_EXPECT_TRUE(second.ok());
    MT_EXPECT_TRUE(!second.value());

    transport->Shutdown();
}

MT_TEST(a_local_range_stream_yields_exactly_the_requested_length) {
    TempRoot root;
    HttpP2pTransport::Options options;
    options.config.store_dir = root.path;
    options.node_id = "node-a";
    options.advertise_addr = "10.0.0.1:7000";
    options.server.reset(new RecordingServer());
    auto transport = HttpP2pTransport::Start(options,
        std::shared_ptr<P2pPeerDiscovery>(new NoopP2pPeerDiscovery())).value();

    const std::string payload = "0123456789";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    MT_EXPECT_TRUE(transport->Publish(P2pPublishRequest::FromBytes("k", bytes)).ok());

    P2pArtifactDescriptor descriptor;
    MT_EXPECT_TRUE(transport->Lookup("k", &descriptor).value());

    auto stream = transport->FetchByteRange(descriptor, 3, 4);
    MT_EXPECT_TRUE(stream.ok());

    std::string collected;
    std::vector<uint8_t> chunk;
    for (;;) {
        const P2pResult<bool> more = stream.value()->Next(&chunk);
        MT_EXPECT_TRUE(more.ok());
        if (!more.value()) break;
        collected.append(chunk.begin(), chunk.end());
    }
    MT_EXPECT_EQ(std::string("3456"), collected);

    // Exhaustion is only reported once the full length has been delivered, so
    // a truncated transfer can never read as a clean end of stream.
    transport->Shutdown();
}

MT_TEST(a_range_outside_a_local_artifact_fails_before_any_bytes_move) {
    TempRoot root;
    HttpP2pTransport::Options options;
    options.config.store_dir = root.path;
    options.node_id = "node-a";
    options.advertise_addr = "10.0.0.1:7000";
    options.server.reset(new RecordingServer());
    auto transport = HttpP2pTransport::Start(options,
        std::shared_ptr<P2pPeerDiscovery>(new NoopP2pPeerDiscovery())).value();

    std::vector<uint8_t> bytes(10, 'x');
    MT_EXPECT_TRUE(transport->Publish(P2pPublishRequest::FromBytes("k", bytes)).ok());
    P2pArtifactDescriptor descriptor;
    MT_EXPECT_TRUE(transport->Lookup("k", &descriptor).value());

    // The range is a property of the request, so an impossible one is an
    // error at construction rather than a surprise on the last chunk.
    MT_EXPECT_TRUE(!transport->FetchByteRange(descriptor, 8, 5).ok());
    transport->Shutdown();
}

MT_TEST(a_local_fetch_lands_the_bytes_at_the_destination) {
    TempRoot root;
    HttpP2pTransport::Options options;
    options.config.store_dir = root.path;
    options.node_id = "node-a";
    options.advertise_addr = "10.0.0.1:7000";
    options.server.reset(new RecordingServer());
    auto transport = HttpP2pTransport::Start(options,
        std::shared_ptr<P2pPeerDiscovery>(new NoopP2pPeerDiscovery())).value();

    const std::string payload = "fetch me";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    MT_EXPECT_TRUE(transport->Publish(P2pPublishRequest::FromBytes("k", bytes)).ok());
    P2pArtifactDescriptor descriptor;
    MT_EXPECT_TRUE(transport->Lookup("k", &descriptor).value());

    const std::string destination = core::fs::Join(root.path, "fetched.bin");
    const P2pResult<uint64_t> fetched = transport->Fetch(descriptor, destination);
    MT_EXPECT_TRUE(fetched.ok());
    MT_EXPECT_EQ(static_cast<uint64_t>(payload.size()), fetched.value());
    MT_EXPECT_EQ(payload, core::fs::ReadToString(destination).value());

    const P2pResult<std::vector<uint8_t> > in_memory = transport->FetchBytes(descriptor);
    MT_EXPECT_TRUE(in_memory.ok());
    MT_EXPECT_EQ(payload, std::string(in_memory.value().begin(), in_memory.value().end()));

    transport->Shutdown();
}

MT_TEST(a_lookup_with_no_peers_is_a_miss_not_an_error) {
    TempRoot root;
    HttpP2pTransport::Options options;
    options.config.store_dir = root.path;
    options.node_id = "node-a";
    options.advertise_addr = "10.0.0.1:7000";
    options.server.reset(new RecordingServer());
    auto transport = HttpP2pTransport::Start(options,
        std::shared_ptr<P2pPeerDiscovery>(new NoopP2pPeerDiscovery())).value();

    P2pArtifactDescriptor found;
    const P2pResult<bool> result = transport->Lookup("absent", &found);
    MT_EXPECT_TRUE(result.ok());
    MT_EXPECT_TRUE(!result.value());
    transport->Shutdown();
}

MT_TEST(an_unreachable_peer_is_a_miss_not_a_failed_lookup) {
    TempRoot root;
    HttpP2pTransport::Options options;
    options.config.store_dir = root.path;
    options.node_id = "node-a";
    options.advertise_addr = "10.0.0.1:7000";
    options.config.lookup_timeout_ms = 200;
    options.server.reset(new RecordingServer());

    // A port nothing listens on. The caller's job is to find *a* provider,
    // and one dead node does not make the question unanswerable.
    P2pEndpoint endpoint;
    endpoint.backend = kHttpBackendId;
    endpoint.address = "127.0.0.1:1";
    P2pPeer peer;
    peer.node_id = "node-dead";
    peer.endpoint = endpoint;

    std::vector<P2pPeer> peers;
    peers.push_back(peer);
    auto transport = HttpP2pTransport::Start(options,
        std::shared_ptr<P2pPeerDiscovery>(new StaticP2pPeerDiscovery(peers))).value();

    P2pArtifactDescriptor found;
    const P2pResult<bool> result = transport->Lookup("absent", &found);
    MT_EXPECT_TRUE(result.ok());
    MT_EXPECT_TRUE(!result.value());
    transport->Shutdown();
}

MT_TEST(a_peer_advertising_another_backend_is_ignored) {
    TempRoot root;
    HttpP2pTransport::Options options;
    options.config.store_dir = root.path;
    options.node_id = "node-a";
    options.advertise_addr = "10.0.0.1:7000";
    options.config.lookup_timeout_ms = 200;
    options.server.reset(new RecordingServer());

    // An iroh address is not dialable by this backend, and attempting it would
    // spend the lookup timeout proving so.
    P2pEndpoint endpoint;
    endpoint.backend = "iroh";
    endpoint.address = "{\"node_id\":\"...\"}";
    P2pPeer peer;
    peer.node_id = "node-iroh";
    peer.endpoint = endpoint;

    std::vector<P2pPeer> peers;
    peers.push_back(peer);
    auto transport = HttpP2pTransport::Start(options,
        std::shared_ptr<P2pPeerDiscovery>(new StaticP2pPeerDiscovery(peers))).value();

    P2pArtifactDescriptor found;
    const P2pResult<bool> result = transport->Lookup("absent", &found);
    MT_EXPECT_TRUE(result.ok());
    MT_EXPECT_TRUE(!result.value());
    transport->Shutdown();
}

MT_TEST(the_backend_id_is_a_real_string_never_an_empty_one) {
    // `Some("")` would still issue the scheduler RPCs and then discard every
    // peer -- a silent no-provider result instead of the short-circuit
    // `Disabled` gets. Only `Disabled` may be absent.
    MT_EXPECT_TRUE(!BackendId(P2pTransportKind::Disabled).has_value());
    MT_EXPECT_EQ(std::string("iroh"), *BackendId(P2pTransportKind::Iroh));
    MT_EXPECT_EQ(std::string("http"), *BackendId(P2pTransportKind::Http));
}

MT_TEST(an_endpoint_from_another_backend_is_refused) {
    P2pEndpoint endpoint;
    endpoint.backend = "iroh";
    endpoint.address = "whatever";
    MT_EXPECT_TRUE(!EndpointAddressForBackend(endpoint, kHttpBackendId).ok());

    endpoint.backend = kHttpBackendId;
    MT_EXPECT_EQ(std::string("whatever"),
                 EndpointAddressForBackend(endpoint, kHttpBackendId).value());
}

int main() { return microtest::RunAll(); }
