// SPDX-License-Identifier: MIT
// Rust: src/p2p/iroh/catalog.rs `mod tests` + iroh/endpoint.rs `mod tests`.
#include "agentenv/p2p/catalog.h"

#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "microtest.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::p2p;  // NOLINT

namespace {

struct TempRoot {
    std::string path;
    TempRoot() {
        auto d = core::fs::CreateTempDir("agentenv-p2p-catalog-");
        path = d.ok() ? d.value() : std::string("/tmp/agentenv-p2p-catalog-fallback");
        core::fs::CreateDirAll(path);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
};

P2pEndpoint Endpoint(const std::string& backend, const std::string& address) {
    P2pEndpoint endpoint;
    endpoint.backend = backend;
    endpoint.address = address;
    return endpoint;
}

P2pPeer Peer(const std::string& node_id, const P2pEndpoint& endpoint) {
    P2pPeer peer;
    peer.node_id  = node_id;
    peer.endpoint = endpoint;
    return peer;
}

P2pArtifactDescriptor Descriptor(const std::string& key) {
    P2pArtifactDescriptor descriptor;
    descriptor.key = key;
    return descriptor;
}

std::shared_ptr<PublishedArtifactCatalog> OpenCatalog(const std::string& db_path) {
    auto c = PublishedArtifactCatalog::Load(db_path, "node-1",
                                            Endpoint("iroh", "{\"id\":\"abc\"}"));
    MT_EXPECT_TRUE(c.ok());
    return c.value();
}

}  // namespace

MT_TEST(p2p_catalog_endpoint_json_round_trip) {
    const P2pEndpoint endpoint = Endpoint("iroh", "{\"id\":\"abc\"}");
    auto decoded = EndpointFromJson(EndpointToJson(endpoint));
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_EQ(decoded.value().backend, std::string("iroh"));
    MT_EXPECT_EQ(decoded.value().address, endpoint.address);
}

MT_TEST(p2p_catalog_provider_json_is_externally_tagged) {
    // The local variant is Rust's unit variant: a bare string, not an object.
    const core::Json local = ProviderToJson(P2pArtifactProvider::MakeLocal());
    MT_EXPECT_TRUE(local.kind() == core::Json::Kind::String);
    MT_EXPECT_EQ(local.as_string(), std::string("local"));

    auto decoded_local = ProviderFromJson(local);
    MT_EXPECT_TRUE(decoded_local.ok());
    MT_EXPECT_TRUE(decoded_local.value().IsLocal());

    const P2pPeer peer = Peer("node-2", Endpoint("iroh", "{}"));
    const core::Json peer_json = ProviderToJson(P2pArtifactProvider::FromPeer(peer));
    MT_EXPECT_TRUE(peer_json.kind() == core::Json::Kind::Object);

    auto decoded_peer = ProviderFromJson(peer_json);
    MT_EXPECT_TRUE(decoded_peer.ok());
    MT_EXPECT_TRUE(!decoded_peer.value().IsLocal());
    MT_EXPECT_EQ(decoded_peer.value().peer.node_id, std::string("node-2"));
}

MT_TEST(p2p_catalog_provider_rejects_unknown_variant) {
    MT_EXPECT_TRUE(!ProviderFromJson(core::Json(std::string("elsewhere"))).ok());
    // An object without `peer` is not a valid tagged variant either.
    MT_EXPECT_TRUE(!ProviderFromJson(core::Json(core::JsonObject())).ok());
}

MT_TEST(p2p_catalog_descriptor_json_round_trip) {
    P2pArtifactDescriptor descriptor = Descriptor("overlaybd-layer/v1/sha256:a");
    descriptor.providers.push_back(P2pArtifactProvider::MakeLocal());
    descriptor.providers.push_back(
        P2pArtifactProvider::FromPeer(Peer("node-2", Endpoint("iroh", "{}"))));
    descriptor.has_backend_locator = true;
    descriptor.backend_locator     = "blake3:deadbeef";
    descriptor.metadata_json       = "{\"size\":4096}";

    auto decoded = DescriptorFromJson(DescriptorToJson(descriptor));
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_EQ(decoded.value().key, descriptor.key);
    MT_EXPECT_EQ(decoded.value().providers.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(decoded.value().has_backend_locator);
    MT_EXPECT_EQ(decoded.value().backend_locator, std::string("blake3:deadbeef"));
}

MT_TEST(p2p_catalog_descriptor_omits_absent_backend_locator) {
    const P2pArtifactDescriptor descriptor = Descriptor("k");
    const core::Json json = DescriptorToJson(descriptor);
    MT_EXPECT_TRUE(json.kind() == core::Json::Kind::Object);
    // Rust's `Option<String>` is skipped when None rather than written null.
    MT_EXPECT_TRUE(json.as_object().find("backend_locator") == json.as_object().end());

    auto decoded = DescriptorFromJson(json);
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_TRUE(!decoded.value().has_backend_locator);
    MT_EXPECT_EQ(decoded.value().metadata_json, std::string("null"));
}

MT_TEST(p2p_catalog_request_response_wire_format) {
    CatalogRequest request;
    request.key = "snapshot/v1/artifacts/x/y";
    auto decoded_request = CatalogRequest::Decode(request.Encode());
    MT_EXPECT_TRUE(decoded_request.ok());
    MT_EXPECT_EQ(decoded_request.value().key, request.key);

    CatalogResponse hit;
    hit.descriptor = core::Optional<P2pArtifactDescriptor>(Descriptor("k"));
    auto decoded_hit = CatalogResponse::Decode(hit.Encode());
    MT_EXPECT_TRUE(decoded_hit.ok());
    MT_EXPECT_TRUE(decoded_hit.value().descriptor.has_value());
    MT_EXPECT_EQ(decoded_hit.value().descriptor->key, std::string("k"));

    // A miss is an explicit null descriptor, not an error.
    CatalogResponse miss;
    auto decoded_miss = CatalogResponse::Decode(miss.Encode());
    MT_EXPECT_TRUE(decoded_miss.ok());
    MT_EXPECT_TRUE(!decoded_miss.value().descriptor.has_value());
}

MT_TEST(p2p_catalog_request_rejects_oversized_payload) {
    // The size guard must reject before parsing, so an oversized payload
    // cannot force an unbounded allocation.
    const std::string oversized(kMaxCatalogRequestBytes + 1, 'x');
    MT_EXPECT_TRUE(!CatalogRequest::Decode(oversized).ok());

    const std::string oversized_response(kMaxCatalogResponseBytes + 1, 'x');
    MT_EXPECT_TRUE(!CatalogResponse::Decode(oversized_response).ok());
}

MT_TEST(p2p_catalog_request_rejects_malformed_payload) {
    MT_EXPECT_TRUE(!CatalogRequest::Decode("{not json").ok());
    MT_EXPECT_TRUE(!CatalogRequest::Decode("[]").ok());
    MT_EXPECT_TRUE(!CatalogRequest::Decode("{}").ok());  // missing `key`
}

MT_TEST(p2p_catalog_upsert_lookup_remove) {
    TempRoot root;
    auto catalog = OpenCatalog(root.path + "/catalog.db");

    MT_EXPECT_TRUE(!catalog->DescriptorFor("absent").has_value());

    P2pArtifactDescriptor descriptor = Descriptor("key-1");
    descriptor.has_backend_locator = true;
    descriptor.backend_locator     = "blake3:aa";
    MT_EXPECT_TRUE(catalog->Upsert(descriptor).ok());
    MT_EXPECT_EQ(catalog->size(), static_cast<std::size_t>(1));

    auto found = catalog->DescriptorFor("key-1");
    MT_EXPECT_TRUE(found.has_value());
    MT_EXPECT_EQ(found->backend_locator, std::string("blake3:aa"));

    auto removed = catalog->Remove("key-1");
    MT_EXPECT_TRUE(removed.ok());
    MT_EXPECT_TRUE(removed.value().has_value());
    MT_EXPECT_EQ(removed.value()->key, std::string("key-1"));
    MT_EXPECT_TRUE(!catalog->DescriptorFor("key-1").has_value());
    MT_EXPECT_EQ(catalog->size(), static_cast<std::size_t>(0));

    // Removing a missing key is not an error; it just reports nothing dropped.
    auto again = catalog->Remove("key-1");
    MT_EXPECT_TRUE(again.ok());
    MT_EXPECT_TRUE(!again.value().has_value());
}

MT_TEST(p2p_catalog_survives_reopen) {
    TempRoot root;
    const std::string db = root.path + "/catalog.db";

    {
        auto catalog = OpenCatalog(db);
        P2pArtifactDescriptor descriptor = Descriptor("persisted");
        descriptor.metadata_json = "{\"size\":7}";
        MT_EXPECT_TRUE(catalog->Upsert(descriptor).ok());
    }

    // A publish is a promise this node can serve those bytes; it has to
    // outlive the process, or peers would hold descriptors pointing at a node
    // that has silently forgotten.
    auto reopened = OpenCatalog(db);
    MT_EXPECT_EQ(reopened->size(), static_cast<std::size_t>(1));
    auto found = reopened->DescriptorFor("persisted");
    MT_EXPECT_TRUE(found.has_value());
    MT_EXPECT_EQ(found->metadata_json, std::string("{\"size\":7}"));
}

MT_TEST(p2p_catalog_load_fails_closed_on_corrupt_entry) {
    TempRoot root;
    const std::string db = root.path + "/catalog.db";

    {
        auto store = local_store::KvStore::Open(db, local_store::Durability::Wal);
        MT_EXPECT_TRUE(store.ok());
        MT_EXPECT_TRUE(store.value()->Put("broken", "{not json").ok());
    }

    // Serving a partial catalog would silently drop promises already made, so
    // the load refuses instead of skipping the bad entry.
    auto catalog = PublishedArtifactCatalog::Load(db, "node-1", Endpoint("iroh", "{}"));
    MT_EXPECT_TRUE(!catalog.ok());
}

MT_TEST(p2p_catalog_response_advertises_only_local_provider) {
    TempRoot root;
    auto catalog = OpenCatalog(root.path + "/catalog.db");

    // Store a descriptor that names a third-party provider.
    P2pArtifactDescriptor descriptor = Descriptor("key-1");
    descriptor.providers.push_back(
        P2pArtifactProvider::FromPeer(Peer("node-other", Endpoint("iroh", "{}"))));
    MT_EXPECT_TRUE(catalog->Upsert(descriptor).ok());

    // The served copy must advertise this node alone: the peer asked what *we*
    // can serve, not what we have heard about.
    auto served = catalog->DescriptorForResponse("key-1");
    MT_EXPECT_TRUE(served.has_value());
    MT_EXPECT_EQ(served->providers.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(!served->providers[0].IsLocal());
    MT_EXPECT_EQ(served->providers[0].peer.node_id, std::string("node-1"));

    // The stored copy is untouched.
    auto stored = catalog->DescriptorFor("key-1");
    MT_EXPECT_TRUE(stored.has_value());
    MT_EXPECT_EQ(stored->providers[0].peer.node_id, std::string("node-other"));

    MT_EXPECT_TRUE(!catalog->DescriptorForResponse("absent").has_value());
}

MT_TEST(p2p_catalog_upsert_replaces_existing_entry) {
    TempRoot root;
    auto catalog = OpenCatalog(root.path + "/catalog.db");

    P2pArtifactDescriptor first = Descriptor("key-1");
    first.backend_locator     = "blake3:aa";
    first.has_backend_locator = true;
    MT_EXPECT_TRUE(catalog->Upsert(first).ok());

    P2pArtifactDescriptor second = Descriptor("key-1");
    second.backend_locator     = "blake3:bb";
    second.has_backend_locator = true;
    MT_EXPECT_TRUE(catalog->Upsert(second).ok());

    MT_EXPECT_EQ(catalog->size(), static_cast<std::size_t>(1));
    auto found = catalog->DescriptorFor("key-1");
    MT_EXPECT_TRUE(found.has_value());
    MT_EXPECT_EQ(found->backend_locator, std::string("blake3:bb"));
}

// Rust `iroh_endpoint_rejects_non_iroh_backend`.
MT_TEST(p2p_catalog_endpoint_backend_check) {
    auto ok = EndpointAddressForBackend(Endpoint("iroh", "{\"id\":\"a\"}"), "iroh");
    MT_EXPECT_TRUE(ok.ok());
    MT_EXPECT_EQ(ok.value(), std::string("{\"id\":\"a\"}"));

    auto wrong = EndpointAddressForBackend(Endpoint("other", "{}"), "iroh");
    MT_EXPECT_TRUE(!wrong.ok());
    MT_EXPECT_TRUE(wrong.error().kind == P2pError::InvalidDescriptor);
}

int main() { return microtest::RunAll(); }
