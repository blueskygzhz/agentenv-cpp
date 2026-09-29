// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/slot.rs
#include "agentenv/sandbox/network/slot.h"

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

#include "agentenv/cfg/network.h"
#include "agentenv/core/capability.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/logging.h"
#include "agentenv/privileges.h"
#include "agentenv/sandbox/network/iptables_util.h"

namespace agentenv {
namespace sandbox {
namespace network {

const char* const kNetnsPrefix      = "agentenv-ns-";
const char* const kHostVethPrefix   = "veth-";
const char* const kArpRetransTimeMs = "100";

namespace {

/// The namespace-local names are fixed, matching Rust's hardcoded strings.
const char* const kVpeerName = "vpeer";
const char* const kTapName   = "tap0";

std::string Join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir[dir.size() - 1] == '/') return dir + name;
    return dir + "/" + name;
}

/// Runs `argv` under CAP_NET_ADMIN and returns a non-empty message on failure.
///
/// Rust reaches the kernel through rtnetlink here; this port goes through `ip`
/// for the same reason `iptables_util.cc` goes through `iptables-restore` —
/// there is no netlink layer in this codebase.
std::string RunIp(const std::vector<std::string>& args) {
    std::vector<std::string> argv;
    argv.push_back("ip");
    for (std::size_t i = 0; i < args.size(); ++i) argv.push_back(args[i]);

    privileges::ScopedSpawnRequest request;
    request.argv = argv;
    request.capabilities.push_back(core::cap::kCapNetAdmin);

    core::Expected<pid_t, std::string> spawned = privileges::SpawnScoped(request);
    if (!spawned.ok()) return spawned.error();

    int status = 0;
    if (::waitpid(spawned.value(), &status, 0) < 0) {
        return std::string("failed to wait for ip: ") + std::strerror(errno);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::ostringstream os;
        os << "ip";
        for (std::size_t i = 0; i < args.size(); ++i) os << " " << args[i];
        os << " failed";
        if (WIFEXITED(status)) os << " (exit " << WEXITSTATUS(status) << ")";
        return os.str();
    }
    return std::string();
}

/// `echo <value> > <path>`; returns errno on failure so callers can special
/// case ENOENT.
int WriteSysctl(const std::string& path, const std::string& value) {
    FILE* f = ::fopen(path.c_str(), "w");
    if (f == NULL) return errno;
    const std::size_t written = ::fwrite(value.data(), 1, value.size(), f);
    const int         write_errno = written == value.size() ? 0 : errno;
    if (::fclose(f) != 0 && write_errno == 0) return errno;
    return write_errno;
}

}  // namespace

// ---- NetworkError ----------------------------------------------------------

NetworkError NetworkError::Namespace(const std::string& source) {
    NetworkError e;
    e.kind   = Kind::NamespaceError;
    e.detail = source;
    return e;
}

NetworkError NetworkError::HostIptables(const std::string& source) {
    NetworkError e;
    e.kind   = Kind::HostIptablesError;
    e.detail = source;
    return e;
}

NetworkError NetworkError::Io(const std::string& source) {
    NetworkError e;
    e.kind   = Kind::IoError;
    e.detail = source;
    return e;
}

NetworkError NetworkError::SlotOutOfRangeErr(uint32_t idx, uint32_t max) {
    NetworkError e;
    e.kind = Kind::SlotOutOfRange;
    e.idx  = idx;
    e.max  = max;
    return e;
}

// Reproduces the Rust `#[error(...)]` format strings.
std::string NetworkError::Message() const {
    std::ostringstream os;
    switch (kind) {
        case Kind::NamespaceError:
            os << "Namespace operation failed: " << detail;
            break;
        case Kind::HostIptablesError:
            os << "Host iptables operation failed: " << detail;
            break;
        case Kind::IoError:
            os << "IO error: " << detail;
            break;
        case Kind::SlotOutOfRange:
            os << "Slot index out of range (max " << max << "): " << idx;
            break;
    }
    return os.str();
}

// ---- DNS resolution --------------------------------------------------------

// Rust `parse_nameserver_ipv4`.
bool ParseNameserverIpv4(const std::string& contents, uint32_t* out) {
    std::istringstream stream(contents);
    std::string        line;
    while (std::getline(stream, line)) {
        // Trim both ends.
        std::size_t begin = line.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) continue;
        std::size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(begin, end - begin + 1);

        if (line.empty() || line[0] == '#') continue;

        std::istringstream parts(line);
        std::string        directive;
        if (!(parts >> directive) || directive != "nameserver") continue;

        std::string candidate;
        if (!(parts >> candidate)) continue;

        uint32_t ip = 0;
        if (!Ipv4Parse(candidate, &ip)) continue;

        // Rust skips loopback and unspecified: a stub resolver on 127.0.0.53
        // is not reachable from inside the guest's namespace.
        const bool is_loopback    = (ip >> 24) == 127u;
        const bool is_unspecified = ip == 0u;
        if (is_loopback || is_unspecified) continue;

        if (out != NULL) *out = ip;
        return true;
    }
    return false;
}

