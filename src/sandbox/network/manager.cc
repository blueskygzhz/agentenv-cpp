// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/manager.rs
#include "agentenv/sandbox/network/manager.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "agentenv/cfg.h"
#include "agentenv/cfg/network.h"
#include "agentenv/core/capability.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/privileges.h"
#include "agentenv/sandbox/network/iptables_util.h"

namespace agentenv {
namespace sandbox {
namespace network {

const char* const kErrShuttingDown = "Network manager is shutting down";

namespace {

/// Rust `HOST_IPTABLES_INSTALLED` — process-wide, because the rules are
/// node-wide rather than per manager instance.
bool        g_host_iptables_installed = false;
std::mutex  g_host_iptables_mu;

/// Rust `MANAGER: OnceLock<NetworkManager>`.
SlotNetworkManager* g_manager = NULL;
std::mutex          g_manager_mu;

/// Rust `network_manager_exit_hook` registered through `libc::atexit`.
///
/// The global manager is a static singleton whose destructor is not guaranteed
/// to run, so without this a crash or a test binary exiting early would leak
/// every pooled slot's namespace and veth.
void NetworkManagerExitHook() {
    SlotNetworkManager* manager = SlotNetworkManager::GlobalIfInitialized();
    if (manager == NULL) return;
    core::Expected<core::Unit, std::string> done = manager->Shutdown();
    if (!done.ok()) {
        AGENTENV_WARN("network manager shutdown on process exit failed: "
                      << done.error());
    }
}

/// Runs `command` under `capabilities` and returns its stdout, or false when
/// it could not be run or exited non-zero. Rust `run_command`.
///
/// This forks directly instead of going through `privileges::SpawnScoped`:
/// that helper runs its pre-exec hook on a launcher thread in the *parent*,
/// before forking, so redirecting stdout there would rewire this process's
/// own stdout rather than the child's. The `dup2` has to happen after the
/// fork, in the child.
bool RunCommandCapturingStdout(const std::string& command,
                               const std::vector<std::string>& args,
                               const std::vector<int>& capabilities,
                               std::string* out) {
    int pipefd[2];
    if (::pipe(pipefd) != 0) return false;

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        AGENTENV_DEBUG("failed to fork for host networking inspection via "
                       << command);
        return false;
    }

    if (pid == 0) {
        // Child: wire stdout to the pipe, drop to the requested capabilities
        // and exec. Diagnostics only, so any failure just exits non-zero.
        ::close(pipefd[0]);
        if (::dup2(pipefd[1], STDOUT_FILENO) < 0) ::_exit(127);
        ::close(pipefd[1]);
        // Stderr would otherwise interleave into the caller's log.
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }

        if (!capabilities.empty()) {
            const core::Expected<core::Unit, std::string> configured =
                core::cap::ConfigureCurrentProcessCapabilities(capabilities);
            if (!configured.ok()) ::_exit(127);
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(command.c_str()));
        for (std::size_t i = 0; i < args.size(); ++i) {
            argv.push_back(const_cast<char*>(args[i].c_str()));
        }
        argv.push_back(NULL);
        ::execvp(argv[0], &argv[0]);
        ::_exit(127);
    }

    // Parent: drain the pipe, then reap. Reading before waiting avoids
    // deadlocking on a child that outruns the pipe buffer.
    ::close(pipefd[1]);
    std::string captured;
    char        buf[4096];
    ssize_t     n = 0;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
        captured.append(buf, static_cast<std::size_t>(n));
    }
    ::close(pipefd[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        AGENTENV_DEBUG("network conflict inspection command " << command
                       << " exited unsuccessfully");
        return false;
    }

    if (out != NULL) *out = captured;
    return true;
}

std::string Trim(const std::string& s) {
    const std::size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const std::size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

}  // namespace

// ---- free functions --------------------------------------------------------

// Rust `slot_index_from_host_veth_name`.
bool SlotIndexFromHostVethName(const std::string& name, uint32_t* out) {
    const std::string prefix(kHostVethPrefix);
    if (name.size() <= prefix.size()) return false;
    if (name.compare(0, prefix.size(), prefix) != 0) return false;

    const std::string digits = name.substr(prefix.size());
    if (digits.empty()) return false;
    // Rust's `parse::<usize>()` rejects anything non-numeric, including a
    // leading sign.
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (digits[i] < '0' || digits[i] > '9') return false;
    }

