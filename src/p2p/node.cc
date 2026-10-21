// SPDX-License-Identifier: MIT
// Rust: src/p2p/mod.rs
#include "agentenv/p2p/node.h"

#include "agentenv/p2p/iroh.h"

namespace agentenv {
namespace p2p {

std::shared_ptr<P2pPeerDiscovery> PeerDiscoveryFromConfig(
    const P2pFactoryInputs& inputs, SchedulerWireTransport wire) {
    // Rust: `let Some(scheduler_endpoint) = config.cluster.scheduler_endpoint
    //        .clone() else { return Arc::new(NoopP2pPeerDiscovery) };`
    if (!inputs.scheduler_endpoint.has_value()) {
        return std::shared_ptr<P2pPeerDiscovery>(new NoopP2pPeerDiscovery());
    }

    // Rust: `SchedulerPeerDiscovery::start(endpoint, node_id, cluster_id,
    //        refresh_interval, transport.backend_id().map(ToString::to_string))`.
    return SchedulerPeerDiscovery::Start(
        *inputs.scheduler_endpoint,
        inputs.node_id,
        inputs.cluster_id,
        inputs.p2p.peer_discovery_refresh_interval_secs,
        BackendId(inputs.p2p.transport),
        std::move(wire.fetch), std::move(wire.record), std::move(wire.lookup));
}

core::Expected<std::shared_ptr<P2pTransport>, std::string> TransportFromConfig(
    const P2pFactoryInputs& inputs, SchedulerWireTransport wire) {
    switch (inputs.p2p.transport) {
        case P2pTransportKind::Disabled:
            // Rust: `Ok(Arc::new(DisabledP2pTransport))`.
            return std::shared_ptr<P2pTransport>(new DisabledP2pTransport());

        case P2pTransportKind::Iroh: {
            // Rust builds the discovery first and hands it to the transport.
            std::shared_ptr<P2pPeerDiscovery> discovery =
                PeerDiscoveryFromConfig(inputs, std::move(wire));
            std::string err;
            std::shared_ptr<IrohBlobsP2pTransport> transport =
                IrohBlobsP2pTransport::Start(inputs.p2p, inputs.node_id, discovery, &err);
            if (!transport) {
                // Rust: `IrohBlobsP2pTransport::new(..).await?` — the error
                // reaches `server.rs` and the process fails to start. Returning
                // a DisabledP2pTransport here instead would silently downgrade
                // a node that was explicitly configured for p2p.
                return core::make_unexpected(err.empty()
                    ? std::string("failed to start the iroh P2P transport")
                    : err);
            }
            return std::shared_ptr<P2pTransport>(transport);
        }
    }
    return core::make_unexpected(std::string("unknown P2P transport kind"));
}

}  // namespace p2p
}  // namespace agentenv
