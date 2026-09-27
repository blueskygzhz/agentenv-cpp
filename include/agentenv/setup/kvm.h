// SPDX-License-Identifier: MIT
// Rust: src/setup/kvm.rs
#ifndef AGENTENV_SETUP_KVM_H_
#define AGENTENV_SETUP_KVM_H_

#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/virtualization.h"

namespace agentenv {
namespace setup {
namespace kvm {

/// Rust `add_user_to_group`. Returns whether the user was actually added;
/// already being a member is success with `false`, not an error.
core::Expected<bool, std::string> AddUserToGroup(const std::string& user,
                                                 const std::string& group);

/// Rust `check` — validates the mode against the host, then requires
/// `/dev/kvm` to exist and be readable *and* writable.
core::Expected<core::Unit, std::string> Check(core::VirtualizationMode mode);

/// Rust `validate_mode`, exposed because it is the unit-testable core: it
/// takes the architecture and module state as parameters rather than reading
/// the host, which is exactly how the upstream tests drive it.
core::Expected<core::Unit, std::string> ValidateMode(core::VirtualizationMode mode,
                                                     const std::string& arch, bool pvm_loaded);

/// The architecture this binary was compiled for. Rust
/// `std::env::consts::ARCH`.
const char* HostArch();

}  // namespace kvm
}  // namespace setup
}  // namespace agentenv
#endif  // AGENTENV_SETUP_KVM_H_
