// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/slot.rs
//
// A `Slot` owns one sandbox's network namespace and the veth pair that links
// it to the host. Each slot index maps deterministically onto a triple of
// addresses (see `NetworkAddressPlan`), so a slot can be torn down and
// recreated without reallocating addresses.
//
// Deviation from Rust: `slot.rs` drives the kernel through in-process
// rtnetlink. This port shells out to `ip`, matching what `iptables_util.cc`
// already does for the filter/nat tables — the project has no netlink layer,
// and adding one only for veth/addr/route setup would duplicate `ip`.
#ifndef AGENTENV_SANDBOX_NETWORK_SLOT_H_
#define AGENTENV_SANDBOX_NETWORK_SLOT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/network.h"
#include "agentenv/sandbox/network/policy.h"

namespace agentenv {
namespace sandbox {
namespace network {

/// Rust `NETNS_PREFIX` — also the marker `PrepareRuntime` uses to decide which
/// files under `<runtime>/netns` are ours to unmount and delete.
extern const char* const kNetnsPrefix;
/// Rust `HOST_VETH_PREFIX`.
extern const char* const kHostVethPrefix;

/// Rust `ARP_RETRANS_TIME_MS` / `NEIGH_SYSCTL_RETRIES` /
/// `NEIGH_SYSCTL_RETRY_DELAY_MS`.
extern const char* const kArpRetransTimeMs;
const int kNeighSysctlRetries      = 5;
const int kNeighSysctlRetryDelayMs = 20;

/// Rust `enum NetworkError`. Modeled as a tagged struct so `Message()`
/// reproduces the `#[error(...)]` strings and `SlotOutOfRange` keeps its
/// payload.
struct NetworkError {
    enum class Kind {
        NamespaceError,
        HostIptablesError,
        IoError,
        SlotOutOfRange,
    };

    Kind        kind = Kind::NamespaceError;
    std::string detail;
    /// `SlotOutOfRange { idx, max }`.
    uint32_t    idx = 0;
    uint32_t    max = 0;

    static NetworkError Namespace(const std::string& source);
    static NetworkError HostIptables(const std::string& source);
    static NetworkError Io(const std::string& source);
    static NetworkError SlotOutOfRangeErr(uint32_t idx, uint32_t max);

    std::string Message() const;
};

/// Rust `fn parse_nameserver_ipv4` — first non-loopback, non-unspecified
/// `nameserver` line. Returns false when the file names none.
bool ParseNameserverIpv4(const std::string& resolv_conf_contents, uint32_t* out);

/// Rust `fn resolve_guest_dns_server` — systemd's stub resolver first, then
/// `/etc/resolv.conf`, then a hardcoded 8.8.8.8 so the guest always gets a
/// usable server.
uint32_t ResolveGuestDnsServer();

/// Rust `struct Slot`.
///
/// Not copyable: the destructor tears down live kernel state, so a copy would
/// unmount a namespace that the original still owns (Rust enforces this by not
/// deriving `Clone`).
class Slot {
 public:
    /// Rust `Slot::new` — validates the index and derives the slot's addresses.
    /// Creates no kernel state; `CreateNetwork` does that.
    static core::Expected<Slot*, NetworkError>
        New(uint32_t idx, const NetworkAddressPlan& address_plan,
            const std::string& netns_dir);

    ~Slot();

    /// Rust `Slot::create_network` — unshares a network namespace, binds it
    /// under `netns_dir`, builds the veth pair and installs the slot's rules.
    ///
    /// Arms destructor cleanup before touching anything, so a failure midway
    /// still unwinds what was already created.
    core::Expected<core::Unit, NetworkError> CreateNetwork();

    /// Rust `Slot::cleanup` — unmounts and removes the namespace and deletes
    /// the host-side veth. Idempotent; safe to call when nothing was created.
    core::Expected<core::Unit, NetworkError> Cleanup(bool best_effort);

    /// Rust `Slot::set_egress_policy`.
    ///
    /// Tracks whether the namespace's user egress chain currently holds rules:
    /// a warm-pool slot keeps its namespace across tenants, so the next tenant
    /// has to clear what the previous one installed.
    core::Expected<core::Unit, NetworkError>
        SetEgressPolicy(const core::Optional<SandboxNetworkPolicy>& policy,
                        uint16_t egress_proxy_port);

    /// Rust `Slot::build_ip_boot_arg` — the guest's `ip=` kernel argument.
    std::string BuildIpBootArg() const;

    /// Rust `Slot::namespace_path` — the bind-mounted namespace file.
    std::string NamespacePath() const;

    /// Rust `Slot::host_veth_name`.
    static std::string HostVethName(uint32_t idx);

    uint32_t Idx() const { return idx_; }
    const std::string& NamespaceId() const { return namespace_id_; }
    uint32_t HostInteractionIp() const { return host_interaction_ip_; }
    uint32_t VethHostIp() const { return veth_host_ip_; }
    uint32_t VethVmIp() const { return veth_vm_ip_; }

 private:
    Slot(uint32_t idx, const NetworkAddressPlan& address_plan,
         const std::string& netns_dir, uint32_t host_interaction_ip,
         uint32_t veth_host_ip, uint32_t veth_vm_ip);

    Slot(const Slot&);
    Slot& operator=(const Slot&);

    /// Rust `Slot::setup_namespace_internal` — runs on a dedicated thread so
    /// the `unshare(CLONE_NEWNET)` does not leak into the caller's thread.
    core::Expected<core::Unit, std::string> SetupNamespaceInternal();

    /// Rust `Slot::configure_namespace_interfaces`.
    core::Expected<core::Unit, std::string> ConfigureNamespaceInterfaces();

    /// Rust `Slot::configure_host_interface_async`.
    core::Expected<core::Unit, std::string> ConfigureHostInterface();

    uint32_t    idx_;
    std::string namespace_id_;
    uint32_t    host_interaction_ip_;
    uint32_t    veth_host_ip_;
    uint32_t    veth_vm_ip_;
    NetworkAddressPlan address_plan_;
    std::string netns_dir_;
    bool        cleanup_armed_;
    bool        user_egress_rules_present_;
};

/// Rust `Slot::configure_namespace_iptables_rules` — the FORWARD/SNAT/DNAT set
/// installed inside a slot's namespace.
///
/// Exposed for tests: the rules are pure string building, while applying them
/// needs a namespace and root.
std::vector<IptablesRestoreCommand> BuildNamespaceIptablesRules(
    uint32_t host_interaction_ip, uint32_t veth_vm_ip, uint32_t vm_ip);

/// Rust `Slot::tune_neigh_retrans_time_ms` — shortens ARP retransmit on the
/// given interface. The sysctl appears asynchronously after the link is
/// created, so a missing path is retried rather than treated as failure
/// (Rust issue #272: otherwise resume pays an ARP timeout).
void TuneNeighRetransTimeMs(const std::string& interface);

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_NETWORK_SLOT_H_