// Rust `resolve_guest_dns_server`.
uint32_t ResolveGuestDnsServer() {
    const char* const paths[] = {"/run/systemd/resolve/resolv.conf",
                                 "/etc/resolv.conf"};
    for (int i = 0; i < 2; ++i) {
        core::Expected<std::string, std::string> contents =
            core::fs::ReadToString(paths[i]);
        if (!contents.ok()) continue;
        uint32_t ip = 0;
        if (ParseNameserverIpv4(contents.value(), &ip)) return ip;
    }

    // Rust falls back to public DNS rather than leaving the guest without a
    // resolver.
    const uint32_t fallback = (8u << 24) | (8u << 16) | (8u << 8) | 8u;
    AGENTENV_WARN("falling back to public DNS " << Ipv4ToString(fallback)
                  << " for guest network");
    return fallback;
}

// ---- iptables rule set -----------------------------------------------------

// Rust `configure_namespace_iptables_rules`, rule construction only.
std::vector<IptablesRestoreCommand> BuildNamespaceIptablesRules(
    uint32_t host_interaction_ip, uint32_t veth_vm_ip, uint32_t vm_ip) {
    const std::string host_ip_s = Ipv4ToString(host_interaction_ip);
    const std::string veth_vm_s = Ipv4ToString(veth_vm_ip);
    const std::string vm_ip_s   = Ipv4ToString(vm_ip);

    std::vector<IptablesRestoreCommand> commands;

    // FORWARD: VM (tap0) out to the host/internet (vpeer).
    commands.push_back(IptablesRestoreCommand::Append(
        "filter", "FORWARD", "-i tap0 -o vpeer -j ACCEPT"));
    // FORWARD: return traffic only for flows the VM started.
    commands.push_back(IptablesRestoreCommand::Append(
        "filter", "FORWARD",
        "-i vpeer -o tap0 -m state --state RELATED,ESTABLISHED -j ACCEPT"));
    // SNAT: give the guest the slot's routable identity, so the host's own
    // MASQUERADE can take it to the internet.
    commands.push_back(IptablesRestoreCommand::Append(
        "nat", "POSTROUTING",
        "-o vpeer -s " + vm_ip_s + " -j SNAT --to " + host_ip_s));
    // The namespace-local egress proxy dials from vpeer's address, not the
    // guest's, so it needs the same identity or the host rules will not match.
    commands.push_back(IptablesRestoreCommand::Append(
        "nat", "POSTROUTING",
        "-o vpeer -s " + veth_vm_s + " -j SNAT --to " + host_ip_s));
    // DNAT: lets the host reach the guest through the slot IP.
    commands.push_back(IptablesRestoreCommand::Append(
        "nat", "PREROUTING",
        "-i vpeer -d " + host_ip_s + " -j DNAT --to " + vm_ip_s));

    return commands;
}

