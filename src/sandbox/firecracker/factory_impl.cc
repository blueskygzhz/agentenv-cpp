// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/factory.rs — `impl SandboxBackendFactory for
// FirecrackerSandboxFactory`.
#include "agentenv/sandbox/firecracker/factory_impl.h"

#include "agentenv/sandbox/firecracker/lifecycle.h"

#include "agentenv/sandbox/firecracker/factory.h"
#include "agentenv/sandbox/firecracker/sandbox_backend.h"

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
    explicit Impl(Config c) : cfg(std::move(c)), has_app_config(false) {}
    Impl(Config c, cfg::AppConfig a)
        : cfg(std::move(c)), app_config(std::move(a)), has_app_config(true) {}

    Config cfg;
    cfg::AppConfig app_config;
    /// False for the config-only constructor. `Build` projects the request
    /// onto the node config, so without one there is nothing to project onto
    /// — and defaulting would silently boot from paths nobody chose.
    bool has_app_config;
    core::Optional<std::string> cpu_config_json;

    /// Stands in for `UblkDeviceManager::get_or_create_shared_mem`, which this
    /// port has no client for. Empty in production, which is what makes a
    /// resume refuse at start instead of loading from a path that is not a
    /// block device.
    std::string mem_device_path_for_testing;
    std::shared_ptr<Connector> connector_for_testing;
    /// Rust `UblkDeviceManager::global()`.
    std::shared_ptr<ublk::DeviceManager> ublk_device_manager;

    /// Rust `crate::sandbox::resolve_tools_image`. Unset means every launch
    /// attaches the legacy ext4 tools drive.
    ToolsDriveResolver tools_drive_resolver;

    /// Rust `NetworkManager::global()`; NULL means no network.
    network::SlotNetworkManager* network_manager = NULL;

    /// Rust `FirecrackerPool::global()`; NULL means resumes always cold-spawn.
    FirecrackerPool* firecracker_pool = NULL;

    /// The node half of Rust `start_resume`'s drive preparation. Absent
    /// without a device manager -- the configuration the resume tests use,
    /// where the work directory is not materialized at all.
    core::Expected<core::Optional<ResumeDrivePlan>, std::string>
    ResumeDrivesFor(const SnapshotConfig& snapshot_config) const {
        if (!ublk_device_manager) return core::Optional<ResumeDrivePlan>();
        if (!has_app_config) {
            return core::make_unexpected(std::string(
                "resume needs the node AppConfig to resolve the tools drive "
                "(construct the factory with one)"));
        }
        const core::Expected<std::string, std::string> tools_drive =
            snapshot_config.common.ResolvedToolsDrivePath(app_config);
        if (!tools_drive.ok()) {
            return core::make_unexpected("resolve tools drive: " + tools_drive.error());
        }
        ResumeDrivePlan plan;
        plan.device_manager = ublk_device_manager;
        plan.tools_drive_path = tools_drive.value();
        // The capture's release, which `CommonConfig::New` already defaulted
        // to the node's when the snapshot recorded none.
        plan.tools_drive_version = snapshot_config.common.tools_drive_version;
        plan.tools_drive_resolver = tools_drive_resolver;
        plan.overlaybd_global_config = app_config.ublk.overlaybd.global_config_path;
        plan.runtime_upper_mode = app_config.ublk.overlaybd.runtime_upper_mode;
        plan.extra_drive_allow_shrink = app_config.ublk.overlaybd.allow_shrink;
        return core::Optional<ResumeDrivePlan>(plan);
    }
};

namespace {

/// Rust `std::env::consts::ARCH`, which `add_damon_monitor_region` keys on.
const char* HostArch() {
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#else
    return "";
#endif
}

}  // namespace

FirecrackerSandboxFactory::FirecrackerSandboxFactory(Config cfg)
    : impl_(new Impl(std::move(cfg))) {}

FirecrackerSandboxFactory::FirecrackerSandboxFactory(Config cfg, cfg::AppConfig app_config)
    : impl_(new Impl(std::move(cfg), std::move(app_config))) {}

