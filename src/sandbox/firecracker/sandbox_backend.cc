// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs — `impl SandboxBackend for
// FirecrackerSandbox`, plus `recover_capture_failure`.
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <dirent.h>
#include "agentenv/sandbox/firecracker/sandbox_backend.h"

#include "agentenv/sandbox/firecracker/process_vm.h"

#include "agentenv/sandbox/firecracker/lifecycle.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

namespace {

/// Rust `impl From<anyhow::Error> for SandboxCaptureError`.
///
/// An error that is not already classified becomes `Recoverable`. The default
/// direction matters: `Terminal` tells the orchestrator the runtime was
/// mutated and must not be trusted, so defaulting there would condemn sandboxes
/// over transient faults. `Recoverable` is only safe to report once the VM is
/// actually running again, which is what `RecoverCaptureFailure` enforces.
SandboxCaptureError ClassifyCaptureError(const std::string& message) {
    return SandboxCaptureError::Recoverable(message);
}

core::AnyError NotAvailable(const char* what) {
    return core::err(std::string(what) + ": not available in this build");
}

}  // namespace

/// Rust: the `ProcessVmReader` element of `src_layers`.
///
/// `CompactTo` addresses sources by byte offset; for this layer that offset is
/// a **host virtual address** in the VMM, not a file position. The mappings
/// were built from `base_host_virt_addr`, so the two agree by construction --
/// but it is why there is only ever one source layer here (tag 0): a second
/// layer would need its own address space to resolve against.
class ProcessVmCompactSource : public storage::overlaybd::lsmt::CompactSource {
 public:
    explicit ProcessVmCompactSource(int64_t pid) : reader_(pid) {}

    bool ReadAt(uint8_t tag, uint64_t src_byte_off, uint8_t* dst, uint64_t len) override {
        if (tag != 0) return false;
        // A short read must fail rather than leave the tail as zeros: zeroed
        // guest memory is indistinguishable from memory that was never read,
        // so a partial read would corrupt the snapshot silently.
        return reader_.ReadExactRemote(src_byte_off, dst, static_cast<std::size_t>(len)).ok();
    }

 private:
    ProcessVmReader reader_;
};

/// Rust `tokio::fs::create_dir_all`.
core::Expected<core::Unit, std::string> MakeDirs(const std::string& path) {
    std::string acc;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!acc.empty() && ::mkdir(acc.c_str(), 0700) != 0 && errno != EEXIST) {
                return core::make_unexpected(std::string("create ") + acc + ": " +
                                             std::strerror(errno));
            }
        }
        if (i < path.size()) acc.push_back(path[i]);
    }
    return core::Unit{};
}

/// Rust `cleanup_failed_snapshot_dir`.
///
/// Best-effort and deliberately silent about a missing directory: the capture
/// may have failed before the mkdir, and Rust treats `NotFound` as success.
/// Any other failure is logged, not propagated — the capture error is what the
/// caller needs, and replacing it with a cleanup error would hide the cause.
void CleanupFailedSnapshotDir(const std::string& path) {
    if (path.empty()) return;
    DIR* dir = ::opendir(path.c_str());
    if (dir == NULL) return;  // includes ENOENT, as Rust's NotFound arm does
    struct dirent* entry = NULL;
    while ((entry = ::readdir(dir)) != NULL) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        const std::string child = path + "/" + name;
        struct stat st;
        // lstat, not stat: a symlink inside the directory must be removed
        // rather than followed into whatever it points at.
        if (::lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            CleanupFailedSnapshotDir(child);
        } else {
            ::unlink(child.c_str());
        }
    }
    ::closedir(dir);
    ::rmdir(path.c_str());
}

core::Expected<core::Unit, std::string> PublishMemoryOverlaybdLayer(
    const std::string& output_path,
    const std::vector<storage::overlaybd::lsmt::SegmentMapping>& mappings,
    uint64_t virtual_size, int64_t firecracker_pid) {
    // Rust: `output_path.with_extension("commit.tmp")`. The final name is what
    // the resume path loads, and a reader cannot tell a truncated commit file
    // from a short one, so it must appear only once complete.
    const std::string tmp_path = output_path + ".tmp";

    ProcessVmCompactSource source(firecracker_pid);
    std::string err;
    const bool ok = storage::overlaybd::lsmt::CompactTo(tmp_path, &source, mappings,
                                                        virtual_size, &err);
    if (!ok) {
        ::unlink(tmp_path.c_str());
        return core::make_unexpected(std::string("compact memory layer: ") + err);
    }
    if (::rename(tmp_path.c_str(), output_path.c_str()) != 0) {
        const std::string why = std::strerror(errno);
        ::unlink(tmp_path.c_str());
        return core::make_unexpected(
            std::string("move sealed memory lower into place failed: ") + output_path +
            ": " + why);
    }
    return core::Unit{};
}