// Rust `tune_neigh_retrans_time_ms`.
void TuneNeighRetransTimeMs(const std::string& interface) {
    const std::string path =
        "/proc/sys/net/ipv4/neigh/" + interface + "/retrans_time_ms";

    for (int attempt = 0; attempt <= kNeighSysctlRetries; ++attempt) {
        const int err = WriteSysctl(path, kArpRetransTimeMs);
        if (err == 0) return;

        // The per-interface sysctl directory shows up slightly after the link,
        // so ENOENT is expected for the first few attempts.
        if (err == ENOENT && attempt < kNeighSysctlRetries) {
            AGENTENV_DEBUG("ARP retransmit sysctl not ready for " << interface
                           << " (" << path << "), retrying");
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kNeighSysctlRetryDelayMs));
            continue;
        }

        // Anything else is non-fatal: a slower first ARP is better than
        // failing the slot.
        AGENTENV_WARN("failed to configure ARP retransmit delay on "
                      << interface << ": " << std::strerror(err));
        return;
    }
}

// ---- Slot ------------------------------------------------------------------

std::string Slot::HostVethName(uint32_t idx) {
    std::ostringstream os;
    os << kHostVethPrefix << idx;
    return os.str();
}

Slot::Slot(uint32_t idx, const NetworkAddressPlan& address_plan,
           const std::string& netns_dir, uint32_t host_interaction_ip,
           uint32_t veth_host_ip, uint32_t veth_vm_ip)
    : idx_(idx),
      host_interaction_ip_(host_interaction_ip),
      veth_host_ip_(veth_host_ip),
      veth_vm_ip_(veth_vm_ip),
      address_plan_(address_plan),
      netns_dir_(netns_dir),
      cleanup_armed_(false),
      user_egress_rules_present_(false) {
    namespace_id_ = std::string(kNetnsPrefix) + core::Uuid::GenV7().ToString();
}

// Rust `Slot::new`.
core::Expected<Slot*, NetworkError> Slot::New(
    uint32_t idx, const NetworkAddressPlan& address_plan,
    const std::string& netns_dir) {
    // Slot 0 is reserved and the plan cannot address anything at or past the
    // configured ceiling.
    const uint32_t max = static_cast<uint32_t>(cfg::kNetworkMaxSlots);
    if (idx == 0 || idx >= max) {
        return core::make_unexpected(
            NetworkError::SlotOutOfRangeErr(idx, max - 1));
    }

    uint32_t host_interaction_ip = 0;
    uint32_t veth_host_ip        = 0;
    uint32_t veth_vm_ip          = 0;
    core::Expected<core::Unit, std::string> ips = address_plan.SlotIps(
        idx, &host_interaction_ip, &veth_host_ip, &veth_vm_ip);
    if (!ips.ok()) {
        return core::make_unexpected(NetworkError::Namespace(ips.error()));
    }

    return new Slot(idx, address_plan, netns_dir, host_interaction_ip,
                    veth_host_ip, veth_vm_ip);
}

// Rust `impl Drop for Slot` — force-sync cleanup, warning instead of throwing.
Slot::~Slot() {
    core::Expected<core::Unit, NetworkError> cleaned = Cleanup(true);
    if (!cleaned.ok()) {
        AGENTENV_WARN("slot " << idx_ << " drop cleanup failed: "
                      << cleaned.error().Message());
    }
}

std::string Slot::NamespacePath() const {
    return Join(netns_dir_, namespace_id_);
}

// Rust `build_ip_boot_arg`.
std::string Slot::BuildIpBootArg() const {
    std::ostringstream os;
    os << "ip=" << Ipv4ToString(address_plan_.VmIp()) << "::"
       << Ipv4ToString(address_plan_.TapIp()) << ":"
       << Ipv4ToString(address_plan_.VmLinkMask())
       << ":instance:eth0:off:" << Ipv4ToString(ResolveGuestDnsServer());
    return os.str();
}

