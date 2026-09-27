// SPDX-License-Identifier: MIT
// Rust: src/sandbox/ublk/{mod,device,overlaybd}.rs
#ifndef AGENTENV_SANDBOX_UBLK_H_
#define AGENTENV_SANDBOX_UBLK_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace sandbox {
namespace ublk {

/// Rust struct `UblkConfig`.
struct UblkConfig {
    int32_t queue_depth = 128;
    int32_t nr_hw_queues = 1;
    std::string dev_path;
};

/// Rust struct `UblkDaemonConfig`.
struct UblkDaemonConfig {
    std::string daemon_bin;
    std::string log_dir;
    int32_t     shutdown_timeout_ms = 5000;
};

/// Rust struct `OverlaybdConfig`.
struct OverlaybdConfig {
    std::string overlaybd_config_path;
    std::string data_dir;
    bool        enable_lsmt = true;
};

/// Rust trait `UblkDeviceManager`.
class DeviceManager {
 public:
    virtual ~DeviceManager() {}
    virtual core::Expected<std::string, std::string>
        Attach(const UblkConfig& cfg) = 0;
    virtual core::Expected<core::Unit, std::string>
        Detach(const std::string& dev_path) = 0;
};

/// Rust trait `UblkBackend`.
class UblkBackend {
 public:
    virtual ~UblkBackend() {}
    virtual core::Expected<std::string, std::string>
        Mount(const OverlaybdConfig& cfg) = 0;
    virtual core::Expected<core::Unit, std::string>
        Umount(const std::string& mount_path) = 0;
};

}  // namespace ublk
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_UBLK_H_
