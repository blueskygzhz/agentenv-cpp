// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/factory.rs — `impl SandboxBackendFactory for
// FirecrackerSandboxFactory`.
#include "agentenv/sandbox/firecracker/factory_impl.h"

#include "agentenv/sandbox/firecracker/factory.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

namespace {

core::AnyError NotPorted(const char* method, const char* needs) {
    return core::err(std::string("FirecrackerSandboxFactory::") + method +
                     " is not available in this build (needs " + needs + ")");
}

}  // namespace

class FirecrackerSandboxFactory::Impl {
 public:
    explicit Impl(Config c) : cfg(std::move(c)) {}
    Config cfg;
};

FirecrackerSandboxFactory::FirecrackerSandboxFactory(Config cfg)
    : impl_(new Impl(std::move(cfg))) {}

FirecrackerSandboxFactory::~FirecrackerSandboxFactory() = default;

core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
FirecrackerSandboxFactory::Build(const FreshSandboxBuildSpec& /*spec*/,
                                 const SandboxLaunchConfig& /*config*/) {
    // A fresh build resolves the tools drive version against the global config,
    // unpacks the user image into an overlaybd stack, brings up a ublk device
    // for it and allocates a network slot. None of that is ported, and a
    // backend returned without it would fail inside the orchestrator's
    // lifecycle, where the error no longer names the cause.
    return core::make_unexpected(
        NotPorted("Build", "image unpack + ublk device + network slot"));
}

core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
FirecrackerSandboxFactory::BuildFromSnapshot(const std::string& /*snapshot_id*/,
                                             const SandboxLaunchConfig& /*config*/) {
    return core::make_unexpected(
        NotPorted("BuildFromSnapshot", "snapshot repository + ublk device"));
}

core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
FirecrackerSandboxFactory::BuildFromPausedState(
    const core::SandboxId& /*id*/, const PausedSandboxState& /*state*/,
    const core::Optional<EnvdAccessToken>& /*token*/) {
    // Resuming a paused state issues `PUT /snapshot/load` against a VMM whose
    // drives are already bound to the captured artifacts, which needs the ublk
    // setup above plus the resume launch mode.
    return core::make_unexpected(
        NotPorted("BuildFromPausedState", "ublk device + snapshot-load launch mode"));
}

core::Expected<std::shared_ptr<PausedSandboxState>, core::AnyError>
FirecrackerSandboxFactory::DecodePausedState(const std::string& artifact_root,
                                             const std::string& state) {
    // Fully ported: this is the one method the restart path cannot do without,
    // and the only one whose inputs are entirely on disk.
    return DecodeFirecrackerPausedState(artifact_root, state);
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