// Rust `configure_namespace_interfaces`, in the `ip` dialect.
//
// Runs with the calling thread already inside the new namespace, so every
// command below lands there rather than on the host.
core::Expected<core::Unit, std::string> Slot::ConfigureNamespaceInterfaces() {
    const std::string veth_name = HostVethName(idx_);

    // veth pair: the host end is named after the slot, the namespace end is
    // always `vpeer`.
    {
        std::vector<std::string> args;
        args.push_back("link");
        args.push_back("add");
        args.push_back(veth_name);
        args.push_back("type");
        args.push_back("veth");
        args.push_back("peer");
        args.push_back("name");
        args.push_back(kVpeerName);
        const std::string err = RunIp(args);
        if (!err.empty()) {
            return core::make_unexpected("Failed to create veth pair: " + err);
        }
    }

    // Move the host end back out to the namespace we came from, addressed by
    // the pid of our own thread's original namespace holder (pid 1 is the
    // host's). Rust passes the captured host ns fd to netlink; `ip` takes a
    // pid or a named namespace, and pid 1 is always the host.
    {
        std::vector<std::string> args;
        args.push_back("link");
        args.push_back("set");
        args.push_back(veth_name);
        args.push_back("netns");
        args.push_back("1");
        const std::string err = RunIp(args);
        if (!err.empty()) {
            return core::make_unexpected("Failed to move veth to host ns: " + err);
        }
    }

    // Loopback up.
    {
        std::vector<std::string> args;
        args.push_back("link");
        args.push_back("set");
        args.push_back("lo");
        args.push_back("up");
        const std::string err = RunIp(args);
        if (!err.empty()) {
            return core::make_unexpected("Failed to set lo up: " + err);
        }
    }

    // vpeer: /31 point-to-point (RFC 3021), so no broadcast address. `ip addr
    // add` already omits it for /31, which is what Rust hand-rolls
    // `add_address_no_broadcast` to achieve over netlink.
    {
        std::vector<std::string> args;
        args.push_back("addr");
        args.push_back("add");
        args.push_back(Ipv4ToString(veth_vm_ip_) + "/31");
        args.push_back("dev");
        args.push_back(kVpeerName);
        const std::string err = RunIp(args);
        if (!err.empty()) {
            return core::make_unexpected("Failed to add address to vpeer: " + err);
        }
    }
    {
        std::vector<std::string> args;
        args.push_back("link");
        args.push_back("set");
        args.push_back(kVpeerName);
        args.push_back("up");
        const std::string err = RunIp(args);
        if (!err.empty()) {
            return core::make_unexpected("Failed to set vpeer up: " + err);
        }
    }

    // tap0 is what firecracker attaches the guest NIC to.
    {
        std::vector<std::string> args;
        args.push_back("tuntap");
        args.push_back("add");
        args.push_back(kTapName);
        args.push_back("mode");
        args.push_back("tap");
        const std::string err = RunIp(args);
        if (!err.empty()) {
            return core::make_unexpected("ip tuntap add failed: " + err);
        }
    }
    {
        std::ostringstream cidr;
        cidr << Ipv4ToString(address_plan_.TapIp()) << "/"
             << static_cast<int>(address_plan_.VmLinkPrefix());
        std::vector<std::string> args;
        args.push_back("addr");
        args.push_back("add");
        args.push_back(cidr.str());
        args.push_back("dev");
        args.push_back(kTapName);
        const std::string err = RunIp(args);
        if (!err.empty()) {
            return core::make_unexpected("Failed to add address to tap0: " + err);
        }
    }
    {
        std::vector<std::string> args;
        args.push_back("link");
        args.push_back("set");
        args.push_back(kTapName);
        args.push_back("up");
        const std::string err = RunIp(args);
        if (!err.empty()) {
            return core::make_unexpected("Failed to set tap0 up: " + err);
        }
    }

    // Default route out through the host end of the pair.
    {
        std::vector<std::string> args;
        args.push_back("route");
        args.push_back("add");
        args.push_back("default");
        args.push_back("via");
        args.push_back(Ipv4ToString(veth_host_ip_));
        args.push_back("dev");
        args.push_back(kVpeerName);
        const std::string err = RunIp(args);
        if (!err.empty()) {
            return core::make_unexpected("Failed to add default route: " + err);
        }
    }

    return core::Unit();
}

