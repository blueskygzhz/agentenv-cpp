// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/ctrl.rs — UVMUblkCtrl + the control-plane commands.
//
// Every command here is an `IORING_OP_URING_CMD` submitted on
// /dev/ublk-control, which is why this sits on top of `storage/util/io_ring`.
// The ring must have been created with `sqe128`: a `ublksrv_ctrl_cmd` is 32
// bytes and a 64-byte SQE has room for 16.
#ifndef AGENTENV_STORAGE_UBLK_CTRL_H_
#define AGENTENV_STORAGE_UBLK_CTRL_H_

#include <cstdint>
#include <memory>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/storage/ublk/abi.h"
#include "agentenv/storage/util/io_ring.h"

namespace agentenv {
namespace storage {
namespace ublk {

/// Rust `ublk_module_loaded()`.
bool UblkModuleLoaded();

/// Rust `ublk_available()` — the module is loaded (or could be loaded) and the
/// control node exists.
///
/// Attempts a silent `modprobe` as a side effect, as Rust does: the common
/// case on a fresh host is a module that is simply not loaded yet, and failing
/// the first sandbox create over that would be gratuitous.
bool UblkAvailable();

/// Rust `load_ublk_module()` — `modprobe ublk_drv`, then `sudo -n modprobe`.
///
/// The error names both the manual command and the setup subcommand, because
/// this is the one failure an operator can always fix themselves.
core::Expected<core::Unit, std::string> LoadUblkModule();

/// Rust `ctrl_open_error` — opening the control node, with the failure
/// explained rather than passed through.
///
/// `ENOENT` with no module loaded, `ENOENT` with one, and `EACCES` are three
/// different problems with three different fixes, and a bare "No such file or
/// directory" distinguishes none of them.
core::Expected<int, std::string> OpenCtrlFile();

/// Rust `UVMUblkCtrl` — the control channel for one ublk device.
class UblkCtrl {
 public:
    ~UblkCtrl();
    UblkCtrl(const UblkCtrl&) = delete;
    UblkCtrl& operator=(const UblkCtrl&) = delete;

    /// Rust `UBLK_U_CMD_GET_FEATURES` — the driver's feature bitmap.
    ///
    /// Device-independent, so `queue_id` is 0xFFFF and no device need exist.
    core::Expected<uint64_t, std::string> GetFeatures();

    /// Rust `UBLK_U_CMD_ADD_DEV`.
    ///
    /// With `dev_id` left at `u32::MAX` the kernel assigns one and writes it
    /// back into `DevInfo()`, so the caller must read the id from there rather
    /// than from what it asked for.
    core::Expected<core::Unit, std::string> AddDev();

    /// Rust `UBLK_U_CMD_GET_DEV_INFO` — refresh `DevInfo()` from the kernel.
    ///
    /// The only way to read `state`: the field is kernel-owned, and the local
    /// copy is stale the moment the device is started or stopped elsewhere.
    core::Expected<core::Unit, std::string> GetDevInfo();

    /// Rust `UBLK_U_CMD_SET_PARAMS` — the device's geometry.
    ///
    /// Must precede `StartDev`: the parameters describe the disk the kernel is
    /// about to publish, and there is no way to change them once /dev/ublkbN
    /// exists.
    core::Expected<core::Unit, std::string> SetParams(const abi::Params& params);

    /// Rust `UBLK_U_CMD_START_DEV` — publish /dev/ublkbN.
    ///
    /// Carries this process's pid, which the kernel records as the server: it
    /// uses it to tear the device down if the server dies, so a wrong pid
    /// leaves an orphan device that nothing will clean up.
    ///
    /// Retries on `EINTR`. The command is idempotent in that a signal arriving
    /// before the kernel acts means nothing happened, so re-sending starts the
    /// device once.
    core::Expected<core::Unit, std::string> StartDev();

    /// Rust `UBLK_U_CMD_STOP_DEV`. Does *not* retry on `EINTR`, matching Rust.
    core::Expected<core::Unit, std::string> StopDev();

    /// Rust `UBLK_U_CMD_DEL_DEV` — release the device id.
    ///
    /// Retries on `EINTR`, as Rust does: the id leaks for the lifetime of the
    /// module if the delete is abandoned, and no later run can tell an
    /// abandoned id from one in use.
    core::Expected<core::Unit, std::string> DelDev();

    /// Rust `UBLK_U_CMD_UPDATE_SIZE` — resize a live device, in 512-byte
    /// sectors.
    ///
    /// Fails early when `kFUpdateSize` is absent from `features`: an older
    /// kernel answers the unknown command with `EOPNOTSUPP`, which the caller
    /// cannot tell from a device that refused the new size.
    core::Expected<core::Unit, std::string> UpdateSize(uint64_t new_sectors,
                                                       uint64_t features);

