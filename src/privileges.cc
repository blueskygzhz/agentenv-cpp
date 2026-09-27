// SPDX-License-Identifier: MIT
// Rust: src/privileges.rs
#include "agentenv/privileges.h"

#include <unistd.h>

namespace agentenv {
namespace privileges {

bool IsRoot() {
    return ::geteuid() == 0;
}

core::Expected<core::Unit, std::string> CheckRequired() {
    // TODO: verify CAP_NET_ADMIN, CAP_SYS_ADMIN, etc.
    return core::Unit{};
}

}  // namespace privileges
}  // namespace agentenv