core::Expected<core::Unit, std::string>
StartFreshInstance(const FreshStartSpec& spec, std::shared_ptr<Connector> connector,
                   std::unique_ptr<FirecrackerInstance>* out) {
    if (out == NULL) return core::make_unexpected(std::string("no output slot"));
    // 1. Work directory. The caller has already validated the configure plan,
    //    so nothing that fails below is knowable-up-front.
    const core::Expected<std::string, std::string> work_dir =
        CreateFirecrackerWorkDir(core::Optional<std::string>(
            spec.chroot_base_dir + "/firecracker/" + spec.sandbox_id.ToString()));
    if (!work_dir.ok()) return core::make_unexpected(work_dir.error());

    // The logger path can only be known now; its emptiness was what decided
    // whether the step exists at all.
    std::vector<ConfigureStep> steps;
    {
        const core::Expected<std::vector<ConfigureStep>, std::string> built =
            BuildConfigureSequence(spec.configure);
        if (!built.ok()) return core::make_unexpected(built.error());
        steps = built.value();
    }
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].kind == ConfigureStep::Kind::Logger) {
            steps[i].path = work_dir.value() + "/firecracker.log";
        }
    }

    // 2. Spawn.
    core::Optional<std::string> stdout_path;
    core::Optional<std::string> stderr_path;
    WarmStdioPaths(work_dir.value(), spec.capture_output, &stdout_path, &stderr_path);

    if (!connector) connector = MakeUnixConnector();
    std::unique_ptr<FirecrackerInstance> instance(
        new FirecrackerInstance(work_dir.value(), connector));

    const core::Expected<core::Unit, std::string> spawned = instance->Spawn(
        spec.firecracker_bin,
        stdout_path.has_value() ? *stdout_path : std::string(),
        stderr_path.has_value() ? *stderr_path : std::string());
    if (!spawned.ok()) {
        return core::make_unexpected(std::string("spawn: ") + spawned.error());
    }

    // Past the spawn, every early return must stop the VMM. `instance` is a
    // unique_ptr and ~FirecrackerInstance is this port's `Drop` (SIGKILL,
    // reap, unlink), so unwinding does it; only the success path moves the
    // instance out.

    // 3. Wait for the API socket. WaitForReady distinguishes "child exited"
    //    from "timed out" and quotes the captured stderr, which is the
    //    difference between a bad kernel path and a merely slow host.
    const core::Expected<core::Unit, std::string> ready =
        instance->WaitForReady(spec.socket_timeout_ms, spec.socket_poll_ms);
    if (!ready.ok()) return core::make_unexpected(ready.error());

    // 4. Apply the sequence.
    const core::Expected<core::Unit, std::string> configured =
        ApplyConfigureSequence(instance.get(), steps);
    if (!configured.ok()) return core::make_unexpected(configured.error());

    // 5. Start the microVM.
    const core::Expected<core::Unit, std::string> start = instance->Start();
    if (!start.ok()) {
        return core::make_unexpected(std::string("InstanceStart: ") + start.error());
    }
    // Only now is ownership handed over; every return above let the unique_ptr
    // unwind, which stops the VMM.
    *out = std::move(instance);
    return core::Unit{};
}

class FirecrackerSandboxBackend::Impl {
 public:
    Impl(std::unique_ptr<FirecrackerInstance> i, SnapshotConfig cfg)
        : instance(std::move(i)), snapshot_config(std::move(cfg)), startable(false) {}

    Impl(FreshStartSpec s, SnapshotConfig cfg, std::shared_ptr<Connector> c)
        : snapshot_config(std::move(cfg)), spec(std::move(s)), connector(std::move(c)),
          startable(true) {}

    std::unique_ptr<FirecrackerInstance> instance;
    SnapshotConfig snapshot_config;
    FreshStartSpec spec;
    std::shared_ptr<Connector> connector;
    /// False for a backend handed an already-booted instance: there is nothing
    /// left to start, and a start would spawn a second VMM over the first.
    bool startable;
    LaunchMode launch;
    EnvdInstance::HealthProbe envd_probe;

