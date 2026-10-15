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

#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/firecracker/sandbox.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust `FirecrackerSandboxFactory`.
///
/// `DecodePausedState` is fully ported — it is the one method the restart path
/// cannot do without, and the only one whose inputs are entirely on disk. The
/// three `Build*` methods need subsystems this port does not have (image
/// unpack, ublk device setup, network slot allocation), so they report that
/// rather than returning a backend that cannot run: a factory that handed back
/// a half-wired sandbox would fail later, inside the orchestrator's lifecycle,
/// where the error no longer names the cause.
class FirecrackerSandboxFactory : public SandboxBackendFactory {
 public:
    explicit FirecrackerSandboxFactory(Config cfg);
    ~FirecrackerSandboxFactory() override;

    FirecrackerSandboxFactory(const FirecrackerSandboxFactory&) = delete;
    FirecrackerSandboxFactory& operator=(const FirecrackerSandboxFactory&) = delete;

    core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
        Build(const FreshSandboxBuildSpec& spec, const SandboxLaunchConfig& config) override;

    core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
        BuildFromSnapshot(const std::string& snapshot_id,
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
