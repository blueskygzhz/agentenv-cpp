// SPDX-License-Identifier: MIT
// Rust: storage/util/src/io_ring/{uring,worker}.rs — AsyncIoRing + the worker
// thread that owns it.
//
// Rust gets its ring from the `io_uring` crate; there is no equivalent here,
// so the submission and completion queues are driven directly: `io_uring_setup`
// for the fd, `mmap` for the three regions, and the kernel's head/tail
// protocol for handing entries across. The ABI copy is in `io_uring_abi.h`.
//
// Why this exists at all: a ublk command is an `IORING_OP_URING_CMD` SQE, so
// every control and queue operation of the data plane is a submission on this
// ring. Nothing below it can be ported without it.
#ifndef AGENTENV_STORAGE_UTIL_IO_RING_H_
#define AGENTENV_STORAGE_UTIL_IO_RING_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/util/id_allocator.h"
#include "agentenv/storage/util/io_uring_abi.h"

namespace agentenv {
namespace storage {
namespace util {

/// Rust `AsyncIoRingBuilder` — the ring's shape, fixed before it is created.
///
/// The caps match Rust's, and each is a kernel limit rather than a policy
/// choice: exceeding one makes `io_uring_setup` fail with a bare `EINVAL`,
/// which says nothing about which field was wrong.
struct IoRingConfig {
    /// Rust `sqe_entries`, capped at 4096.
    uint32_t sq_entries;
    /// Rust `cqe_entries`, capped at 8192. Larger than the SQ on purpose: a
    /// full CQ makes the kernel drop completions, and a dropped completion is
    /// an I/O whose result is simply never reported.
    uint32_t cq_entries;
    /// Rust `setup_coop_taskrun` + `setup_single_issuer`.
    ///
    /// Both are promises about how the ring is used, not optimisations that
    /// can be toggled freely -- `single_issuer` in particular is undefined if
    /// a second thread submits, so it travels with the rule that only the
    /// owning thread calls `Submit`.
    bool coop_taskrun;
    bool single_issuer;
    /// 128-byte SQEs. Required for `IORING_OP_URING_CMD`, hence for ublk.
    bool sqe128;
    /// Rust `nr_sparse_file` — size of the fixed-file table, capped at 8192.
    /// Zero registers no table.
    uint32_t sparse_files;
    /// Rust `nr_sparse_buffer` — capped at 4096. The kernel's own ceiling is
    /// 1 << 14; Rust holds to the lower number and so does this.
    uint32_t sparse_buffers;

    IoRingConfig()
        : sq_entries(128), cq_entries(256), coop_taskrun(true),
          single_issuer(true), sqe128(false), sparse_files(0), sparse_buffers(0) {}
};

/// Rust `io_uring::squeue::Entry` / `Entry128` — one submission entry.
///
/// Carries the full 128-byte form; `Ring::Submit` copies 64 or 128 bytes
/// depending on how the ring was created. Keeping one type avoids a templated
/// ring whose only variation is a length.
class SubmissionEntry {
 public:
    SubmissionEntry();

    /// `IORING_OP_NOP`. Completes immediately with res 0; used to prove the
    /// ring is live without touching a file.
    static SubmissionEntry Nop();
    /// `IORING_OP_READ` / `IORING_OP_WRITE` on a raw fd.
    static SubmissionEntry Read(int fd, void* buf, uint32_t len, uint64_t offset);
    static SubmissionEntry Write(int fd, const void* buf, uint32_t len, uint64_t offset);
    /// `IORING_OP_READ_FIXED` / `_WRITE_FIXED` against a registered buffer.
    static SubmissionEntry ReadFixed(int fd, void* buf, uint32_t len, uint64_t offset,
                                     uint16_t buf_index);
    static SubmissionEntry WriteFixed(int fd, const void* buf, uint32_t len, uint64_t offset,
                                      uint16_t buf_index);
    static SubmissionEntry Fsync(int fd);