    /// Rust `recover_capture_failure`.
    ///
    /// A terminal error is returned as-is: the runtime is already mutated, so
    /// resuming would not make the sandbox trustworthy again. Otherwise the VM
    /// is put back into a running state *before* the error is reported,
    /// because `Recoverable` is a promise to the caller that the sandbox is
    /// still runnable. If that resume fails, the promise cannot be kept and
    /// the error is upgraded to `Terminal` — carrying both messages, since the
    /// original fault is what an operator needs and the resume failure is what
    /// makes it unrecoverable.
    SandboxCaptureError RecoverCaptureFailure(const char* operation,
                                              const SandboxCaptureError& error) {
        if (error.IsTerminal()) return error;
        const core::Expected<core::Unit, std::string> resumed = instance->Resume();
        if (!resumed.ok()) {
            return SandboxCaptureError::Terminal(
                std::string(operation) + " failed and sandbox could not be resumed: " +
                error.message + "; resume error: " + resumed.error());
        }
        return error;
    }
};

FirecrackerSandboxBackend::FirecrackerSandboxBackend(
    std::unique_ptr<FirecrackerInstance> instance, SnapshotConfig snapshot_config)
    : impl_(new Impl(std::move(instance), std::move(snapshot_config))) {}

FirecrackerSandboxBackend::FirecrackerSandboxBackend(
    FreshStartSpec spec, SnapshotConfig snapshot_config,
    std::shared_ptr<Connector> connector)
    : impl_(new Impl(std::move(spec), std::move(snapshot_config), std::move(connector))) {}

FirecrackerSandboxBackend::~FirecrackerSandboxBackend() = default;

void FirecrackerSandboxBackend::SetEnvdProbeForTesting(
    const EnvdInstance::HealthProbe& probe) {
    impl_->envd_probe = probe;
}

SandboxCaptureResult<std::shared_ptr<PausedSandboxState> >
FirecrackerSandboxBackend::Pause(const core::Optional<std::string>& artifact_root) {
    if (!impl_->instance) {
        return core::make_unexpected(
            SandboxCaptureError::Terminal("pause: no live microVM"));
    }
    // Writing capture artifacts into a caller-managed directory is Rust's
    // `pause_to_dir`, which also flushes writable volumes through envd. Neither
    // is ported, so an artifact root is refused rather than ignored: honouring
    // the pause but dropping the artifacts would hand back a paused state whose
    // files were never written.
    if (artifact_root.has_value()) {
        return core::make_unexpected(SandboxCaptureError::Recoverable(
            "pause to a caller-managed artifact root is not available in this build"));
    }

    const core::Expected<core::Unit, std::string> paused = impl_->instance->Pause();
    if (!paused.ok()) {
        return core::make_unexpected(
            impl_->RecoverCaptureFailure("pause", ClassifyCaptureError(paused.error())));
    }
    return std::shared_ptr<PausedSandboxState>(
        new FirecrackerPausedState(impl_->snapshot_config));
}

core::Expected<core::Unit, core::AnyError> FirecrackerSandboxBackend::Resume() {
    if (!impl_->instance) return core::make_unexpected(core::err("resume: no live microVM"));
    // Forwarded unconditionally. Firecracker accepts a resume on a running VM,
    // which is where the trait's idempotence comes from; a local paused flag
    // could only drift from the VMM's actual state.
    const core::Expected<core::Unit, std::string> r = impl_->instance->Resume();
    if (!r.ok()) return core::make_unexpected(core::err(r.error()));
    return core::Unit{};
}

core::Expected<core::Unit, core::AnyError> FirecrackerSandboxBackend::Stop() {
    // Idempotent, as the trait requires: with no instance there is nothing to
    // stop and that is a success, not an error. `Stop` on an already-dead
    // child reaps it and removes the socket, so a second call is also fine.
    if (!impl_->instance) return core::Unit{};
    const core::Expected<core::Unit, std::string> r =
        impl_->instance->Stop(kPoolFirecrackerStopTimeoutMs);
    if (!r.ok()) return core::make_unexpected(core::err(r.error()));
    return core::Unit{};
}

SandboxRuntimeInfo FirecrackerSandboxBackend::RuntimeInfoOf() const {
    SandboxRuntimeInfo info;
    // `rootfs_virtual_size` is cached by Rust's `start()` from the live block
    // device and is required before a snapshot; this port never records it, so
    // it stays absent rather than being guessed. `RuntimeArtifacts` comes from
    // the capture descriptor, which is the same source Rust uses.
    info.runtime_artifacts = FirecrackerPausedState(impl_->snapshot_config).RuntimeArtifacts();
    return info;
}

