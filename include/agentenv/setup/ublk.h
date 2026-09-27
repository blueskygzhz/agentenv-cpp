// SPDX-License-Identifier: MIT
// Rust: src/setup/ublk.rs
#ifndef AGENTENV_SETUP_UBLK_H_
#define AGENTENV_SETUP_UBLK_H_

#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace setup {
namespace ublk {

/// Rust `UDEV_RULES_DIR`.
extern const char* const kUdevRulesDir;
/// Where `write_udev_rule` installs its rule.
extern const char* const kUdevRulePath;
/// Where `provision` pins the module for the next boot.
extern const char* const kModulesLoadPath;

/// Rust `provision` — loads `ublk_drv`, pins it for the next boot, and grants
/// `group` access to the control and block devices.
///
/// Requires root; intended for `server --setup-host`.
core::Expected<core::Unit, std::string> Provision(const std::string& group);

/// Rust `check` — the module must be loaded *and* `/dev/ublk-control` open-able
/// for read/write.
core::Expected<core::Unit, std::string> Check();

/// Rust `write_udev_rule`, exposed for testing against a temporary rules
/// directory. Skips silently when `rules_dir` is absent, mirroring upstream's
/// behaviour on hosts without udev.
core::Expected<core::Unit, std::string> WriteUdevRule(const std::string& rules_dir,
                                                      const std::string& group);

/// Rust's rule body, exposed so a test can assert the exact content that lands
/// in `/etc/udev/rules.d`.
std::string UdevRuleContent(const std::string& group);

}  // namespace ublk
}  // namespace setup
}  // namespace agentenv
#endif  // AGENTENV_SETUP_UBLK_H_
