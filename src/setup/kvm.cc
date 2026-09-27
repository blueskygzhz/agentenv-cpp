// SPDX-License-Identifier: MIT
// Rust: src/setup/kvm.rs
#include "agentenv/setup/kvm.h"

#include <sstream>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/process.h"

namespace agentenv {
namespace setup {
namespace kvm {
namespace {

using core::Unit;
using core::VirtualizationMode;
namespace process = core::process;

/// Rust `kvm_accessible` — the check is read *and* write: a sandbox cannot be
/// launched with a read-only handle, so O_RDWR is the real requirement.
bool KvmAccessible() {
    const int fd = ::open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

}  // namespace

const char* HostArch() {
    // Rust `std::env::consts::ARCH`. Only the two architectures AgentENV
    // supports are distinguished; anything else reports the compiler's name so
    // the error message stays informative.
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#else
    return "unknown";
#endif
}

core::Expected<bool, std::string> AddUserToGroup(const std::string& user,
                                                 const std::string& group) {
    std::vector<std::string> id_argv;
    id_argv.push_back("id");
    id_argv.push_back("-nG");
    id_argv.push_back(user);

    const core::Expected<process::Output, std::string> groups = process::Run(id_argv);
    if (!groups.ok()) {
        std::ostringstream oss;
        oss << "look up groups for " << user << ": " << groups.error();
        return core::make_unexpected(oss.str());
    }
    if (!groups.value().success()) {
        std::ostringstream oss;
        oss << "failed to look up groups for " << user;
        return core::make_unexpected(oss.str());
    }

    // `id -nG` prints a whitespace-separated list; membership means there is
    // nothing to do.
    std::istringstream stream(groups.value().stdout_text);
    std::string current;
    while (stream >> current) {
        if (current == group) return false;
    }

    std::vector<std::string> usermod_argv;
    usermod_argv.push_back("usermod");
    usermod_argv.push_back("-aG");
    usermod_argv.push_back(group);
    usermod_argv.push_back(user);

    const core::Expected<int, std::string> status = process::Status(usermod_argv);
    if (!status.ok()) {
        return core::make_unexpected(std::string("failed to run usermod: ") + status.error());
    }
    if (status.value() != 0) {
        std::ostringstream oss;
        oss << "failed to add " << user << " to the " << group << " group";
        return core::make_unexpected(oss.str());
    }
    return true;
}

core::Expected<Unit, std::string> ValidateMode(VirtualizationMode mode, const std::string& arch,
                                               bool pvm_loaded) {
    if (mode == VirtualizationMode::Pvm && arch != "x86_64") {
        return core::make_unexpected(
            std::string("PVM virtualization mode is only supported on x86_64 hosts"));
    }

    // The two modes are mutually exclusive with the host module state: KVM
    // cannot share a host with kvm_pvm, and PVM cannot run without it.
    if (mode == VirtualizationMode::Kvm && pvm_loaded) {
        return core::make_unexpected(
            std::string("KVM mode cannot start while the kvm_pvm module is loaded; configure "
                        "virtualization_mode = \"pvm\" or unload kvm_pvm"));
    }
    if (mode == VirtualizationMode::Pvm && !pvm_loaded) {
        return core::make_unexpected(
            std::string("PVM mode requires the kvm_pvm host module to be loaded"));
    }
    return Unit();
}

core::Expected<Unit, std::string> Check(VirtualizationMode mode) {
    const core::Expected<Unit, std::string> valid =
        ValidateMode(mode, HostArch(), core::fs::Exists("/sys/module/kvm_pvm"));
    if (!valid.ok()) return valid;

    if (!core::fs::Exists("/dev/kvm")) {
        std::ostringstream oss;
        oss << "KVM device not found (/dev/kvm). Ensure the host virtualization module for "
            << core::VirtualizationModeToString(mode)
            << " is loaded and exposes /dev/kvm (see the deployment documentation for more "
               "information)";
        return core::make_unexpected(oss.str());
    }

    if (KvmAccessible()) {
        AGENTENV_INFO("/dev/kvm is accessible virtualization_mode="
                      << core::VirtualizationModeToString(mode));
        return Unit();
    }
    return core::make_unexpected(
        std::string("/dev/kvm is not accessible for read/write; add the runtime user to the kvm "
                    "group and restart its session"));
}

}  // namespace kvm
}  // namespace setup
}  // namespace agentenv
