// SPDX-License-Identifier: MIT
// The scheduler discovery wire, over plain HTTP/1.1.
//
// Rust reaches the scheduler over gRPC: `src/p2p/discovery/scheduler.rs` holds
// a `SchedulerClient<Channel>` and calls four RPCs declared in
// `services/api/proto/scheduler.proto` — `ListP2pPeers`, `LookupP2pArtifact`,
// `RecordP2pArtifact` and `ForgetP2pArtifact`.
//
// Why this is HTTP and not gRPC. The gRPC path is gated on
// `AGENTENV_WITH_GRPC`, which needs protobuf >= 3 and `grpc_cpp_plugin`;
// neither is a dependency this tree carries. More to the point, the `http`
// P2P transport already decided not to be wire-compatible with Rust nodes, so
// a cluster running this build has no gRPC scheduler to talk to either. Four
// calls whose payloads are three strings in and a peer list out do not justify
// dragging in a code generator.
//
// What this file is *not*. It does not reimplement discovery. The caching,
// the jittered refresh loop, the backend filtering and the record/forget
// bookkeeping are all already in `SchedulerPeerDiscovery`, which takes its
// transport as three injected callbacks precisely so the wire can be swapped
// without touching any of that. This file is only those three callbacks.
//
// Wire format, `agentenv-scheduler-http/v1`. Each RPC becomes one POST whose
// body is the request message as JSON, using the proto field names verbatim:
//
//   POST /scheduler/v1/p2p/peers    {clusterId, backend, excludeNodeId}
//                                -> {peers: [{nodeId, endpoint: {backend, address}}]}
//   POST /scheduler/v1/p2p/lookup   {clusterId, backend, key, excludeNodeId}
//                                -> {peers: [...]}
//   POST /scheduler/v1/p2p/record   {clusterId, backend, key, nodeId} -> {}
//   POST /scheduler/v1/p2p/forget   {clusterId, backend, key, nodeId} -> {}
//
// Keeping the proto's field set exactly — no additions, no omissions — is what
// would let a gRPC-to-HTTP shim sit in front of a real scheduler later without
// renegotiating anything.
#ifndef AGENTENV_P2P_SCHEDULER_HTTP_H_
#define AGENTENV_P2P_SCHEDULER_HTTP_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/p2p/discovery.h"
#include "agentenv/p2p/node.h"

namespace agentenv {
namespace p2p {

extern const char* const kSchedulerPeersPath;
extern const char* const kSchedulerLookupPath;
extern const char* const kSchedulerRecordPath;
extern const char* const kSchedulerForgetPath;

/// Bound on one scheduler response body.
///
/// A peer list is the only unbounded field, and the scheduler is a trusted
/// component — but "trusted" is not the same as "incapable of a bug", and
/// `core::http` buffers a whole body before it is parsed. This is the ceiling
/// that keeps a runaway response from being this node's problem.
extern const std::size_t kMaxSchedulerResponseBytes;

/// Builds the three callbacks `SchedulerPeerDiscovery` needs.
///
/// `scheduler_endpoint` is the same string the gRPC path takes, e.g.
/// `http://scheduler:9090`. It is parsed here rather than in the callbacks so
/// a malformed endpoint is reported once, at wiring time; the callbacks
/// themselves must stay infallible-to-construct because `Start` has already
/// decided by then whether discovery runs at all.
///
/// On a parse failure the returned callbacks are empty. That is deliberately
/// *not* the same as callbacks that report success: an absent transport makes
/// `SchedulerPeerDiscovery` report its RPCs as failed, which is the honest
/// answer and is what keeps a stale peer cache in place instead of being
/// cleared by a fabricated empty result.
///
/// `shared_token`, when non-empty, is sent as `X-Agentenv-P2p-Token` — the
/// same header the blob surface uses, because within one cluster there is one
/// secret and giving the scheduler calls a second one would mean two things to
/// rotate for no gain.
SchedulerWireTransport MakeHttpSchedulerWire(const std::string& scheduler_endpoint,
                                             const std::string& shared_token,
                                             uint64_t refresh_timeout_ms,
                                             uint64_t artifact_timeout_ms);

// ---- pieces exposed for testing --------------------------------------------

/// Renders a `ListP2pPeers` / `LookupP2pArtifact` request body.
///
/// `key` empty selects the peers form, which carries no key at all rather than
/// an empty one: the proto has two distinct messages, and sending a field the
/// scheduler does not expect invites it to be ignored by one implementation
/// and rejected by another.
std::string EncodeSchedulerPeersRequest(const std::string& cluster_id,
                                        const std::string& backend,
                                        const std::string& exclude_node_id,
                                        const std::string& key);

/// Renders a `RecordP2pArtifact` / `ForgetP2pArtifact` request body.
std::string EncodeSchedulerArtifactRequest(const std::string& cluster_id,
                                           const std::string& backend,
                                           const std::string& key,
                                           const std::string& node_id);

/// Parses a `{peers: [...]}` response into the wire peer type.
///
/// A peer missing its endpoint is kept with `has_endpoint == false` rather
/// than dropped here, because dropping is `FilterSchedulerP2pPeers`'s job and
/// Rust's `filter_scheduler_p2p_peers` is what the port must agree with. Two
/// places deciding which peers survive is how the two drift.
core::Expected<std::vector<SchedulerWirePeer>, std::string>
    DecodeSchedulerPeersResponse(const std::string& body);

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_SCHEDULER_HTTP_H_