    /// Rust `get_cdev_path()` — `/dev/ublkcN`, the queue-side char device.
    std::string CharDevPath() const;
    /// `/dev/ublkbN`, the block device `StartDev` publishes.
    std::string BlockDevPath() const;

    const abi::CtrlDevInfo& DevInfo() const { return dev_info_; }
    uint32_t DevId() const { return dev_info_.dev_id; }
    const std::string& Name() const { return name_; }

 private:
    friend class UblkCtrlBuilder;
    UblkCtrl();

    /// Submits one control command and waits for it.
    ///
    /// `res` is an errno, not a byte count: these commands transfer through
    /// `addr`, so a non-zero result is always a failure. Returned with the
    /// command named -- a bare errno from a control channel carrying eight
    /// different commands is nearly unattributable.
    core::Expected<core::Unit, std::string> Issue(const char* what, uint32_t cmd_op,
                                                  const abi::CtrlCmd& cmd,
                                                  bool retry_on_eintr);

    std::string name_;
    abi::CtrlDevInfo dev_info_;
    int ctrl_fd_;
    std::shared_ptr<util::Ring> ring_;
};

/// Rust `UVMUblkCtrlBuilder`.
class UblkCtrlBuilder {
 public:
    UblkCtrlBuilder();

    UblkCtrlBuilder& Name(const std::string& name);
    UblkCtrlBuilder& NrQueues(uint16_t n);
    /// Per-queue IO depth.
    UblkCtrlBuilder& Depth(uint16_t d);
    UblkCtrlBuilder& MaxIoBufBytes(uint32_t bytes);
    /// Rust `zero_copy` — sets or clears `UBLK_F_AUTO_BUF_REG`.
    UblkCtrlBuilder& ZeroCopy(bool enabled);
    /// Ask for a specific id. Left unset, the kernel assigns one.
    UblkCtrlBuilder& DevId(uint32_t id);

    /// Rust `build(ring)`.
    ///
    /// Loads the module and opens the control node, so a failure here is a
    /// host problem rather than a device one. Requires an `sqe128` ring: a
    /// 32-byte `ublksrv_ctrl_cmd` does not fit a 64-byte SQE, and the
    /// submission would be refused one command later with a message about SQE
    /// sizes rather than about ublk.
    core::Expected<std::unique_ptr<UblkCtrl>, std::string> Build(
        std::shared_ptr<util::Ring> ring);

    /// The `ublksrv_ctrl_dev_info` this builder would send on ADD_DEV.
    ///
    /// Separate from `Build` because it is the half that needs no kernel:
    /// Rust fills its `dev_info` and opens the control file as two
    /// independent steps, and keeping them apart here means the flag and
    /// geometry projection can be checked on a host with no ublk module --
    /// which is also where getting it wrong is cheapest to catch.
    abi::CtrlDevInfo DevInfoProjection() const;

    /// Rust `get_cdev_path`, before a device exists.
    ///
    /// Takes the id rather than reading one off a device, so the two node
    /// names can be checked without one: `ublkc` is the queue-side char
    /// device the data plane opens and `ublkb` is the block device the kernel
    /// publishes, and confusing them yields a successful open and no I/O.
    static std::string CharDevPathFor(uint32_t dev_id);
    static std::string BlockDevPathFor(uint32_t dev_id);

 private:
    std::string name_;
    uint16_t nr_queues_;
    uint16_t depth_;
    uint32_t io_buf_bytes_;
    uint64_t ctrl_flags_;
    uint32_t dev_id_;
};

/// Rust `build_ublk_params`, lifted out of the overlaybd target.
///
/// Here because it is the only correct way to fill `abi::Params` -- `len` must
/// equal the struct size and `types` must match the members that were set, and
/// both are easy to get wrong at a call site.
abi::Params BuildParams(uint64_t dev_sectors, uint8_t logical_bs_shift,
                        uint8_t physical_bs_shift, uint32_t max_io_buf_bytes,
                        bool discard_supported);

/// Rust `DEFAULT_PHYSICAL_BS_SHIFT` — 4 KiB.
const uint8_t kDefaultPhysicalBsShift = 12;
/// Rust `DEFAULT_IO_OPT_SHIFT`, defined as equal to the physical block shift.
const uint8_t kDefaultIoOptShift = kDefaultPhysicalBsShift;

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_CTRL_H_
