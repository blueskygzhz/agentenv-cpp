// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/factory.rs — `impl SandboxBackendFactory for
// FirecrackerSandboxFactory`.
//
// The factory is what the orchestrator's startup path needs: `Orchestrator::New`
// hands every persisted record to `persister.load_all(factory)`, and without an
// implementor there is no way to turn a stored record back into a backend. That
// is why `main.cc` still wires an in-memory store — not because persistence is
// missing, but because nothing could decode what it saved.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_FACTORY_IMPL_H_
#define AGENTENV_SANDBOX_FIRECRACKER_FACTORY_IMPL_H_

#include <memory>
#include <string>

#include "agentenv/cfg.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/firecracker/sandbox.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust `FirecrackerSandboxFactory`.
///
/// `Build` and `DecodePausedState` are ported. `Build` is config assembly, as
/// it is upstream: Rust's `build` projects the request onto a
/// `FirecrackerSandboxConfig` and constructs the sandbox object, while the
/// ublk device and the network slot are brought up later by `start`. The
/// sandbox it returns therefore starts as far as this port's `StartNowait`
/// goes, and fails there — naming the missing subsystem — rather than at
/// construction, which is also where Rust would fail.
///
/// `BuildFromSnapshot` and `BuildFromPausedState` remain unported: both issue
/// `PUT /snapshot/load` against drives already bound to captured artifacts,
/// which needs the resume launch mode.
class FirecrackerSandboxFactory : public SandboxBackendFactory {
 public:
    /// The firecracker-only config. `Build` needs the whole `AppConfig` (the
    /// rootfs image, the tools drive, the boot-arg allowlist), so a factory
    /// made this way reports `Build` as unconfigured rather than inventing
    /// defaults for paths an operator is supposed to choose.
    explicit FirecrackerSandboxFactory(Config cfg);

    /// The form `main` uses. Carries the node config `Build` projects onto a
    /// `SandboxConfig`.
    FirecrackerSandboxFactory(Config cfg, cfg::AppConfig app_config);

    ~FirecrackerSandboxFactory() override;

    /// Rust `cpu_config_arc` — the cluster-wide CPU template the scheduler
    /// distributes, applied only on the fresh-boot path.
    void SetCpuConfigJson(const core::Optional<std::string>& cpu_config_json);

    /// Stands in for the two resume dependencies this port has no client for:
    /// the shared memory ublk device (`get_or_create_shared_mem`) and the API
    /// connector.
    ///
    /// Without these a resume is refused at start, naming the missing daemon.
    /// They exist so the resume sequence can be driven end-to-end against a
    /// fake VMM, which is otherwise unreachable — and the sequence's ordering
    /// is the part that decides whether a restore works at all.
    void SetResumeDepsForTesting(const std::string& mem_device_path,
                                 std::shared_ptr<Connector> connector);

    FirecrackerSandboxFactory(const FirecrackerSandboxFactory&) = delete;
    FirecrackerSandboxFactory& operator=(const FirecrackerSandboxFactory&) = delete;

    core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
        Build(const FreshSandboxBuildSpec& spec, const SandboxLaunchConfig& config) override;

    /// Rust `build_from_snapshot` — `snapshot_config_for_launch` then a
    /// `LaunchMode::Resume` sandbox.
    core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
        BuildFromSnapshot(const RunnableSnapshotView& snapshot,
                          const SnapshotManifestView& manifest,
                          const SandboxLaunchConfig& config) override;

    core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
        BuildFromPausedState(const core::SandboxId& id, const PausedSandboxState& state,
                             const core::Optional<EnvdAccessToken>& token) override;

    /// Rust `decode_paused_state` — rebuild the paused state from a persisted
    /// record so it can be resumed after a restart.
    ///
    /// Delegates to `DecodeFirecrackerPausedState`, which also runs the
    /// artifact checks: a record whose own files are gone is refused here
    /// rather than being offered for a resume that would fail on load.
    core::Expected<std::shared_ptr<PausedSandboxState>, core::AnyError>
        DecodePausedState(const std::string& artifact_root,
                          const std::string& state) override;

 private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_FACTORY_IMPL_H_
