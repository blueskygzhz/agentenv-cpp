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
/// Rust `FirecrackerSandbox::snapshot_config_for_launch` — layer this launch's
/// inputs onto a committed snapshot's recorded config.
///
/// Distinct from `PrepareSnapshotConfigForResume`, which revives *this node's
/// own* paused capture: there the config is already this sandbox's and only
/// the identity is restated, whereas here a shared committed snapshot is being
/// launched as a brand-new sandbox, so the launch supplies the whole of its
/// identity, its network policy and its extra drives.
///
/// The volume check comes first and is a hard error: Firecracker cannot add a
/// virtio-block device after a restore, so a launch asking for more volumes
/// than the snapshot reserved slots for can never be satisfied — and silently
/// dropping the surplus would boot a sandbox whose caller finds its mount
/// points empty.
core::Expected<core::Unit, std::string> ApplyLaunchToSnapshotConfig(
    SnapshotConfig* config, const SandboxLaunchConfig& launch_config,
    const core::Optional<std::string>& snapshot_custom_extension_params);

/// Rust `FirecrackerSandbox::initial_guest_drive_mounts_for_snapshot_launch`.
///
/// The `(slot_index, drive)` pairs for volumes that are *not* already in the
/// source state and so were loaded as placeholders. Empty when
/// `extra_drives_in_snapshot` is set, because those were bound before the load
/// and need no patch afterwards.
///
/// Slot indices start at `physical_extra_drive_count`: the physical drives
/// occupy the slots before it, and a volume bound into one of those would be
/// mounted as the wrong filesystem.
std::vector<std::pair<std::size_t, ExtraDrive> >
    InitialGuestDriveMountsForSnapshotLaunch(const SnapshotConfig& config,
                                             const SandboxLaunchConfig& launch_config);

/// Rust `FirecrackerSandbox::from_snapshot_config_with_override` — prepare a
/// captured config for a resume under a new sandbox id.
///
/// Three things happen, and all three are load-bearing:
///
///  1. A legacy capture (`volume_drive_slots == 0`) predates reserved volume
///     slots and lists every drive as physical. Read as-is it would attach
///     volumes into physical slots, so its physical count is restated as the
///     whole list.
///
///  2. The drive counts are checked against each other. Firecracker reopens
///     every serialized slot path on load, so a config claiming more physical
///     drives than it carries, or more volumes than it reserved, cannot be
///     loaded — and failing here names the record rather than the load.
///
///  3. Runtime identity overrides what the capture recorded: the new sandbox
///     id and this launch's envd token replace the captured ones, in the MMDS
///     document as well as the config. The guest reads its identity from MMDS,
///     so a stale id there makes a resumed sandbox report as the one that was
///     captured — and the token is a live credential that must not be the
///     captured sandbox's.
core::Expected<core::Unit, std::string> PrepareSnapshotConfigForResume(
    SnapshotConfig* config, const core::SandboxId& sandbox_id,
    const core::Optional<EnvdAccessToken>& envd_access_token);

/// Rust `FirecrackerSandbox::new_with_id` — the drive partitioning it does
/// before handing the config to `build`.
///
/// Physical drives come first and volumes after, because the guest names
/// drives positionally (`vdc`, `vdd`, ...) and a volume that lands in a
/// physical slot is mounted as the wrong filesystem. `volume_drive_slots` is
/// the number of placeholders reserved on the first boot: Firecracker cannot
/// add a virtio-block device after a snapshot restore, so a volume attached
/// later has to occupy a slot that already exists.
///
/// Fails rather than truncating when there are more volumes than slots. The
/// alternative is a sandbox that boots without the volume its caller asked
/// for, which is only noticed when the guest finds the mount point empty.
core::Expected<core::Unit, std::string> PartitionExtraDrives(
    SandboxConfig* config, std::size_t max_volume_count);

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
