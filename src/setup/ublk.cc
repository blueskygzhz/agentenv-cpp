// SPDX-License-Identifier: MIT
// Rust: src/setup/ublk.rs
#include "agentenv/setup/ublk.h"

#include <sstream>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/process.h"
#include "agentenv/storage/ublk/ctrl.h"

namespace agentenv {
namespace setup {
namespace ublk {

const char* const kUdevRulesDir = "/etc/udev/rules.d";
const char* const kUdevRulePath = "/etc/udev/rules.d/99-agentenv-ublk.rules";
const char* const kModulesLoadPath = "/etc/modules-load.d/aenv-ublk.conf";

namespace {

using core::Unit;
namespace process = core::process;

/// Rust `reload_udev`.
core::Expected<Unit, std::string> ReloadUdev() {
    if (!process::Exists("udevadm")) {
        AGENTENV_WARN(
            "udevadm not found; ublk permissions will apply after udev reloads rules");
        return Unit();
    }

    std::vector<std::string> reload;
    reload.push_back("udevadm");
    reload.push_back("control");
    reload.push_back("--reload-rules");
    const core::Expected<int, std::string> status = process::Status(reload);
    if (!status.ok()) {
        return core::make_unexpected(std::string("reload udev rules: ") + status.error());
    }
    if (status.value() != 0) {
        std::ostringstream oss;
        oss << "udevadm control --reload-rules failed with exit status: " << status.value();
        return core::make_unexpected(oss.str());
    }

    // The triggers below are best-effort in Rust too (`.ok()`): the rules are
    // already installed, so a failure here only delays the permission change
    // until udev next re-reads them.
    std::vector<std::string> trigger_control;
    trigger_control.push_back("udevadm");
    trigger_control.push_back("trigger");
    trigger_control.push_back("--subsystem-match=misc");
    trigger_control.push_back("--sysname-match=ublk-control");
    process::Status(trigger_control);

    const char* const patterns[] = {"ublkc*", "ublkb*"};
    for (std::size_t i = 0; i < 2; ++i) {
        std::vector<std::string> trigger;
        trigger.push_back("udevadm");
        trigger.push_back("trigger");
        trigger.push_back(std::string("--sysname-match=") + patterns[i]);
        process::Status(trigger);
    }

    std::vector<std::string> settle;
    settle.push_back("udevadm");
    settle.push_back("settle");
    process::Status(settle);
    return Unit();
}

/// Rust `install_apt_extra_kernel_modules` — wholly best-effort: on a host
/// without apt, or without a matching package, setup continues and the module
/// load below is what actually decides success.
void InstallAptExtraKernelModules() {
    if (!process::Exists("apt-get")) return;

    std::vector<std::string> uname_argv;
    uname_argv.push_back("uname");
    uname_argv.push_back("-r");
    const core::Expected<process::Output, std::string> uname = process::Run(uname_argv);
    if (!uname.ok()) return;

    const std::string release = process::TrimmedStdout(uname.value());
    if (release.empty()) return;

    std::vector<std::string> install;
    install.push_back("apt-get");
    install.push_back("install");
    install.push_back("-y");
    install.push_back("linux-modules-extra-" + release);

    const core::Expected<int, std::string> status = process::Status(install);
    if (!status.ok() || status.value() != 0) {
        AGENTENV_WARN("extra kernel modules not found via apt for this kernel");
    }
}

}  // namespace

std::string UdevRuleContent(const std::string& group) {
    std::ostringstream oss;
    oss << "# Managed by agentenv server setup\n"
        << "KERNEL==\"ublk-control\", MODE=\"0660\", GROUP=\"" << group << "\"\n"
        << "KERNEL==\"ublkc*\", MODE=\"0660\", GROUP=\"" << group << "\"\n"
        << "KERNEL==\"ublkb*\", MODE=\"0660\", GROUP=\"" << group << "\"\n";
    return oss.str();
}

core::Expected<Unit, std::string> WriteUdevRule(const std::string& rules_dir,
                                                const std::string& group) {
    // Rust `supports_persistent_udev_rules`: a host without the directory is
    // not an error, it just cannot persist the rule.
    if (!core::fs::Exists(rules_dir)) {
        AGENTENV_WARN("udev rules directory not present; skipping persistent ublk rule install "
                      "rules_dir="
                      << rules_dir);
        return Unit();
    }

    const std::string path = core::fs::Join(rules_dir, "99-agentenv-ublk.rules");
    const core::Expected<Unit, std::string> written =
        core::fs::Write(path, UdevRuleContent(group));
    if (!written.ok()) {
        return core::make_unexpected(std::string("install ") + path + ": " + written.error());
    }
    return Unit();
}

core::Expected<Unit, std::string> Provision(const std::string& group) {
    // Only try apt when the module is genuinely missing; on most hosts it is
    // already present and the package does not exist.
    if (!storage::ublk::UblkModuleLoaded()) {
        InstallAptExtraKernelModules();
    }

    const core::Expected<Unit, std::string> loaded = storage::ublk::LoadUblkModule();
    if (!loaded.ok()) return loaded;

    const core::Optional<std::string> modules_dir = core::fs::Parent(kModulesLoadPath);
    if (modules_dir.has_value()) {
        const core::Expected<Unit, std::string> made = core::fs::CreateDirAll(*modules_dir);
        if (!made.ok()) {
            return core::make_unexpected(std::string("create ") + *modules_dir + ": " +
                                         made.error());
        }
    }
    const core::Expected<Unit, std::string> pinned =
        core::fs::Write(kModulesLoadPath, "ublk_drv\n");
    if (!pinned.ok()) {
        return core::make_unexpected(std::string("install ") + kModulesLoadPath + ": " +
                                     pinned.error());
    }

    AGENTENV_INFO("installing ublk device access rules group=" << group);
    const core::Expected<Unit, std::string> rule = WriteUdevRule(kUdevRulesDir, group);
    if (!rule.ok()) return rule;
    return ReloadUdev();
}

core::Expected<Unit, std::string> Check() {
    if (!storage::ublk::UblkModuleLoaded()) {
        return core::make_unexpected(
            std::string("ublk_drv is not loaded; run `server --setup-host` as root"));
    }

    // Read/write, because the control device is used to create devices, not
    // just to inspect them.
    const int fd = ::open("/dev/ublk-control", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return core::make_unexpected(
            std::string("open /dev/ublk-control for read/write access failed"));
    }
    ::close(fd);
    return Unit();
}

}  // namespace ublk
}  // namespace setup
}  // namespace agentenv
