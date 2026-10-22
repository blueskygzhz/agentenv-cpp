// SPDX-License-Identifier: MIT
// Rust: src/p2p/discovery/scheduler.rs — the four scheduler RPCs, over HTTP.
#include "agentenv/p2p/scheduler_http.h"

#include "agentenv/core/http.h"
#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/url.h"

namespace agentenv {
namespace p2p {

const char* const kSchedulerPeersPath = "/scheduler/v1/p2p/peers";
const char* const kSchedulerLookupPath = "/scheduler/v1/p2p/lookup";
const char* const kSchedulerRecordPath = "/scheduler/v1/p2p/record";
const char* const kSchedulerForgetPath = "/scheduler/v1/p2p/forget";

const std::size_t kMaxSchedulerResponseBytes = 4u * 1024 * 1024;

namespace {

/// Rust `REFRESH_RPC_TIMEOUT` / `ARTIFACT_RPC_TIMEOUT` are the defaults the
/// caller passes down; these only guard against a zero reaching the client,
/// where it would mean "no bound" and let one wedged scheduler hold a refresh
/// thread forever.
const uint64_t kMinTimeoutMs = 1000;

uint64_t FloorTimeout(uint64_t timeout_ms) {
    return timeout_ms < kMinTimeoutMs ? kMinTimeoutMs : timeout_ms;
}

/// The scheme check `core::Url::Parse` does not do.
///
/// A `grpc://` endpoint parses fine as a URL and would then be dialled as if
/// it were HTTP, failing at the first request with a transport error that says
/// nothing about the real mistake. Catching it here names it.
bool SchemeIsHttp(const core::Url& url) {
    return url.scheme == "http" || url.scheme == "https";
}

uint16_t EffectivePort(const core::Url& url) {
    if (url.port.has_value()) return *url.port;
    return url.scheme == "https" ? 443 : 80;
}

/// One POST to the scheduler. `*out_body` is only written on a 2xx.
core::Expected<core::Unit, std::string> Post(const std::string& host, uint16_t port,
                                             const std::string& path,
                                             const std::string& token,
                                             const std::string& body,
                                             uint64_t timeout_ms,
                                             std::string* out_body) {
    core::http::Request request;
    request.method = "POST";
    request.path = path;
    request.headers["Content-Type"] = "application/json";
    request.headers["Accept"] = "application/json";
    if (!token.empty()) request.headers["X-Agentenv-P2p-Token"] = token;
    request.body = body;

    core::http::Timeouts timeouts;
    timeouts.read_ms = static_cast<int64_t>(timeout_ms);

    const core::http::HttpClient client(core::http::Target::Tcp(host, port), timeouts);
    const core::Expected<core::http::Response, std::string> response =
        client.Request(request);
    if (!response.ok()) return core::make_unexpected(response.error());

    if (!response.value().IsSuccess()) {
        char status[48];
        std::snprintf(status, sizeof(status), "scheduler answered %d",
                      response.value().status);
        return core::make_unexpected(std::string(status));
    }
    if (response.value().body.size() > kMaxSchedulerResponseBytes) {
        return core::make_unexpected(std::string("scheduler response too large"));
    }
    if (out_body) *out_body = response.value().body;
    return core::Unit{};
}

}  // namespace

// ---- encoding ---------------------------------------------------------------

std::string EncodeSchedulerPeersRequest(const std::string& cluster_id,
                                        const std::string& backend,
                                        const std::string& exclude_node_id,
                                        const std::string& key) {
    core::JsonObject o;
    o["clusterId"] = core::Json(cluster_id);
    o["backend"] = core::Json(backend);
    o["excludeNodeId"] = core::Json(exclude_node_id);
    // Present only for the lookup form. `ListP2pPeersRequest` has no `key`
    // field at all, and sending one anyway would be a message the scheduler
    // never agreed to parse.
    if (!key.empty()) o["key"] = core::Json(key);
    return core::Json(o).ToString();
}

std::string EncodeSchedulerArtifactRequest(const std::string& cluster_id,
                                           const std::string& backend,
                                           const std::string& key,
                                           const std::string& node_id) {
    core::JsonObject o;
    o["clusterId"] = core::Json(cluster_id);
    o["backend"] = core::Json(backend);
    o["key"] = core::Json(key);
    // `nodeId`, not `excludeNodeId`: record and forget name the node that
    // holds the artifact, where list and lookup name the one to leave out.
    // The two are the same string here and opposite in meaning, which is
    // exactly why the proto spells them differently.
    o["nodeId"] = core::Json(node_id);
    return core::Json(o).ToString();
}

// ---- decoding ---------------------------------------------------------------

core::Expected<std::vector<SchedulerWirePeer>, std::string>
DecodeSchedulerPeersResponse(const std::string& body) {
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(body);
    if (!parsed.ok()) {
        return core::make_unexpected(std::string("scheduler response is not valid JSON"));
    }
    if (parsed.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("scheduler response is not an object"));
    }

