// SPDX-License-Identifier: MIT
// The io_uring kernel ABI, as used by the ublk data plane.
//
// Declared here rather than taken from <linux/io_uring.h> on purpose. The
// features this port needs -- `IORING_SETUP_SQE128` and `IORING_OP_URING_CMD`,
// which are how ublk commands are issued at all -- appeared in 5.19 and 5.6,
// so a build host with older kernel headers could not compile against them
// even though the target host runs them fine. The ABI is stable and additive,
// so a private copy is the portable choice; liburing ships one for the same
// reason.
//
// Layout is asserted against the documented offsets in `io_ring.cc`: these
// structs are read and written by the kernel, so a field at the wrong offset
// is not a compile error, it is a wrong fd or a wrong length handed to the
// kernel at runtime.
#ifndef AGENTENV_STORAGE_UTIL_IO_URING_ABI_H_
#define AGENTENV_STORAGE_UTIL_IO_URING_ABI_H_

#include <cstdint>

namespace agentenv {
namespace storage {
namespace util {
namespace uring_abi {

// ---- syscall numbers ------------------------------------------------------
//
// The same on every architecture: io_uring was added after the kernel moved
// to a shared syscall table for new calls, so these are not per-arch.
const long kSysIoUringSetup = 425;
const long kSysIoUringEnter = 426;
const long kSysIoUringRegister = 427;

// ---- mmap offsets ---------------------------------------------------------

const uint64_t kOffSqRing = 0;
const uint64_t kOffCqRing = 0x8000000ull;
const uint64_t kOffSqes = 0x10000000ull;

// ---- setup flags ----------------------------------------------------------

/// A caller-chosen CQ size, independent of the SQ size.
const uint32_t kSetupCqSize = 1u << 3;
/// Skip the inter-processor interrupt when completing work on another CPU.
/// Safe only because this ring is driven by one thread that polls it.
const uint32_t kSetupCoopTaskrun = 1u << 8;
/// 128-byte SQEs. Required for `IORING_OP_URING_CMD` payloads longer than the
/// 16 bytes a 64-byte SQE leaves free -- which is every ublk command.
const uint32_t kSetupSqe128 = 1u << 10;
const uint32_t kSetupCqe32 = 1u << 11;
/// Promises the kernel that only one task submits, letting it skip the
/// locking it would otherwise need. Violating it is undefined, not merely
/// slow, which is why submission is confined to the ring's own thread.
const uint32_t kSetupSingleIssuer = 1u << 12;

// ---- sqe flags ------------------------------------------------------------

/// `IOSQE_FIXED_FILE` — `fd` is an index into the registered-file table.
const uint8_t kSqeFixedFile = 1u << 0;

// ---- features -------------------------------------------------------------

/// The SQ and CQ rings live in one mapping, so the second `mmap` is skipped
/// and the CQ is addressed through the SQ's base pointer.
const uint32_t kFeatSingleMmap = 1u << 0;

// ---- enter flags ----------------------------------------------------------

const uint32_t kEnterGetEvents = 1u << 0;

// ---- opcodes --------------------------------------------------------------

const uint8_t kOpNop = 0;
const uint8_t kOpFsync = 3;
const uint8_t kOpReadFixed = 4;
const uint8_t kOpWriteFixed = 5;
const uint8_t kOpRead = 22;
const uint8_t kOpWrite = 23;
/// The opcode every ublk control and queue command rides on.
const uint8_t kOpUringCmd = 46;

// ---- register opcodes -----------------------------------------------------

const uint32_t kRegisterFiles = 2;
const uint32_t kRegisterFilesUpdate = 6;
/// Sparse variants: reserve a table of N empty slots to be filled in later.
/// The ublk device registers its fds after the ring exists, so the table has
/// to be sized before any of them are known.
const uint32_t kRegisterFiles2 = 13;
const uint32_t kRegisterBuffers2 = 15;
const uint32_t kRegisterBuffersUpdate = 16;

/// `IORING_RSRC_REGISTER_SPARSE`.
const uint32_t kRsrcRegisterSparse = 1u << 0;

// ---- structures -----------------------------------------------------------

/// `struct io_uring_sqe`. 64 bytes; with `kSetupSqe128` the kernel reads 128
/// and the trailing 64 extend `cmd`.
///
/// The unions of the kernel header are flattened to the one member this port
/// uses, keeping the offsets identical. Anonymous unions would be the closer
/// transcription but are not C++11.
struct Sqe {
    uint8_t  opcode;        ///< @0
    uint8_t  flags;         ///< @1
    uint16_t ioprio;        ///< @2
    int32_t  fd;            ///< @4
    /// @8 — `off` for read/write; for `kOpUringCmd` the low half is `cmd_op`.
    uint64_t off_or_cmd_op;
    uint64_t addr;          ///< @16
    uint32_t len;           ///< @24
    uint32_t rw_flags;      ///< @28 — op-specific flags union
    uint64_t user_data;     ///< @32
    uint16_t buf_index;     ///< @40
    uint16_t personality;   ///< @42
    int32_t  splice_fd_in;  ///< @44 — also `file_index`
    /// @48 — `addr3` + padding in the read/write form; for `kOpUringCmd` this
    /// is where the command payload starts. 16 bytes in a 64-byte SQE, 80 in
    /// a 128-byte one, which is the whole reason `kSetupSqe128` is required.
    uint8_t  cmd[16];
};

/// The command payload of a 128-byte SQE: `cmd` extended by the second half.
const uint32_t kSqe128CmdBytes = 80;

/// `struct io_uring_cqe`.
struct Cqe {
    uint64_t user_data;
    int32_t  res;
    uint32_t flags;
};

/// `struct io_sqring_offsets`.
struct SqRingOffsets {
    uint32_t head;
    uint32_t tail;
    uint32_t ring_mask;
    uint32_t ring_entries;
    uint32_t flags;
    uint32_t dropped;
    uint32_t array;
    uint32_t resv1;
    uint64_t user_addr;
};

/// `struct io_cqring_offsets`.
struct CqRingOffsets {
    uint32_t head;
    uint32_t tail;
    uint32_t ring_mask;
    uint32_t ring_entries;
    uint32_t overflow;
    uint32_t cqes;
    uint32_t flags;
    uint32_t resv1;
    uint64_t user_addr;
};

/// `struct io_uring_params`.
struct Params {
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t flags;
    uint32_t sq_thread_cpu;
    uint32_t sq_thread_idle;
    uint32_t features;
    uint32_t wq_fd;
    uint32_t resv[3];
    SqRingOffsets sq_off;
    CqRingOffsets cq_off;
};

/// `struct io_uring_rsrc_register`, for the sparse register opcodes.
struct RsrcRegister {
    uint32_t nr;
    uint32_t flags;
    uint64_t resv2;
    uint64_t data;
    uint64_t tags;
};

/// `struct io_uring_rsrc_update2`, for `kRegisterBuffersUpdate`.
///
/// The kernel wants `nr_args` to be this struct's *size*, not the number of
/// entries -- the count rides in `nr`. Passing the count instead is answered
/// with EINVAL and nothing more.
struct RsrcUpdate2 {
    uint32_t offset;
    uint32_t resv;
    uint64_t data;
    uint64_t tags;
    uint32_t nr;
    uint32_t resv2;
};

}  // namespace uring_abi
}  // namespace util
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UTIL_IO_URING_ABI_H_
