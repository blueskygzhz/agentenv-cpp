// SPDX-License-Identifier: MIT
// Rust: src/setup/network_capacity.rs
//
// Porting note. Upstream `check_inner` only logs; it returns nothing, and every
// sysctl path is a hard-coded `/proc/sys/...` literal, so none of it is
// reachable from a test. This port keeps the same behaviour but splits it in
// two: `Inspect` computes a report against a *caller-supplied* root, and
// `Check`/`CheckAndAdjust` are the logging wrappers that run it against `/`.
// The message strings are unchanged, since operators grep for them.
#ifndef AGENTENV_SETUP_NETWORK_CAPACITY_H_
#define AGENTENV_SETUP_NETWORK_CAPACITY_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace setup {
namespace network_capacity {

/// Rust `FORCE_SYSCTL_ENV`.
extern const char* const kForceSysctlEnv;
/// Where `InstallPersistentConfig` writes, relative to the root.
extern const char* const kSysctlConfRelativePath;

/// Rust struct `CapacityThreshold` — one kernel parameter and the value large
/// deployments need.
struct CapacityThreshold {
    /// sysctl name, e.g. `net.ipv4.neigh.default.gc_thresh3`.
    std::string name;
    /// Path under `/proc/sys`, *without* a leading `/proc/sys` prefix so it can
    /// be rerooted for testing.
    std::string relative_path;
    uint64_t recommended = 0;
    std::string reason;
    /// Rust `optional_read_failure_reason`: when set, an unreadable sysctl is
    /// expected on some kernels and is skipped instead of warned about.
    core::Optional<std::string> optional_read_failure_reason;
};

/// Rust `THRESHOLDS`, in the same order (the order shows up in the generated
/// config file and in operator-facing messages).
const std::vector<CapacityThreshold>& Thresholds();

/// What `Inspect` found. Upstream builds these three lists inside
/// `check_inner` and then logs them.
struct CapacityReport {
    /// Parameters below the recommendation, or unreadable when that is not
    /// expected.
    std::vector<std::string> warnings;
    /// `sysctl -w ...` lines an operator can paste.
    std::vector<std::string> remediation_commands;
    /// Parameters this process actually raised (only when adjusting).
    std::vector<std::string> adjusted;

    bool passed() const { return warnings.empty(); }
};

/// Rust `check_inner`, made testable.
///
/// `sysctl_root` is prepended to each threshold's relative path; pass
/// `"/proc/sys"` for the real thing. When `adjust` is set, values below the
/// recommendation are written back and re-read, exactly as upstream does.
CapacityReport Inspect(const std::string& sysctl_root, bool adjust);

/// Rust `check` — inspect `/proc/sys` read-only and log the findings.
void Check();

/// Rust `check_and_adjust` — inspect and raise what it can.
void CheckAndAdjust();

/// Rust `install_persistent_config` — writes `99-aenv.conf` so the values
/// survive a reboot. `root` is "" for the real filesystem.
core::Expected<core::Unit, std::string> InstallPersistentConfig(const std::string& root);

/// The exact body `InstallPersistentConfig` writes.
std::string PersistentConfigContent();

/// Rust `env_truthy` — only the listed spellings count as true.
bool EnvTruthy(const char* name);

/// Rust `running_in_container` — best-effort marker scan.
bool RunningInContainer();

/// Rust `should_skip_sysctl_tuning`: a container skips tuning unless the
/// operator forces it, because the sysctls it would write are the host's.
bool ShouldSkipSysctlTuning();

/// Rust `read_sysctl_u64`.
core::Expected<uint64_t, std::string> ReadSysctl(const std::string& path);

/// Rust `write_sysctl_u64`.
core::Expected<core::Unit, std::string> WriteSysctl(const std::string& path, uint64_t value);

}  // namespace network_capacity
}  // namespace setup
}  // namespace agentenv
#endif  // AGENTENV_SETUP_NETWORK_CAPACITY_H_
