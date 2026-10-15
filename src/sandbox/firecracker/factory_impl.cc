// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/factory.rs — `impl SandboxBackendFactory for
// FirecrackerSandboxFactory`.
#include "agentenv/sandbox/firecracker/factory_impl.h"

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
};

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
    start.chroot_base_dir = impl_->cfg.chroot_base_dir;
    start.sandbox_id = config.sandbox_id;
    start.firecracker_work_dir = sandbox_config.common.firecracker_work_base_dir;
    // Rust `capture_output` follows the log level: both or neither, per
    // `WarmStdioPaths`. Half the diagnostics is worse than none, because it
    // looks complete.
    start.capture_output = sandbox_config.common.firecracker_log_level.has_value();
    start.socket_timeout_ms =
        static_cast<int64_t>(sandbox_config.common.runtime_policy.socket_timeout_ms);
    start.socket_poll_ms =
        static_cast<int64_t>(sandbox_config.common.runtime_policy.socket_poll_interval_ms);
    start.envd_timeout_ms =
        static_cast<int64_t>(sandbox_config.common.runtime_policy.envd_timeout_ms);
    start.envd_poll_ms =
        static_cast<int64_t>(sandbox_config.common.runtime_policy.envd_poll_interval_ms);

    ConfigurePlan& plan = start.configure;
    plan.mem_size_mib = sandbox_config.mem_size_mib;
    plan.vcpu_count = sandbox_config.vcpu_count;
    plan.track_dirty_pages = sandbox_config.common.track_dirty_pages;
    if (sandbox_config.common.firecracker_log_level.has_value()) {
        plan.log_level = *sandbox_config.common.firecracker_log_level;
        // Resolved against the work directory by StartFreshInstance, which is
        // the only thing that knows where it ended up.
        plan.log_path = "firecracker.log";
    }
    if (sandbox_config.common.cpu_config_json.has_value()) {
        plan.cpu_config_json = *sandbox_config.common.cpu_config_json;
    }
    plan.kernel_image = sandbox_config.kernel_image;
    plan.boot_args = sandbox_config.boot_args;
    plan.disk_rate_limit = sandbox_config.common.disk_rate_limit;
    plan.physical_extra_drive_count = sandbox_config.common.physical_extra_drive_count;
    plan.volume_drive_slots = sandbox_config.common.volume_drive_slots;
    // No network slot is allocated by this port, so the interface, the MMDS
    // config and the MMDS document stay out together rather than configuring
    // a document the guest could not reach.
    plan.has_network_slot = false;

    // The rootfs drive is attached from the ublk device the daemon exports,
    // which this port does not bring up. Validated here so the failure names
    // the missing subsystem instead of surfacing as a guest with no root.
    if (!sandbox_config.common.ublk_config.has_value()) {
        return core::make_unexpected(
            NotPorted("Build", "a ublk device for the rootfs image"));
    }

    SnapshotConfig snapshot_config;
    std::unique_ptr<SandboxBackend> backend(new FirecrackerSandboxBackend(
        start, snapshot_config, std::shared_ptr<Connector>()));
    return core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>(
        std::move(backend));
}

core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError>
FirecrackerSandboxFactory::BuildFromSnapshot(const std::string& /*snapshot_id*/,
                                             const SandboxLaunchConfig& /*config*/) {
    return core::make_unexpected(
        NotPorted("BuildFromSnapshot", "snapshot repository + ublk device"));
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
    spec.capture_output = snapshot_config.common.firecracker_log_level.has_value();
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
