// SPDX-License-Identifier: MIT
// Rust: src/setup/*.rs
#include "agentenv/setup.h"

#include <sys/stat.h>

namespace agentenv {
namespace setup {

std::vector<DependencyCheck> CheckDependencies() {
    // Minimal skeleton: only check /dev/kvm presence.
    std::vector<DependencyCheck> out;
    DependencyCheck c;
    c.name = "kvm";
    struct stat st;
    c.ok = (::stat("/dev/kvm", &st) == 0);
    c.detail = c.ok ? "/dev/kvm present" : "/dev/kvm missing";
    out.push_back(c);
    return out;
}

core::Expected<core::Unit, std::string> CheckKvm() {
    struct stat st;
    if (::stat("/dev/kvm", &st) != 0) {
        return core::make_unexpected(std::string("/dev/kvm not present"));
    }
    return core::Unit{};
}

core::Expected<uint32_t, std::string> ProbeNetworkCapacity() {
    // TODO: real probing (num TAP devices creatable, etc). Skeleton returns 0.
    return static_cast<uint32_t>(0);
}

core::Expected<core::Unit, std::string> InstallOverlaybd(const std::string&) {
    return core::Unit{};
}
core::Expected<core::Unit, std::string> InstallHostPackages() {
    return core::Unit{};
}
core::Expected<core::Unit, std::string> LoadUblkModule() {
    return core::Unit{};
}

}  // namespace setup
}  // namespace agentenv
