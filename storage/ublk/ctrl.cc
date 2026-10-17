// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/ctrl.rs
#include "agentenv/storage/ublk/ctrl.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>

namespace agentenv {
namespace storage {
namespace ublk {

namespace abi {
const char kCtrlPath[] = "/dev/ublk-control";
const char kModulePath[] = "/sys/module/ublk_drv";
}  // namespace abi

namespace {

// The kernel reads and writes these, so a field at the wrong offset is a wrong
// device id or buffer address at runtime rather than a compile error. Sizes
// are from the uapi header this file deliberately does not include.
// NOLINTBEGIN
static_assert(sizeof(abi::CtrlCmd) == 32, "ublksrv_ctrl_cmd must be 32 bytes");
static_assert(sizeof(abi::CtrlDevInfo) == 64, "ublksrv_ctrl_dev_info must be 64 bytes");
static_assert(sizeof(abi::ParamBasic) == 40, "ublk_param_basic must be 40 bytes");
static_assert(sizeof(abi::ParamDiscard) == 24, "ublk_param_discard must be 24 bytes");
static_assert(sizeof(abi::ParamDevt) == 16, "ublk_param_devt must be 16 bytes");
static_assert(sizeof(abi::ParamZoned) == 32, "ublk_param_zoned must be 32 bytes");
static_assert(sizeof(abi::IoDesc) == 24, "ublksrv_io_desc must be 24 bytes");
// `Params` is what SET_PARAMS sends, and its `len` field carries this number.
// The kernel compares it against its own struct size and refuses a mismatch,
// so a silent change to any member above turns into a rejected SET_PARAMS
// rather than a device with the wrong geometry -- but only if the total stays
// what the kernel expects. 8 header bytes plus 40 + 24 + 16 + 32.
static_assert(sizeof(abi::Params) == 120, "ublk_params must be 120 bytes");
// The command number encodes the argument size, so this is what keeps
// `Iowr()` producing the numbers the kernel recognises.
static_assert(sizeof(abi::CtrlCmd) == abi::kCtrlCmdSize, "kCtrlCmdSize must match the struct");
// NOLINTEND

bool PathExists(const char* path) {
    struct stat st;
    return ::stat(path, &st) == 0;
}

/// Rust `run_command` — true only on a clean exit 0.
///
/// Deliberately quiet: this runs on the normal path when the module simply is
/// not loaded yet, and `modprobe` failing for a user without privileges is
/// expected rather than notable. The caller reports the aggregate failure.
bool RunCommand(const char* program, const char* const argv[]) {
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // Output discarded: a permission error from `modprobe` would otherwise
        // land in the server's log on every start of a host where the module
        // is loaded by other means.
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::execvp(program, const_cast<char* const*>(argv));
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/// Rust `MODPROBE_ATTEMPTS` — bare `modprobe`, then non-interactive sudo.
///
/// `-n` on the sudo attempt matters: without it a host with a password prompt
/// would block the server on stdin nobody is watching.
bool TryLoadModule() {
    const char* const plain[] = {"modprobe", "ublk_drv", NULL};
    if (RunCommand("modprobe", plain) && UblkModuleLoaded()) return true;
    const char* const with_sudo[] = {"sudo", "-n", "modprobe", "ublk_drv", NULL};
    if (RunCommand("sudo", with_sudo) && UblkModuleLoaded()) return true;
    return false;
}

std::string Errno(const std::string& what, int err) {
    std::ostringstream os;
    os << what << ": " << std::strerror(err) << " (errno " << err << ")";
    return os.str();
}

}  // namespace

bool UblkModuleLoaded() {
    return PathExists(abi::kModulePath);
}

bool UblkAvailable() {
    if (!UblkModuleLoaded()) {
        TryLoadModule();
    }
    // Both conditions: a loaded module with no control node is a host where
    // devtmpfs has not caught up or the node was removed, and a control node
    // with no module cannot answer anything.
    return UblkModuleLoaded() && PathExists(abi::kCtrlPath);
}

core::Expected<core::Unit, std::string> LoadUblkModule() {
    if (UblkModuleLoaded()) return core::Unit();
    if (TryLoadModule()) return core::Unit();
    // Names the fix. This is the one failure here an operator can always
    // resolve themselves, and a bare "not loaded" sends them reading code.
    return core::make_unexpected(std::string(
        "ublk kernel module is not loaded and automatic loading failed. "
        "Try `sudo modprobe ublk_drv`, or run the server with --setup-only."));
}

core::Expected<int, std::string> OpenCtrlFile() {
    const int fd = ::open(abi::kCtrlPath, O_RDWR | O_CLOEXEC);
    if (fd >= 0) return fd;

    const int err = errno;
    // Rust `ctrl_open_error`. Three different problems with three different
    // fixes, which a passed-through "No such file or directory" conflates.
    std::ostringstream why;
    why << "failed to open " << abi::kCtrlPath << ": ";
    if (err == ENOENT && !UblkModuleLoaded()) {
        why << "the ublk kernel module may not be loaded. "
               "Try `sudo modprobe ublk_drv`, or run the server with --setup-only";
    } else if (err == ENOENT) {
        why << "the ublk control device is missing even though the module is loaded. "
               "Ensure the device node is present, then run the server with --setup-only";
    } else if (err == EACCES || err == EPERM) {
        why << "permission denied. Ensure the current user has access to the ublk "
               "control device, or run the server with --setup-only to refresh permissions";
    } else {
        why << std::strerror(err) << " (errno " << err << ")";
    }
    return core::make_unexpected(why.str());
}

// ---------------------------------------------------------------------------
// UblkCtrl
// ---------------------------------------------------------------------------

UblkCtrl::UblkCtrl() : ctrl_fd_(-1) {
    std::memset(&dev_info_, 0, sizeof(dev_info_));
}

UblkCtrl::~UblkCtrl() {
    if (ctrl_fd_ >= 0) ::close(ctrl_fd_);
}

core::Expected<core::Unit, std::string> UblkCtrl::Issue(const char* what, uint32_t cmd_op,
                                                        const abi::CtrlCmd& cmd,
                                                        bool retry_on_eintr) {
    while (true) {
        util::SubmissionEntry sqe =
            util::SubmissionEntry::UringCmd(ctrl_fd_, cmd_op, &cmd, sizeof(cmd));
        // The token is unused -- one command is in flight at a time on this
        // channel -- but it still identifies the completion as this command's
        // rather than a stray one left on a shared ring.
        sqe.UserData(static_cast<uint64_t>(cmd_op));

        const core::Expected<uint32_t, std::string> sent = ring_->SubmitAndWait(sqe, 1);
        if (!sent.ok()) {
            return core::make_unexpected(std::string("send ") + what + " ctrl cmd to ring: " +
                                         sent.error());
        }
        const core::Expected<std::vector<util::CompletionEntry>, std::string> done =
            ring_->WaitAndReap(1);
        if (!done.ok()) {
            return core::make_unexpected(std::string("await ") + what + " ctrl cmd: " +
                                         done.error());
        }
        if (done.value().empty()) {
            return core::make_unexpected(std::string(what) + " ctrl cmd: no completion");
        }

        // `res` is an errno here, never a byte count: these commands transfer
        // through `addr`, so anything non-zero is a failure.
        const int32_t res = done.value()[0].res;
        if (res == 0) return core::Unit();
        if (res == -EINTR && retry_on_eintr) {
            // A signal arriving before the kernel acted means nothing
            // happened, so re-sending performs the operation once.
            continue;
        }
        return core::make_unexpected(Errno(std::string(what) + " ctrl request", -res));
    }
}

core::Expected<uint64_t, std::string> UblkCtrl::GetFeatures() {
    uint64_t features = 0;
    abi::CtrlCmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.addr = reinterpret_cast<uint64_t>(&features);
    cmd.len = static_cast<uint16_t>(sizeof(features));
    // Driver-wide rather than device-scoped, so no device id is set.
    cmd.queue_id = 0xFFFF;

    const core::Expected<core::Unit, std::string> r =
        Issue("get_features", abi::CmdGetFeatures(), cmd, false);
    if (!r.ok()) return core::make_unexpected(r.error());
    return features;
}

core::Expected<core::Unit, std::string> UblkCtrl::AddDev() {
    abi::CtrlCmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    // The kernel fills `dev_info_` in place, including the id it assigned when
    // one was not requested -- which is why this points at the member rather
    // than at a local.
    cmd.addr = reinterpret_cast<uint64_t>(&dev_info_);
    cmd.len = static_cast<uint16_t>(sizeof(dev_info_));
    // ADD_DEV requires `cmd.dev_id == dev_info.dev_id`; the kernel checks it.
    cmd.dev_id = dev_info_.dev_id;
    // And it explicitly checks `queue_id == (u16)-1`.
    cmd.queue_id = 0xFFFF;

    return Issue("add_dev", abi::CmdAddDev(), cmd, false);
}

core::Expected<core::Unit, std::string> UblkCtrl::GetDevInfo() {
    abi::CtrlCmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.addr = reinterpret_cast<uint64_t>(&dev_info_);
    cmd.len = static_cast<uint16_t>(sizeof(dev_info_));
    cmd.dev_id = dev_info_.dev_id;
    return Issue("get_dev_info", abi::CmdGetDevInfo(), cmd, false);
}

core::Expected<core::Unit, std::string> UblkCtrl::SetParams(const abi::Params& params) {
    abi::CtrlCmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.addr = reinterpret_cast<uint64_t>(&params);
    cmd.len = static_cast<uint16_t>(sizeof(params));
    cmd.dev_id = dev_info_.dev_id;
    return Issue("set_params", abi::CmdSetParams(), cmd, false);
}

core::Expected<core::Unit, std::string> UblkCtrl::StartDev() {
    abi::CtrlCmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    // The kernel records this as the serving process and tears the device down
    // if it dies. A wrong pid leaves a device nothing will clean up.
    cmd.data[0] = static_cast<uint64_t>(::getpid());
    cmd.dev_id = dev_info_.dev_id;
    cmd.queue_id = 0xFFFF;
    return Issue("start_dev", abi::CmdStartDev(), cmd, true);
}

core::Expected<core::Unit, std::string> UblkCtrl::StopDev() {
    abi::CtrlCmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.dev_id = dev_info_.dev_id;
    cmd.queue_id = 0xFFFF;
    // No EINTR retry, matching Rust: a stop that was interrupted leaves the
    // device live, and the caller's own teardown path reaches here again.
    return Issue("stop_dev", abi::CmdStopDev(), cmd, false);
}

core::Expected<core::Unit, std::string> UblkCtrl::DelDev() {
    abi::CtrlCmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.dev_id = dev_info_.dev_id;
    cmd.queue_id = 0xFFFF;
    // Retried, unlike stop: abandoning the delete leaks the device id for the
    // module's lifetime, and nothing later can distinguish a leaked id from
    // one legitimately in use.
    return Issue("del_dev", abi::CmdDelDev(), cmd, true);
}

core::Expected<core::Unit, std::string> UblkCtrl::UpdateSize(uint64_t new_sectors,
                                                             uint64_t features) {
    // Checked before sending. A kernel without the feature answers the unknown
    // command with EOPNOTSUPP, which reads identically to a device refusing
    // the size -- and the caller would retry the wrong thing.
    if ((features & abi::kFUpdateSize) == 0) {
        return core::make_unexpected(std::string(
            "update_size: this kernel does not support UBLK_F_UPDATE_SIZE"));
    }
    abi::CtrlCmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.dev_id = dev_info_.dev_id;
    cmd.queue_id = 0xFFFF;
    // Sectors, not bytes: the kernel's unit here is 512 bytes regardless of
    // the device's logical block size.
    cmd.data[0] = new_sectors;
    return Issue("update_size", abi::CmdUpdateSize(), cmd, false);
}

std::string UblkCtrl::CharDevPath() const {
    std::ostringstream os;
    os << "/dev/ublkc" << dev_info_.dev_id;
    return os.str();
}

std::string UblkCtrl::BlockDevPath() const {
    std::ostringstream os;
    os << "/dev/ublkb" << dev_info_.dev_id;
    return os.str();
}

// ---------------------------------------------------------------------------
// UblkCtrlBuilder
// ---------------------------------------------------------------------------

UblkCtrlBuilder::UblkCtrlBuilder()
    : name_("unknown"), nr_queues_(1), depth_(16),
      // Rust's default: 256 KiB.
      io_buf_bytes_(256 * 1024), ctrl_flags_(0),
      // Rust's `u32::MAX` — "kernel, pick one".
      dev_id_(0xFFFFFFFFu) {}

UblkCtrlBuilder& UblkCtrlBuilder::Name(const std::string& name) {
    name_ = name;
    return *this;
}
UblkCtrlBuilder& UblkCtrlBuilder::NrQueues(uint16_t n) {
    nr_queues_ = n;
    return *this;
}
UblkCtrlBuilder& UblkCtrlBuilder::Depth(uint16_t d) {
    depth_ = d;
    return *this;
}
UblkCtrlBuilder& UblkCtrlBuilder::MaxIoBufBytes(uint32_t bytes) {
    io_buf_bytes_ = bytes;
    return *this;
}
UblkCtrlBuilder& UblkCtrlBuilder::ZeroCopy(bool enabled) {
    // Cleared as well as set, so a builder reused with `false` does not keep
    // a flag an earlier call turned on.
    if (enabled) {
        ctrl_flags_ |= abi::kFAutoBufReg;
    } else {
        ctrl_flags_ &= ~abi::kFAutoBufReg;
    }
    return *this;
}
UblkCtrlBuilder& UblkCtrlBuilder::DevId(uint32_t id) {
    dev_id_ = id;
    return *this;
}

abi::CtrlDevInfo UblkCtrlBuilder::DevInfoProjection() const {
    abi::CtrlDevInfo info;
    std::memset(&info, 0, sizeof(info));
    info.nr_hw_queues = nr_queues_;
    info.queue_depth = depth_;
    info.max_io_buf_bytes = io_buf_bytes_;
    info.dev_id = dev_id_;
    info.flags = ctrl_flags_;
    // Set at build time, as Rust does, so ADD_DEV already carries it. The
    // kernel uses it to attribute the device to this process.
    info.ublksrv_pid = static_cast<int32_t>(::getpid());
    // `state` is kernel-owned and stays zero; sending anything here would be
    // claiming a state this process does not get to assert.
    return info;
}

std::string UblkCtrlBuilder::CharDevPathFor(uint32_t dev_id) {
    std::ostringstream os;
    os << "/dev/ublkc" << dev_id;
    return os.str();
}

std::string UblkCtrlBuilder::BlockDevPathFor(uint32_t dev_id) {
    std::ostringstream os;
    os << "/dev/ublkb" << dev_id;
    return os.str();
}

core::Expected<std::unique_ptr<UblkCtrl>, std::string> UblkCtrlBuilder::Build(
    std::shared_ptr<util::Ring> ring) {
    if (!ring) {
        return core::make_unexpected(std::string("ublk ctrl: null io_uring"));
    }
    // Refused here rather than at the first command. Every control command
    // carries a 32-byte `ublksrv_ctrl_cmd` and a 64-byte SQE holds 16, so the
    // failure is certain -- and reported one layer down it would read as a
    // complaint about SQE sizes rather than about how this ring was made.
    if (!ring->Sqe128()) {
        return core::make_unexpected(std::string(
            "ublk ctrl: needs an sqe128 io_uring (a 32-byte ublksrv_ctrl_cmd "
            "does not fit a 64-byte SQE)"));
    }

    const core::Expected<core::Unit, std::string> loaded = LoadUblkModule();
    if (!loaded.ok()) return core::make_unexpected(loaded.error());

    const core::Expected<int, std::string> fd = OpenCtrlFile();
    if (!fd.ok()) return core::make_unexpected(fd.error());

    std::unique_ptr<UblkCtrl> ctrl(new UblkCtrl());
    ctrl->name_ = name_;
    ctrl->ctrl_fd_ = fd.value();
    ctrl->ring_ = ring;
    ctrl->dev_info_ = DevInfoProjection();

    return std::move(ctrl);
}

// ---------------------------------------------------------------------------
// params
// ---------------------------------------------------------------------------

abi::Params BuildParams(uint64_t dev_sectors, uint8_t logical_bs_shift,
                        uint8_t physical_bs_shift, uint32_t max_io_buf_bytes,
                        bool discard_supported) {
    const uint32_t logical_block_size = 1u << logical_bs_shift;

    abi::Params p;
    std::memset(&p, 0, sizeof(p));
    // Both required. The kernel rejects a `len` that disagrees with its own
    // struct size, which is how an ABI mismatch surfaces at SET_PARAMS rather
    // than as a device with garbage geometry.
    p.len = static_cast<uint32_t>(sizeof(p));
    p.types = abi::kParamTypeBasic | (discard_supported ? abi::kParamTypeDiscard : 0u);

    p.basic.attrs = 0;
    p.basic.logical_bs_shift = logical_bs_shift;
    p.basic.physical_bs_shift = physical_bs_shift;
    p.basic.io_min_shift = logical_bs_shift;
    // `max` of the two: an io_opt below the physical block size would advertise
    // an optimal transfer smaller than the device's own write granularity.
    p.basic.io_opt_shift = kDefaultIoOptShift > physical_bs_shift ? kDefaultIoOptShift
                                                                  : physical_bs_shift;
    // In logical blocks, not sectors: this bounds one request against the
    // buffer the queue actually has for it.
    p.basic.max_sectors = max_io_buf_bytes >> logical_bs_shift;
    p.basic.dev_sectors = dev_sectors;

    if (discard_supported) {
        p.discard.discard_alignment = logical_block_size;
        p.discard.discard_granularity = logical_block_size;
        p.discard.max_discard_sectors = max_io_buf_bytes >> logical_bs_shift;
        // Zero: write-zeroes is not implemented, and advertising it would have
        // the kernel send requests the target answers with an error.
        p.discard.max_write_zeroes_sectors = 0;
        p.discard.max_discard_segments = 1;
    }
    // `devt` is kernel-filled and `zoned` unused, so both stay zeroed -- with
    // their bits absent from `types`, the kernel ignores them.
    return p;
}

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
