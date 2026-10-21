// SPDX-License-Identifier: MIT
// Rust: src/p2p/config.rs — P2pTransportKind + ResolvedP2pConfig.
#ifndef AGENTENV_P2P_CONFIG_H_
#define AGENTENV_P2P_CONFIG_H_

#include <cstdint>
#include <string>

#include "agentenv/core/optional.h"

namespace agentenv {
namespace p2p {

/// Rust: `P2pTransportKind`.
enum class P2pTransportKind { Disabled, Iroh };

/// Rust: `iroh::IROH_BACKEND_ID`.
extern const char* const kIrohBackendId;

/// Rust: `P2pTransportKind::backend_id()` — `Option<&'static str>`.
///
/// `Disabled` is `None`, and the distinction is load-bearing rather than
/// cosmetic. `mod.rs:57` feeds this into `SchedulerPeerDiscovery`'s
/// `backend: Option<String>`, and the scheduler short-circuits
/// `peers_for_key`/`record_key`/`forget_key` on `None` without issuing an RPC.
/// Collapsing `None` into `""` would instead produce `Some("")`, which still
/// talks to the scheduler and then filters every peer out in
/// `filter_scheduler_p2p_peers` — a silent no-provider result where Rust
/// performs no call at all.
core::Optional<std::string> BackendId(P2pTransportKind kind);

/// Rust: `cfg::P2pConfig` (the raw config subset needed to resolve).
struct P2pRawConfig {
    bool enabled = false;
    P2pTransportKind transport = P2pTransportKind::Disabled;
    std::string      store_dir;
    std::string   listen_addr;
    uint64_t     lookup_timeout_ms = 5000;
    uint64_t      fetch_timeout_ms = 30000;
    uint64_t         peer_discovery_refresh_interval_secs = 30;
};

/// Rust: `ResolvedP2pConfig`.
struct ResolvedP2pConfig {
    P2pTransportKind transport = P2pTransportKind::Disabled;
    std::string      store_dir;
    bool   has_listen_addr = false;
  std::string      listen_addr;
    uint64_t         lookup_timeout_ms = 0;
    uint64_t  fetch_timeout_ms = 0;
    uint64_t         peer_discovery_refresh_interval_secs = 0;

    /// Rust `from_config` — disabled overrides transport; listen_addr trimmed &
    /// emptied-to-None; refresh interval floored to >= 1s.
    static ResolvedP2pConfig FromConfig(const P2pRawConfig& c);
};

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_CONFIG_H_