FirecrackerSandboxFactory::~FirecrackerSandboxFactory() = default;

void FirecrackerSandboxFactory::SetCpuConfigJson(
    const core::Optional<std::string>& cpu_config_json) {
    impl_->cpu_config_json = cpu_config_json;
}

void FirecrackerSandboxFactory::SetResumeDepsForTesting(
    const std::string& mem_device_path, std::shared_ptr<Connector> connector) {
    impl_->mem_device_path_for_testing = mem_device_path;
    impl_->connector_for_testing = connector;
}

void FirecrackerSandboxFactory::SetUblkDeviceManager(
    std::shared_ptr<ublk::DeviceManager> manager) {
    impl_->ublk_device_manager = std::move(manager);
}

void FirecrackerSandboxFactory::SetToolsDriveResolver(const ToolsDriveResolver& resolver) {
    impl_->tools_drive_resolver = resolver;
}

void FirecrackerSandboxFactory::SetNetworkManager(network::SlotNetworkManager* manager) {
    impl_->network_manager = manager;
}

void FirecrackerSandboxFactory::SetFirecrackerPool(FirecrackerPool* pool) {
    impl_->firecracker_pool = pool;
}

core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
FirecrackerSandboxFactory::Build(const FreshSandboxBuildSpec& spec,
                                 const SandboxLaunchConfig& config) {
    if (!impl_->has_app_config) {
        return core::make_unexpected(core::err(
            "FirecrackerSandboxFactory::Build needs the node AppConfig "
            "(construct the factory with one)"));
    }

    // Rust `build`: project the request onto a FirecrackerSandboxConfig...
    core::Expected<SandboxConfig, std::string> projected = FreshSandboxConfigForBuild(
        impl_->app_config, impl_->cpu_config_json, spec, config);
    if (!projected.ok()) {
        return core::make_unexpected(core::err("firecracker build: " + projected.error()));
    }
    SandboxConfig sandbox_config = projected.value();

    // ...then `new_with_id`, whose substance is the drive partitioning. It is
    // validated here rather than at start: more volumes than reserved slots
    // is a permanent misconfiguration, and Firecracker cannot add a block
    // device after a restore, so discovering it later means killing a VM for
    // something that was knowable now.
    const core::Expected<core::Unit, std::string> partitioned = PartitionExtraDrives(
        &sandbox_config, impl_->app_config.volume.max_volume_count);
    if (!partitioned.ok()) {
        return core::make_unexpected(core::err("firecracker build: " + partitioned.error()));
    }

    FreshStartSpec start;
    start.firecracker_bin = sandbox_config.common.firecracker_binary.empty()
                                ? impl_->cfg.firecracker_bin
                                : sandbox_config.common.firecracker_binary;
    start.chroot_base_dir = sandbox_config.common.firecracker_work_base_dir.has_value()
                                ? *sandbox_config.common.firecracker_work_base_dir
                                : std::string();
    start.sandbox_id = config.sandbox_id;
    start.firecracker_work_dir = sandbox_config.common.firecracker_work_base_dir;
    // Rust `capture_output = logging_enabled(firecracker_log_level)`: a
    // blank level counts as unset.
    start.capture_output = LoggingEnabled(sandbox_config.common.firecracker_log_level);
    start.serial_output_base_dir = sandbox_config.common.serial_output_base_dir;
    start.stdout_path = sandbox_config.common.stdout_path;
    start.stderr_path = sandbox_config.common.stderr_path;
    start.firecracker_pool = impl_->firecracker_pool;
    start.socket_timeout_ms =
        static_cast<int64_t>(sandbox_config.common.runtime_policy.socket_timeout_ms);
    start.socket_poll_ms =
        static_cast<int64_t>(sandbox_config.common.runtime_policy.socket_poll_interval_ms);
    start.envd_timeout_ms =
        static_cast<int64_t>(sandbox_config.common.runtime_policy.envd_timeout_ms);
    start.envd_poll_ms =
        static_cast<int64_t>(sandbox_config.common.runtime_policy.envd_poll_interval_ms);
    start.control_plane_port = sandbox_config.common.control_plane_port;
    start.envd_access_token = sandbox_config.common.envd_access_token;

    ConfigurePlan& plan = start.configure;
    plan.mem_size_mib = sandbox_config.mem_size_mib;
    plan.vcpu_count = sandbox_config.vcpu_count;
    plan.track_dirty_pages = sandbox_config.common.track_dirty_pages;
    // Rust `configure_logger` returns early for a blank level.
    if (LoggingEnabled(sandbox_config.common.firecracker_log_level)) {
        plan.log_level = *sandbox_config.common.firecracker_log_level;
        // Replaced by StartFreshInstance with `firecracker_log_path()`, which
        // depends on the work directory only it knows.
        plan.log_path = "firecracker.log";
    }
    if (sandbox_config.common.cpu_config_json.has_value()) {
        plan.cpu_config_json = *sandbox_config.common.cpu_config_json;
    }
    plan.kernel_image = sandbox_config.kernel_image;
    // Rust `start_fresh`: `init=/init`, the guest mount table
    // (`agentenv_drives=vdc:<mountPath>[:<subPath>],...`) and the DAMON
    // region. Without the drives argument the guest has the block devices
    // but nothing tells it where to mount them.
    plan.boot_args = ComposeFreshBootArgs(sandbox_config.boot_args,
                                          sandbox_config.common.extra_drives,
                                          sandbox_config.mem_size_mib, HostArch());
    plan.disk_rate_limit = sandbox_config.common.disk_rate_limit;
    plan.physical_extra_drive_count = sandbox_config.common.physical_extra_drive_count;
    plan.volume_drive_slots = sandbox_config.common.volume_drive_slots;
    // The interface, the MMDS config and the MMDS document move together with
    // the slot, which only exists at start: `StartFreshInstance` sets
    // `has_network_slot` once it has allocated one, and recomposes the boot
    // args so the slot's `ip=` lands before the DAMON region as in Rust.
    plan.has_network_slot = false;
    if (impl_->network_manager != NULL) {
        FreshNetworkPlan network;
        network.manager = impl_->network_manager;
        network.policy = sandbox_config.common.network_policy;
        network.boot_args = sandbox_config.boot_args;
        network.extra_drives = sandbox_config.common.extra_drives;
        network.arch = HostArch();
        // Rust `mmds_metadata(&config.common)`: the recorded document, or a
        // fresh one naming this sandbox with snapshot "unknown".
        network.mmds_json = sandbox_config.common.mmds_metadata.has_value()
                                ? sandbox_config.common.mmds_metadata->ToJson()
                                : MmdsMetadata(start.sandbox_id, "unknown").ToJson();
        start.network = network;
    }

    // Rust `start_fresh` attaches the user rootfs from a ublk device the
    // daemon exports, so a node without a reachable daemon cannot boot one.
    // Refused here, naming the missing subsystem, rather than as a guest with
    // no root filesystem.
    if (!sandbox_config.common.rootfs_image_config.has_value()) {
        return core::make_unexpected(
            core::err("firecracker build: fresh sandbox rootfs image config is missing"));
    }
    if (!impl_->ublk_device_manager || !impl_->ublk_device_manager->is_available()) {
        return core::make_unexpected(
            NotPorted("Build", "a reachable ublk daemon for the rootfs image"));
    }
    const core::Expected<std::string, std::string> tools_drive =
        sandbox_config.common.ResolvedToolsDrivePath(impl_->app_config);
    if (!tools_drive.ok()) {
        return core::make_unexpected(
            core::err("firecracker build: resolve tools drive: " + tools_drive.error()));
    }

    FreshDrivePlan drives;
    drives.device_manager = impl_->ublk_device_manager;
    drives.tools_drive_path = tools_drive.value();
    drives.tools_drive_version = sandbox_config.common.tools_drive_version;
    drives.tools_drive_resolver = impl_->tools_drive_resolver;
    drives.rootfs_image_config = *sandbox_config.common.rootfs_image_config;
    drives.rootfs_virtual_size = sandbox_config.common.rootfs_virtual_size;
    drives.rootfs_allow_shrink = sandbox_config.common.rootfs_allow_shrink;
    drives.extra_drives = sandbox_config.common.extra_drives;
    drives.overlaybd_global_config = impl_->app_config.ublk.overlaybd.global_config_path;
    drives.runtime_upper_mode = impl_->app_config.ublk.overlaybd.runtime_upper_mode;
    drives.extra_drive_allow_shrink = impl_->app_config.ublk.overlaybd.allow_shrink;
    start.drives = drives;

    SnapshotConfig snapshot_config;
    std::unique_ptr<SandboxBackend> backend(new FirecrackerSandboxBackend(
        start, snapshot_config, std::shared_ptr<Connector>()));
    return core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>(
        std::move(backend));
}

