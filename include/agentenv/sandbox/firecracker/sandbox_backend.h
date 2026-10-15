// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs — the `impl SandboxBackend for
// FirecrackerSandbox` block (pause / resume / stop and their recovery rule).
//
// `SandboxBackend` is the interface the orchestrator actually drives, and the
// one whose signatures match the Rust trait: `Pause` yields a
// `PausedSandboxState` rather than nothing, and reports a
// `SandboxCaptureError` whose variant tells the caller whether the sandbox is
// still safe to run. Until now it had no implementation at all — the
// Firecracker port implemented only the legacy `Backend` shim, whose
// `Pause(SandboxId) -> Unit` cannot express either of those.
//
// Adding to the shim instead would have meant hard-coding the answer to the
// question the variant exists to answer, so this implements the real trait.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_SANDBOX_BACKEND_H_
#define AGENTENV_SANDBOX_FIRECRACKER_SANDBOX_BACKEND_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/envd.h"
#include "agentenv/sandbox/firecracker/instance.h"
#include "agentenv/sandbox/firecracker/dirty_memory.h"
#include "agentenv/sandbox/firecracker/paused_state.h"
#include "agentenv/sandbox/firecracker/sandbox.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust `publish_memory_overlaybd_layer` — writes the captured guest memory
/// out as a sealed LSMT layer.
///
/// Built on `storage::overlaybd::lsmt::CompactTo`, with the live VMM's address
/// space as the source: a `CompactSource` backed by `ProcessVmReader`, so the
/// bytes are pulled straight out of the paused VM with `process_vm_readv`
/// rather than staged through a file.
///
/// Rust writes to `<path>.tmp` and renames on success, removing the temp file
/// on failure. Reproduced exactly: the final name is what the resume path
/// loads, so it must never exist in a half-written state — a reader cannot
/// tell a truncated commit file from a short one.
core::Expected<core::Unit, std::string> PublishMemoryOverlaybdLayer(
    const std::string& output_path,
    const std::vector<storage::overlaybd::lsmt::SegmentMapping>& mappings,
    uint64_t virtual_size, int64_t firecracker_pid);

/// Overrides the memory-layer write. Null means use
/// [`PublishMemoryOverlaybdLayer`].
typedef std::function<core::Expected<core::Unit, std::string>(
    const std::string& data_path,
    const std::vector<storage::overlaybd::lsmt::SegmentMapping>& mappings,
    uint64_t memory_size, int64_t firecracker_pid)>
    MemoryLayerWriter;

/// Rust `FirecrackerSandbox::pause_to_dir` — capture a paused VM's state into
/// a caller-supplied directory.
///
/// The ordering is the substance here, and three rules in it are load-bearing:
///
///   * **Writable volumes are flushed before the VM is paused.** The flush runs
///     `busybox sync` *inside the guest* through envd, and envd cannot be
///     reached once the VM is paused. Reversing these two silently captures
///     volumes with dirty page cache, so the resumed sandbox sees a filesystem
///     that is stale rather than merely old.
///   * **The pid is read before the state snapshot is taken.** Guest memory is
///     pulled out of the live VMM with `process_vm_readv`, so the capture needs
///     a pid that is still valid; reading it later would race a VMM that died
///     during the snapshot and attribute the failure to the wrong step.
///   * **A failed capture removes the directory it was writing into.** A
///     half-written snapshot directory is indistinguishable from a complete one
///     by name, and the resume path would later load a `vm_state.bin` whose
///     memory layer was never finished.
struct PauseToDirSpec {
    /// Destination directory. Created by this call, removed again on failure.
    std::string snapshot_dir;
    /// Rust `has_writable_persistent_volumes` — true when any extra drive is
    /// writable *and* has a snapshot output directory.
    bool has_writable_persistent_volumes = false;
    /// Runs `busybox sync` in the guest. Required when the flag above is set;
    /// Rust fails with "cannot flush writable volumes because envd is not
    /// running" when envd is absent, rather than skipping the flush.
    std::function<core::Expected<core::Unit, std::string>()> flush_volumes;
    /// Overrides the memory-layer write. Null uses the real one.
    MemoryLayerWriter write_memory_layer;

    /// Invoked with the VMM pid the moment it is read, before the state
    /// snapshot is requested. Observation only -- it exists so the ordering
    /// rule above can be asserted, since the pid's *value* is identical
    /// whether it is read before or after and therefore proves nothing.
    std::function<void(int64_t)> on_pid_read;
};

