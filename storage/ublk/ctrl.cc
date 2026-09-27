// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/ctrl.rs
#include "agentenv/storage/ublk/ctrl.h"
#include "agentenv/storage/ublk/caps.h"

#include <sys/stat.h>

namespace agentenv {
namespace storage {
namespace ublk {

bool UblkAvailable() {
    struct stat st;
    return ::stat("/dev/ublk-control", &st) == 0;
}

bool UblkModuleLoaded() {
    struct stat st;
  return ::stat("/sys/module/ublk_drv", &st) == 0;
}

core::Expected<core::Unit, std::string> LoadUblkModule() {
    // TODO: modprobe ublk_drv. Skeleton returns error if not present.
    if (UblkModuleLoaded()) return core::Unit{};
    return core::make_unexpected(std::string("ublk_drv not loaded (modprobe required)"));
}

core::Expected<std::unique_ptr<UblkCtrl>, std::string>
UblkCtrlBuilder::Build() {
  return core::make_unexpected(std::string("ublk ctrl requires AGENTENV_WITH_LIBURING"));
}

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
