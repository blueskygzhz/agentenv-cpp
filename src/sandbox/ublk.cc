// SPDX-License-Identifier: MIT
// Rust: src/sandbox/ublk/device.rs
#include "agentenv/sandbox/ublk.h"

namespace agentenv {
namespace sandbox {
namespace ublk {

UblkConfig UblkConfig::OverlaybdWithRuntimeUpperMode(
    const std::string& image_config_path, bool read_only,
    storage::overlaybd::UpperMode runtime_upper_mode) {
    UblkConfig config;
    config.backend                     = UblkBackendKind::Overlaybd;
    config.overlaybd.image_config_path = image_config_path;
    config.overlaybd.read_only         = read_only;
    config.overlaybd.runtime_upper_mode = runtime_upper_mode;
    return config;
}

}  // namespace ublk
}  // namespace sandbox
}  // namespace agentenv
