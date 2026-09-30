// SPDX-License-Identifier: MIT
// Rust: src/p2p/iroh/catalog.rs (+ the backend check from iroh/endpoint.rs).
#include "agentenv/p2p/catalog.h"

#include <sstream>

#include "agentenv/core/logging.h"

namespace agentenv {
namespace p2p {

const char* const kCatalogAlpn = "/agentenv/artifact-catalog/v1";
const std::size_t kMaxCatalogResponseBytes = 8 * 1024 * 1024;
const std::size_t kMaxCatalogRequestBytes  = 1024 * 1024;
const int64_t     kCatalogConnectionCloseTimeoutMs = 5000;

// ---- descriptor encoding ----------------------------------------------------

core::Json EndpointToJson(const P2pEndpoint& endpoint) {
    core::JsonObject object;
    object["backend"] = core::Json(endpoint.backend);
    object["address"] = core::Json(endpoint.address);
    return core::Json(object);
}

namespace {

core::Expected<std::string, std::string> RequiredString(const core::JsonObject& fields,
                                                        const char* name) {
    core::JsonObject::const_iterator it = fields.find(name);
    if (it == fields.end() || it->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("missing field `") + name + "`");
    }
    return it->second.as_string();
}

}  // namespace

core::Expected<P2pEndpoint, std::string> EndpointFromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("endpoint must be an object"));
    }
    const core::JsonObject& fields = json.as_object();

    core::Expected<std::string, std::string> backend = RequiredString(fields, "backend");
    if (!backend.ok()) return core::make_unexpected(backend.error());
    core::Expected<std::string, std::string> address = RequiredString(fields, "address");
    if (!address.ok()) return core::make_unexpected(address.error());

    P2pEndpoint endpoint;
    endpoint.backend = backend.value();
    endpoint.address = address.value();
    return endpoint;
}

core::Json PeerToJson(const P2pPeer& peer) {
    core::JsonObject object;
    object["node_id"]  = core::Json(peer.node_id);
    object["endpoint"] = EndpointToJson(peer.endpoint);
    return core::Json(object);
}

core::Expected<P2pPeer, std::string> PeerFromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("peer must be an object"));
    }
    const core::JsonObject& fields = json.as_object();

    core::Expected<std::string, std::string> node_id = RequiredString(fields, "node_id");
    if (!node_id.ok()) return core::make_unexpected(node_id.error());

    core::JsonObject::const_iterator endpoint = fields.find("endpoint");
    if (endpoint == fields.end()) {
        return core::make_unexpected(std::string("missing field `endpoint`"));
    }
    core::Expected<P2pEndpoint, std::string> parsed = EndpointFromJson(endpoint->second);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());

    P2pPeer peer;
    peer.node_id  = node_id.value();
    peer.endpoint = parsed.value();
    return peer;
}

core::Json ProviderToJson(const P2pArtifactProvider& provider) {
    // Rust's externally-tagged enum: a unit variant is the bare string.
    if (provider.IsLocal()) return core::Json(std::string("local"));
    core::JsonObject object;
    object["peer"] = PeerToJson(provider.peer);
    return core::Json(object);
}

core::Expected<P2pArtifactProvider, std::string>
ProviderFromJson(const core::Json& json) {
    if (json.kind() == core::Json::Kind::String) {
        if (json.as_string() != "local") {
            return core::make_unexpected(std::string("unknown provider variant `") +
                                         json.as_string() + "`");
        }
        return P2pArtifactProvider::MakeLocal();
    }
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("provider must be a string or object"));
    }
    const core::JsonObject& fields = json.as_object();
    core::JsonObject::const_iterator peer = fields.find("peer");
    if (peer == fields.end()) {
        return core::make_unexpected(std::string("provider object must carry `peer`"));
    }
    core::Expected<P2pPeer, std::string> parsed = PeerFromJson(peer->second);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());
    return P2pArtifactProvider::FromPeer(parsed.value());
}

core::Json DescriptorToJson(const P2pArtifactDescriptor& descriptor) {
    core::JsonObject object;
    object["key"] = core::Json(descriptor.key);

    core::JsonArray providers;
    for (std::size_t i = 0; i < descriptor.providers.size(); ++i) {
        providers.push_back(ProviderToJson(descriptor.providers[i]));
    }
    object["providers"] = core::Json(providers);

    // Rust `Option<String>`: absent rather than null.
    if (descriptor.has_backend_locator) {
        object["backend_locator"] = core::Json(descriptor.backend_locator);
    }

    // `metadata` is an opaque `serde_json::Value` held here as raw JSON text.
    core::Expected<core::Json, core::AnyError> metadata =
        core::Json::Parse(descriptor.metadata_json);
    object["metadata"] = metadata.ok() ? metadata.value() : core::Json();
    return core::Json(object);
}

