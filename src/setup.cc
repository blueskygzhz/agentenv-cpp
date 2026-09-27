// SPDX-License-Identifier: MIT
// Rust: src/setup/mod.rs
#include "agentenv/setup.h"

#include <cerrno>
#include <cstring>
#include <sstream>

#include <grp.h>
#include <pwd.h>
#include <sys/resource.h>
#include <unistd.h>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/storage/ublk/ctrl.h"

namespace agentenv {
namespace setup {
namespace {

using core::Unit;
namespace fs = core::fs;

const char* const kIpForwardPath = "/proc/sys/net/ipv4/ip_forward";
/// Rust's target for `RLIMIT_NOFILE`.
const uint64_t kDesiredNoFile = 65536;

EnvironmentCheck Ok(const std::string& step) {
    EnvironmentCheck check;
    check.step = step;
    check.ok = true;
    return check;
}

EnvironmentCheck Failed(const std::string& step, const std::string& detail) {
    EnvironmentCheck check;
    check.step = step;
    check.ok = false;
    check.detail = detail;
    return check;
}

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

}  // namespace

bool IsValidRuntimeAccountName(const std::string& name) {
    if (name.empty()) return false;

    // First byte: lowercase letter or underscore. A leading digit or hyphen is
    // rejected because some tools would parse it as an option or a uid.
    const unsigned char first = static_cast<unsigned char>(name[0]);
    const bool first_ok = (first >= 'a' && first <= 'z') || first == '_';
    if (!first_ok) return false;

    for (std::size_t i = 1; i < name.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(name[i]);
        const bool ok = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
                        byte == '_' || byte == '-';
        if (!ok) return false;
    }
    return true;
}

core::Expected<RuntimeAccount, std::string> ValidateRuntimeAccount(
    const std::string& runtime_user, const std::string& runtime_group) {
    if (!IsValidRuntimeAccountName(runtime_user) ||
        !IsValidRuntimeAccountName(runtime_group)) {
        std::ostringstream oss;
        oss << "runtime user \"" << runtime_user << "\" or group \"" << runtime_group
            << "\" is invalid (must match [a-z_][a-z0-9_-]*)";
        return core::make_unexpected(oss.str());
    }

    errno = 0;
    const struct passwd* user = ::getpwnam(runtime_user.c_str());
    if (user == NULL) {
        std::ostringstream oss;
        if (errno != 0) {
            oss << "look up runtime user " << runtime_user << ": " << std::strerror(errno);
        } else {
            oss << "runtime user does not exist: " << runtime_user;
        }
        return core::make_unexpected(oss.str());
    }
    if (user->pw_uid == 0) {
        return core::make_unexpected(std::string("runtime user must be non-root"));
    }

    errno = 0;
    const struct group* grp = ::getgrnam(runtime_group.c_str());
    if (grp == NULL) {
        std::ostringstream oss;
        if (errno != 0) {
            oss << "look up runtime group " << runtime_group << ": " << std::strerror(errno);
        } else {
            oss << "runtime group does not exist: " << runtime_group;
        }
        return core::make_unexpected(oss.str());
    }
    if (grp->gr_gid == 0) {
        return core::make_unexpected(std::string("runtime group must be non-root"));
    }

    RuntimeAccount account;
    account.uid = static_cast<uint32_t>(user->pw_uid);
    account.gid = static_cast<uint32_t>(grp->gr_gid);
    return account;
}

core::Expected<RuntimeAccount, std::string> CheckHostProvisioningPreconditions(
    const std::string& runtime_user, const std::string& runtime_group) {
    if (!fs::EffectiveUidIsRoot()) {
        return core::make_unexpected(
            std::string("host provisioning requires root; rerun with sudo"));
    }
    return ValidateRuntimeAccount(runtime_user, runtime_group);
}

core::Expected<Unit, std::string> EnableIpForwarding() {
    const core::Expected<Unit, std::string> written = fs::Write(kIpForwardPath, "1\n");
    if (!written.ok()) {
        return core::make_unexpected(std::string("enable host IPv4 forwarding: ") +
                                     written.error());
    }
    return Unit();
}

core::Expected<Unit, std::string> CheckIpForwarding() {
    const core::Expected<std::string, std::string> raw = fs::ReadToString(kIpForwardPath);
    if (!raw.ok()) {
        return core::make_unexpected(std::string("read host IPv4 forwarding setting: ") +
                                     raw.error());
    }
    if (Trim(raw.value()) != "1") {
        return core::make_unexpected(
            std::string("host IPv4 forwarding is disabled; run `server --setup-host` as root"));
    }
    return Unit();
}

core::Expected<uint64_t, std::string> RaiseFileDescriptorLimit() {
    struct rlimit limit;
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        return core::make_unexpected(std::string("get rlimit: ") + std::strerror(errno));
    }

