// SPDX-License-Identifier: MIT
// Rust: src/p2p/config.rs
#include "agentenv/p2p/config.h"

namespace agentenv {
namespace p2p {

const char* const kIrohBackendId = "iroh";
const char* const kHttpBackendId = "http";

core::Optional<std::string> BackendId(P2pTransportKind kind) {
    switch (kind) {
        // Rust: `Self::Disabled => None`.
        case P2pTransportKind::Disabled: return core::Optional<std::string>();
        case P2pTransportKind::Iroh:     return std::string(kIrohBackendId);
        // Must be a real id, never `Some("")`: the scheduler filters peers by
        // an exact backend match, so an empty string would issue the RPCs and
        // then discard every peer — a silent no-provider result instead of the
        // short-circuit `Disabled` gets.
        case P2pTransportKind::Http:     return std::string(kHttpBackendId);
    }
    return core::Optional<std::string>();
}

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, e - b + 1);
}

ResolvedP2pConfig ResolvedP2pConfig::FromConfig(const P2pRawConfig& c) {
    ResolvedP2pConfig r;
    // Rust: disabled overrides transport to Disabled.
    r.transport = c.enabled ? c.transport : P2pTransportKind::Disabled;
    r.store_dir = c.store_dir;

    std::string trimmed = trim(c.listen_addr);
    if (!trimmed.empty()) {
        r.has_listen_addr = true;
        r.listen_addr = trimmed;
    } else {
        r.has_listen_addr = false;
    }

    r.lookup_timeout_ms = c.lookup_timeout_ms;
    r.fetch_timeout_ms = c.fetch_timeout_ms;
    // Rust: floored to >= 1s.
    uint64_t refresh = c.peer_discovery_refresh_interval_secs;
    r.peer_discovery_refresh_interval_secs = refresh < 1 ? 1 : refresh;
    return r;
}

}  // namespace p2p
}  // namespace agentenv