core::Expected<P2pArtifactDescriptor, std::string>
DescriptorFromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("descriptor must be an object"));
    }
    const core::JsonObject& fields = json.as_object();

    core::Expected<std::string, std::string> key = RequiredString(fields, "key");
    if (!key.ok()) return core::make_unexpected(key.error());

    P2pArtifactDescriptor descriptor;
    descriptor.key = key.value();

    core::JsonObject::const_iterator providers = fields.find("providers");
    if (providers != fields.end()) {
        if (providers->second.kind() != core::Json::Kind::Array) {
            return core::make_unexpected(std::string("field `providers` must be an array"));
        }
        const core::JsonArray& array = providers->second.as_array();
        for (std::size_t i = 0; i < array.size(); ++i) {
            core::Expected<P2pArtifactProvider, std::string> parsed =
                ProviderFromJson(array[i]);
            if (!parsed.ok()) return core::make_unexpected(parsed.error());
            descriptor.providers.push_back(parsed.value());
        }
    }

    core::JsonObject::const_iterator locator = fields.find("backend_locator");
    if (locator != fields.end() && locator->second.kind() == core::Json::Kind::String) {
        descriptor.has_backend_locator = true;
        descriptor.backend_locator     = locator->second.as_string();
    }

    core::JsonObject::const_iterator metadata = fields.find("metadata");
    descriptor.metadata_json =
        metadata != fields.end() ? metadata->second.ToString() : std::string("null");
    return descriptor;
}

// ---- wire format ------------------------------------------------------------

std::string CatalogRequest::Encode() const {
    core::JsonObject object;
    object["key"] = core::Json(key);
    return core::Json(object).ToString();
}

core::Expected<CatalogRequest, std::string>
CatalogRequest::Decode(const std::string& bytes) {
    // Bound the payload before parsing: the limit exists so a peer cannot
    // force an unbounded allocation on this node.
    if (bytes.size() > kMaxCatalogRequestBytes) {
        std::ostringstream os;
        os << "catalog request exceeds " << kMaxCatalogRequestBytes << " bytes";
        return core::make_unexpected(os.str());
    }
    core::Expected<core::Json, core::AnyError> json = core::Json::Parse(bytes);
    if (!json.ok()) {
        return core::make_unexpected(std::string("parse catalog request: ") +
                                     json.error().chain());
    }
    if (json.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("catalog request must be an object"));
    }
    core::Expected<std::string, std::string> key =
        RequiredString(json.value().as_object(), "key");
    if (!key.ok()) return core::make_unexpected(key.error());

    CatalogRequest request;
    request.key = key.value();
    return request;
}

std::string CatalogResponse::Encode() const {
    core::JsonObject object;
    // Rust serialises `Option<P2pArtifactDescriptor>`, so a miss is an explicit
    // null rather than an omitted field.
    object["descriptor"] =
        descriptor.has_value() ? DescriptorToJson(*descriptor) : core::Json();
    return core::Json(object).ToString();
}

core::Expected<CatalogResponse, std::string>
CatalogResponse::Decode(const std::string& bytes) {
    if (bytes.size() > kMaxCatalogResponseBytes) {
        std::ostringstream os;
        os << "catalog response exceeds " << kMaxCatalogResponseBytes << " bytes";
        return core::make_unexpected(os.str());
    }
    core::Expected<core::Json, core::AnyError> json = core::Json::Parse(bytes);
    if (!json.ok()) {
        return core::make_unexpected(std::string("parse catalog response: ") +
                                     json.error().chain());
    }
    if (json.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("catalog response must be an object"));
    }

    CatalogResponse response;
    const core::JsonObject& fields = json.value().as_object();
    core::JsonObject::const_iterator found = fields.find("descriptor");
    if (found != fields.end() && found->second.kind() != core::Json::Kind::Null) {
        core::Expected<P2pArtifactDescriptor, std::string> parsed =
            DescriptorFromJson(found->second);
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        response.descriptor = core::Optional<P2pArtifactDescriptor>(parsed.value());
    }
    return response;
}

// ---- catalog ----------------------------------------------------------------