core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
FirecrackerSandboxFactory::BuildFromSnapshot(const RunnableSnapshotView& snapshot,
                                             const SnapshotManifestView& manifest,
                                             const SandboxLaunchConfig& config) {
    if (!impl_->has_app_config) {
        return core::make_unexpected(core::err(
            "FirecrackerSandboxFactory::BuildFromSnapshot needs the node AppConfig "
            "(construct the factory with one)"));
    }

    // Rust `FirecrackerSnapshotConfig::from_runnable_snapshot`. This is where
    // a capture from another VMM, or from another virtualization mode, is
    // refused -- before anything is created, because neither can be fixed.
    const CommonConfig base = CommonConfig::New(
        impl_->app_config.ResolvedFirecrackerBinaryPath(),
        snapshot.tools_drive_version.empty() ? impl_->app_config.ResolvedToolsVersion()
                                             : snapshot.tools_drive_version,
        RuntimePolicy::FromAppConfig(impl_->app_config));
    core::Expected<SnapshotConfig, std::string> projected =
        SnapshotConfigFromRunnable(base, impl_->app_config, snapshot, manifest);
    if (!projected.ok()) {
        return core::make_unexpected(
            core::err("firecracker build from snapshot: " + projected.error()));
    }
    SnapshotConfig snapshot_config = projected.value();

    // Rust `snapshot_config_for_launch` — this launch's identity, network
    // policy and volumes layered over the recorded config.
    const core::Expected<core::Unit, std::string> applied = ApplyLaunchToSnapshotConfig(
        &snapshot_config, config, core::Optional<std::string>());
    if (!applied.ok()) {
        return core::make_unexpected(
            core::err("firecracker build from snapshot: " + applied.error()));
    }

    ResumeStartSpec spec;
    spec.snapshot_config = snapshot_config;
    spec.firecracker_bin = snapshot_config.common.firecracker_binary.empty()
                               ? impl_->cfg.firecracker_bin
                               : snapshot_config.common.firecracker_binary;
    spec.chroot_base_dir = impl_->cfg.chroot_base_dir;
    spec.sandbox_id = config.sandbox_id;
    spec.firecracker_work_dir = snapshot_config.common.firecracker_work_base_dir;
    spec.capture_output = LoggingEnabled(snapshot_config.common.firecracker_log_level);
    spec.socket_timeout_ms =
        static_cast<int64_t>(snapshot_config.common.runtime_policy.socket_timeout_ms);
    spec.socket_poll_ms =
        static_cast<int64_t>(snapshot_config.common.runtime_policy.socket_poll_interval_ms);
    spec.envd_timeout_ms =
        static_cast<int64_t>(snapshot_config.common.runtime_policy.envd_timeout_ms);
    spec.envd_poll_ms =
        static_cast<int64_t>(snapshot_config.common.runtime_policy.envd_poll_interval_ms);
    // Same as the paused-state path: Rust acquires the shared memory device in
    // `start_resume`, and this port has no ublk client, so the refusal lands
    // at start where Rust's own `ensure!` fires.
    spec.mem_device_path = impl_->mem_device_path_for_testing;
    // Rust `from_snapshot`: the launch's drives are absent from the source
    // state and are bound and patched after the load.
    spec.initial_guest_drive_mounts =
        InitialGuestDriveMountsForSnapshotLaunch(snapshot_config, config);
    const core::Expected<core::Optional<ResumeDrivePlan>, std::string> drives =
        impl_->ResumeDrivesFor(snapshot_config);
    if (!drives.ok()) {
        return core::make_unexpected(
            core::err("firecracker build from snapshot: " + drives.error()));
    }
    spec.drives = drives.value();
    spec.network_manager = impl_->network_manager;
    spec.firecracker_pool = impl_->firecracker_pool;

    std::unique_ptr<SandboxBackend> backend(
        FirecrackerSandboxBackend::NewResuming(spec, impl_->connector_for_testing));
    return core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>(
        std::move(backend));
}

