// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/p2p/mod.rs
#include "agentenv/overlaybd/p2p/runtime.h"

#include "agentenv/core/logging.h"

namespace agentenv {
namespace overlaybd {
namespace p2p {

using core::Optional;
using core::Unit;

Optional<FacadeConfig> FacadeConfigFromAppConfig(
    const cfg::AppConfig& config, const agentenv::p2p::P2pTransport& transport,
    const std::vector<std::string>& publishable_roots) {
    if (!config.p2p.enabled) return Optional<FacadeConfig>();

    // Without a local endpoint no peer can reach this node, so a facade would
    // only add a lookup round-trip to every read.
    agentenv::p2p::P2pEndpoint endpoint;
    if (!transport.LocalEndpoint(&endpoint)) return Optional<FacadeConfig>();

    FacadeConfig facade_config;  // starts at the DEFAULT_* values
    facade_config.allowed_publish_roots = publishable_roots;
    // These two come from the ublk overlaybd section, not from `[p2p]`: they
    // bound a *per-read* operation and are far tighter than the transport's
    // own timeouts.
    facade_config.lookup_timeout_ms = config.ublk.overlaybd.p2p_lookup_timeout_ms;
    facade_config.fetch_range_timeout_ms = config.ublk.overlaybd.p2p_fetch_range_timeout_ms;
    return Optional<FacadeConfig>(facade_config);
}

OverlaybdP2pRuntime OverlaybdP2pRuntime::Started(std::shared_ptr<FacadeCore> core,
                                                 const FacadeAddresses& addresses) {
    OverlaybdP2pRuntime runtime;
    if (!core) return runtime;  // stays disabled

    runtime.core_ = core;
    runtime.addresses_ = addresses;
    AGENTENV_INFO("enabled p2p http facade for overlaybd address="
                  << addresses.address << " uuid_address=" << addresses.uuid_address
                  << " publish_address=" << addresses.publish_address);
    return runtime;
}

Optional<std::string> OverlaybdP2pRuntime::ReadFacadeAddress() const {
    if (!core_) return Optional<std::string>();
    return Optional<std::string>(addresses_.address);
}

Optional<std::string> OverlaybdP2pRuntime::UuidAddress() const {
    if (!core_) return Optional<std::string>();
    return Optional<std::string>(addresses_.uuid_address);
}

Optional<std::string> OverlaybdP2pRuntime::PublishAddress() const {
    if (!core_) return Optional<std::string>();
    return Optional<std::string>(addresses_.publish_address);
}

core::Expected<Unit, std::string> OverlaybdP2pRuntime::Shutdown() {
    // Idempotent, like Rust's consuming `shutdown` followed by `Drop`.
    core_.reset();
    return Unit();
}

}  // namespace p2p
}  // namespace overlaybd
}  // namespace agentenv
