// SPDX-License-Identifier: MIT
// Rust: src/setup/{mod,deps,kvm,network_capacity,overlaybd,packages,ublk}.rs
// Host prerequisite / diagnostic checks that `agentenv setup` runs on install.
#ifndef AGENTENV_SETUP_H_
#define AGENTENV_SETUP_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace setup {

/// Rust: setup/deps.rs
struct DependencyCheck {
    std::string name;
    bool        ok = false;
    std::string detail;
};

std::vector<DependencyCheck> CheckDependencies();

/// Rust: setup/kvm.rs
core::Expected<core::Unit, std::string> CheckKvm();

/// Rust: setup/network_capacity.rs
core::Expected<uint32_t, std::string> ProbeNetworkCapacity();

/// Rust: setup/overlaybd.rs
core::Expected<core::Unit, std::string> InstallOverlaybd(const std::string& target_dir);

/// Rust: setup/packages.rs
core::Expected<core::Unit, std::string> InstallHostPackages();

/// Rust: setup/ublk.rs
core::Expected<core::Unit, std::string> LoadUblkModule();

}  // namespace setup
}  // namespace agentenv
#endif  // AGENTENV_SETUP_H_
