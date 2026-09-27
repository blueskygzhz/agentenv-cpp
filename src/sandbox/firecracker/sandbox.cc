// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs — Firecracker backend.
//
// A full driver needs a running Firecracker binary + KVM, so the API calls that
// require a live microVM remain unavailable in this environment. What *is* real
// here is the pre-boot phase: input validation and the jailer chroot path
// layout, faithfully following the first steps of the Rust `boot` path. Boot
// therefore fails with a precise, actionable error (missing binary / kernel /
// rootfs, or "requires KVM") instead of a blanket "not implemented".
#include "agentenv/sandbox/firecracker/sandbox.h"
#include "agentenv/sandbox/firecracker/config.h"

#include <sys/stat.h>

#include <string>

namespace agentenv {
namespace sandbox {
namespace firecracker {

class FirecrackerBackend::Impl {
 public:
    explicit Impl(Config c) : cfg(std::move(c)) {}
    Config cfg;

    // Rust: jailer lays out {chroot_base}/firecracker/{sandbox_id}/root .
    std::string ChrootPath(const core::SandboxId& id) const {
        return cfg.chroot_base_dir + "/firecracker/" + id.ToString() + "/root";
    }
    // The API socket lives inside the chroot when jailed.
    std::string ApiSocketPath(const core::SandboxId& id) const {
        return ChrootPath(id) + "/run/firecracker.socket";
    }
};

FirecrackerBackend::FirecrackerBackend(Config cfg)
    : impl_(new Impl(std::move(cfg))) {}
FirecrackerBackend::~FirecrackerBackend() = default;

namespace {
template <typename T>
std::future<core::Expected<T, core::AnyError>> ready_err(const std::string& msg) {
    std::promise<core::Expected<T, core::AnyError>> p;
    p.set_value(core::make_unexpected(core::err(msg)));
    return p.get_future();
}
template <typename T>
std::future<core::Expected<T, core::AnyError>> ready_ok(T value) {
    std::promise<core::Expected<T, core::AnyError>> p;
    p.set_value(core::Expected<T, core::AnyError>(std::move(value)));
    return p.get_future();
}
bool is_file(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
}  // namespace

std::future<core::Expected<Handle, core::AnyError>>
FirecrackerBackend::Boot(LaunchPlan plan) {
    // --- pre-boot validation (this part is real and testable) ---
    CommonConfig common;
    common.firecracker_bin = impl_->cfg.firecracker_bin;
    common.kernel_image_path = plan.kernel_path;
    auto v = ValidateCommonConfig(common);
    if (!v.ok()) {
        return ready_err<Handle>(std::string("firecracker boot: ") + v.error());
    }
    if (!plan.rootfs_path.empty() && !is_file(plan.rootfs_path)) {
        return ready_err<Handle>(std::string("firecracker boot: rootfs not found at ") +
                                 plan.rootfs_path);
    }
    // KVM is required to actually launch; we can only get this far without it.
    if (!exists("/dev/kvm")) {
        return ready_err<Handle>(std::string(
            "firecracker boot: /dev/kvm not present; cannot launch a microVM on this host"));
    }
    // The rest (spawn firecracker/jailer, drive the API socket to configure
    // machine/boot-source/drives, then start-instance) needs the binary + KVM.
    return ready_err<Handle>(std::string(
        "firecracker boot: pre-boot checks passed; live VM launch not available in this build "
        "(api socket would be ") + impl_->ApiSocketPath(plan.sandbox_id) + ")");
}

std::future<core::Expected<core::Unit, core::AnyError>>
FirecrackerBackend::Shutdown(core::SandboxId /*id*/) {
    return ready_err<core::Unit>("firecracker Shutdown: no live VM in this build");
}
std::future<core::Expected<core::Unit, core::AnyError>>
FirecrackerBackend::Pause(core::SandboxId /*id*/) {
    return ready_err<core::Unit>("firecracker Pause: no live VM in this build");
}
std::future<core::Expected<core::Unit, core::AnyError>>
FirecrackerBackend::Resume(core::SandboxId /*id*/) {
    return ready_err<core::Unit>("firecracker Resume: no live VM in this build");
}
std::future<core::Expected<std::string, core::AnyError>>
FirecrackerBackend::Snapshot(core::SandboxId /*id*/, const std::string& /*out_dir*/) {
    return ready_err<std::string>("firecracker Snapshot: no live VM in this build");
}
std::future<core::Expected<Handle, core::AnyError>>
FirecrackerBackend::Restore(LaunchPlan /*plan*/, const std::string& /*snapshot_dir*/) {
    return ready_err<Handle>("firecracker Restore: no live VM in this build");
}
std::future<core::Expected<ExecResult, core::AnyError>>
FirecrackerBackend::Exec(core::SandboxId /*id*/, ExecSpec /*spec*/) {
    return ready_err<ExecResult>("firecracker Exec: no live VM in this build");
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