// Rust `setup_namespace_internal` — everything that must happen inside the
// new namespace.
core::Expected<core::Unit, std::string> Slot::SetupNamespaceInternal() {
    // The bind-mount target has to exist before `mount`.
    core::Expected<core::Unit, std::string> dir = core::fs::CreateDirAll(netns_dir_);
    if (!dir.ok()) {
        return core::make_unexpected(
            "Failed to create AENV network namespace directory " + netns_dir_ +
            ": " + dir.error());
    }

    const std::string netns_path = NamespacePath();
    if (!core::fs::Exists(netns_path)) {
        FILE* f = ::fopen(netns_path.c_str(), "w");
        if (f == NULL) {
            return core::make_unexpected(std::string("Failed to create netns file: ") +
                                         std::strerror(errno));
        }
        ::fclose(f);
    }

    if (::unshare(CLONE_NEWNET) != 0) {
        return core::make_unexpected(std::string("Failed to unshare(CLONE_NEWNET): ") +
                                     std::strerror(errno));
    }

    // Bind-mounting the thread's net namespace keeps it alive after this
    // thread exits, which is what makes the namespace nameable later.
    if (::mount("/proc/thread-self/ns/net", netns_path.c_str(), NULL, MS_BIND,
                NULL) != 0) {
        return core::make_unexpected(std::string("Failed to bind mount new namespace: ") +
                                     std::strerror(errno));
    }

    core::Expected<core::Unit, std::string> interfaces =
        ConfigureNamespaceInterfaces();
    if (!interfaces.ok()) return interfaces;

    // The guest's packets arrive on tap0 and have to be forwarded to vpeer.
    const int fwd = WriteSysctl("/proc/sys/net/ipv4/ip_forward", "1");
    if (fwd != 0) {
        return core::make_unexpected(
            std::string("Failed to enable IP forwarding in namespace: ") +
            std::strerror(fwd));
    }

    TuneNeighRetransTimeMs(kTapName);
    TuneNeighRetransTimeMs(kVpeerName);

    std::vector<IptablesRestoreCommand> rules = BuildNamespaceIptablesRules(
        host_interaction_ip_, veth_vm_ip_, address_plan_.VmIp());
    core::Expected<core::Unit, std::string> applied =
        ApplyIptablesCommands(rules, OpenFailurePolicy::ReturnErr());
    if (!applied.ok()) return applied;

    return InitializeNamespaceEgressChain(
        ResolveGuestDnsServer(), address_plan_.InternalEgressDeniedCidrs(),
        std::vector<std::string>());
}

// Rust `configure_host_interface_async`.
core::Expected<core::Unit, std::string> Slot::ConfigureHostInterface() {
    const std::string veth_name = HostVethName(idx_);

    // Only the link address goes on the interface; `host_interaction_ip` is a
    // routing destination, not an interface address.
    {
        std::vector<std::string> args;
        args.push_back("addr");
        args.push_back("add");
        args.push_back(Ipv4ToString(veth_host_ip_) + "/31");
        args.push_back("dev");
        args.push_back(veth_name);
        const std::string err = RunIp(args);
        if (!err.empty()) {
            return core::make_unexpected("Failed to add IP to host veth: " + err);
        }
    }
    {
        std::vector<std::string> args;
        args.push_back("link");
        args.push_back("set");
        args.push_back(veth_name);
        args.push_back("up");
        const std::string err = RunIp(args);
        if (!err.empty()) {
            return core::make_unexpected("Failed to set host veth up: " + err);
        }
    }

    // Route host traffic for the slot IP into the namespace; the namespace's
    // DNAT rule then rewrites it to the guest.
    {
        std::vector<std::string> args;
        args.push_back("route");
        args.push_back("add");
        args.push_back(Ipv4ToString(host_interaction_ip_) + "/32");
        args.push_back("via");
        args.push_back(Ipv4ToString(veth_vm_ip_));
        args.push_back("dev");
        args.push_back(veth_name);
        const std::string err = RunIp(args);
        if (!err.empty()) {
            return core::make_unexpected(
                "Failed to add route to " + Ipv4ToString(host_interaction_ip_) +
                "/32 via " + Ipv4ToString(veth_vm_ip_) + " dev " + veth_name +
                ": " + err);
        }
    }

    return core::Unit();
}

