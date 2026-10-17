// SPDX-License-Identifier: MIT
// The ublk kernel ABI (Linux uapi `include/uapi/linux/ublk_cmd.h`).
//
// Rust gets these from the generated `ublk_sys` bindings. There is no such
// crate here, and <linux/ublk_cmd.h> only exists on build hosts whose kernel
// headers are 6.0 or newer, so the structs and command numbers are declared
// directly -- same reasoning as `io_uring_abi.h`.
//
// Every struct below is read or written by the kernel. A field at the wrong
// offset is not a compile error: it is a wrong device id, a wrong buffer
// address, or a disk advertised at the wrong size. The layout is asserted in
// `ctrl.cc` against the documented sizes.
#ifndef AGENTENV_STORAGE_UBLK_ABI_H_
#define AGENTENV_STORAGE_UBLK_ABI_H_

#include <cstdint>

namespace agentenv {
namespace storage {
namespace ublk {
namespace abi {

// ---- device nodes ---------------------------------------------------------

/// The control device every command below is issued against.
extern const char kCtrlPath[];
/// Presence of this directory is how the module's state is read; there is no
/// ioctl that answers "are you loaded".
extern const char kModulePath[];

// ---- command numbers ------------------------------------------------------
//
// `_IOWR('u', nr, struct ublksrv_ctrl_cmd)`. The size is part of the encoding,
// so a wrong `sizeof` here produces a command number the kernel does not
// recognise rather than a misread argument -- which is the better failure, and
// the reason the constant is derived rather than hardcoded.

/// `sizeof(struct ublksrv_ctrl_cmd)`.
const uint32_t kCtrlCmdSize = 32;

/// `_IOC(_IOC_READ|_IOC_WRITE, 'u', nr, size)`.
///
/// Expanded by hand because `_IOWR` lives in a header this file deliberately
/// does not include.
inline uint32_t Iowr(uint32_t nr) {
    return (3u << 30) | (kCtrlCmdSize << 16) | (static_cast<uint32_t>('u') << 8) | nr;
}

inline uint32_t CmdGetDevInfo()  { return Iowr(0x02); }
inline uint32_t CmdAddDev()      { return Iowr(0x04); }
inline uint32_t CmdDelDev()      { return Iowr(0x05); }
inline uint32_t CmdStartDev()    { return Iowr(0x06); }
inline uint32_t CmdStopDev()     { return Iowr(0x07); }
inline uint32_t CmdSetParams()   { return Iowr(0x09); }
inline uint32_t CmdGetFeatures() { return Iowr(0x13); }
inline uint32_t CmdUpdateSize()  { return Iowr(0x15); }

// ---- feature flags --------------------------------------------------------

/// `UBLK_F_SUPPORT_ZERO_COPY` — bit 0.
const uint64_t kFSupportZeroCopy = 1ull << 0;
/// `UBLK_F_UNPRIVILEGED_DEV` — bit 5.
const uint64_t kFUnprivilegedDev = 1ull << 5;
/// `UBLK_F_UPDATE_SIZE` — bit 10.
///
/// Not bit 0: that is zero-copy. Checked before `CmdUpdateSize` is sent,
/// because a kernel without it answers an unknown command with `EOPNOTSUPP`
/// and the caller cannot tell that from a device that refused to resize.
const uint64_t kFUpdateSize = 1ull << 10;
/// `UBLK_F_AUTO_BUF_REG` — bit 11. What `zero_copy(true)` sets.
const uint64_t kFAutoBufReg = 1ull << 11;

// ---- device states --------------------------------------------------------

const uint16_t kDevStateDead = 0;
const uint16_t kDevStateLive = 1;
const uint16_t kDevStateQuiesced = 2;

// ---- parameter types ------------------------------------------------------

const uint32_t kParamTypeBasic = 1u << 0;
const uint32_t kParamTypeDiscard = 1u << 1;
const uint32_t kParamTypeDevt = 1u << 2;
const uint32_t kParamTypeZoned = 1u << 3;

// ---- structures -----------------------------------------------------------

/// `struct ublksrv_ctrl_cmd` — the 32-byte payload of every control command.
///
/// Rust overlays this on an 80-byte array via a union before handing it to
/// `UringCmd80`; here it is memcpy'd into the SQE's `cmd` region, which comes
/// to the same bytes on the wire.
struct CtrlCmd {
    uint32_t dev_id;     ///< @0
    /// @4 — must be 0xFFFF for every device-scoped command. The kernel checks
    /// it explicitly on ADD_DEV, and a zero here would be read as "queue 0".
    uint16_t queue_id;
    uint16_t len;        ///< @6 — bytes at `addr`
    uint64_t addr;       ///< @8 — user buffer the kernel reads or fills
    uint64_t data[1];    ///< @16 — command-specific scalar
    uint16_t dev_path_len;  ///< @24
    uint16_t pad;        ///< @26
    uint32_t reserved;   ///< @28
};

/// `struct ublksrv_ctrl_dev_info` — 64 bytes.
struct CtrlDevInfo {
    uint16_t nr_hw_queues;      ///< @0
    uint16_t queue_depth;       ///< @2
    uint16_t state;             ///< @4 — kDevState*, filled by the kernel
    uint16_t pad0;              ///< @6
    uint32_t max_io_buf_bytes;  ///< @8
    /// @12 — `u32::MAX` on ADD_DEV asks the kernel to assign one, and it
    /// writes the assigned id back into this same field.
    uint32_t dev_id;
    int32_t  ublksrv_pid;       ///< @16
    uint32_t pad1;              ///< @20
    uint64_t flags;             ///< @24 — kF*
    uint64_t ublksrv_flags;     ///< @32
    uint32_t owner_uid;         ///< @40
    uint32_t owner_gid;         ///< @44
    uint64_t reserved1;         ///< @48
    uint64_t reserved2;         ///< @56
};

/// `struct ublk_param_basic` — 40 bytes.
struct ParamBasic {
    uint32_t attrs;
    uint8_t  logical_bs_shift;
    uint8_t  physical_bs_shift;
    uint8_t  io_opt_shift;
    uint8_t  io_min_shift;
    uint32_t max_sectors;
    uint32_t chunk_sectors;
    uint64_t dev_sectors;
    uint64_t virt_boundary_mask;
    uint64_t reserved;  // tail padding to the kernel's 40 bytes
};

/// `struct ublk_param_discard` — 24 bytes.
///
/// The trailing reserved words are 32-bit, not one 64-bit field: a `uint64_t`
/// here would raise the struct's alignment to 8 and pad it out to 32, which
/// shifts every member of `Params` that follows.
struct ParamDiscard {
    uint32_t discard_alignment;
    uint32_t discard_granularity;
    uint32_t max_discard_sectors;
    uint32_t max_write_zeroes_sectors;
    uint16_t max_discard_segments;
    uint16_t reserved0;
    uint32_t reserved1;
};

/// `struct ublk_param_devt` — 16 bytes. Filled by the kernel.
struct ParamDevt {
    uint32_t char_major;
    uint32_t char_minor;
    uint32_t disk_major;
    uint32_t disk_minor;
};

/// `struct ublk_param_zoned` — 32 bytes.
struct ParamZoned {
    uint32_t max_open_zones;
    uint32_t max_active_zones;
    uint32_t max_zone_append_sectors;
    uint8_t  reserved[20];
};

/// `struct ublk_params`.
///
/// `len` is the whole struct and `types` says which members are meaningful.
/// Both are required: the kernel rejects a `len` that does not match its own
/// idea of the struct, which is how an ABI mismatch is caught at SET_PARAMS
/// instead of becoming a device with garbage geometry.
struct Params {
    uint32_t len;
    uint32_t types;
    ParamBasic   basic;
    ParamDiscard discard;
    ParamDevt    devt;
    ParamZoned   zoned;
};

/// `struct ublksrv_io_desc` — one queued request, read out of the mapped
/// descriptor array rather than delivered by a syscall.
struct IoDesc {
    /// Packed: op in the low 8 bits, flags above. Accessors below.
    uint32_t op_flags;
    uint32_t nr_sectors;
    uint64_t start_sector;
    uint64_t addr;
};

const uint32_t kIoOpMask = 0xFF;

/// The mask is redundant with the narrowing cast -- `uint8_t` already keeps
/// only the low eight bits -- and is kept to state where the boundary is. The
/// flags accessor below is the one that needs it.
inline uint8_t IoDescOp(const IoDesc& d) {
    return static_cast<uint8_t>(d.op_flags & kIoOpMask);
}
inline uint32_t IoDescFlags(const IoDesc& d) {
    return d.op_flags & ~kIoOpMask;
}

// ---- io opcodes (`req_op` values the kernel passes through) ---------------

const uint8_t kIoOpRead = 0;
const uint8_t kIoOpWrite = 1;
const uint8_t kIoOpFlush = 2;
const uint8_t kIoOpDiscard = 3;
const uint8_t kIoOpWriteSame = 4;
const uint8_t kIoOpWriteZeroes = 5;

}  // namespace abi
}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_ABI_H_
