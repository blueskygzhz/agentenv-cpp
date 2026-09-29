// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/egress_proxy.rs
//
// A namespace-local transparent proxy for the egress capabilities that need
// connection inspection. iptables REDIRECTs the guest's outbound TCP to this
// listener, which then reads just enough of the connection preface to learn
// the intended hostname (HTTP `Host:` or TLS SNI), checks it against the
// sandbox's policy, and — for domain grants — reconnects to an address
// resolved in the *host* namespace.
//
// The host-namespace resolution is the security property, not an
// optimisation: a guest that controls its own `/etc/hosts`, DNS server, or
// routes must not be able to point an allowed hostname at an address of its
// choosing. See `resolver.h`.
//
// Deviation from Rust: the accept/relay threads and `SO_ORIGINAL_DST`
// plumbing need live sockets and an iptables REDIRECT, so this header exposes
// the two pieces that carry the actual policy semantics — the preface parsers
// and the upstream decision — plus the policy staging state machine. Those are
// what decide whether a connection is authorised, and they are testable
// without a network.
#ifndef AGENTENV_SANDBOX_NETWORK_EGRESS_PROXY_H_
#define AGENTENV_SANDBOX_NETWORK_EGRESS_PROXY_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/network/policy.h"
#include "agentenv/sandbox/network/resolver.h"

namespace agentenv {
namespace sandbox {
namespace network {

/// Rust `MAX_PREFACE_BYTES` — how much of the preface we will buffer before
/// giving up on finding a hostname.
const size_t kMaxPrefaceBytes = 64 * 1024;
/// Rust `PREFACE_TIMEOUT` = 10s.
const int kPrefaceTimeoutMs = 10000;
/// Rust `UPSTREAM_TIMEOUT` = 30s.
const int kUpstreamTimeoutMs = 30000;
/// Rust `EGRESS_PROXY_PORT`.
const uint16_t kEgressProxyPort = 15000;
/// Rust `MAX_CONNECTIONS_PER_PROXY` — bounds resource use without imposing an
/// idle timeout on established streams, since long-lived application
/// connections are permitted.
const size_t kMaxConnectionsPerProxy = 256;
/// Rust `SO_ORIGINAL_DST`.
const int kSoOriginalDst = 80;

/// Rust `enum UpstreamDecision`.
struct UpstreamDecision {
    enum class Kind {
        /// Connect to one of these addresses.
        Forward,
        /// Policy refuses this connection.
        Deny,
        /// Policy would allow it, but no usable upstream could be resolved.
        /// Distinct from `Deny` so the caller can report a transient failure
        /// rather than a policy violation.
        Unavailable,
    };

    Kind kind = Kind::Deny;
    std::vector<ResolvedAddr> addresses;

    static UpstreamDecision Forward(const std::vector<ResolvedAddr>& addrs) {
        UpstreamDecision d;
        d.kind = Kind::Forward;
        d.addresses = addrs;
        return d;
    }
    static UpstreamDecision Deny() {
        UpstreamDecision d;
        d.kind = Kind::Deny;
        return d;
    }
    static UpstreamDecision Unavailable() {
        UpstreamDecision d;
        d.kind = Kind::Unavailable;
        return d;
    }

