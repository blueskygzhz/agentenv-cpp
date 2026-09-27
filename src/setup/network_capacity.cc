// SPDX-License-Identifier: MIT
// Rust: src/setup/network_capacity.rs
#include "agentenv/setup/network_capacity.h"

#include <cstdlib>
#include <sstream>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"

namespace agentenv {
namespace setup {
namespace network_capacity {

const char* const kForceSysctlEnv = "AENV_FORCE_SYSCTL_TUNING";
const char* const kSysctlConfRelativePath = "etc/sysctl.d/99-aenv.conf";

namespace {

using core::Optional;
using core::Unit;
namespace fs = core::fs;

CapacityThreshold Make(const char* name, const char* relative_path, uint64_t recommended,
                       const char* reason, const char* optional_reason) {
    CapacityThreshold threshold;
    threshold.name = name;
    threshold.relative_path = relative_path;
    threshold.recommended = recommended;
    threshold.reason = reason;
    if (optional_reason != NULL) threshold.optional_read_failure_reason = std::string(optional_reason);
    return threshold;
}

std::string Join(const std::vector<std::string>& lines, const char* separator) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) oss << separator;
        oss << lines[i];
    }
    return oss.str();
}

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

/// Rust `format!("sysctl -w {}={}", name, recommended)`.
std::string RemediationCommand(const CapacityThreshold& threshold) {
    std::ostringstream oss;
    oss << "sysctl -w " << threshold.name << "=" << threshold.recommended;
    return oss.str();
}

/// Rust's "below recommended" warning line.
std::string BelowWarning(const CapacityThreshold& threshold, uint64_t current) {
    std::ostringstream oss;
    oss << threshold.name << ": current=" << current
        << ", recommended=" << threshold.recommended << " — " << threshold.reason;
    return oss.str();
}

/// Rust `adjust_threshold`. Returns an empty string on success, or the warning
/// to record. Appends to `adjusted` / `remediation_commands` as upstream does.
std::string AdjustThreshold(const CapacityThreshold& threshold, const std::string& path,
                            uint64_t current, std::vector<std::string>* adjusted,
                            std::vector<std::string>* remediation_commands) {
    const core::Expected<Unit, std::string> written = WriteSysctl(path, threshold.recommended);
    if (!written.ok()) {
        remediation_commands->push_back(RemediationCommand(threshold));
        std::ostringstream oss;
        oss << threshold.name << ": current=" << current
            << ", recommended=" << threshold.recommended << " — " << threshold.reason
            << " (automatic adjustment failed: " << written.error() << ")";
        return oss.str();
    }

    // Re-read: some kernels clamp a written value, so a successful write is
    // not proof that the parameter actually reached the recommendation.
    const core::Expected<uint64_t, std::string> updated = ReadSysctl(path);
    if (!updated.ok()) {
        // Unreadable after writing: trust the write and say so. Upstream logs
        // this at debug and still counts it as adjusted.
        std::ostringstream line;
        line << threshold.name << ": " << current << " -> " << threshold.recommended << " ("
             << threshold.reason << ")";
        adjusted->push_back(line.str());
        AGENTENV_DEBUG("unable to re-read kernel capacity threshold after automatic adjustment "
                       "sysctl="
                       << threshold.name << " path=" << path << " error=" << updated.error());
        return std::string();
    }

    if (updated.value() >= threshold.recommended) {
        std::ostringstream line;
        line << threshold.name << ": " << current << " -> " << updated.value() << " ("
             << threshold.reason << ")";
        adjusted->push_back(line.str());
        return std::string();
    }

    remediation_commands->push_back(RemediationCommand(threshold));
    std::ostringstream oss;
    oss << threshold.name << ": attempted to set recommended value, but current="
        << updated.value() << " remains below recommended=" << threshold.recommended << " — "
        << threshold.reason;
    return oss.str();
}

void LogReport(const CapacityReport& report) {
    if (!report.adjusted.empty()) {
        AGENTENV_INFO("automatically adjusted kernel parameters for large-scale deployments:\n  "
                      << Join(report.adjusted, "\n  "));
    }
    if (report.warnings.empty()) {
        AGENTENV_INFO("kernel networking capacity checks passed");
        return;
    }
    AGENTENV_WARN("kernel parameters below recommended thresholds for large-scale deployments:\n  "
                  << Join(report.warnings, "\n  "));
    if (!report.remediation_commands.empty()) {
        AGENTENV_WARN("to fix the low kernel parameters, run as root:\n  "
                      << Join(report.remediation_commands, "\n  "));
    }
}

/// Shared body of `Check` / `CheckAndAdjust`: the two container/procfs guards
/// that make tuning meaningless, then the inspection itself.
void CheckInner(bool adjust) {
    AGENTENV_INFO("checking kernel networking capacity for large-scale deployments");

    if (ShouldSkipSysctlTuning()) {
        AGENTENV_WARN("skipping automatic sysctl tuning inside a container; configure these host "
                      "kernel parameters on the host, or set "
                      << kForceSysctlEnv
                      << "=1 only for a privileged container with writable host sysctls");
        return;
    }

    // Rust `proc_sys_readable`. Treated separately from the container check
    // because cgroup v2 containers often expose only "0::/" in /proc/1/cgroup.
    if (!fs::ReadDir("/proc/sys").ok()) {
        AGENTENV_WARN("skipping automatic sysctl tuning because /proc/sys is not readable; mount "
                      "procfs or configure the host kernel parameters manually");
        return;
    }

    LogReport(Inspect("/proc/sys", adjust));
}

}  // namespace

