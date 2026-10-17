// SPDX-License-Identifier: MIT
// Rust: storage/util/src/io_ring/{uring,worker}.rs
#include "agentenv/storage/util/io_ring.h"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>

namespace agentenv {
namespace storage {
namespace util {

namespace {

using namespace uring_abi;  // NOLINT(build/namespaces) — ABI constants only.

// The kernel reads and writes these structs directly, so a field at the wrong
// offset is not a compile error: it is a wrong fd, length or opcode handed to
// the kernel at runtime, and the failure appears as an unexplained EINVAL or
// as I/O against the wrong file. Checked here against the documented layout.
#define AENV_SQE_OFF(field) \
    static_cast<int>(reinterpret_cast<std::size_t>(&(((Sqe*)0)->field)))
// NOLINTBEGIN
static_assert(sizeof(Sqe) == 64, "io_uring_sqe must be 64 bytes");
static_assert(sizeof(Cqe) == 16, "io_uring_cqe must be 16 bytes");
static_assert(sizeof(SqRingOffsets) == 40, "io_sqring_offsets layout");
static_assert(sizeof(CqRingOffsets) == 40, "io_cqring_offsets layout");
static_assert(sizeof(Params) == 120, "io_uring_params layout");
static_assert(sizeof(RsrcRegister) == 32, "io_uring_rsrc_register layout");
// NOLINTEND

long SysSetup(uint32_t entries, Params* p) {
    return ::syscall(kSysIoUringSetup, static_cast<long>(entries), p);
}
long SysEnter(int fd, uint32_t to_submit, uint32_t min_complete, uint32_t flags) {
    return ::syscall(kSysIoUringEnter, static_cast<long>(fd), static_cast<long>(to_submit),
                     static_cast<long>(min_complete), static_cast<long>(flags),
                     static_cast<void*>(NULL), static_cast<long>(0));
}
long SysRegister(int fd, uint32_t opcode, void* arg, uint32_t nr_args) {
    return ::syscall(kSysIoUringRegister, static_cast<long>(fd), static_cast<long>(opcode), arg,
                     static_cast<long>(nr_args));
}

/// The kernel's side of the head/tail protocol needs real barriers, not just
/// volatile reads: on a weakly-ordered machine the entry written into the SQE
/// array can otherwise become visible *after* the tail that advertises it, and
/// the kernel would then read an entry that has not been filled in.
uint32_t LoadAcquire(const uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
void StoreRelease(uint32_t* p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
uint32_t LoadRelaxed(const uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

std::string Errno(const char* what) {
    std::ostringstream os;
    os << what << ": " << std::strerror(errno) << " (errno " << errno << ")";
    return os.str();
}

uint8_t* BytePtr(void* base, uint32_t offset) {
    return static_cast<uint8_t*>(base) + offset;
}

}  // namespace

bool UringCmdNeedsSqe128(std::size_t cmd_len) {
    return cmd_len > sizeof(Sqe) - 48;  // 16 bytes of `cmd` in a 64-byte SQE
}

// ---------------------------------------------------------------------------
// SubmissionEntry
// ---------------------------------------------------------------------------

SubmissionEntry::SubmissionEntry() : cmd_len_(0) {
    std::memset(&sqe_, 0, sizeof(sqe_));
    std::memset(cmd_tail_, 0, sizeof(cmd_tail_));
}

SubmissionEntry SubmissionEntry::Nop() {
    SubmissionEntry e;
    e.sqe_.opcode = kOpNop;
    e.sqe_.fd = -1;
    return e;
}

SubmissionEntry SubmissionEntry::Read(int fd, void* buf, uint32_t len, uint64_t offset) {
    SubmissionEntry e;
    e.sqe_.opcode = kOpRead;
    e.sqe_.fd = fd;
    e.sqe_.off_or_cmd_op = offset;
    e.sqe_.addr = reinterpret_cast<uint64_t>(buf);
    e.sqe_.len = len;
    return e;
}

SubmissionEntry SubmissionEntry::Write(int fd, const void* buf, uint32_t len, uint64_t offset) {
    SubmissionEntry e;
    e.sqe_.opcode = kOpWrite;
    e.sqe_.fd = fd;
    e.sqe_.off_or_cmd_op = offset;
    e.sqe_.addr = reinterpret_cast<uint64_t>(buf);
    e.sqe_.len = len;
    return e;
}

SubmissionEntry SubmissionEntry::ReadFixed(int fd, void* buf, uint32_t len, uint64_t offset,
                                           uint16_t buf_index) {
    SubmissionEntry e = Read(fd, buf, len, offset);
    e.sqe_.opcode = kOpReadFixed;
    // The buffer is still addressed by pointer; `buf_index` only tells the
    // kernel which registration it falls inside, so a wrong index is a read
    // into another request's memory rather than an error.
    e.sqe_.buf_index = buf_index;
    return e;
}

SubmissionEntry SubmissionEntry::WriteFixed(int fd, const void* buf, uint32_t len,
                                            uint64_t offset, uint16_t buf_index) {
    SubmissionEntry e = Write(fd, buf, len, offset);
    e.sqe_.opcode = kOpWriteFixed;
    e.sqe_.buf_index = buf_index;
    return e;
}

SubmissionEntry SubmissionEntry::Fsync(int fd) {
    SubmissionEntry e;
    e.sqe_.opcode = kOpFsync;
    e.sqe_.fd = fd;
    return e;
}

SubmissionEntry SubmissionEntry::UringCmd(int fd, uint32_t cmd_op, const void* cmd_bytes,
                                          std::size_t cmd_len) {
    SubmissionEntry e;
    e.sqe_.opcode = kOpUringCmd;
    e.sqe_.fd = fd;
    // `cmd_op` occupies the low half of the field the read/write form uses for
    // `off`. The high half is reserved and must stay zero.
    e.sqe_.off_or_cmd_op = static_cast<uint64_t>(cmd_op);

    const std::size_t capped = cmd_len > kSqe128CmdBytes ? kSqe128CmdBytes : cmd_len;
    e.cmd_len_ = capped;
    if (cmd_bytes != NULL && capped > 0) {
        const uint8_t* src = static_cast<const uint8_t*>(cmd_bytes);
        const std::size_t head = capped < sizeof(e.sqe_.cmd) ? capped : sizeof(e.sqe_.cmd);
        std::memcpy(e.sqe_.cmd, src, head);
        if (capped > head) {
            // The rest lands in the SQE's second 64 bytes, which exist only on
            // an sqe128 ring. `Submit` refuses the entry if the ring is not
            // one, rather than dropping the tail.
            std::memcpy(e.cmd_tail_, src + head, capped - head);
        }
    }
    return e;
}

SubmissionEntry& SubmissionEntry::UserData(uint64_t token) {
    sqe_.user_data = token;
    return *this;
}

SubmissionEntry& SubmissionEntry::UseFixedFile(bool fixed) {
    if (fixed) {
        sqe_.flags |= kSqeFixedFile;
    } else {
        sqe_.flags &= static_cast<uint8_t>(~kSqeFixedFile);
    }
    return *this;
}

SubmissionEntry& SubmissionEntry::OverrideAddr(uint64_t addr) {
    sqe_.addr = addr;
    return *this;
}

// ---------------------------------------------------------------------------
// Ring
// ---------------------------------------------------------------------------

Ring::Ring()
    : ring_fd_(-1), sqe128_(false), sq_entries_(0), cq_entries_(0),
      sparse_files_(0), sparse_buffers_(0),
      sq_ring_(NULL), sq_ring_len_(0), sqes_(NULL), sqes_len_(0),
      cq_ring_(NULL), cq_ring_len_(0), cq_shares_sq_(false),
      sq_head_(NULL), sq_tail_(NULL), sq_mask_(0), sq_array_(NULL), sq_dropped_(NULL),
      cq_head_(NULL), cq_tail_(NULL), cq_mask_(0), cq_overflow_(NULL), cqes_(NULL) {}

Ring::~Ring() {
    if (sqes_ != NULL) ::munmap(sqes_, sqes_len_);
    // Unmapped before the SQ, and only when it is a separate mapping: with
    // `kFeatSingleMmap` the two share one region and a second `munmap` would
    // either fail or tear down an unrelated mapping made since.
    if (cq_ring_ != NULL && !cq_shares_sq_) ::munmap(cq_ring_, cq_ring_len_);
    if (sq_ring_ != NULL) ::munmap(sq_ring_, sq_ring_len_);
    if (ring_fd_ >= 0) ::close(ring_fd_);
}

core::Expected<std::unique_ptr<Ring>, std::string> Ring::Open(const IoRingConfig& config) {
    // Checked here, with the field named. `io_uring_setup` answers every one
    // of these with a bare EINVAL, which cannot be told apart from a kernel
    // that simply lacks `sqe128`.
    if (config.sq_entries == 0 || config.sq_entries > 4096) {
        return core::make_unexpected(std::string("io_ring: sq_entries must be 1..4096"));
    }
    if (config.cq_entries > 8192) {
        return core::make_unexpected(std::string("io_ring: cq_entries must be <= 8192"));
    }
    if (config.sparse_files > 8192) {
        return core::make_unexpected(std::string("io_ring: sparse_files must be <= 8192"));
    }
    if (config.sparse_buffers > 4096) {
        return core::make_unexpected(std::string("io_ring: sparse_buffers must be <= 4096"));
    }

    Params p;
    std::memset(&p, 0, sizeof(p));
    if (config.cq_entries > 0) {
        p.flags |= kSetupCqSize;
        p.cq_entries = config.cq_entries;
    }
    if (config.coop_taskrun) p.flags |= kSetupCoopTaskrun;
    if (config.single_issuer) p.flags |= kSetupSingleIssuer;
    if (config.sqe128) p.flags |= kSetupSqe128;

    const long fd = SysSetup(config.sq_entries, &p);
    if (fd < 0) {
        std::string why = Errno("io_uring_setup");
        if (config.sqe128 && errno == EINVAL) {
            // The overwhelmingly likely cause, and worth saying: without
            // sqe128 there is no `IORING_OP_URING_CMD` payload room, so ublk
            // cannot work at all and the operator needs to know why.
            why += " (IORING_SETUP_SQE128 needs Linux 5.19 or newer)";
        }
        return core::make_unexpected(why);
    }

    std::unique_ptr<Ring> ring(new Ring());
    ring->ring_fd_ = static_cast<int>(fd);
    ring->sqe128_ = config.sqe128;
    ring->sq_entries_ = p.sq_entries;
    ring->cq_entries_ = p.cq_entries;

    // The SQ mapping has to cover its index array, which sits at the end.
    ring->sq_ring_len_ = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
    ring->cq_ring_len_ = p.cq_off.cqes + p.cq_entries * sizeof(Cqe);
    if ((p.features & kFeatSingleMmap) != 0) {
        // One region holds both; it must be sized for the larger of the two.
        if (ring->cq_ring_len_ > ring->sq_ring_len_) {
            ring->sq_ring_len_ = ring->cq_ring_len_;
        }
        ring->cq_ring_len_ = ring->sq_ring_len_;
    }

    void* sq = ::mmap(NULL, ring->sq_ring_len_, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, ring->ring_fd_,
                      static_cast<off_t>(kOffSqRing));
    if (sq == MAP_FAILED) {
        return core::make_unexpected(Errno("io_ring: mmap sq ring"));
    }
    ring->sq_ring_ = sq;

    if ((p.features & kFeatSingleMmap) != 0) {
        ring->cq_ring_ = sq;
        ring->cq_shares_sq_ = true;
    } else {
        void* cq = ::mmap(NULL, ring->cq_ring_len_, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE, ring->ring_fd_,
                          static_cast<off_t>(kOffCqRing));
        if (cq == MAP_FAILED) {
            return core::make_unexpected(Errno("io_ring: mmap cq ring"));
        }
        ring->cq_ring_ = cq;
    }

    // Doubled for sqe128: the kernel strides the array by 128 bytes, so a
    // mapping sized for 64-byte entries would be half as long as it reads.
    const std::size_t sqe_stride = config.sqe128 ? 2 * sizeof(Sqe) : sizeof(Sqe);
    ring->sqes_len_ = p.sq_entries * sqe_stride;
    void* sqes = ::mmap(NULL, ring->sqes_len_, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, ring->ring_fd_,
                        static_cast<off_t>(kOffSqes));
    if (sqes == MAP_FAILED) {
        return core::make_unexpected(Errno("io_ring: mmap sqes"));
    }
    ring->sqes_ = sqes;

    ring->sq_head_ = reinterpret_cast<uint32_t*>(BytePtr(sq, p.sq_off.head));
    ring->sq_tail_ = reinterpret_cast<uint32_t*>(BytePtr(sq, p.sq_off.tail));
    ring->sq_mask_ = *reinterpret_cast<uint32_t*>(BytePtr(sq, p.sq_off.ring_mask));
    ring->sq_array_ = reinterpret_cast<uint32_t*>(BytePtr(sq, p.sq_off.array));
    ring->sq_dropped_ = reinterpret_cast<uint32_t*>(BytePtr(sq, p.sq_off.dropped));
    ring->cq_head_ = reinterpret_cast<uint32_t*>(BytePtr(ring->cq_ring_, p.cq_off.head));
    ring->cq_tail_ = reinterpret_cast<uint32_t*>(BytePtr(ring->cq_ring_, p.cq_off.tail));
    ring->cq_mask_ = *reinterpret_cast<uint32_t*>(BytePtr(ring->cq_ring_, p.cq_off.ring_mask));
    ring->cq_overflow_ =
        reinterpret_cast<uint32_t*>(BytePtr(ring->cq_ring_, p.cq_off.overflow));
    ring->cqes_ = reinterpret_cast<Cqe*>(BytePtr(ring->cq_ring_, p.cq_off.cqes));

    // Sparse tables are sized now because the fds and buffers that fill them
    // are not known yet: a ublk device registers its char-dev fd only after
    // the ring it will be driven by exists.
    if (config.sparse_files > 0) {
        RsrcRegister reg;
        std::memset(&reg, 0, sizeof(reg));
        reg.nr = config.sparse_files;
        reg.flags = kRsrcRegisterSparse;
        if (SysRegister(ring->ring_fd_, kRegisterFiles2, &reg, sizeof(reg)) < 0) {
            return core::make_unexpected(Errno("io_ring: register_files_sparse"));
        }
        ring->sparse_files_ = config.sparse_files;
        ring->file_ids_.reset(new ReloadableIDAllocator(0));
    }
    if (config.sparse_buffers > 0) {
        RsrcRegister reg;
        std::memset(&reg, 0, sizeof(reg));
        reg.nr = config.sparse_buffers;
        reg.flags = kRsrcRegisterSparse;
        if (SysRegister(ring->ring_fd_, kRegisterBuffers2, &reg, sizeof(reg)) < 0) {
            return core::make_unexpected(Errno("io_ring: register_buffers_sparse"));
        }
        ring->sparse_buffers_ = config.sparse_buffers;
        ring->buffer_ids_.reset(new ReloadableIDAllocator(0));
    }

    return std::move(ring);
}

core::Expected<uint32_t, std::string> Ring::Submit(const SubmissionEntry& entry) {
    return SubmitAndWait(entry, 0);
}

core::Expected<uint32_t, std::string> Ring::SubmitAndWait(const SubmissionEntry& entry,
                                                          uint32_t min_complete) {
    // Refused, not truncated. A command cut to fit would still be a
    // structurally valid SQE, so the kernel would act on whatever its tail
    // fields happened to contain.
    if (!sqe128_ && UringCmdNeedsSqe128(entry.CmdLen())) {
        std::ostringstream why;
        why << "io_ring: " << entry.CmdLen()
            << "-byte uring_cmd payload needs an sqe128 ring";
        return core::make_unexpected(why.str());
    }

    const uint32_t tail = LoadRelaxed(sq_tail_);
    // Acquire: the kernel advances the head as it consumes entries, and the
    // slot it frees must not be reused before that write is visible.
    const uint32_t head = LoadAcquire(sq_head_);
    if (tail - head >= sq_entries_) {
        // Full. Reported rather than waited on: the caller owns the retry
        // policy, and blocking here on the thread that also reaps would
        // deadlock against the completions that would free a slot.
        //
        // NOTE: hard to reach from a single-threaded caller, since the kernel
        // consumes entries as fast as they are staged. The test for it accepts
        // either outcome, so this branch is reasoned-about rather than
        // observed -- the unsigned subtraction is deliberate and handles the
        // wrap of `tail` past UINT32_MAX.
        return core::make_unexpected(std::string("io_ring: submission queue full"));
    }

    const uint32_t index = tail & sq_mask_;
    const std::size_t stride = sqe128_ ? 2 * sizeof(Sqe) : sizeof(Sqe);
    uint8_t* slot = static_cast<uint8_t*>(sqes_) + static_cast<std::size_t>(index) * stride;
    std::memcpy(slot, &entry.Raw(), sizeof(Sqe));
    if (sqe128_) {
        // Always written, even when the payload is short: the slot may hold a
        // previous command's tail, and the kernel reads all 80 bytes of `cmd`.
        //
        // NOTE: the doubled stride above and this copy are only exercised on a
        // kernel with `IORING_SETUP_SQE128` (5.19+). On an older one the
        // sqe128 tests skip, so a wrong stride here would not be caught --
        // treat changes to either line as unverified until run on 5.19+.
        std::memcpy(slot + sizeof(Sqe), entry.CmdTail(), sizeof(Sqe));
    }

    sq_array_[index] = index;
    // Release: publishes both the SQE above and the array slot. Without it the
    // kernel can observe the new tail while the entry is still unwritten.
    StoreRelease(sq_tail_, tail + 1);

    const uint32_t flags = min_complete > 0 ? kEnterGetEvents : 0;
    long submitted = SysEnter(ring_fd_, 1, min_complete, flags);
    while (submitted < 0 && errno == EINTR) {
        // A signal before anything was consumed. The entry is still staged, so
        // retrying submits it once -- not twice.
        submitted = SysEnter(ring_fd_, 1, min_complete, flags);
    }
    if (submitted < 0) {
        return core::make_unexpected(Errno("io_uring_enter"));
    }
    return static_cast<uint32_t>(submitted);
}

std::vector<CompletionEntry> Ring::Reap() {
    std::vector<CompletionEntry> out;
    uint32_t head = LoadRelaxed(cq_head_);
    // Acquire: pairs with the kernel's release of the tail, and is what makes
    // the CQE contents at that index safe to read.
    const uint32_t tail = LoadAcquire(cq_tail_);
    if (tail == head) return out;

    out.reserve(tail - head);
    for (; head != tail; ++head) {
        const Cqe& cqe = cqes_[head & cq_mask_];
        CompletionEntry e;
        e.user_data = cqe.user_data;
        e.res = cqe.res;
        e.flags = cqe.flags;
        out.push_back(e);
    }
    // Release, and only after every CQE has been copied out: the kernel may
    // refill a slot as soon as it sees the head move.
    StoreRelease(cq_head_, head);
    return out;
}

core::Expected<std::vector<CompletionEntry>, std::string> Ring::WaitAndReap(
    uint32_t min_complete) {
    if (min_complete == 0) return Reap();

    while (true) {
        std::vector<CompletionEntry> ready = Reap();
        if (ready.size() >= min_complete) return ready;

        // Nothing is submitted here (`to_submit` is 0); this only waits. Any
        // entries reaped above are kept and the wait is for the remainder, so
        // a partial reap is not thrown away.
        const uint32_t still_needed = min_complete - static_cast<uint32_t>(ready.size());
        long r = SysEnter(ring_fd_, 0, still_needed, kEnterGetEvents);
        if (r < 0 && errno == EINTR) {
            if (!ready.empty()) return ready;
            continue;
        }
        if (r < 0) return core::make_unexpected(Errno("io_uring_enter(wait)"));

        std::vector<CompletionEntry> more = Reap();
        ready.insert(ready.end(), more.begin(), more.end());
        if (!ready.empty()) return ready;
    }
}

core::Expected<core::Unit, std::string> Ring::RegisterFiles(const std::vector<int>& fds) {
    if (fds.empty()) {
        return core::make_unexpected(std::string("io_ring: register_files with no fds"));
    }
    // Rust's cap. The kernel allows far more, but a table this code did not
    // size itself is a table it cannot index safely.
    if (fds.size() > 64) {
        return core::make_unexpected(std::string("io_ring: register_files limit is 64"));
    }
    if (SysRegister(ring_fd_, kRegisterFiles, const_cast<int*>(&fds[0]),
                    static_cast<uint32_t>(fds.size())) < 0) {
        return core::make_unexpected(Errno("io_ring: register_files"));
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> Ring::UpdateFile(uint32_t index, int fd) {
    // `IORING_REGISTER_FILES_UPDATE` takes the slot in `off`, which the
    // syscall's `nr_args` cannot express; the kernel reads it from the
    // 4-argument form below.
    struct Update {
        uint32_t offset;
        uint32_t resv;
        uint64_t fds;
    } update;
    std::memset(&update, 0, sizeof(update));
    update.offset = index;
    update.fds = reinterpret_cast<uint64_t>(&fd);
    if (SysRegister(ring_fd_, kRegisterFilesUpdate, &update, 1) < 0) {
        return core::make_unexpected(Errno("io_ring: register_files_update"));
    }
    return core::Unit();
}

core::Expected<uint32_t, std::string> Ring::RegisterFd(int fd) {
    if (sparse_files_ == 0 || !file_ids_) {
        return core::make_unexpected(
            std::string("io_ring: no sparse file table (set sparse_files at Open)"));
    }
    const uint32_t idx = file_ids_->Allocate();
    if (idx >= sparse_files_) {
        // Returned to the pool: leaking it would shrink the table by one on
        // every overflow, so a transient burst would permanently cost slots.
        file_ids_->Recycle(idx);
        return core::make_unexpected(
            std::string("io_ring: sparse file table is full"));
    }
    const core::Expected<core::Unit, std::string> installed = UpdateFile(idx, fd);
    if (!installed.ok()) {
        file_ids_->Recycle(idx);
        return core::make_unexpected(installed.error());
    }
    return idx;
}

core::Expected<core::Unit, std::string> Ring::UnregisterFd(uint32_t index) {
    if (!file_ids_ || file_ids_->IsFree(index)) {
        // Refused rather than ignored: an index this ring never handed out
        // belongs to nobody, and clearing the slot would pull the fd out from
        // under whichever caller does own it.
        return core::make_unexpected(
            std::string("io_ring: unregister of an unregistered file index"));
    }
    // -1 is how the kernel is told to empty a slot.
    const core::Expected<core::Unit, std::string> cleared = UpdateFile(index, -1);
    if (!cleared.ok()) return core::make_unexpected(cleared.error());
    file_ids_->Recycle(index);
    return core::Unit();
}

core::Expected<uint32_t, std::string> Ring::RegisterBuffer(void* buf, std::size_t len) {
    if (sparse_buffers_ == 0 || !buffer_ids_) {
        return core::make_unexpected(
            std::string("io_ring: no sparse buffer table (set sparse_buffers at Open)"));
    }
    if (buf == NULL || len == 0) {
        return core::make_unexpected(std::string("io_ring: empty buffer registration"));
    }
    const uint32_t idx = buffer_ids_->Allocate();
    if (idx >= sparse_buffers_) {
        buffer_ids_->Recycle(idx);
        return core::make_unexpected(std::string("io_ring: sparse buffer table is full"));
    }

    struct IoVec {
        void* base;
        std::size_t len;
    } vec;
    vec.base = buf;
    vec.len = len;

    // `IORING_REGISTER_BUFFERS_UPDATE` takes `io_uring_rsrc_update2` and the
    // kernel requires `nr_args` to be that struct's size, not the number of
    // buffers -- the count rides in the struct's own `nr`.
    RsrcUpdate2 up;
    std::memset(&up, 0, sizeof(up));
    up.offset = idx;
    up.data = reinterpret_cast<uint64_t>(&vec);
    up.nr = 1;
    if (SysRegister(ring_fd_, kRegisterBuffersUpdate, &up, sizeof(up)) < 0) {
        const std::string why = Errno("io_ring: register_buffers_update");
        buffer_ids_->Recycle(idx);
        return core::make_unexpected(why);
    }
    return idx;
}

core::Expected<core::Unit, std::string> Ring::UnregisterBuffer(uint32_t index) {
    if (!buffer_ids_ || buffer_ids_->IsFree(index)) {
        return core::make_unexpected(
            std::string("io_ring: unregister of an unregistered buffer index"));
    }
    // A zero-length iovec is how a slot is emptied; the kernel unpins the
    // pages it was holding for it.
    struct IoVec {
        void* base;
        std::size_t len;
    } vec;
    vec.base = NULL;
    vec.len = 0;

    RsrcUpdate2 up;
    std::memset(&up, 0, sizeof(up));
    up.offset = index;
    up.data = reinterpret_cast<uint64_t>(&vec);
    up.nr = 1;
    if (SysRegister(ring_fd_, kRegisterBuffersUpdate, &up, sizeof(up)) < 0) {
        return core::make_unexpected(Errno("io_ring: unregister buffer"));
    }
    buffer_ids_->Recycle(index);
    return core::Unit();
}

core::Expected<uint32_t, std::string> Ring::OccupySparseBufferIndex() {
    if (sparse_buffers_ == 0 || !buffer_ids_) {
        return core::make_unexpected(
            std::string("io_ring: no sparse buffer table (set sparse_buffers at Open)"));
    }
    const uint32_t idx = buffer_ids_->Allocate();
    if (idx >= sparse_buffers_) {
        buffer_ids_->Recycle(idx);
        return core::make_unexpected(std::string("io_ring: sparse buffer table is full"));
    }
    // Nothing is registered. The slot is reserved so that nothing else claims
    // it while the kernel fills it on request delivery -- which is the whole
    // contract of `UBLK_F_AUTO_BUF_REG`.
    return idx;
}

core::Expected<core::Unit, std::string> Ring::ReleaseSparseBufferIndex(uint32_t index) {
    if (!buffer_ids_ || buffer_ids_->IsFree(index)) {
        return core::make_unexpected(
            std::string("io_ring: release of an unoccupied buffer index"));
    }
    // No kernel call: nothing was ever registered in this slot.
    buffer_ids_->Recycle(index);
    return core::Unit();
}

uint32_t Ring::DroppedCompletions() const {
    return sq_dropped_ != NULL ? LoadRelaxed(sq_dropped_) : 0;
}

uint32_t Ring::CompletionOverflow() const {
    return cq_overflow_ != NULL ? LoadRelaxed(cq_overflow_) : 0;
}

}  // namespace util
}  // namespace storage
}  // namespace agentenv
