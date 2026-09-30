// SPDX-License-Identifier: MIT
// Rust: src/sandbox/ublk/device.rs — `UblkConfig` / `UblkBackend` /
// `OverlaybdConfig`, the description of one attached ublk device.
//
// Not to be confused with `storage::overlaybd::OverlaybdConfig`, which is an
// image's *layer stack* (its config.json), or with `storage::ublk::UblkDev`,
// which is the kernel device driver and legitimately owns the queue geometry.
// This header is only the per-device backend description the sandbox layer
// hands to the daemon.
#ifndef AGENTENV_SANDBOX_UBLK_H_
#define AGENTENV_SANDBOX_UBLK_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace sandbox {
namespace ublk {

/// Rust struct `OverlaybdConfig` (the ublk backend variant's payload).
struct OverlaybdDeviceConfig {
    std::string image_config_path;
    bool        read_only = false;
    /// Which writable-upper format the *runtime* layer uses. Distinct from the
    /// mode recorded in a committed image, so a node can run a different
    /// upper format than the one the image was built with.
    storage::overlaybd::UpperMode runtime_upper_mode =
        storage::overlaybd::UpperMode::LogStructured;

    bool operator==(const OverlaybdDeviceConfig& o) const {
        return image_config_path == o.image_config_path && read_only == o.read_only &&
               runtime_upper_mode == o.runtime_upper_mode;
    }
    bool operator!=(const OverlaybdDeviceConfig& o) const { return !(*this == o); }
};

/// Rust enum `UblkBackend`.
///
/// Only one variant exists today. The legacy `Cow` backend was removed and its
/// persisted form is explicitly rejected on load (a sandbox captured under it
/// cannot be resumed and must be rebuilt), which is why this stays an enum
/// rather than collapsing into the struct.
enum class UblkBackendKind {
    Overlaybd,
};

/// Rust struct `UblkConfig`.
struct UblkConfig {
    UblkBackendKind       backend = UblkBackendKind::Overlaybd;
    OverlaybdDeviceConfig overlaybd;

    /// Rust `UblkConfig::overlaybd_with_runtime_upper_mode`.
    static UblkConfig OverlaybdWithRuntimeUpperMode(
        const std::string& image_config_path, bool read_only,
        storage::overlaybd::UpperMode runtime_upper_mode);

    bool operator==(const UblkConfig& o) const {
        return backend == o.backend && overlaybd == o.overlaybd;
    }
    bool operator!=(const UblkConfig& o) const { return !(*this == o); }
};

/// Rust struct `UblkDaemonConfig` — how to spawn `uvm-ublk-daemon`.
struct UblkDaemonConfig {
    std::string daemon_bin;
    std::string log_dir;
    int32_t     shutdown_timeout_ms = 5000;
};

/// Rust trait `UblkDeviceManager`.
class DeviceManager {
 public:
    virtual ~DeviceManager() {}
    virtual core::Expected<std::string, std::string> Attach(const UblkConfig& cfg) = 0;
    virtual core::Expected<core::Unit, std::string> Detach(const std::string& dev_path) = 0;
};

}  // namespace ublk
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_UBLK_H_