/// The artifacts a successful capture produced.
struct PauseToDirResult {
    /// `{snapshot_dir}/vm_state.bin`.
    std::string vm_state_path;
    /// `{snapshot_dir}/mem_overlaybd/overlaybd.commit`.
    std::string memory_layer_path;
    /// Total size of the contiguous guest-memory image.
    uint64_t memory_size = 0;
};

/// Rust `enum LaunchMode { Fresh(..), Resume(..) }`.
///
/// Modelled explicitly even though only `Fresh` is implemented. `start_nowait`
/// dispatches on it in Rust, and collapsing it to "there is only fresh" would
/// make the Resume arm an untyped afterthought later — the two paths differ in
/// what they send the VMM, not just in their inputs.
struct LaunchMode {
    enum class Kind {
        Fresh,
        Resume,
    };
    Kind kind = Kind::Fresh;
};

/// What a fresh start needs. Separate from `ConfigurePlan`, which describes the
/// API calls: this is the process-level half (which binary, where it works,
/// how long to wait).
struct FreshStartSpec {
    /// Already-validated plan for the pre-boot API calls.
    ConfigurePlan configure;
    std::string firecracker_bin;
    /// Parent for the per-instance work directory.
    std::string chroot_base_dir;
    core::SandboxId sandbox_id;
    bool capture_output = true;
    int64_t socket_timeout_ms = 10 * 1000;
    int64_t socket_poll_ms = 10;
    /// Rust `RuntimePolicy::{envd_timeout, envd_poll_interval}`.
    int64_t envd_timeout_ms = 60 * 1000;
    int64_t envd_poll_ms = 100;
};

/// Rust `FirecrackerSandbox::start_fresh` — the process half of a fresh boot.
///
/// Spawns the VMM, waits for its API socket, applies `spec.configure` and
/// issues `InstanceStart`. Returns the live instance; on any failure the VMM is
/// stopped before returning, so a caller never receives an error alongside a
/// running Firecracker.
///
/// Shared by `FirecrackerSandboxBackend::StartNowait` and the legacy
/// `FirecrackerBackend::Boot`, which previously each had their own copy of this
/// ordering. Two copies of a sequence whose order is the whole point is exactly
/// how the two drift.
/// The started instance is returned through `out` rather than in the
/// `Expected`: this port's `Expected` unconditionally instantiates its value
/// type's copy constructor, which a `unique_ptr` does not have. Ownership is
/// still all-or-nothing — `out` is only assigned on success.
core::Expected<core::Unit, std::string>
    StartFreshInstance(const FreshStartSpec& spec, std::shared_ptr<Connector> connector,
                       std::unique_ptr<FirecrackerInstance>* out);

/// Rust `impl SandboxBackend for FirecrackerSandbox`.
///
/// Owns one live microVM. Only the lifecycle methods this port can honour are
/// implemented; the rest report that they are not available in this build
/// rather than silently succeeding, because a caller that believes a snapshot
/// was taken will later resume from a capture that does not exist.
class FirecrackerSandboxBackend : public SandboxBackend {
 public:
    /// Takes ownership of an instance that has already been booted.
    ///
    /// `snapshot_config` is the capture descriptor handed back by `Pause`. The
    /// paused state carries it verbatim, so a resume on another node reads the
    /// same fields this node wrote.
    FirecrackerSandboxBackend(std::unique_ptr<FirecrackerInstance> instance,
                              SnapshotConfig snapshot_config);

    /// A backend that has not started yet, as Rust's `FirecrackerSandbox::new`
    /// returns: the object and its work directory exist, Firecracker does not.
    ///
    /// `connector` may be null, in which case the real unix-socket connector
    /// is built at start time.
    FirecrackerSandboxBackend(FreshStartSpec spec, SnapshotConfig snapshot_config,
                              std::shared_ptr<Connector> connector);

    /// Injects the envd health probe. Null uses the real one, which this port
    /// does not have — so without an injected probe `WaitForReady` reports
    /// that it cannot confirm readiness rather than assuming it.
    void SetEnvdProbeForTesting(const EnvdInstance::HealthProbe& probe);
    ~FirecrackerSandboxBackend() override;

    FirecrackerSandboxBackend(const FirecrackerSandboxBackend&) = delete;
    FirecrackerSandboxBackend& operator=(const FirecrackerSandboxBackend&) = delete;