    /// Rust `UringCmd80` — an `IORING_OP_URING_CMD` carrying `cmd_bytes`.
    ///
    /// This is how every ublk command is sent. Fails to build (see
    /// `UringCmdNeedsSqe128`) rather than truncate when the payload does not
    /// fit the ring's SQE size: a silently cut command would reach the kernel
    /// as a valid-looking request with garbage in its tail fields.
    static SubmissionEntry UringCmd(int fd, uint32_t cmd_op, const void* cmd_bytes,
                                    std::size_t cmd_len);

    /// Rust `.user_data(...)` — the token echoed back in the completion.
    SubmissionEntry& UserData(uint64_t token);

    /// Rust `types::Fixed(idx)` — the `fd` field is an index into the
    /// registered-file table rather than a real descriptor.
    ///
    /// A separate flag because the kernel cannot tell the two apart from the
    /// value: fd 3 and fixed-slot 3 are both plausible, and without this the
    /// command goes to whatever file descriptor 3 happens to be.
    SubmissionEntry& UseFixedFile(bool fixed);

    /// Rust `override_sqe!(&mut sqe, addr, value)`.
    ///
    /// Only `UBLK_F_AUTO_BUF_REG` needs it: the buffer slot travels in the
    /// SQE's `addr` while the command payload's own `addr` must stay zero.
    SubmissionEntry& OverrideAddr(uint64_t addr);

    /// Bytes of `cmd` this entry actually uses; decides whether it needs a
    /// 128-byte SQE.
    std::size_t CmdLen() const { return cmd_len_; }

    const uring_abi::Sqe& Raw() const { return sqe_; }
    /// The second 64 bytes, meaningful only on an `sqe128` ring.
    const uint8_t* CmdTail() const { return cmd_tail_; }

 private:
    uring_abi::Sqe sqe_;
    /// `cmd` bytes past the first 16, which live in the SQE's second half.
    uint8_t cmd_tail_[64];
    std::size_t cmd_len_;
};

/// A reaped completion. Rust reads these off `CompletionQueue`.
struct CompletionEntry {
    uint64_t user_data;
    /// Bytes transferred, or a negative errno. Never translated here: the
    /// caller decides which errnos are expected, and `EAGAIN` on a queue
    /// command means something different than on a read.
    int32_t res;
    uint32_t flags;

    CompletionEntry() : user_data(0), res(0), flags(0) {}
};

/// Rust `AsyncIoRing` — an io_uring instance and its three mapped regions.
///
/// Single-threaded by construction, matching `kSetupSingleIssuer`: submission
/// updates the SQ tail with a release store that only one writer may make.
class Ring {
 public:
    /// Rust `AsyncIoRingBuilder::build`.
    ///
    /// Fails with the offending field named. A bare `EINVAL` out of
    /// `io_uring_setup` is almost impossible to attribute, and the common
    /// cause -- a kernel too old for `sqe128` -- looks identical to a bad
    /// queue depth.
    static core::Expected<std::unique_ptr<Ring>, std::string> Open(const IoRingConfig& config);

    ~Ring();
    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    /// Rust `send` — stage one entry and tell the kernel about it.
    ///
    /// Returns the number of entries the kernel accepted. Fails when the SQ is
    /// full rather than blocking: the caller owns the retry policy, and a
    /// blocking submit on the thread that also reaps is a deadlock.
    core::Expected<uint32_t, std::string> Submit(const SubmissionEntry& entry);

    /// Submit and wait for at least `min_complete` completions.
    core::Expected<uint32_t, std::string> SubmitAndWait(const SubmissionEntry& entry,
                                                        uint32_t min_complete);

    /// Drain every completion currently visible, oldest first.
    ///
    /// Advancing the CQ head is what frees the slots, so a caller that reads
    /// the ring without reaping will see the kernel start dropping
    /// completions once it wraps.
    std::vector<CompletionEntry> Reap();

    /// Block until at least one completion is available, then reap.
    core::Expected<std::vector<CompletionEntry>, std::string> WaitAndReap(
        uint32_t min_complete);

