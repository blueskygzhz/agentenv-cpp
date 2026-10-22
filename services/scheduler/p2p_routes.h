// SPDX-License-Identifier: MIT
// The scheduler's P2P HTTP surface — the server side of
// `agentenv/p2p/scheduler_http.h`.
//
// Go upstream: the four P2P methods of services/scheduler/internal/service.go
// (`ListP2PPeers`, `RecordP2PArtifact`, `ForgetP2PArtifact`,
// `LookupP2PArtifact`), which are gRPC there. The client half of this tree
// already speaks them over HTTP because `AGENTENV_WITH_GRPC` needs a code
// generator the build does not carry; this is the matching server, so a
// cluster can run P2P discovery without an external component.
//
// What the two halves share. Nothing but the wire format, deliberately: these
// handlers are reached by `POST /scheduler/v1/p2p/*` with the proto's field
// names as JSON, exactly as `scheduler_http.h` documents. Linking the client
// in to reuse its encoders would make the test that they agree vacuous.
#ifndef AGENTENV_SERVICES_SCHEDULER_P2P_ROUTES_H_
#define AGENTENV_SERVICES_SCHEDULER_P2P_ROUTES_H_

#include <memory>
#include <string>

#include "agentenv/api/server.h"
#include "services/scheduler/artifact_index.h"
#include "services/scheduler/internal.h"

namespace agentenv {
namespace services {
namespace scheduler {

/// Registers the four P2P endpoints on `server`.
///
/// `shared_token`, when non-empty, is required as `X-Agentenv-P2p-Token` —
/// the same header and the same cluster secret the blob surface uses, so
/// there is one thing to rotate rather than two. Empty disables the check.
///
/// `nodes` and `artifacts` outlive the server: the handlers capture them by
/// pointer because the registry is also what placement reads, and copying it
/// per request would hand out a stale view.
void RegisterSchedulerP2pRoutes(api::Server* server, NodeRegistry* nodes,
                                InMemoryArtifactStore* artifacts,
                                const std::string& shared_token);

/// Registers a minimal node-registration surface.
///
/// No Go counterpart: upstream nodes arrive through Kubernetes discovery or a
/// heartbeat RPC, neither of which is ported. Without some way to populate the
/// registry the P2P endpoints would answer every query with an empty list, so
/// this exists to make the scheduler usable on its own:
///
///   POST   /scheduler/v1/nodes         {nodeId, clusterId, endpoint,
///                                       p2pBackend, p2pAddress}
///   DELETE /scheduler/v1/nodes/{id}    also drops the node's artifact claims
///   GET    /scheduler/v1/nodes         {nodes: [...]}
///
/// Kept separate from `RegisterSchedulerP2pRoutes` so a deployment that gets
/// its nodes from somewhere real can decline to expose it.
void RegisterSchedulerNodeRoutes(api::Server* server, NodeRegistry* nodes,
                                 InMemoryArtifactStore* artifacts,
                                 const std::string& shared_token);

// ---- pieces exposed for testing --------------------------------------------

/// Renders `{peers: [{nodeId, endpoint: {backend, address}}]}`.
std::string EncodeP2pPeersResponse(const std::vector<Node>& peers);

}  // namespace scheduler
}  // namespace services
}  // namespace agentenv
#endif  // AGENTENV_SERVICES_SCHEDULER_P2P_ROUTES_H_
