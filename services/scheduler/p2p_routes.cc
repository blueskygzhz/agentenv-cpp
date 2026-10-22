// SPDX-License-Identifier: MIT
// Go upstream: the P2P methods of services/scheduler/internal/service.go.
#include "services/scheduler/p2p_routes.h"

#include <vector>

#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"

namespace agentenv {
namespace services {
namespace scheduler {

namespace {

/// Bound on one request body. These are four short strings; anything larger is
/// a bug or an attempt, and `api::HttpRequest::body` is buffered whole.
const std::size_t kMaxRequestBytes = 64 * 1024;

std::string Trim(const std::string& s) {
    const std::size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const std::size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

/// Constant-time comparison; a check that exits on the first differing byte
/// leaks the token's prefix to anyone who can time it.
bool TokenAccepted(const api::HttpRequest& request, const std::string& expected) {
    if (expected.empty()) return true;
    // `api::Server` lowercases header names as it parses, but a direct caller
    // (a test, another adapter) may not, so both spellings are tried.
    std::unordered_map<std::string, std::string>::const_iterator it =
        request.headers.find("x-agentenv-p2p-token");
    if (it == request.headers.end()) {
        it = request.headers.find("X-Agentenv-P2p-Token");
    }
    if (it == request.headers.end()) return false;
    if (it->second.size() != expected.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        diff = static_cast<unsigned char>(
            diff | (static_cast<unsigned char>(expected[i]) ^
                    static_cast<unsigned char>(it->second[i])));
    }
    return diff == 0;
}

void Fail(api::HttpResponse* response, int status, const std::string& message) {
    response->status = status;
    response->headers["Content-Type"] = "application/json";
    core::JsonObject o;
    o["error"] = core::Json(message);
    response->body = core::Json(o).ToString();
}

void Ok(api::HttpResponse* response, const std::string& body) {
    response->status = 200;
    response->headers["Content-Type"] = "application/json";
    response->body = body;
}

/// Reads a trimmed string field. Absent and blank are the same thing here,
/// because every caller of this treats a blank id as missing.
std::string StringField(const core::JsonObject& o, const std::string& name) {
    core::JsonObject::const_iterator it = o.find(name);
    if (it == o.end() || it->second.kind() != core::Json::Kind::String) {
        return std::string();
    }
    return Trim(it->second.as_string());
}

/// Parses a request body into an object, or writes the failure and returns
/// false.
bool ParseBody(const api::HttpRequest& request, api::HttpResponse* response,
               core::Json* out) {
    if (request.body.size() > kMaxRequestBytes) {
        Fail(response, 400, "request too large");
        return false;
    }
    const core::Expected<core::Json, core::AnyError> parsed =
        core::Json::Parse(request.body);
    if (!parsed.ok() || parsed.value().kind() != core::Json::Kind::Object) {
        Fail(response, 400, "request body must be a JSON object");
        return false;
    }
    if (out) *out = parsed.value();
    return true;
}

}  // namespace

std::string EncodeP2pPeersResponse(const std::vector<Node>& peers) {
    core::JsonArray array;
    array.reserve(peers.size());
    for (std::size_t i = 0; i < peers.size(); ++i) {
        core::JsonObject endpoint;
        endpoint["backend"] = core::Json(peers[i].p2p_backend);
        endpoint["address"] = core::Json(peers[i].p2p_address);

        core::JsonObject peer;
        peer["nodeId"] = core::Json(peers[i].id);
        peer["endpoint"] = core::Json(endpoint);
        array.push_back(core::Json(peer));
    }
    core::JsonObject root;
    root["peers"] = core::Json(array);
    return core::Json(root).ToString();
}

void RegisterSchedulerP2pRoutes(api::Server* server, NodeRegistry* nodes,
                                InMemoryArtifactStore* artifacts,
                                const std::string& shared_token) {
    // ---- ListP2PPeers -------------------------------------------------------

    server->Route("POST", "/scheduler/v1/p2p/peers",
                  [nodes, shared_token](const api::HttpRequest& request,
                                        api::HttpResponse* response) {
        if (!TokenAccepted(request, shared_token)) {
            Fail(response, 401, "scheduler token rejected");
            return;
        }
        core::Json body;
        if (!ParseBody(request, response, &body)) return;
        const core::JsonObject& o = body.as_object();

        // Go applies no required-field check to this one: an empty cluster id
        // means "every cluster", which is a legitimate question for an
        // operator even though a node always sends its own.
        const std::vector<Node> peers =
            nodes->ListP2pPeers(StringField(o, "clusterId"), StringField(o, "backend"),
                                StringField(o, "excludeNodeId"));
        Ok(response, EncodeP2pPeersResponse(peers));
    });

    // ---- LookupP2PArtifact --------------------------------------------------

    server->Route("POST", "/scheduler/v1/p2p/lookup",
                  [nodes, artifacts, shared_token](const api::HttpRequest& request,
                                                   api::HttpResponse* response) {
        if (!TokenAccepted(request, shared_token)) {
            Fail(response, 401, "scheduler token rejected");
            return;
        }
        core::Json body;
        if (!ParseBody(request, response, &body)) return;
        const core::JsonObject& o = body.as_object();

        const std::string cluster_id = StringField(o, "clusterId");
        const std::string backend = StringField(o, "backend");
        const std::string key = StringField(o, "key");
        // Go: `InvalidArgument` when any of the three is blank. Answering with
        // an empty peer list instead would be indistinguishable from "nobody
        // has it", and the caller would fall back to the origin rather than
        // fix the request.
        if (cluster_id.empty() || backend.empty() || key.empty()) {
            Fail(response, 400, "clusterId, backend and key are required");
            return;
        }

        // Two steps, in this order: the index says which nodes claim the key,
        // the registry says which of those are currently usable peers. A node
        // that claimed an artifact and then went away is filtered here rather
        // than being handed out and failing to dial.
        const std::vector<std::string> node_ids =
            artifacts->Lookup(cluster_id, backend, key);
        const std::vector<Node> peers = nodes->FilterP2pPeers(
            cluster_id, backend, node_ids, StringField(o, "excludeNodeId"));
        Ok(response, EncodeP2pPeersResponse(peers));
    });

    // ---- RecordP2PArtifact / ForgetP2PArtifact ------------------------------

    server->Route("POST", "/scheduler/v1/p2p/record",
                  [nodes, artifacts, shared_token](const api::HttpRequest& request,
                                                   api::HttpResponse* response) {
        if (!TokenAccepted(request, shared_token)) {
            Fail(response, 401, "scheduler token rejected");
            return;
        }
        core::Json body;
        if (!ParseBody(request, response, &body)) return;
        const core::JsonObject& o = body.as_object();

        const std::string cluster_id = StringField(o, "clusterId");
        const std::string backend = StringField(o, "backend");
        const std::string key = StringField(o, "key");
        const std::string node_id = StringField(o, "nodeId");
        if (cluster_id.empty() || backend.empty() || key.empty() || node_id.empty()) {
            Fail(response, 400, "clusterId, backend, key and nodeId are required");
            return;
        }
        // Go gates on `Resolve`. An unknown node's claim is refused rather
        // than indexed: advertising it would send peers to an address the
        // scheduler has never heard of, and each would pay a failed dial to
        // discover that.
        if (!nodes->Resolve(node_id, NULL)) {
            Fail(response, 400, "node is not in the scheduler node list");
            return;
        }

        artifacts->Record(cluster_id, backend, key, node_id);
        AGENTENV_DEBUG("scheduler recorded P2P artifact"
                       << " cluster_id=" << cluster_id << " backend=" << backend
                       << " key=" << key << " node_id=" << node_id);
        Ok(response, "{}");
    });

    server->Route("POST", "/scheduler/v1/p2p/forget",
                  [artifacts, shared_token](const api::HttpRequest& request,
                                            api::HttpResponse* response) {
        if (!TokenAccepted(request, shared_token)) {
            Fail(response, 401, "scheduler token rejected");
            return;
        }
        core::Json body;
        if (!ParseBody(request, response, &body)) return;
        const core::JsonObject& o = body.as_object();

        const std::string cluster_id = StringField(o, "clusterId");
        const std::string backend = StringField(o, "backend");
        const std::string key = StringField(o, "key");
        const std::string node_id = StringField(o, "nodeId");
        if (cluster_id.empty() || backend.empty() || key.empty() || node_id.empty()) {
            Fail(response, 400, "clusterId, backend, key and nodeId are required");
            return;
        }
        // No `Resolve` gate here, matching Go. A node that has already left
        // the registry must still be able to retract its claims — refusing
        // would strand them until the LRU got round to it.
        artifacts->Forget(cluster_id, backend, key, node_id);
        AGENTENV_DEBUG("scheduler forgot P2P artifact"
                       << " cluster_id=" << cluster_id << " backend=" << backend
                       << " key=" << key << " node_id=" << node_id);
        Ok(response, "{}");
    });
}

void RegisterSchedulerNodeRoutes(api::Server* server, NodeRegistry* nodes,
                                 InMemoryArtifactStore* artifacts,
                                 const std::string& shared_token) {
    server->Route("POST", "/scheduler/v1/nodes",
                  [nodes, shared_token](const api::HttpRequest& request,
                                        api::HttpResponse* response) {
        if (!TokenAccepted(request, shared_token)) {
            Fail(response, 401, "scheduler token rejected");
            return;
        }
        core::Json body;
        if (!ParseBody(request, response, &body)) return;
        const core::JsonObject& o = body.as_object();

        Node node;
        node.id = StringField(o, "nodeId");
        node.cluster_id = StringField(o, "clusterId");
        node.endpoint = StringField(o, "endpoint");
        node.p2p_backend = StringField(o, "p2pBackend");
        node.p2p_address = StringField(o, "p2pAddress");

        if (node.id.empty()) {
            Fail(response, 400, "nodeId is required");
            return;
        }
        // The P2P endpoint is optional: a node that registers without one is
        // schedulable for placement but never appears in a peer list, which
        // is exactly the state a node running with p2p disabled should be in.
        if (node.p2p_backend.empty() != node.p2p_address.empty()) {
            // Half of one is neither. A backend with no address cannot be
            // dialled, and an address with no backend cannot be interpreted;
            // either would be filtered out later, silently.
            Fail(response, 400,
                 "p2pBackend and p2pAddress must be given together or not at all");
            return;
        }

        nodes->Upsert(node);
        AGENTENV_INFO("scheduler registered node"
                      << " node_id=" << node.id << " cluster_id=" << node.cluster_id
                      << " p2p_backend=" << node.p2p_backend
                      << " p2p_address=" << node.p2p_address);
        Ok(response, "{}");
    });

    server->Route("DELETE", "/scheduler/v1/nodes/{id}",
                  [nodes, artifacts, shared_token](const api::HttpRequest& request,
                                                   api::HttpResponse* response) {
        if (!TokenAccepted(request, shared_token)) {
            Fail(response, 401, "scheduler token rejected");
            return;
        }
        const std::string prefix = "/scheduler/v1/nodes/";
        if (request.path.size() <= prefix.size()) {
            Fail(response, 400, "node id is required");
            return;
        }
        const std::string node_id = Trim(request.path.substr(prefix.size()));
        if (node_id.empty()) {
            Fail(response, 400, "node id is required");
            return;
        }

        // Both, and in this order. Dropping the node while leaving its
        // artifact claims would keep directing peers at something that is
        // gone; doing the claims first would briefly let a lookup return a
        // node that is still registered but no longer claims anything, which
        // is merely a miss.
        nodes->Remove(node_id);
        artifacts->ForgetNode(node_id);
        AGENTENV_INFO("scheduler removed node node_id=" << node_id);
        Ok(response, "{}");
    });

    server->Route("GET", "/scheduler/v1/nodes",
                  [nodes, shared_token](const api::HttpRequest& request,
                                        api::HttpResponse* response) {
        if (!TokenAccepted(request, shared_token)) {
            Fail(response, 401, "scheduler token rejected");
            return;
        }
        const std::vector<RichNode> rich = nodes->RichNodes();
        core::JsonArray array;
        array.reserve(rich.size());
        for (std::size_t i = 0; i < rich.size(); ++i) {
            core::JsonObject entry;
            entry["nodeId"] = core::Json(rich[i].node.id);
            entry["clusterId"] = core::Json(rich[i].node.cluster_id);
            entry["endpoint"] = core::Json(rich[i].node.endpoint);
            entry["p2pBackend"] = core::Json(rich[i].node.p2p_backend);
            entry["p2pAddress"] = core::Json(rich[i].node.p2p_address);
            array.push_back(core::Json(entry));
        }
        core::JsonObject root;
        root["nodes"] = core::Json(array);
        Ok(response, core::Json(root).ToString());
    });
}

}  // namespace scheduler
}  // namespace services
}  // namespace agentenv