    const core::JsonObject& root = parsed.value().as_object();
    core::JsonObject::const_iterator peers_it = root.find("peers");
    // An absent `peers` is an empty list, not an error: proto3 does not
    // distinguish an empty repeated field from a missing one, so a scheduler
    // with nothing to report may legitimately omit it.
    if (peers_it == root.end()) return std::vector<SchedulerWirePeer>();
    if (peers_it->second.kind() != core::Json::Kind::Array) {
        return core::make_unexpected(std::string("scheduler peers field is not an array"));
    }

    const core::JsonArray& array = peers_it->second.as_array();
    std::vector<SchedulerWirePeer> out;
    out.reserve(array.size());

    for (std::size_t i = 0; i < array.size(); ++i) {
        if (array[i].kind() != core::Json::Kind::Object) {
            return core::make_unexpected(std::string("scheduler peer is not an object"));
        }
        const core::JsonObject& entry = array[i].as_object();

        core::JsonObject::const_iterator node_it = entry.find("nodeId");
        if (node_it == entry.end() ||
            node_it->second.kind() != core::Json::Kind::String) {
            // A peer with no id cannot be dialled, recorded or logged about.
            // Rejecting the response rather than skipping the entry is
            // deliberate: a scheduler emitting these is broken, and silently
            // dropping them would hide that while quietly shrinking the peer
            // set every refresh.
            return core::make_unexpected(std::string("scheduler peer has no nodeId"));
        }

        SchedulerWirePeer peer;
        peer.node_id = node_it->second.as_string();

        core::JsonObject::const_iterator endpoint_it = entry.find("endpoint");
        if (endpoint_it != entry.end() &&
            endpoint_it->second.kind() == core::Json::Kind::Object) {
            const core::JsonObject& endpoint = endpoint_it->second.as_object();
            core::JsonObject::const_iterator backend_it = endpoint.find("backend");
            core::JsonObject::const_iterator address_it = endpoint.find("address");
            if (backend_it != endpoint.end() &&
                backend_it->second.kind() == core::Json::Kind::String &&
                address_it != endpoint.end() &&
                address_it->second.kind() == core::Json::Kind::String) {
                peer.has_endpoint = true;
                peer.endpoint.backend = backend_it->second.as_string();
                peer.endpoint.address = address_it->second.as_string();
            }
        }
        // Peers whose endpoint is absent or malformed are kept with
        // `has_endpoint == false` and dropped by `FilterSchedulerP2pPeers`,
        // which is where Rust's `filter_scheduler_p2p_peers` makes the same
        // decision. Filtering in two places is how the two come to disagree.
        out.push_back(peer);
    }
    return out;
}

// ---- the wire ---------------------------------------------------------------

SchedulerWireTransport MakeHttpSchedulerWire(const std::string& scheduler_endpoint,
                                             const std::string& shared_token,
                                             uint64_t refresh_timeout_ms,
                                             uint64_t artifact_timeout_ms) {
    SchedulerWireTransport wire;

    const core::Expected<core::Url, std::string> parsed =
        core::Url::Parse(scheduler_endpoint);
    if (!parsed.ok()) {
        // Left empty on purpose. `SchedulerPeerDiscovery::Start` performs the
        // same parse and returns a Noop discovery, so in the normal wiring
        // these callbacks are never consulted; a caller that builds the wire
        // without that check gets callbacks that report failure rather than
        // fabricate success.
        AGENTENV_WARN("scheduler HTTP wire disabled because the endpoint is invalid"
                      << " scheduler_endpoint=" << scheduler_endpoint
                      << " error=" << parsed.error());
        return wire;
    }
    if (!SchemeIsHttp(parsed.value())) {
        AGENTENV_WARN("scheduler HTTP wire disabled because the scheme is not http(s)"
                      << " scheduler_endpoint=" << scheduler_endpoint
                      << " scheme=" << parsed.value().scheme);
        return wire;
    }
    if (parsed.value().scheme == "https") {
        // `core::http` speaks plaintext TCP; it has no TLS. Dialling the port
        // anyway would send an unencrypted request to a TLS listener, which
        // fails obscurely at the first byte. Saying so here is the difference
        // between a configuration error and a mystery.
        AGENTENV_WARN("scheduler HTTP wire disabled because core::http has no TLS"
                      << " scheduler_endpoint=" << scheduler_endpoint);
        return wire;
    }

    const std::string host = parsed.value().host;
    const uint16_t port = EffectivePort(parsed.value());
    const std::string token = shared_token;
    const uint64_t refresh_ms = FloorTimeout(refresh_timeout_ms);
    const uint64_t artifact_ms = FloorTimeout(artifact_timeout_ms);

    AGENTENV_INFO("scheduler HTTP wire ready"
                  << " host=" << host << " port=" << port
                  << " refresh_timeout_ms=" << refresh_ms
                  << " artifact_timeout_ms=" << artifact_ms);

    // Rust `list_p2p_peers`, with REFRESH_RPC_TIMEOUT.
    wire.fetch = [host, port, token, refresh_ms](
                     const std::string& cluster_id, const std::string& backend,
                     const std::string& exclude_node_id,
                     std::vector<SchedulerWirePeer>* out) -> bool {
        std::string body;
        const core::Expected<core::Unit, std::string> posted = Post(
            host, port, kSchedulerPeersPath, token,
            EncodeSchedulerPeersRequest(cluster_id, backend, exclude_node_id,
                                        std::string()),
            refresh_ms, &body);
        if (!posted.ok()) {
            AGENTENV_DEBUG("scheduler list_p2p_peers failed"
                           << " error=" << posted.error());
            return false;
        }
        const core::Expected<std::vector<SchedulerWirePeer>, std::string> decoded =
            DecodeSchedulerPeersResponse(body);
        if (!decoded.ok()) {
            AGENTENV_WARN("scheduler list_p2p_peers returned an undecodable body"
                          << " error=" << decoded.error());
            return false;
        }
        if (out) *out = decoded.value();
        return true;
    };

    // Rust `lookup_p2p_artifact`, with ARTIFACT_RPC_TIMEOUT.
    wire.lookup = [host, port, token, artifact_ms](
                      const std::string& cluster_id, const std::string& backend,
                      const std::string& key, const std::string& exclude_node_id,
                      std::vector<SchedulerWirePeer>* out) -> bool {
        std::string body;
        const core::Expected<core::Unit, std::string> posted = Post(
            host, port, kSchedulerLookupPath, token,
            EncodeSchedulerPeersRequest(cluster_id, backend, exclude_node_id, key),
            artifact_ms, &body);
        if (!posted.ok()) {
            AGENTENV_DEBUG("scheduler lookup_p2p_artifact failed"
                           << " key=" << key << " error=" << posted.error());
            return false;
        }
        const core::Expected<std::vector<SchedulerWirePeer>, std::string> decoded =
            DecodeSchedulerPeersResponse(body);
        if (!decoded.ok()) {
            AGENTENV_WARN("scheduler lookup_p2p_artifact returned an undecodable body"
                          << " key=" << key << " error=" << decoded.error());
            return false;
        }
        if (out) *out = decoded.value();
        return true;
    };

    // Rust `record_p2p_artifact` / `forget_p2p_artifact`. One callback with a
    // selector, which is the shape `SchedulerPeerDiscovery` already declares:
    // the two requests differ only in which endpoint they reach.
    wire.record = [host, port, token, artifact_ms](
                      const std::string& cluster_id, const std::string& backend,
                      const std::string& key, const std::string& node_id,
                      bool record) -> bool {
        const char* const path = record ? kSchedulerRecordPath : kSchedulerForgetPath;
        const core::Expected<core::Unit, std::string> posted = Post(
            host, port, path, token,
            EncodeSchedulerArtifactRequest(cluster_id, backend, key, node_id),
            artifact_ms, NULL);
        if (!posted.ok()) {
            AGENTENV_DEBUG("scheduler artifact bookkeeping failed"
                           << " path=" << path << " key=" << key
                           << " error=" << posted.error());
            return false;
        }
        return true;
    };

    return wire;
}

}  // namespace p2p
}  // namespace agentenv
