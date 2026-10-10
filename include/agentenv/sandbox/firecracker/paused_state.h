// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs :: FirecrackerPausedState
//
// The backend's half of surviving a restart. The orchestrator treats a paused
// state as opaque: it stores whatever `Encode()` produces and hands it back to
// the factory on resume. For Firecracker that payload is the whole
// `SnapshotConfig`, so this type is the bridge between the persister's record
// and a resumable VM.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_PAUSED_STATE_H_
#define AGENTENV_SANDBOX_FIRECRACKER_PAUSED_STATE_H_

#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/firecracker/config.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust `struct FirecrackerPausedState`.
class FirecrackerPausedState : public PausedSandboxState {
 public:
    /// Rust `FirecrackerPausedState::new`.
    explicit FirecrackerPausedState(const SnapshotConfig& snapshot_config)
        : snapshot_config_(snapshot_config) {}

    /// Rust `FirecrackerPausedState::decode`.
    ///
    /// Deserializes, then runs `validate_persisted()` — the artifact checks
    /// that do *not* need this node's launch dependencies. A record is refused
    /// here when its own files are gone (vm_state, memory image, rootfs
    /// config), which is what stops a resume from being offered for a capture
    /// whose backing files were already collected.
    ///
    /// `artifact_root` is unused, exactly as in Rust: this backend stores
    /// absolute paths in the config rather than paths relative to the
    /// persister's directory. It stays in the signature because the trait is
    /// shared with backends that would need it.
    static core::Expected<FirecrackerPausedState, std::string> Decode(
        const std::string& artifact_root, const core::Json& state);

    /// Rust `snapshot_config()`.
    const SnapshotConfig& snapshot_config() const { return snapshot_config_; }

    // ---- PausedSandboxState ----------------------------------------------

    /// Rust `encode` — `serde_json::to_value(&self.snapshot_config)`.
    std::string Encode() const override;

    /// Rust `runtime_artifacts` — the rootfs plus every extra drive's image
    /// config. These are what the image-liveness layer must keep alive while
    /// the sandbox is paused, or a resume would reopen collected layers.
    RuntimeArtifactSet RuntimeArtifacts() const override;

    /// Rust `control_plane_port` — `(port != 0).then_some(port)`. Zero means
    /// the capture never recorded one, which is distinct from a real port.
    core::Optional<uint16_t> ControlPlanePort() const override;

 private:
    SnapshotConfig snapshot_config_;
};

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_PAUSED_STATE_H_