SandboxCaptureResult<PauseToDirResult>
FirecrackerSandboxBackend::PauseToDir(const PauseToDirSpec& spec) {
    if (!impl_->instance) {
        return core::make_unexpected(
            SandboxCaptureError::Terminal("pause_to_dir: no live microVM"));
    }

    // 1. Flush writable volumes -- BEFORE the pause. The flush runs inside the
    //    guest through envd, which cannot be reached once the VM is paused.
    //    Doing this after would capture volumes with dirty page cache, so the
    //    resumed sandbox would see a stale filesystem rather than an old one.
    if (spec.has_writable_persistent_volumes) {
        if (!spec.flush_volumes) {
            return core::make_unexpected(SandboxCaptureError::Recoverable(
                "cannot flush writable volumes because envd is not running"));
        }
        const core::Expected<core::Unit, std::string> flushed = spec.flush_volumes();
        if (!flushed.ok()) {
            // Nothing has been paused or created yet, so this is recoverable
            // without any recovery: the sandbox is still running.
            return core::make_unexpected(SandboxCaptureError::Recoverable(flushed.error()));
        }
    }

    // 2. Pause.
    const core::Expected<core::Unit, std::string> paused = impl_->instance->Pause();
    if (!paused.ok()) {
        return core::make_unexpected(
            impl_->RecoverCaptureFailure("pause", ClassifyCaptureError(paused.error())));
    }

    // From here the VM is paused, so every failure must run the recovery
    // resume *and* remove the partial directory. A half-written snapshot
    // directory cannot be told from a complete one by name, and the resume path
    // would later load a vm_state.bin whose memory layer was never finished.
    PauseToDirResult result;
    const core::Expected<core::Unit, std::string> captured = [&]()
        -> core::Expected<core::Unit, std::string> {
        const core::Expected<core::Unit, std::string> made = MakeDirs(spec.snapshot_dir);
        if (!made.ok()) return made;

        // 3. Read the pid BEFORE the state snapshot. Guest memory is pulled
        //    from the live VMM with process_vm_readv, so the capture needs a
        //    pid that is still valid; reading it after would race a VMM that
        //    died during the snapshot and blame the wrong step.
        const core::Expected<int64_t, std::string> pid = impl_->instance->Pid();
        if (!pid.ok()) return core::make_unexpected(pid.error());
        if (spec.on_pid_read) spec.on_pid_read(pid.value());

        result.vm_state_path = spec.snapshot_dir + "/" + kVmStateFileName;
        const core::Expected<core::Unit, std::string> snap =
            impl_->instance->CreateStateOnlySnapshot(result.vm_state_path);
        if (!snap.ok()) return core::make_unexpected(snap.error());

        // 4. vm_state.bin now describes this paused VM. Dirty ranges must be
        //    read after it, so the two describe the same instant.
        const core::Expected<DirtyMemoryRanges, std::string> ranges =
            impl_->instance->GetDirtyMemoryRanges();
        if (!ranges.ok()) return core::make_unexpected(ranges.error());

        uint64_t memory_size = 0;
        const core::Expected<std::vector<storage::overlaybd::lsmt::SegmentMapping>,
                             std::string>
            mappings = DirtyRangesToSegmentMappings(ranges.value(), &memory_size);
        if (!mappings.ok()) return core::make_unexpected(mappings.error());

        const std::string mem_dir = spec.snapshot_dir + "/mem_overlaybd";
        const core::Expected<core::Unit, std::string> mem_made = MakeDirs(mem_dir);
        if (!mem_made.ok()) return mem_made;

        result.memory_layer_path = mem_dir + "/overlaybd.commit";
        result.memory_size = memory_size;
        if (spec.write_memory_layer) {
            return spec.write_memory_layer(result.memory_layer_path, mappings.value(),
                                           memory_size, pid.value());
        }
        return PublishMemoryOverlaybdLayer(result.memory_layer_path, mappings.value(),
                                           memory_size, pid.value());
    }();

    if (!captured.ok()) {
        CleanupFailedSnapshotDir(spec.snapshot_dir);
        return core::make_unexpected(
            impl_->RecoverCaptureFailure("snapshot capture",
                                         ClassifyCaptureError(captured.error())));
    }
    return result;
}

// ---- not available in this build ------------------------------------------