// Rust `create_network`.
core::Expected<core::Unit, NetworkError> Slot::CreateNetwork() {
    // Arm before touching anything so a mid-way failure still unwinds.
    cleanup_armed_ = true;

    // `unshare` is per-thread and irreversible for that thread, so the
    // namespace work runs on a thread of its own (Rust spawns one for the
    // same reason).
    core::Expected<core::Unit, std::string> setup =
        core::make_unexpected(std::string("network setup thread did not run"));
    std::thread worker([this, &setup]() { setup = SetupNamespaceInternal(); });
    worker.join();

    if (!setup.ok()) {
        return core::make_unexpected(NetworkError::Namespace(setup.error()));
    }

    // The host end is back in our namespace now, so configure it here.
    core::Expected<core::Unit, std::string> host = ConfigureHostInterface();
    if (!host.ok()) {
        return core::make_unexpected(NetworkError::Namespace(host.error()));
    }

    TuneNeighRetransTimeMs(HostVethName(idx_));
    return core::Unit();
}

// Rust `set_egress_policy`.
core::Expected<core::Unit, NetworkError> Slot::SetEgressPolicy(
    const core::Optional<SandboxNetworkPolicy>& policy,
    uint16_t egress_proxy_port) {
    // A warm-pool slot keeps its namespace between tenants, so "no rules
    // wanted and none present" is the only case that can skip the work.
    const bool wants_rules = policy && policy->HasRuntimeEgressRules();
    if (!wants_rules && !user_egress_rules_present_) return core::Unit();

    const std::string netns_path = NamespacePath();

    // Entering a namespace is per-thread, so this runs on a dedicated thread
    // exactly as in Rust.
    core::Expected<core::Unit, std::string> result =
        core::make_unexpected(std::string("egress policy thread did not run"));
    std::thread worker([&netns_path, &policy, egress_proxy_port, &result]() {
        const int fd = ::open(netns_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            result = core::make_unexpected(
                "failed to open network namespace " + netns_path + ": " +
                std::strerror(errno));
            return;
        }
        if (::setns(fd, CLONE_NEWNET) != 0) {
            result = core::make_unexpected(
                std::string("failed to enter sandbox network namespace: ") +
                std::strerror(errno));
            ::close(fd);
            return;
        }
        ::close(fd);
        result = SetNamespaceEgressPolicy(policy, egress_proxy_port);
    });
    worker.join();

    if (!result.ok()) {
        // Rust keeps the slot marked dirty when a cleanup apply fails, so the
        // next tenant retries the clear instead of inheriting stale rules.
        if (wants_rules) user_egress_rules_present_ = true;
        return core::make_unexpected(NetworkError::Namespace(result.error()));
    }

    user_egress_rules_present_ = wants_rules;
    return core::Unit();
}

// Rust `cleanup`.
core::Expected<core::Unit, NetworkError> Slot::Cleanup(bool /*best_effort*/) {
    // A slot that never created anything must not touch host networking.
    if (!cleanup_armed_) return core::Unit();
    cleanup_armed_ = false;

    // Deleting the host end destroys the pair.
    {
        std::vector<std::string> args;
        args.push_back("link");
        args.push_back("del");
        args.push_back(HostVethName(idx_));
        const std::string err = RunIp(args);
        // Already gone is success: cleanup has to stay idempotent.
        if (!err.empty() && core::fs::Exists("/sys/class/net/" + HostVethName(idx_))) {
            cleanup_armed_ = true;
            return core::make_unexpected(NetworkError::Namespace(err));
        }
    }

    const std::string netns_path = NamespacePath();
    if (core::fs::Exists(netns_path)) {
        // The namespace may have been bind-mounted more than once; unmount
        // until the kernel says there is nothing left.
        for (;;) {
            if (::umount(netns_path.c_str()) == 0) continue;
            if (errno == EINVAL || errno == ENOENT) break;
            cleanup_armed_ = true;
            return core::make_unexpected(NetworkError::Namespace(
                std::string("Failed to unmount netns: ") + std::strerror(errno)));
        }

        if (::unlink(netns_path.c_str()) != 0 && errno != ENOENT) {
            cleanup_armed_ = true;
            return core::make_unexpected(
                NetworkError::Io(std::strerror(errno)));
        }
    }

    return core::Unit();
}

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