core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
FirecrackerSandboxFactory::BuildFromPausedState(
    const core::SandboxId& id, const PausedSandboxState& state,
    const core::Optional<EnvdAccessToken>& token) {
    // Rust `state.downcast_ref::<FirecrackerPausedState>()`. A state from
    // another backend carries a different capture shape entirely, so this is
    // refused rather than reinterpreted.
    const FirecrackerPausedState* paused =
        dynamic_cast<const FirecrackerPausedState*>(&state);
    if (paused == NULL) {
        return core::make_unexpected(core::err(
            "the provided PausedSandboxState is not a Firecracker paused state"));
    }

    SnapshotConfig snapshot_config = paused->snapshot_config();
    const core::Expected<core::Unit, std::string> prepared =
        PrepareSnapshotConfigForResume(&snapshot_config, id, token);
    if (!prepared.ok()) {
        return core::make_unexpected(
            core::err("firecracker resume: " + prepared.error()));
    }

    ResumeStartSpec spec;
    spec.snapshot_config = snapshot_config;
    spec.firecracker_bin = snapshot_config.common.firecracker_binary.empty()
                               ? impl_->cfg.firecracker_bin
                               : snapshot_config.common.firecracker_binary;
    spec.chroot_base_dir = impl_->cfg.chroot_base_dir;
    spec.sandbox_id = id;
    spec.firecracker_work_dir = snapshot_config.common.firecracker_work_base_dir;
    spec.capture_output = LoggingEnabled(snapshot_config.common.firecracker_log_level);
    spec.socket_timeout_ms =
        static_cast<int64_t>(snapshot_config.common.runtime_policy.socket_timeout_ms);
    spec.socket_poll_ms =
        static_cast<int64_t>(snapshot_config.common.runtime_policy.socket_poll_interval_ms);
    spec.envd_timeout_ms =
        static_cast<int64_t>(snapshot_config.common.runtime_policy.envd_timeout_ms);
    spec.envd_poll_ms =
        static_cast<int64_t>(snapshot_config.common.runtime_policy.envd_poll_interval_ms);
    // Left empty: Rust gets it from `get_or_create_shared_mem`, and this port
    // has no ublk daemon client. `BuildResumeSequence` turns that into a
    // refusal naming the missing daemon, so the backend is constructed and
    // fails at start -- where Rust's own `ensure!(is_available())` fires too,
    // rather than at construction where Rust would still succeed.
    spec.mem_device_path = impl_->mem_device_path_for_testing;
    // No `initial_guest_drive_mounts`: every drive of a paused capture is in
    // its state, so all of them are bound before the load.
    const core::Expected<core::Optional<ResumeDrivePlan>, std::string> drives =
        impl_->ResumeDrivesFor(snapshot_config);
    if (!drives.ok()) {
        return core::make_unexpected(core::err("firecracker resume: " + drives.error()));
    }
    spec.drives = drives.value();
    spec.network_manager = impl_->network_manager;
    spec.firecracker_pool = impl_->firecracker_pool;

    std::unique_ptr<SandboxBackend> backend(
        FirecrackerSandboxBackend::NewResuming(spec, impl_->connector_for_testing));
    return core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>(
        std::move(backend));
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