    /// Rust `register_files` — install a fixed-file table.
    core::Expected<core::Unit, std::string> RegisterFiles(const std::vector<int>& fds);
    /// Rust `register_files_update` — fill one slot of a sparse table.
    core::Expected<core::Unit, std::string> UpdateFile(uint32_t index, int fd);

    /// Rust `register_fd` — take the next free slot of the sparse file table
    /// and install `fd` in it, returning the index.
    ///
    /// The index, not the fd, is what a submission refers to afterwards
    /// (`Fixed(idx)`), which is how the kernel skips the fd lookup per
    /// request. Fails when the table was not sized for it at `Open`.
    core::Expected<uint32_t, std::string> RegisterFd(int fd);
    /// Rust `unregister_fd_sync` — release the slot and the index.
    core::Expected<core::Unit, std::string> UnregisterFd(uint32_t index);

    /// Rust `register_buffer` — take the next free sparse buffer slot and
    /// register `buf` into it.
    ///
    /// The buffer must stay at this address and length until unregistered:
    /// the kernel pins its pages, so moving or freeing it leaves the ring
    /// pointing at memory this process no longer owns.
    core::Expected<uint32_t, std::string> RegisterBuffer(void* buf, std::size_t len);
    /// Rust `unregister_buffer`.
    core::Expected<core::Unit, std::string> UnregisterBuffer(uint32_t index);

    /// Rust `occupy_sparse_buffer_index` — reserve an index *without*
    /// registering anything in it.
    ///
    /// What `UBLK_F_AUTO_BUF_REG` needs: the kernel installs the buffer into
    /// the slot itself when it delivers a request, so the server must only
    /// make sure nothing else claims that index meanwhile.
    core::Expected<uint32_t, std::string> OccupySparseBufferIndex();
    /// Rust `release_sparse_buffer_index`.
    core::Expected<core::Unit, std::string> ReleaseSparseBufferIndex(uint32_t index);

    /// Completions the kernel discarded because the CQ was full.
    ///
    /// Surfaced rather than hidden: a non-zero value means some I/O will never
    /// be reported, so a caller waiting on it would wait forever.
    uint32_t DroppedCompletions() const;
    uint32_t CompletionOverflow() const;

    int Fd() const { return ring_fd_; }
    uint32_t SqEntries() const { return sq_entries_; }
    uint32_t CqEntries() const { return cq_entries_; }
    bool Sqe128() const { return sqe128_; }

 private:
    Ring();

    int ring_fd_;
    bool sqe128_;
    uint32_t sq_entries_;
    uint32_t cq_entries_;
    /// Table sizes from `Open`; zero means the table was never created, which
    /// the registration calls report instead of letting the kernel answer a
    /// bare EINVAL.
    uint32_t sparse_files_;
    uint32_t sparse_buffers_;
    /// Rust's `ReloadableIDAllocator` pair, one per sparse table.
    std::unique_ptr<ReloadableIDAllocator> file_ids_;
    std::unique_ptr<ReloadableIDAllocator> buffer_ids_;

    void* sq_ring_;
    std::size_t sq_ring_len_;
    void* sqes_;
    std::size_t sqes_len_;
    void* cq_ring_;
    std::size_t cq_ring_len_;
    /// True when `kFeatSingleMmap` folded the CQ into the SQ mapping, so it
    /// must not be unmapped twice.
    bool cq_shares_sq_;

    // Kernel-owned cells inside the mapped rings.
    uint32_t* sq_head_;
    uint32_t* sq_tail_;
    uint32_t  sq_mask_;
    uint32_t* sq_array_;
    uint32_t* sq_dropped_;
    uint32_t* cq_head_;
    uint32_t* cq_tail_;
    uint32_t  cq_mask_;
    uint32_t* cq_overflow_;
    uring_abi::Cqe* cqes_;
};

/// Whether an entry's payload needs a 128-byte SQE.
///
/// A 64-byte SQE leaves 16 bytes for `cmd`; every ublk command is larger,
/// which is why `IoRingConfig::sqe128` is not optional for the data plane.
bool UringCmdNeedsSqe128(std::size_t cmd_len);

}  // namespace util
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UTIL_IO_RING_H_