const std::vector<CapacityThreshold>& Thresholds() {
    // Function-local static: initialised on first use, so there is no
    // cross-translation-unit initialisation order hazard.
    static const std::vector<CapacityThreshold>* const kThresholds = []() {
        std::vector<CapacityThreshold>* out = new std::vector<CapacityThreshold>();
        out->push_back(Make("net.ipv4.neigh.default.gc_thresh3",
                            "net/ipv4/neigh/default/gc_thresh3", 16384,
                            "ARP table GC threshold 3", NULL));
        out->push_back(Make("net.ipv4.neigh.default.gc_thresh2",
                            "net/ipv4/neigh/default/gc_thresh2", 8192,
                            "ARP table GC threshold 2", NULL));
        out->push_back(Make("net.ipv4.neigh.default.gc_thresh1",
                            "net/ipv4/neigh/default/gc_thresh1", 4096,
                            "ARP table GC threshold 1", NULL));
        out->push_back(Make("net.netfilter.nf_conntrack_max",
                            "net/netfilter/nf_conntrack_max", 1048576,
                            "connection tracking table for thousands of sandboxes",
                            "nf_conntrack may not be loaded until conntrack is used"));
        out->push_back(Make("kernel.pid_max", "kernel/pid_max", 4194304,
                            "process ID exhaustion with 10k+ sandboxes", NULL));
        out->push_back(Make("fs.inotify.max_user_instances",
                            "fs/inotify/max_user_instances", 8192,
                            "envd/firecracker inotify watches", NULL));
        return out;
    }();
    return *kThresholds;
}

core::Expected<uint64_t, std::string> ReadSysctl(const std::string& path) {
    const core::Expected<std::string, std::string> raw = fs::ReadToString(path);
    if (!raw.ok()) return core::make_unexpected(raw.error());

    const std::string text = Trim(raw.value());
    if (text.empty()) return core::make_unexpected(std::string("parse failed"));

    uint64_t value = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return core::make_unexpected(std::string("parse failed"));
        }
        const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
        // Overflow would wrap and could make a low value look acceptable.
        if (value > (UINT64_MAX - digit) / 10) {
            return core::make_unexpected(std::string("parse failed"));
        }
        value = value * 10 + digit;
    }
    return value;
}

