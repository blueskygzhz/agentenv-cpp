// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs :: FirecrackerPausedState
#include "agentenv/sandbox/firecracker/paused_state.h"

#include <vector>

#include "agentenv/sandbox/firecracker/sandbox.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

core::Expected<FirecrackerPausedState, std::string> FirecrackerPausedState::Decode(
    const std::string& /*artifact_root*/, const core::Json& state) {
    const core::Expected<SnapshotConfig, std::string> snapshot_config =
        SnapshotConfig::FromJson(state);
    if (!snapshot_config.ok()) {
        // Rust `.context("deserialize Firecracker paused state")`.
        return core::make_unexpected(std::string("deserialize Firecracker paused state: ") +
                                     snapshot_config.error());
    }
    const core::Expected<core::Unit, std::string> valid =
        snapshot_config.value().ValidatePersisted();
    if (!valid.ok()) {
        return core::make_unexpected(
            std::string("validate Firecracker paused state artifacts: ") + valid.error());
    }
    return FirecrackerPausedState(snapshot_config.value());
}

std::string FirecrackerPausedState::Encode() const {
    return snapshot_config_.ToJson().ToString();
}

RuntimeArtifactSet FirecrackerPausedState::RuntimeArtifacts() const {
    RuntimeArtifactSet artifacts;
    // Rust passes the whole common config; the ported helper takes the two
    // pieces because only the caller knows whether the rootfs entry is set.
    const CommonConfig& common = snapshot_config_.common;
    const core::Optional<std::string> rootfs_path =
        common.rootfs_image_config.has_value()
            ? core::Optional<std::string>(common.rootfs_image_config->image_config_path)
            : core::Optional<std::string>();
    artifacts.overlaybd_image_config_paths =
        RootfsAndExtraDriveImageConfigPaths(rootfs_path, common.extra_drives);
    return artifacts;
}

core::Optional<uint16_t> FirecrackerPausedState::ControlPlanePort() const {
    const uint16_t port = snapshot_config_.common.control_plane_port;
    if (port == 0) return core::Optional<uint16_t>();
    return core::Optional<uint16_t>(port);
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