    const unsigned long parsed = std::strtoul(digits.c_str(), NULL, 10);
    if (parsed == 0 || parsed >= cfg::kNetworkMaxSlots) return false;

    if (out != NULL) *out = static_cast<uint32_t>(parsed);
    return true;
}

// Rust `collect_conflict_report`.
ConflictReport CollectConflictReport(const std::string& output,
                                     const std::vector<std::string>& patterns) {
    ConflictReport report;
    std::istringstream stream(output);
    std::string        raw;

    while (std::getline(stream, raw)) {
        const std::string line = Trim(raw);
        if (line.empty()) continue;

        bool matched = false;
        for (std::size_t i = 0; i < patterns.size() && !matched; ++i) {
            if (!patterns[i].empty() &&
                line.find(patterns[i]) != std::string::npos) {
                matched = true;
            }
        }
        if (!matched) continue;

        // Count every match, but keep only the first few as samples.
        ++report.total_matches;
        if (report.samples.size() < kConflictSampleLimit) {
            report.samples.push_back(line);
        }
    }
    return report;
}

// Rust `global_host_iptables_commands`.
std::vector<IptablesRestoreCommand> GlobalHostIptablesCommands(
    const Ipv4Cidr& host_interaction_cidr) {
    const std::string cidr   = host_interaction_cidr.ToString();
    const std::string iface  = std::string(kHostVethPrefix) + "+";
    std::vector<IptablesRestoreCommand> commands;

    // Guest replies to host-initiated proxy/envd connections stay reachable;
    // the REJECT below drops everything else guest-to-host. Order matters, so
    // both go in at fixed positions.
    commands.push_back(IptablesRestoreCommand::Insert(
        "filter", "INPUT", 1,
        "-i " + iface + " -s " + cidr +
            " -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT"));
    commands.push_back(IptablesRestoreCommand::Insert(
        "filter", "INPUT", 2, "-i " + iface + " -s " + cidr + " -j REJECT"));
    // The namespace SNATs to the host-interaction CIDR before the host's
    // FORWARD chain sees the packet, so a host with a DROP policy has to
    // accept that post-SNAT source range explicitly.
    commands.push_back(IptablesRestoreCommand::Append(
        "filter", "FORWARD", "-i " + iface + " -s " + cidr + " -j ACCEPT"));
    commands.push_back(IptablesRestoreCommand::Append(
        "filter", "FORWARD",
        "-o " + iface + " -d " + cidr +
            " -m state --state RELATED,ESTABLISHED -j ACCEPT"));
    commands.push_back(IptablesRestoreCommand::Append(
        "nat", "POSTROUTING", "-s " + cidr + " -j MASQUERADE"));

    return commands;
}

// Rust `global_host_iptables_delete_commands`.
std::vector<IptablesRestoreCommand> GlobalHostIptablesDeleteCommands(
    const Ipv4Cidr& host_interaction_cidr) {
    const std::string cidr  = host_interaction_cidr.ToString();
    const std::string iface = std::string(kHostVethPrefix) + "+";
    std::vector<IptablesRestoreCommand> commands;

    commands.push_back(IptablesRestoreCommand::Delete(
        "filter", "INPUT",
        "-i " + iface + " -s " + cidr +
            " -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT"));
    commands.push_back(IptablesRestoreCommand::Delete(
        "filter", "INPUT", "-i " + iface + " -s " + cidr + " -j REJECT"));
    commands.push_back(IptablesRestoreCommand::Delete(
        "filter", "FORWARD", "-i " + iface + " -s " + cidr + " -j ACCEPT"));
    commands.push_back(IptablesRestoreCommand::Delete(
        "filter", "FORWARD",
        "-o " + iface + " -d " + cidr +
            " -m state --state RELATED,ESTABLISHED -j ACCEPT"));
    commands.push_back(IptablesRestoreCommand::Delete(
        "nat", "POSTROUTING", "-s " + cidr + " -j MASQUERADE"));

    return commands;
}

// ---- SlotNetworkManager ----------------------------------------------------

// Rust `with_config`.
SlotNetworkManager::SlotNetworkManager(const NetworkManagerConfig& config)
    : pool_(config.pool),
      address_plan_(config.address_plan),
      netns_dir_(config.netns_dir),
      shutting_down_(false) {
    allocated_.assign((cfg::kNetworkMaxSlots + 63) / 64, 0);

    // Slot 0 can never be handed out: the plan's arithmetic reserves it, and
    // a zero index would produce a network address rather than a host one.
    allocated_[0] |= 1ULL;

    std::vector<uint32_t> existing = ReserveExistingHostVethSlots();
    LogExternalConflicts(existing);

    // Rust only warns here: a node without CAP_NET_ADMIN can still serve
    // already-running sandboxes, and `AllocateFreshSlot` retries the install.
    core::Expected<core::Unit, std::string> installed = InstallGlobalHostIptables();
    if (!installed.ok()) {
        AGENTENV_WARN("failed to install global host iptables rules during "
                      "network manager init: " << installed.error());
    }
}

SlotNetworkManager::~SlotNetworkManager() {
    // Rust `impl Drop for NetworkManager`.
    core::Expected<core::Unit, std::string> done = Shutdown();
    if (!done.ok()) {
        AGENTENV_WARN("network manager drop cleanup failed: " << done.error());
    }
}

SlotNetworkManager& SlotNetworkManager::Global() {
    std::lock_guard<std::mutex> lg(g_manager_mu);
    if (g_manager == NULL) {
        NetworkManagerConfig config;
        config.address_plan = NetworkAddressPlan::Default();

        // Rust reads `ConfigManager::global_config()`; fall back to defaults
        // when the process never loaded a config (tests, tools).
        const cfg::AppConfig* app = cfg::ConfigManager::GlobalConfig();
        if (app != NULL) {
            config.pool      = app->NetworkPoolConfig();
            config.netns_dir = app->runtime_path + "/netns";
        } else {
            config.netns_dir = "/run/aenv/netns";
        }
        g_manager = new SlotNetworkManager(config);

        // Registered after construction so the hook never observes a
        // half-built manager.
        if (std::atexit(NetworkManagerExitHook) != 0) {
            AGENTENV_WARN("failed to register network manager process-exit hook");
        }
    }
    return *g_manager;
}

SlotNetworkManager* SlotNetworkManager::GlobalIfInitialized() {
    std::lock_guard<std::mutex> lg(g_manager_mu);
    return g_manager;
}

bool SlotNetworkManager::ShuttingDown() const {
    std::lock_guard<std::mutex> lg(mu_);
    return shutting_down_;
}

bool SlotNetworkManager::IsAllocated(uint32_t idx) const {
    std::lock_guard<std::mutex> lg(mu_);
    if (idx >= cfg::kNetworkMaxSlots) return false;
    return (allocated_[idx / 64] & (1ULL << (idx % 64))) != 0;
}

warmpool::PoolMaintenanceAction
SlotNetworkManager::ComputeMaintenanceAction(std::size_t pool_len) const {
    return pool_.ComputeMaintenanceAction(pool_len);
}

// Rust `allocate_slot`.
core::Expected<Slot*, std::string> SlotNetworkManager::AllocateSlot(uint32_t idx) {
    const uint32_t max = static_cast<uint32_t>(cfg::kNetworkMaxSlots);
    if (idx == 0 || idx >= max) {
        std::ostringstream os;
        os << "Slot index " << idx << " out of range (max " << (max - 1) << ")";
        return core::make_unexpected(os.str());
    }

    {
        std::lock_guard<std::mutex> lg(mu_);
        const uint64_t bit = 1ULL << (idx % 64);
        if ((allocated_[idx / 64] & bit) != 0) {
            std::ostringstream os;
            os << "Slot " << idx << " already allocated";
            return core::make_unexpected(os.str());
        }
        allocated_[idx / 64] |= bit;
    }

    core::Expected<Slot*, NetworkError> slot =
        Slot::New(idx, address_plan_, netns_dir_);
    if (!slot.ok()) {
        // The index was just validated, so this should not happen; give the
        // bit back rather than leaking it.
        ReleaseSlotBit(idx);
        return core::make_unexpected(slot.error().Message());
    }
    return slot.value();
}

// Rust `release_slot_bit`.
core::Expected<core::Unit, std::string>
SlotNetworkManager::ReleaseSlotBit(uint32_t idx) {
    const uint32_t max = static_cast<uint32_t>(cfg::kNetworkMaxSlots);
    if (idx == 0 || idx >= max) {
        std::ostringstream os;
        os << "Slot index " << idx << " out of range";
        return core::make_unexpected(os.str());
    }

    std::lock_guard<std::mutex> lg(mu_);
    const uint64_t bit = 1ULL << (idx % 64);
    if ((allocated_[idx / 64] & bit) == 0) {
        std::ostringstream os;
        os << "Slot " << idx << " not allocated";
        return core::make_unexpected(os.str());
    }
    allocated_[idx / 64] &= ~bit;
    return core::Unit();
}

// Rust `cleanup_slot_and_release_bit_inner`.
core::Expected<core::Unit, std::string>
SlotNetworkManager::CleanupSlotAndReleaseBit(Slot* slot, bool sync_cleanup) {
    if (slot == NULL) return core::Unit();

    const uint32_t idx = slot->Idx();
    core::Expected<core::Unit, NetworkError> cleanup = slot->Cleanup(sync_cleanup);
    // The destructor would try again; deleting here makes ownership explicit.
    delete slot;
    core::Expected<core::Unit, std::string> bit = ReleaseSlotBit(idx);

    if (cleanup.ok() && bit.ok()) return core::Unit();
    if (!cleanup.ok() && bit.ok()) {
        return core::make_unexpected(cleanup.error().Message());
    }
    if (cleanup.ok() && !bit.ok()) return core::make_unexpected(bit.error());

    // Both failed: report the bitmap error, since a leaked bit permanently
    // costs capacity while a leaked device is visible and reclaimable.
    AGENTENV_WARN("network slot cleanup failed alongside bitset release error: "
                  << cleanup.error().Message());
    return core::make_unexpected(bit.error());
}

core::Expected<core::Unit, std::string>
SlotNetworkManager::CleanupAllocatedSlot(Slot* slot, bool sync_cleanup) {
    return CleanupSlotAndReleaseBit(slot, sync_cleanup);
}

// Rust `allocate_any`.
core::Expected<Slot*, std::string> SlotNetworkManager::AllocateAny() {
    if (ShuttingDown()) return core::make_unexpected(std::string(kErrShuttingDown));

    // Fast path: a pooled slot already has its namespace and veth.
    Slot* warm = NULL;
    if (pool_.TryAcquire(&warm)) {
        // Shutdown may have started while we were dequeuing.
        if (ShuttingDown()) {
            CleanupSlotAndReleaseBit(warm, false);
            return core::make_unexpected(std::string(kErrShuttingDown));
        }
        AGENTENV_DEBUG("reused warm network slot " << warm->Idx() << " from pool");
        if (pool_.Len() < pool_.Config().low_watermark) {
            pool_.RequestMaintenance();
        }
        return warm;
    }

    return AllocateFreshSlot();
}

// Rust `allocate_fresh_slot`.
core::Expected<Slot*, std::string> SlotNetworkManager::AllocateFreshSlot() {
    if (ShuttingDown()) return core::make_unexpected(std::string(kErrShuttingDown));

    // The node-wide rules must exist before any slot carries traffic. Retried
    // here because the install at construction may have lacked privileges.
    {
        std::lock_guard<std::mutex> lg(g_host_iptables_mu);
        if (!g_host_iptables_installed) {
            core::Expected<core::Unit, std::string> installed =
                InstallGlobalHostIptables();
            if (!installed.ok()) {
                return core::make_unexpected(
                    NetworkError::HostIptables(installed.error()).Message());
            }
        }
    }

    // Take the next free index.
    uint32_t idx = 0;
    {
        std::lock_guard<std::mutex> lg(mu_);
        bool found = false;
        for (std::size_t word = 0; word < allocated_.size() && !found; ++word) {
            if (allocated_[word] == ~0ULL) continue;
            for (int bit = 0; bit < 64; ++bit) {
                const uint32_t candidate =
                    static_cast<uint32_t>(word * 64 + static_cast<std::size_t>(bit));
                if (candidate >= cfg::kNetworkMaxSlots) break;
                if ((allocated_[word] & (1ULL << bit)) == 0) {
                    allocated_[word] |= (1ULL << bit);
                    idx   = candidate;
                    found = true;
                    break;
                }
            }
        }
        if (!found) return core::make_unexpected(std::string("No available slots"));
    }

    core::Expected<Slot*, NetworkError> slot =
        Slot::New(idx, address_plan_, netns_dir_);
    if (!slot.ok()) {
        ReleaseSlotBit(idx);
        return core::make_unexpected(slot.error().Message());
    }

    core::Expected<core::Unit, NetworkError> created = slot.value()->CreateNetwork();
    if (!created.ok()) {
        delete slot.value();
        ReleaseSlotBit(idx);
        return core::make_unexpected("Failed to create network: " +
                                     created.error().Message());
    }

    // Shutdown may have started during setup; do not hand out a slot the
    // caller could never release.
    if (ShuttingDown()) {
        core::Expected<core::Unit, std::string> cleaned =
            CleanupSlotAndReleaseBit(slot.value(), false);
        if (!cleaned.ok()) {
            std::ostringstream os;
            os << "Network manager is shutting down and failed to cleanup newly "
                  "allocated slot " << idx << ": " << cleaned.error();
            return core::make_unexpected(os.str());
        }
        return core::make_unexpected(std::string(kErrShuttingDown));
    }

    return slot.value();
}

// Rust `release`.
core::Expected<core::Unit, std::string> SlotNetworkManager::Release(Slot* slot) {
    if (slot == NULL) return core::Unit();
    if (ShuttingDown()) return CleanupSlotAndReleaseBit(slot, false);

    const uint32_t idx = slot->Idx();
    if (pool_.Release(slot)) {
        AGENTENV_DEBUG("returned network slot " << idx << " to pool (pool_len="
                       << pool_.Len() << ")");
        return core::Unit();
    }
    // Pool full and maintenance disabled: tear it down now.
    return CleanupSlotAndReleaseBit(slot, false);
}

// Rust `run_pool_maintenance_cycle`.
core::Expected<core::Unit, std::string>
SlotNetworkManager::RunPoolMaintenanceCycle() {
    const warmpool::PoolMaintenanceAction action =
        pool_.ComputeMaintenanceAction(pool_.Len());

    if (action.kind == warmpool::PoolMaintenanceKind::Fill) {
        for (std::size_t i = 0; i < action.count; ++i) {
            if (ShuttingDown()) break;

            core::Expected<Slot*, std::string> slot = AllocateFreshSlot();
            if (!slot.ok()) {
                // Refill is opportunistic: stop at the first failure rather
                // than hammering a node that cannot build slots right now.
                AGENTENV_DEBUG("skipping pool refill attempt: " << slot.error());
                break;
            }

            const uint32_t idx = slot.value()->Idx();
            if (pool_.TryPushBounded(slot.value())) {
                AGENTENV_DEBUG("refilled warm network slot " << idx
                               << " (pool_len=" << pool_.Len() << ")");
            } else {
                core::Expected<core::Unit, std::string> cleaned =
                    CleanupSlotAndReleaseBit(slot.value(), false);
                if (!cleaned.ok()) return cleaned;
            }
        }
    } else if (action.kind == warmpool::PoolMaintenanceKind::Drain) {
        for (std::size_t i = 0; i < action.count; ++i) {
            Slot* slot = NULL;
            if (!pool_.TryDrainOne(&slot)) break;
            const uint32_t idx = slot->Idx();
            core::Expected<core::Unit, std::string> cleaned =
                CleanupSlotAndReleaseBit(slot, false);
            if (!cleaned.ok()) return cleaned;
            AGENTENV_DEBUG("drained excess warm network slot " << idx << " from pool");
        }
    }

    return core::Unit();
}

// Rust `shutdown_inner`.
core::Expected<core::Unit, std::string> SlotNetworkManager::Shutdown() {
    {
        std::lock_guard<std::mutex> lg(mu_);
        shutting_down_ = true;
    }

    std::vector<Slot*> drained = pool_.DrainAll();
    std::vector<std::string> failures;

    if (!drained.empty()) {
        AGENTENV_DEBUG("draining " << drained.size()
                       << " warm network slots during shutdown");
        for (std::size_t i = 0; i < drained.size(); ++i) {
            const uint32_t idx = drained[i]->Idx();
            // Sync cleanup: at process exit there is no runtime left to defer to.
            core::Expected<core::Unit, std::string> cleaned =
                CleanupSlotAndReleaseBit(drained[i], true);
            if (!cleaned.ok()) {
                std::ostringstream os;
                os << "slot " << idx << " cleanup failed: " << cleaned.error();
                failures.push_back(os.str());
            }
        }
    }

    CleanupGlobalHostIptables();

    if (failures.empty()) return core::Unit();

    std::ostringstream os;
    os << "failed to clean up pooled network slots during shutdown: ";
    for (std::size_t i = 0; i < failures.size(); ++i) {
        if (i != 0) os << " | ";
        os << failures[i];
    }
    return core::make_unexpected(os.str());
}

// Rust `reserve_existing_host_veth_slots`.
std::vector<uint32_t> SlotNetworkManager::ReserveExistingHostVethSlots() {
    std::vector<uint32_t> reserved;

    core::Expected<std::vector<std::string>, std::string> entries =
        core::fs::ReadDir("/sys/class/net");
    if (!entries.ok()) {
        AGENTENV_WARN("failed to scan host interfaces /sys/class/net: "
                      << entries.error());
        return reserved;
    }

    for (std::size_t i = 0; i < entries.value().size(); ++i) {
        uint32_t idx = 0;
        if (!SlotIndexFromHostVethName(entries.value()[i], &idx)) continue;

        {
            std::lock_guard<std::mutex> lg(mu_);
            allocated_[idx / 64] |= (1ULL << (idx % 64));
        }
        reserved.push_back(idx);
    }

    std::sort(reserved.begin(), reserved.end());
    reserved.erase(std::unique(reserved.begin(), reserved.end()), reserved.end());
    return reserved;
}

// Rust `check_conflicts`.
void SlotNetworkManager::CheckConflicts(
    const std::string& source, const std::string& command,
    const std::vector<std::string>& args, const std::vector<int>& capabilities,
    const std::vector<std::string>& patterns, const std::string& message) {
    std::string output;
    if (!RunCommandCapturingStdout(command, args, capabilities, &output)) return;

    const ConflictReport report = CollectConflictReport(output, patterns);
    if (report.total_matches == 0) return;

    std::ostringstream os;
    os << message << " (source=" << source
       << ", match_count=" << report.total_matches << ", samples=[";
    for (std::size_t i = 0; i < report.samples.size(); ++i) {
        if (i != 0) os << "; ";
        os << report.samples[i];
    }
    os << "])";
    AGENTENV_WARN(os.str());
}

// Rust `log_external_conflicts`.
void SlotNetworkManager::LogExternalConflicts(
    const std::vector<uint32_t>& existing_slots) {
    if (!existing_slots.empty()) {
        std::ostringstream os;
        os << "detected " << existing_slots.size()
           << " existing host veth interfaces using AgentENV slot naming; "
              "another sandbox runtime may already be active or previous "
              "cleanup may have left conflicting devices behind (sample slots=[";
        const std::size_t limit =
            std::min(existing_slots.size(), kConflictSampleLimit);
        for (std::size_t i = 0; i < limit; ++i) {
            if (i != 0) os << ", ";
            os << existing_slots[i];
        }
        os << "])";
        AGENTENV_WARN(os.str());
    }

    const std::vector<std::string> range_patterns = address_plan_.ConflictPatterns();

    std::vector<std::string> addr_args;
    addr_args.push_back("-o");
    addr_args.push_back("addr");
    addr_args.push_back("show");
    CheckConflicts("host interface addresses", "ip", addr_args,
                   std::vector<int>(), range_patterns,
                   "detected host interface addresses overlapping AgentENV "
                   "network ranges; another program may already be using the "
                   "same address space");

    std::vector<std::string> route_args;
    route_args.push_back("-o");
    route_args.push_back("route");
    route_args.push_back("show");
    CheckConflicts("host routes", "ip", route_args, std::vector<int>(),
                   range_patterns,
                   "detected host routes overlapping AgentENV network ranges; "
                   "another program may already be using the same address space");

    // iptables-save needs CAP_NET_ADMIN, unlike the two `ip` queries above.
    std::vector<std::string> firewall_patterns;
    firewall_patterns.push_back(std::string(kHostVethPrefix));
    for (std::size_t i = 0; i < range_patterns.size(); ++i) {
        firewall_patterns.push_back(range_patterns[i]);
    }
    std::vector<int> caps;
    caps.push_back(core::cap::kCapNetAdmin);
    CheckConflicts("iptables-save", "iptables-save", std::vector<std::string>(),
                   caps, firewall_patterns,
                   "detected firewall rules referencing AgentENV interfaces or "
                   "network ranges; another program may already be managing "
                   "sandbox networking");
}

// Rust `install_global_host_iptables`.
core::Expected<core::Unit, std::string>
SlotNetworkManager::InstallGlobalHostIptables() {
    // Caller may already hold g_host_iptables_mu (AllocateFreshSlot does), so
    // the flag is checked and set without re-locking here.
    if (g_host_iptables_installed) return core::Unit();
    g_host_iptables_installed = true;

    std::vector<IptablesRestoreCommand> commands =
        GlobalHostIptablesCommands(address_plan_.HostInteractionCidr());

    core::Expected<core::Unit, std::string> applied =
        ApplyIptablesCommands(commands, OpenFailurePolicy::ReturnErr());
    if (!applied.ok()) {
        // Clear the flag so the next allocation retries.
        g_host_iptables_installed = false;
        return applied;
    }
    AGENTENV_DEBUG("installed global host iptables rules for sandbox networking");
    return core::Unit();
}

// Rust `cleanup_global_host_iptables`.
void SlotNetworkManager::CleanupGlobalHostIptables() {
    {
        std::lock_guard<std::mutex> lg(g_host_iptables_mu);
        if (!g_host_iptables_installed) return;
        g_host_iptables_installed = false;
    }

    std::vector<IptablesRestoreCommand> commands =
        GlobalHostIptablesDeleteCommands(address_plan_.HostInteractionCidr());

    // WarnAndIgnore: one externally removed rule must not abort the loop and
    // leak every rule after it.
    core::Expected<core::Unit, std::string> applied = ApplyIptablesCommands(
        commands,
        OpenFailurePolicy::WarnAndIgnore("failed to open iptables for cleanup"));
    if (!applied.ok()) {
        AGENTENV_WARN("failed to cleanup global host iptables rules: "
                      << applied.error());
    }
}

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
