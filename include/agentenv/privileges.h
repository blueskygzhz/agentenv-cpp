// SPDX-License-Identifier: MIT
// Rust: src/privileges.rs — capability/uid preflight checks.
#ifndef AGENTENV_PRIVILEGES_H_
#define AGENTENV_PRIVILEGES_H_

#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace privileges {

/// Rust fn: verify the process has all required Linux capabilities.
core::Expected<core::Unit, std::string> CheckRequired();

/// Rust fn: return true iff running as uid 0.
bool IsRoot();

}  // namespace privileges
}  // namespace agentenv
#endif  // AGENTENV_PRIVILEGES_H_
