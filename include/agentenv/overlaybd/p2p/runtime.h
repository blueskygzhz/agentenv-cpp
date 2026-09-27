// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/p2p/mod.rs
//
// Owns the facade's lifetime and decides whether it runs at all.
#ifndef AGENTENV_OVERLAYBD_P2P_RUNTIME_H_
#define AGENTENV_OVERLAYBD_P2P_RUNTIME_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/cfg.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/overlaybd/p2p/facade.h"
#include "agentenv/p2p/transport.h"

namespace agentenv {
namespace overlaybd {
namespace p2p {

/// Rust `P2pHttpFacadeConfig` as derived from `AppConfig`, split out so the
/// derivation is testable without starting anything.
///
/// Returns an empty optional when the facade must stay disabled, which happens
/// in two cases: P2P is switched off in config, or the transport has no local
/// endpoint (nothing could reach us, so a facade would only add latency).
core::Optional<FacadeConfig> FacadeConfigFromAppConfig(
    const cfg::AppConfig& config, const agentenv::p2p::P2pTransport& transport,
    const std::vector<std::string>& publishable_roots);

/// Rust struct `OverlaybdP2pRuntime`.
///
/// A disabled runtime is a first-class state rather than an error: a failure
/// to start the facade degrades to direct-from-origin reads, which are slower
/// but correct.
class OverlaybdP2pRuntime {
 public:
    /// Rust `disabled`.
    OverlaybdP2pRuntime() {}

    /// Rust `start_from_app_config`, minus the socket bind.
    ///
    /// `addresses` is what a started listener reported; passing an empty
    /// optional records the runtime as disabled.
    static OverlaybdP2pRuntime Started(std::shared_ptr<FacadeCore> core,
                                       const FacadeAddresses& addresses);

    bool enabled() const { return static_cast<bool>(core_); }

    /// Rust `read_facade_address` / `uuid_address` / `publish_address` — all
    /// absent while disabled, which is what makes the generated overlaybd
    /// config fall back to `enable: false`.
    core::Optional<std::string> ReadFacadeAddress() const;
    core::Optional<std::string> UuidAddress() const;
    core::Optional<std::string> PublishAddress() const;

    const std::shared_ptr<FacadeCore>& core() const { return core_; }

    /// Rust `shutdown`.
    core::Expected<core::Unit, std::string> Shutdown();

 private:
    std::shared_ptr<FacadeCore> core_;
    FacadeAddresses addresses_;
};

}  // namespace p2p
}  // namespace overlaybd
}  // namespace agentenv
#endif  // AGENTENV_OVERLAYBD_P2P_RUNTIME_H_