    // ---- implemented ------------------------------------------------------

    /// Rust `start` — `start_nowait` then `wait_for_ready`.
    ///
    /// The split is not cosmetic: `start_nowait` returns once the VM has been
    /// told to run, while `wait_for_ready` waits for **envd inside the guest**
    /// to answer. A caller that submits work after only `start_nowait` is
    /// talking to a VM whose agent may not exist yet.
    core::Expected<core::Unit, core::AnyError> Start() override;

    /// Rust `start_nowait` — dispatches on `LaunchMode` and, for `Fresh`,
    /// runs the spawn/configure/start sequence.
    ///
    /// Refuses a second start rather than spawning another VMM over the first,
    /// which would leak the original and leave two processes on one socket
    /// path.
    core::Expected<core::Unit, core::AnyError> StartNowait() override;

    /// Rust `wait_for_ready` — waits for envd, not for the API socket.
    ///
    /// The API socket is waited on inside `StartNowait`; this is the readiness
    /// the *workload* depends on. Rust errors when no envd instance exists,
    /// and so does this: reporting readiness without having confirmed it is
    /// the one answer that cannot be recovered from.
    core::Expected<core::Unit, core::AnyError> WaitForReady() override;

    /// Rust `pause` — `PATCH /vm {"state":"Paused"}`, then wrap the capture
    /// descriptor in a `FirecrackerPausedState`.
    ///
    /// On failure this runs Rust's `recover_capture_failure`: a recoverable
    /// capture error is reported only after the VM has been put back into a
    /// running state, and a resume that itself fails upgrades the error to
    /// `Terminal`. The two variants are not interchangeable — `Recoverable`
    /// promises the caller the sandbox is still runnable, so returning it
    /// without having resumed would strand a paused VM that everyone believes
    /// is live.
    SandboxCaptureResult<std::shared_ptr<PausedSandboxState> >
        Pause(const core::Optional<std::string>& artifact_root) override;

    /// Rust `pause_to_dir` — pause, then write the capture artifacts into
    /// `spec.snapshot_dir`. See [`PauseToDirSpec`] for the ordering rules.
    ///
    /// Reports through `SandboxCaptureError` like `Pause` does, and for the
    /// same reason: a caller needs to know whether the runtime was mutated.
    /// Unlike `Pause`, a failure here has usually already paused the VM, so
    /// the recovery resume matters more, not less.
    SandboxCaptureResult<PauseToDirResult> PauseToDir(const PauseToDirSpec& spec);

    /// Rust `resume` — `PATCH /vm {"state":"Resumed"}`.
    ///
    /// Idempotent, as the trait requires. The idempotence is Firecracker's:
    /// resuming an already-running VM is accepted, so Rust forwards the call
    /// unconditionally and this does too. Tracking a local "is paused" flag
    /// and skipping the call would be worse than useless — the flag can only
    /// disagree with the VMM, and the disagreement surfaces as a sandbox that
    /// silently stays paused.
    core::Expected<core::Unit, core::AnyError> Resume() override;

    /// Rust `stop` — idempotent. Stops the VMM and removes its API socket.
    core::Expected<core::Unit, core::AnyError> Stop() override;

    /// Rust `runtime_info`.
    SandboxRuntimeInfo RuntimeInfoOf() const override;

    // ---- not available in this build --------------------------------------
    //
    // Reported as errors rather than as success. A caller that is told a
    // snapshot succeeded will later try to resume from a capture that was
    // never written, and the failure then appears on a different node, at a
    // different time, as a corrupt record.

    SandboxCaptureResult<std::string> Snapshot() override;
    SandboxCaptureResult<core::Unit> SnapshotVolumes() override;
    SandboxCaptureResult<core::Unit> FreezeAndSnapshotVolumes() override;
    core::Expected<core::Unit, core::AnyError> ThawVolumes() override;
    SandboxCaptureResult<std::vector<SandboxForkResult> >
        Fork(const std::vector<SandboxForkSpec>& specs) override;
    core::Optional<std::string> HostInteractionIp() const override;
    RuntimeArtifactSet StartupArtifacts() const override;
    core::Expected<core::Unit, core::AnyError>
        UpdateNetworkPolicy(const core::Optional<std::string>& policy) override;
    void UpdateCustomExtensionParams(const core::Optional<std::string>& params) override;

 private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_SANDBOX_BACKEND_H_