core::Expected<std::shared_ptr<PublishedArtifactCatalog>, P2pError>
PublishedArtifactCatalog::Load(const std::string& db_path, const std::string& node_id,
                               const P2pEndpoint& local_endpoint) {
    core::Expected<std::shared_ptr<local_store::KvStore>, std::string> store =
        local_store::KvStore::Open(db_path, local_store::Durability::Wal);
    if (!store.ok()) {
        return core::make_unexpected(P2pError::InternalMessage(
            std::string("open P2P catalog ") + db_path, store.error()));
    }

    P2pPeer provider;
    provider.node_id  = node_id;
    provider.endpoint = local_endpoint;

    std::shared_ptr<PublishedArtifactCatalog> catalog(
        new PublishedArtifactCatalog(store.value(), provider));

    core::Expected<std::vector<local_store::Entry>, std::string> entries =
        store.value()->Entries();
    if (!entries.ok()) {
        return core::make_unexpected(P2pError::InternalMessage(
            std::string("scan P2P catalog ") + db_path, entries.error()));
    }
    for (std::size_t i = 0; i < entries.value().size(); ++i) {
        const local_store::Entry& entry = entries.value()[i];
        core::Expected<core::Json, core::AnyError> json = core::Json::Parse(entry.second);
        if (!json.ok()) {
            return core::make_unexpected(P2pError::InternalMessage(
                std::string("parse P2P catalog entry ") + entry.first,
                json.error().chain()));
        }
        core::Expected<P2pArtifactDescriptor, std::string> descriptor =
            DescriptorFromJson(json.value());
        if (!descriptor.ok()) {
            return core::make_unexpected(P2pError::InternalMessage(
                std::string("parse P2P catalog entry ") + entry.first, descriptor.error()));
        }
        catalog->entries_[descriptor.value().key] = descriptor.value();
    }

    std::ostringstream os;
    os << "loaded persisted P2P catalog " << db_path << " with "
       << catalog->entries_.size() << " entries";
    AGENTENV_DEBUG(os.str());
    return catalog;
}

core::Optional<P2pArtifactDescriptor>
PublishedArtifactCatalog::DescriptorFor(const P2pArtifactKey& key) const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::map<P2pArtifactKey, P2pArtifactDescriptor>::const_iterator it = entries_.find(key);
    if (it == entries_.end()) return core::Optional<P2pArtifactDescriptor>();
    return core::Optional<P2pArtifactDescriptor>(it->second);
}

core::Optional<P2pArtifactDescriptor>
PublishedArtifactCatalog::DescriptorForResponse(const P2pArtifactKey& key) const {
    core::Optional<P2pArtifactDescriptor> found = DescriptorFor(key);
    if (!found.has_value()) return found;

    // Rust replaces the provider list wholesale: the peer asked what *we* can
    // serve, so relaying third-party providers we merely learned about would
    // be answering a different question.
    P2pArtifactDescriptor served = *found;
    served.providers.clear();
    served.providers.push_back(P2pArtifactProvider::FromPeer(local_provider_));
    return core::Optional<P2pArtifactDescriptor>(served);
}

core::Expected<core::Unit, P2pError>
PublishedArtifactCatalog::Upsert(const P2pArtifactDescriptor& descriptor) {
    // Persist before indexing: if the write fails, the index must not start
    // claiming an artifact the store never recorded.
    core::Expected<core::Unit, std::string> written =
        store_->Put(descriptor.key, DescriptorToJson(descriptor).ToString());
    if (!written.ok()) {
        return core::make_unexpected(P2pError::InternalMessage(
            std::string("persist P2P catalog entry ") + descriptor.key, written.error()));
    }
    std::lock_guard<std::mutex> guard(mutex_);
    entries_[descriptor.key] = descriptor;
    return core::Unit{};
}

core::Expected<core::Optional<P2pArtifactDescriptor>, P2pError>
PublishedArtifactCatalog::Remove(const P2pArtifactKey& key) {
    core::Expected<core::Unit, std::string> removed = store_->Delete(key);
    if (!removed.ok()) {
        return core::make_unexpected(P2pError::InternalMessage(
            std::string("delete P2P catalog entry ") + key, removed.error()));
    }
    std::lock_guard<std::mutex> guard(mutex_);
    std::map<P2pArtifactKey, P2pArtifactDescriptor>::iterator it = entries_.find(key);
    if (it == entries_.end()) return core::Optional<P2pArtifactDescriptor>();
    const P2pArtifactDescriptor dropped = it->second;
    entries_.erase(it);
    return core::Optional<P2pArtifactDescriptor>(dropped);
}

std::size_t PublishedArtifactCatalog::size() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return entries_.size();
}

// ---- endpoint backend check -------------------------------------------------

core::Expected<std::string, P2pError>
EndpointAddressForBackend(const P2pEndpoint& endpoint, const std::string& backend) {
    if (endpoint.backend != backend) {
        return core::make_unexpected(P2pError::Invalid(
            std::string("unsupported P2P endpoint backend ") + endpoint.backend));
    }
    return endpoint.address;
}

}  // namespace p2p
}  // namespace agentenv