core::Expected<core::Unit, core::AnyError> FirecrackerSandboxBackend::StartNowait() {
    if (!impl_->startable) {
        return core::make_unexpected(
            core::err("start_nowait: this backend was handed an already-booted microVM"));
    }
    if (impl_->instance) {
        // A second spawn would leak the first VMM and put two processes on one
        // socket path, where the second's requests reach whichever bound it.
        return core::make_unexpected(core::err("start_nowait: sandbox is already started"));
    }
    if (impl_->launch.kind != LaunchMode::Kind::Fresh) {
        // Resume sends a different sequence (`PUT /snapshot/load` instead of
        // boot-source plus drives), so it cannot borrow the fresh path.
        return core::make_unexpected(
            core::err("start_nowait: resume from snapshot is not available in this build"));
    }
    std::unique_ptr<FirecrackerInstance> started;
    const core::Expected<core::Unit, std::string> r =
        StartFreshInstance(impl_->spec, impl_->connector, &started);
    if (!r.ok()) return core::make_unexpected(core::err(r.error()));
    impl_->instance = std::move(started);
    return core::Unit{};
}

core::Expected<core::Unit, core::AnyError> FirecrackerSandboxBackend::WaitForReady() {
    if (!impl_->instance) {
        return core::make_unexpected(core::err("wait_for_ready: sandbox is not started"));
    }
    // Rust errors when no envd instance is initialized. This port has no envd
    // client, so without an injected probe it must say it cannot confirm
    // readiness -- claiming readiness it never observed would hand the
    // orchestrator a sandbox that accepts work and answers nothing.
    if (!impl_->envd_probe) {
        return core::make_unexpected(core::err("wait_for_ready: envd instance not initialized"));
    }
    EnvdInstance envd(std::string("http://127.0.0.1"));
    const core::Expected<core::Unit, std::string> r = envd.WaitForReady(
        impl_->spec.envd_timeout_ms, impl_->spec.envd_poll_ms, impl_->envd_probe);
    if (!r.ok()) return core::make_unexpected(core::err(r.error()));
    return core::Unit{};
}

core::Expected<core::Unit, core::AnyError> FirecrackerSandboxBackend::Start() {
    const core::Expected<core::Unit, core::AnyError> started = StartNowait();
    if (!started.ok()) return started;
    return WaitForReady();
}
SandboxCaptureResult<std::string> FirecrackerSandboxBackend::Snapshot() {
    return core::make_unexpected(
        SandboxCaptureError::Recoverable("snapshot: not available in this build"));
}
SandboxCaptureResult<core::Unit> FirecrackerSandboxBackend::SnapshotVolumes() {
    return core::make_unexpected(
        SandboxCaptureError::Recoverable("snapshot_volumes: not available in this build"));
}
SandboxCaptureResult<core::Unit> FirecrackerSandboxBackend::FreezeAndSnapshotVolumes() {
    return core::make_unexpected(SandboxCaptureError::Recoverable(
        "freeze_and_snapshot_volumes: not available in this build"));
}
core::Expected<core::Unit, core::AnyError> FirecrackerSandboxBackend::ThawVolumes() {
    return core::make_unexpected(NotAvailable("thaw_volumes"));
}
SandboxCaptureResult<std::vector<SandboxForkResult> >
FirecrackerSandboxBackend::Fork(const std::vector<SandboxForkSpec>& /*specs*/) {
    return core::make_unexpected(
        SandboxCaptureError::Recoverable("fork: not available in this build"));
}
core::Optional<std::string> FirecrackerSandboxBackend::HostInteractionIp() const {
    // No network slot is allocated by this port, so there is no address to
    // report. Rust returns `None` in the same situation.
    return core::nullopt;
}
RuntimeArtifactSet FirecrackerSandboxBackend::StartupArtifacts() const {
    return FirecrackerPausedState(impl_->snapshot_config).RuntimeArtifacts();
}
core::Expected<core::Unit, core::AnyError>
FirecrackerSandboxBackend::UpdateNetworkPolicy(const core::Optional<std::string>& /*policy*/) {
    return core::make_unexpected(NotAvailable("update_network_policy"));
}
void FirecrackerSandboxBackend::UpdateCustomExtensionParams(
    const core::Optional<std::string>& /*params*/) {
    // Rust's signature returns nothing, so there is no way to report that this
    // is unported. Left as a no-op deliberately: the params only affect a
    // later capture, and every capture path in this build already fails.
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