    const uint64_t hard = static_cast<uint64_t>(limit.rlim_max);
    if (static_cast<uint64_t>(limit.rlim_cur) >= kDesiredNoFile) {
        return static_cast<uint64_t>(limit.rlim_cur);
    }

    // Never ask for more than the hard limit: that would fail outright and
    // lose the raise we could have had.
    struct rlimit raised;
    raised.rlim_cur = static_cast<rlim_t>(kDesiredNoFile < hard ? kDesiredNoFile : hard);
    raised.rlim_max = limit.rlim_max;
    if (::setrlimit(RLIMIT_NOFILE, &raised) != 0) {
        return core::make_unexpected(std::string("set rlimit: ") + std::strerror(errno));
    }
    return static_cast<uint64_t>(raised.rlim_cur);
}

std::vector<EnvironmentCheck> CheckEnvironment(core::VirtualizationMode mode) {
    std::vector<EnvironmentCheck> checks;

    // 2. Selected virtualization mode and /dev/kvm access.
    AGENTENV_INFO("checking virtualization availability virtualization_mode="
                  << core::VirtualizationModeToString(mode));
    const core::Expected<Unit, std::string> kvm_check = kvm::Check(mode);
    checks.push_back(kvm_check.ok() ? Ok("kvm") : Failed("kvm", kvm_check.error()));

    // 3. ublk module and permissions.
    AGENTENV_INFO("checking ublk module and permissions");
    const core::Expected<Unit, std::string> ublk_check = ublk::Check();
    checks.push_back(ublk_check.ok() ? Ok("ublk") : Failed("ublk", ublk_check.error()));

    // 6. Host networking is provisioned separately and only validated here.
    const core::Expected<Unit, std::string> forwarding = CheckIpForwarding();
    checks.push_back(forwarding.ok() ? Ok("ip_forward")
                                     : Failed("ip_forward", forwarding.error()));

    return checks;
}

core::Expected<Unit, std::string> EnsureHost(const std::string& deps_path,
                                             const std::string& runtime_user,
                                             const std::string& runtime_group) {
    const core::Expected<RuntimeAccount, std::string> account =
        CheckHostProvisioningPreconditions(runtime_user, runtime_group);
    if (!account.ok()) return core::make_unexpected(account.error());

    // Group membership only takes effect on a new session, so the operator is
    // warned rather than left with a runtime that silently cannot open /dev/kvm.
    const core::Expected<bool, std::string> added_runtime =
        kvm::AddUserToGroup(runtime_user, runtime_group);
    if (!added_runtime.ok()) return core::make_unexpected(added_runtime.error());
    if (added_runtime.value()) {
        AGENTENV_WARN("runtime user added to the configured runtime group; restart its session "
                      "before starting AENV manually user="
                      << runtime_user << " group=" << runtime_group);
    }

    const core::Expected<bool, std::string> added_kvm =
        kvm::AddUserToGroup(runtime_user, "kvm");
    if (!added_kvm.ok()) return core::make_unexpected(added_kvm.error());
    if (added_kvm.value()) {
        AGENTENV_WARN("runtime user added to the kvm group; restart its session before starting "
                      "AENV manually user="
                      << runtime_user);
    }

    const core::Expected<Unit, std::string> provisioned = ublk::Provision(runtime_group);
    if (!provisioned.ok()) return provisioned;

    // Rust also installs the overlaybd system default config here; that step
    // lives in setup/overlaybd.rs and is not ported yet. `deps_path` is
    // accepted now so the signature does not have to change later.
    (void)deps_path;

    const core::Expected<Unit, std::string> sysctl =
        network_capacity::InstallPersistentConfig(std::string());
    if (!sysctl.ok()) {
        return core::make_unexpected(std::string("install /etc/sysctl.d/99-aenv.conf: ") +
                                     sysctl.error());
    }

    const core::Expected<Unit, std::string> forwarding = EnableIpForwarding();
    if (!forwarding.ok()) return forwarding;

    network_capacity::CheckAndAdjust();
    return Unit();
}

}  // namespace setup
}  // namespace agentenv