    bool is_forward() const { return kind == Kind::Forward; }
    bool is_deny() const { return kind == Kind::Deny; }
    bool is_unavailable() const { return kind == Kind::Unavailable; }
};

// ---- preface inspection --------------------------------------------------
//
// All three return a tri-state, matching Rust's `Result<Option<String>>`:
//   ok() == false            -> the preface is malformed (Rust `Err`)
//   ok(), !out->has_value()  -> need more bytes (Rust `Ok(None)`)
//   ok(), *out == ""         -> parsed, but no usable hostname; policy then
//                               fails closed (Rust `Ok(Some(String::new()))`)
//   ok(), *out == "host"     -> the hostname to authorise

/// Rust `parse_protocol_host` — TLS when the port is 443 or the first byte is
/// the TLS handshake record type (22), otherwise HTTP.
core::Expected<core::Unit, std::string>
    ParseProtocolHost(const uint8_t* preface, size_t len, uint16_t original_port,
                      core::Optional<std::string>* out);

/// Rust `parse_http_host`.
///
/// A duplicate or malformed `Host` header yields the empty string rather than
/// the first value: authorising on one of several conflicting Host headers is
/// exactly the request-smuggling shape this proxy has to refuse.
core::Expected<core::Unit, std::string>
    ParseHttpHost(const uint8_t* preface, size_t len, core::Optional<std::string>* out);

/// Rust `normalize_host` — trims, drops a trailing root dot, strips an
/// `:port` suffix, lowercases.
///
/// A bracketed IPv6 literal returns empty: the dataplane is IPv4-only, so such
/// a value cannot match a domain allowlist entry and must fail closed rather
/// than be partially interpreted.
std::string NormalizeHost(const uint8_t* value, size_t len);
std::string NormalizeHost(const std::string& value);

/// Rust `parse_tls_sni`.
///
/// Reassembles handshake payloads across TLS record boundaries before looking
/// for the SNI extension, so a client that fragments its ClientHello cannot
/// slip past inspection by choosing unusual record sizes.
core::Expected<core::Unit, std::string>
    ParseTlsSni(const uint8_t* preface, size_t len, core::Optional<std::string>* out);

// ---- upstream selection --------------------------------------------------

/// Rust `select_upstream`.
///
/// Order matters and follows E2B: an explicitly allowed CIDR is honoured
/// *before* a hostname is required, so direct-IP HTTPS keeps working while
/// domain interception is enabled.
UpstreamDecision SelectUpstream(const SandboxNetworkPolicy& policy,
                                HostNetResolver* resolver,
                                const std::string& hostname,
                                uint32_t original_dst_ip, uint16_t original_dst_port,
                                const std::atomic<bool>* cancel);

/// Rust `resolve_trusted_upstream` — resolves in the host namespace, then
/// keeps only the addresses that the policy also allows for this hostname.
/// Returns false when nothing survives the filter.
bool ResolveTrustedUpstream(const SandboxNetworkPolicy& policy, HostNetResolver* resolver,
                            const std::string& hostname, uint16_t port,
                            const std::atomic<bool>* cancel,
                            std::vector<ResolvedAddr>* out);

/// Rust `original_destination` — reads `SO_ORIGINAL_DST` off a redirected
/// socket. IPv4 only, deliberately: an AF_INET6 original-dst plus ip6tables
/// enforcement is a separate end-to-end dataplane change.
core::Expected<core::Unit, std::string>
    OriginalDestination(int fd, uint32_t* ip_out, uint16_t* port_out);

/// Rust `struct EgressProxy`, restricted to the policy bookkeeping.
///
/// Policies are staged rather than swapped in place: `Prepare` parks the new
/// policy, and only `Activate` promotes it. A launch that fails between the
/// two calls therefore leaves the previous tenant's policy in force instead of
/// a half-applied one.
class EgressProxy {
 public:
    EgressProxy() {}

    /// Rust `port`.
    uint16_t Port() const { return kEgressProxyPort; }

    /// Rust `prepare` — stage a policy for `host_interaction_ip`.
    void Prepare(uint32_t host_interaction_ip, const SandboxNetworkPolicy& policy);
    /// Rust `activate` — promote the staged policy, if any.
    void Activate(uint32_t host_interaction_ip);
    /// Rust `discard_pending` — drop the staged policy, keeping the active one.
    void DiscardPending(uint32_t host_interaction_ip);
    /// Rust `deactivate` — remove the active policy but keep existing
    /// connections registered so teardown can still stop them.
    void Deactivate(uint32_t host_interaction_ip);
    /// Rust `teardown`.
    void Teardown(uint32_t host_interaction_ip);
    /// Rust `has_active`.
    bool HasActive(uint32_t host_interaction_ip) const;

    /// Rust `active_policy`.
    bool ActivePolicy(uint32_t host_interaction_ip, SandboxNetworkPolicy* out) const;
    /// Whether a staged (not yet activated) policy exists.
    bool HasPending(uint32_t host_interaction_ip) const;

    /// Rust `register_connection` — returns false once the per-listener limit
    /// is reached. The limit is scoped per host-interaction IP, so one busy
    /// sandbox cannot starve another.
    bool RegisterConnection(uint32_t host_interaction_ip, size_t* id_out);
    /// Rust `unregister_connection`.
    void UnregisterConnection(uint32_t host_interaction_ip, size_t id);
    /// Live connection count for one listener.
    size_t ConnectionCount(uint32_t host_interaction_ip) const;

 private:
    EgressProxy(const EgressProxy&);
    EgressProxy& operator=(const EgressProxy&);

    std::map<uint32_t, SandboxNetworkPolicy>   active_;
    std::map<uint32_t, SandboxNetworkPolicy>   pending_;
    std::map<uint32_t, std::vector<size_t> >   connections_;
    size_t                                     next_connection_id_ = 0;
};

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_NETWORK_EGRESS_PROXY_H_
