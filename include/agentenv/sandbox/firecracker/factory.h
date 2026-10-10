// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/factory.rs — FirecrackerSandboxFactory.
//
// Ported here: the config half of `build`, i.e. everything `build` does before
// it hands the finished `FirecrackerSandboxConfig` to
// `FirecrackerSandbox::new_with_id`. That half is where a fresh sandbox's
// launch inputs are decided — the machine shape, the rootfs size, which
// API-supplied kernel arguments survive, and how launch overrides layer onto
// the image's defaults. The factory type itself (and its `build*` methods)
// needs the live `FirecrackerSandbox`, which is not ported yet.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_FACTORY_H_
#define AGENTENV_SANDBOX_FIRECRACKER_FACTORY_H_

#include <memory>
#include <string>

#include "agentenv/cfg.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/firecracker/config.h"
#include "agentenv/sandbox/firecracker/paused_state.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust `FirecrackerSandboxFactory::build`, up to (not including)
/// `FirecrackerSandbox::new_with_id(config, launch_config.sandbox_id)`.
///
/// `cpu_config_json` is the factory's `cpu_config_arc` read at build time —
/// the cluster-wide CPU template the scheduler distributes. It is only ever
/// applied on this fresh-boot path (`PUT /cpu-config` before first boot).
///
/// Rust reads `ConfigManager::global_config()`; the config is a parameter here
/// so the projection can be tested without installing a global.
core::Expected<SandboxConfig, std::string> FreshSandboxConfigForBuild(
    const cfg::AppConfig& app_config,
    const core::Optional<std::string>& cpu_config_json,
    const FreshSandboxBuildSpec& build_spec,
    const SandboxLaunchConfig& launch_config);

/// Rust `FirecrackerSandboxFactory::decode_paused_state`.
///
/// A free function rather than a `SandboxBackendFactory` method because the
/// factory's other three methods construct a live `FirecrackerSandbox`, which
/// is not ported yet. This is the part the orchestrator's restart path needs:
/// `SandboxPersister::LoadAll` calls it for every stored record, and a record
/// it rejects is dropped together with its artifacts.
core::Expected<std::shared_ptr<PausedSandboxState>, core::AnyError>
    DecodeFirecrackerPausedState(const std::string& artifact_root, const std::string& state);

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_FACTORY_H_
