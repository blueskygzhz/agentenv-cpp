// SPDX-License-Identifier: MIT
// Rust: src/setup/mod.rs — the orchestration layer over the individual
// host-provisioning steps.
//
// The steps themselves live in the per-file headers under `agentenv/setup/`,
// mirroring the Rust module layout.
#ifndef AGENTENV_SETUP_H_
#define AGENTENV_SETUP_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/virtualization.h"
#include "agentenv/setup/kvm.h"
#include "agentenv/setup/network_capacity.h"
#include "agentenv/setup/ublk.h"

namespace agentenv {
namespace setup {

/// Rust `is_valid_runtime_account_name` — the installer's own validation:
/// `[a-z_][a-z0-9_-]*`. Deliberately stricter than what the OS would accept,
/// so a name that survives here is safe to interpolate into a udev rule or a
/// `usermod` argument.
bool IsValidRuntimeAccountName(const std::string& name);

/// A resolved runtime account. Rust returns just the `Gid`, but the uid is
/// carried too because `validate_runtime_account` checks both for root.
struct RuntimeAccount {
    uint32_t uid = 0;
    uint32_t gid = 0;
};

/// Rust `validate_runtime_account` — both names must be valid, must exist, and
/// must be non-root. AgentENV never runs sandboxes as root, so a root runtime
/// account is a configuration error rather than a warning.
core::Expected<RuntimeAccount, std::string> ValidateRuntimeAccount(
    const std::string& runtime_user, const std::string& runtime_group);

/// Rust `ensure_host`'s preconditions, split out so they can be checked
/// without performing the (root-only, side-effecting) provisioning itself.
core::Expected<RuntimeAccount, std::string> CheckHostProvisioningPreconditions(
    const std::string& runtime_user, const std::string& runtime_group);

/// Rust `ensure_host` — provisions machine-wide prerequisites for the runtime
/// account. Requires root.
///
/// Intentionally separate from the `--setup-only` provisioning path, which
/// also runs on unprivileged builders while assembling release artifacts.
core::Expected<core::Unit, std::string> EnsureHost(const std::string& deps_path,
                                                   const std::string& runtime_user,
                                                   const std::string& runtime_group);

/// One step's outcome in `EnsureEnvironment`. Rust fails fast with `?`; this
/// reports which step failed so the caller can log it precisely.
struct EnvironmentCheck {
    std::string step;
    bool ok = false;
    std::string detail;
};

/// Rust `ensure_environment`, steps 1-3 and 6 — the validations that do not
/// download anything. Runs every check and reports all of them rather than
/// stopping at the first failure, which is what makes it useful as a
/// diagnostic. `RaiseFileDescriptorLimit` and the dependency download are
/// separate calls.
std::vector<EnvironmentCheck> CheckEnvironment(core::VirtualizationMode mode);

/// Rust `ensure_environment` step 5 — raises `RLIMIT_NOFILE` towards 65536
/// without exceeding the hard limit. Returns the soft limit now in effect.
core::Expected<uint64_t, std::string> RaiseFileDescriptorLimit();

/// Rust `ensure_environment` step 6 — host IPv4 forwarding must already be on;
/// this path only validates it, since enabling it belongs to `EnsureHost`.
core::Expected<core::Unit, std::string> CheckIpForwarding();

/// Enables host IPv4 forwarding. Part of `ensure_host`, hence root-only.
core::Expected<core::Unit, std::string> EnableIpForwarding();

}  // namespace setup
}  // namespace agentenv
#endif  // AGENTENV_SETUP_H_