core::Expected<Unit, std::string> WriteSysctl(const std::string& path, uint64_t value) {
    std::ostringstream oss;
    oss << value << "\n";
    return fs::Write(path, oss.str());
}

bool EnvTruthy(const char* name) {
    const char* raw = ::getenv(name);
    if (raw == NULL) return false;
    const std::string value = raw;
    // Rust matches an explicit list; "0"/"false"/anything else is false.
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" ||
           value == "YES" || value == "on" || value == "ON";
}

bool RunningInContainer() {
    if (fs::Exists("/.dockerenv")) return true;

    const core::Expected<std::string, std::string> cgroup =
        fs::ReadToString("/proc/1/cgroup");
    if (!cgroup.ok()) return false;

    // Best-effort cgroup v1 / container-runtime marker scan. Some cgroup v2
    // environments expose only "0::/" here, so an unreadable /proc/sys is
    // treated as a separate skip signal by the caller.
    const char* const markers[] = {"/docker/", "/kubepods/", "/containerd/", "/podman/"};
    for (std::size_t i = 0; i < 4; ++i) {
        if (cgroup.value().find(markers[i]) != std::string::npos) return true;
    }
    return false;
}

bool ShouldSkipSysctlTuning() {
    if (EnvTruthy(kForceSysctlEnv)) return false;
    return RunningInContainer();
}

CapacityReport Inspect(const std::string& sysctl_root, bool adjust) {
    CapacityReport report;
    const std::vector<CapacityThreshold>& thresholds = Thresholds();

    for (std::size_t i = 0; i < thresholds.size(); ++i) {
        const CapacityThreshold& threshold = thresholds[i];
        const std::string path = fs::Join(sysctl_root, threshold.relative_path);

        const core::Expected<uint64_t, std::string> current = ReadSysctl(path);
        if (!current.ok()) {
            if (threshold.optional_read_failure_reason.has_value()) {
                // Expected on kernels where the module is not loaded yet.
                AGENTENV_DEBUG("skipping optional kernel capacity threshold sysctl="
                               << threshold.name << " path=" << path
                               << " error=" << current.error()
                               << " reason=" << *threshold.optional_read_failure_reason);
                continue;
            }
            std::ostringstream oss;
            oss << threshold.name << ": unable to read (path: " << path << ")";
            report.warnings.push_back(oss.str());
            continue;
        }

        if (current.value() >= threshold.recommended) continue;

        if (adjust) {
            const std::string warning = AdjustThreshold(threshold, path, current.value(),
                                                        &report.adjusted,
                                                        &report.remediation_commands);
            if (!warning.empty()) report.warnings.push_back(warning);
        } else {
            report.remediation_commands.push_back(RemediationCommand(threshold));
            report.warnings.push_back(BelowWarning(threshold, current.value()));
        }
    }
    return report;
}

void Check() { CheckInner(false); }

void CheckAndAdjust() { CheckInner(true); }

std::string PersistentConfigContent() {
    std::ostringstream oss;
    oss << "# Managed by AENV host setup\n" << "net.ipv4.ip_forward = 1\n";
    const std::vector<CapacityThreshold>& thresholds = Thresholds();
    for (std::size_t i = 0; i < thresholds.size(); ++i) {
        oss << thresholds[i].name << " = " << thresholds[i].recommended << "\n";
    }
    return oss.str();
}

core::Expected<Unit, std::string> InstallPersistentConfig(const std::string& root) {
    const std::string path = root.empty() ? std::string("/") + kSysctlConfRelativePath
                                          : fs::Join(root, kSysctlConfRelativePath);
    const Optional<std::string> parent = fs::Parent(path);
    if (parent.has_value()) {
        const core::Expected<Unit, std::string> made = fs::CreateDirAll(*parent);
        if (!made.ok()) return made;
    }
    return fs::Write(path, PersistentConfigContent());
}

}  // namespace network_capacity
}  // namespace setup
}  // namespace agentenv
