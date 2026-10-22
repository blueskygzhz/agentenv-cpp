// SPDX-License-Identifier: MIT
// Rust: src/p2p/mod.rs — `transport_from_config` / `peer_discovery_from_config`.
//
// These two functions are the only way the rest of the system is supposed to
// obtain a transport or a discovery. Without them the p2p library is a set of
// types nobody can assemble, which is why `[p2p].enabled = true` previously
// resolved to "no p2p" instead of to a transport.
//
// Deviation from Rust, deliberate and load-bearing: Rust takes `&AppConfig`
// because `cfg` and `p2p` live in one crate. Here `agentenv-cfg` links
// `agentenv-p2p` (cfg.h includes p2p/all.h for `P2pTransportKind`), so a
// `p2p -> cfg` dependency would be a CMake cycle. The factories therefore take
// the already-resolved inputs and the caller does the `AppConfig` projection.
// `P2pFactoryInputs` names exactly the fields Rust reads, so the projection is
// mechanical and reviewable.
#ifndef AGENTENV_P2P_NODE_H_
#define AGENTENV_P2P_NODE_H_

#include <memory>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/p2p/config.h"
#include "agentenv/p2p/discovery.h"
#include "agentenv/p2p/http_transport.h"
#include "agentenv/p2p/transport.h"

namespace agentenv {
namespace p2p {

/// The `AppConfig` + `NodeIdentity` subset that `src/p2p/mod.rs` reads.
///
/// Rust reads, in order: `config.p2p` (via `ResolvedP2pConfig::from_config`),
/// `config.cluster.scheduler_endpoint`, `node_identity.id` and
/// `node_identity.cluster_id`.
struct P2pFactoryInputs {
    /// Rust `ResolvedP2pConfig::from_config(&config.p2p)`.
    ResolvedP2pConfig p2p;
    /// Rust `config.cluster.scheduler_endpoint` — `None` disables discovery.
    core::Optional<std::string> scheduler_endpoint;
    /// Rust `node_identity.id`.
    std::string node_id;
    /// Rust `node_identity.cluster_id.to_string()`.
    std::string cluster_id;

    /// `host:port` peers should dial. Only read by the `Http` transport.
    ///
    /// No Rust counterpart: iroh learns what it is reachable on from
    /// `wait_for_dialable_addr`, which asks the endpoint after it binds.
    /// Without that machinery the answer has to be configured, and it cannot
    /// be derived from `p2p.listen_addr` because a node normally binds a
    /// wildcard — advertising `0.0.0.0:p` would hand every peer an address
    /// that resolves to itself.
    std::string p2p_advertise_addr;

    /// Shared secret guarding the `Http` transport's surface.
    ///
    /// `core::http` speaks plaintext TCP, so this is the only credential on
    /// the blob port. Empty disables the check, which is only defensible when
    /// the port is unreachable from outside the cluster by other means.
    std::string p2p_shared_token;
};

/// The gated gRPC scheduler transport, injected rather than linked.
///
/// Rust builds a `SchedulerClient<Channel>` here. The C++ gRPC transport is
/// behind `AGENTENV_WITH_GRPC`; when it is absent these stay empty and the
/// resulting discovery reports scheduler RPCs as failed (it does not pretend
/// they succeeded). See `SchedulerPeerDiscovery`.
struct SchedulerWireTransport {
    SchedulerPeerDiscovery::WireFetch  fetch;
    SchedulerPeerDiscovery::WireRecord record;
    SchedulerPeerDiscovery::WireLookup lookup;
};

/// Rust `p2p::peer_discovery_from_config`.
///
/// Returns `NoopP2pPeerDiscovery` when no scheduler endpoint is configured, or
/// when the configured endpoint does not parse (the latter also warns). The
/// backend id comes from `BackendId(p2p.transport)`, so a disabled transport
/// yields a discovery that issues no scheduler RPCs at all.
std::shared_ptr<P2pPeerDiscovery> PeerDiscoveryFromConfig(
    const P2pFactoryInputs& inputs, SchedulerWireTransport wire);

/// Rust `p2p::transport_from_config`.
///
/// `Disabled` yields `DisabledP2pTransport`. `Iroh` constructs the iroh-blobs
/// transport and **propagates a construction failure** — Rust's `?` makes this
/// a startup error, and a node configured for p2p must not come up silently
/// without it. In a build without `AGENTENV_WITH_IROH` that failure is exactly
/// what `IrohBlobsP2pTransport::Start` reports.
///
/// `Http` has no Rust counterpart; it needs `blob_server` because
/// `agentenv-p2p` cannot link `agentenv-api` (api -> orchestrator -> snapshot
/// -> p2p would make it a cycle). Leaving the factory empty is not a quiet
/// downgrade: the `Http` arm fails with that as the stated reason.
core::Expected<std::shared_ptr<P2pTransport>, std::string> TransportFromConfig(
    const P2pFactoryInputs& inputs, SchedulerWireTransport wire,
    P2pBlobServerFactory blob_server = P2pBlobServerFactory());

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_NODE_H_
