// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/queue.rs
#include "agentenv/storage/ublk/queue.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>

namespace agentenv {
namespace storage {
namespace ublk {

namespace {

// NOLINTBEGIN
static_assert(sizeof(abi::IoCmd) == 16, "ublksrv_io_cmd must be 16 bytes");
static_assert(sizeof(abi::IoCmd) == abi::kIoCmdSize, "kIoCmdSize must match the struct");
static_assert(sizeof(abi::AutoBufReg) == 8, "ublk_auto_buf_reg must be 8 bytes");
// NOLINTEND

std::string Errno(const char* what) {
    std::ostringstream os;
    os << what << ": " << std::strerror(errno) << " (errno " << errno << ")";
    return os.str();
}

/// Round `v` up to a multiple of `m`. Rust's `next_multiple_of`.
std::size_t RoundUp(std::size_t v, std::size_t m) {
    if (m == 0) return v;
    const std::size_t rem = v % m;
    return rem == 0 ? v : v + (m - rem);
}

}  // namespace

core::Optional<UblkDescOperation> DescOperationFrom(uint8_t op) {
    switch (op) {
        case abi::kIoOpRead:         return core::Optional<UblkDescOperation>(UblkDescOperation::Read);
        case abi::kIoOpWrite:        return core::Optional<UblkDescOperation>(UblkDescOperation::Write);
        case abi::kIoOpFlush:        return core::Optional<UblkDescOperation>(UblkDescOperation::Flush);
        case abi::kIoOpDiscard:      return core::Optional<UblkDescOperation>(UblkDescOperation::Discard);
        case abi::kIoOpWriteSame:    return core::Optional<UblkDescOperation>(UblkDescOperation::WriteSame);
        case abi::kIoOpWriteZeroes:  return core::Optional<UblkDescOperation>(UblkDescOperation::WriteZeroes);
        case abi::kIoOpZoneOpen:     return core::Optional<UblkDescOperation>(UblkDescOperation::ZoneOpen);
        case abi::kIoOpZoneClose:    return core::Optional<UblkDescOperation>(UblkDescOperation::ZoneClose);
        case abi::kIoOpZoneFinish:   return core::Optional<UblkDescOperation>(UblkDescOperation::ZoneFinish);
        case abi::kIoOpZoneAppend:   return core::Optional<UblkDescOperation>(UblkDescOperation::ZoneAppend);
        case abi::kIoOpZoneResetAll: return core::Optional<UblkDescOperation>(UblkDescOperation::ZoneResetAll);
        case abi::kIoOpZoneReset:    return core::Optional<UblkDescOperation>(UblkDescOperation::ZoneReset);
        default:
            // Including `ReportZones`, which this server does not implement.
            // Absent, so the caller fails the request back to the kernel
            // rather than handling it as some other operation.
            return core::Optional<UblkDescOperation>();
    }
}

uint64_t QueueDescMapOffset(uint16_t queue_id, std::size_t page_size) {
    // The stride is the *maximum* queue depth, not this device's: the kernel
    // reserves a full-size region per queue regardless of how deep it is.
    // Striding by the actual depth would place queue 1 inside queue 0's
    // region, which maps successfully and serves another queue's requests.
    const std::size_t max_area = RoundUp(
        static_cast<std::size_t>(abi::kMaxQueueDepth) * sizeof(abi::IoDesc), page_size);
    return abi::kCmdBufOffset + static_cast<uint64_t>(queue_id) * max_area;
}

// ---------------------------------------------------------------------------
// QueueIoDescs
// ---------------------------------------------------------------------------

QueueIoDescs::QueueIoDescs() : descs_(NULL), map_len_(0), depth_(0) {}

QueueIoDescs::~QueueIoDescs() {
    if (descs_ != NULL) {
        ::munmap(const_cast<abi::IoDesc*>(descs_), map_len_);
    }
}

core::Expected<std::unique_ptr<QueueIoDescs>, std::string> QueueIoDescs::Map(
    uint16_t queue_id, uint32_t queue_depth, int cdev_fd) {
    if (queue_depth == 0) {
        return core::make_unexpected(std::string("ublk queue: zero depth"));
    }
    if (queue_depth > abi::kMaxQueueDepth) {
        std::ostringstream why;
        why << "ublk queue: depth " << queue_depth << " exceeds UBLK_MAX_QUEUE_DEPTH "
            << abi::kMaxQueueDepth;
        return core::make_unexpected(why.str());
    }
    if (cdev_fd < 0) {
        return core::make_unexpected(std::string("ublk queue: invalid char device fd"));
    }

    const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return core::make_unexpected(std::string("ublk queue: cannot read page size"));
    }

    // Only this queue's own depth is mapped, even though the region reserved
    // for it is larger: the slots past the depth are never filled, and mapping
    // them would make an out-of-range tag read plausible zeroes instead of
    // failing.
    const std::size_t len = static_cast<std::size_t>(queue_depth) * sizeof(abi::IoDesc);
    const uint64_t off = QueueDescMapOffset(queue_id, static_cast<std::size_t>(page));

    void* p = ::mmap(NULL, len, PROT_READ, MAP_SHARED | MAP_POPULATE, cdev_fd,
                     static_cast<off_t>(off));
    if (p == MAP_FAILED) {
        return core::make_unexpected(Errno("ublk queue: mmap descriptor area"));
    }

    std::unique_ptr<QueueIoDescs> out(new QueueIoDescs());
    out->descs_ = static_cast<const abi::IoDesc*>(p);
    out->map_len_ = len;
    out->depth_ = queue_depth;
    return std::move(out);
}

const abi::IoDesc* QueueIoDescs::At(uint16_t tag) const {
    if (descs_ == NULL || static_cast<uint32_t>(tag) >= depth_) return NULL;
    return descs_ + tag;
}

// ---------------------------------------------------------------------------
// UblkQueue
// ---------------------------------------------------------------------------

UblkQueue::UblkQueue()
    : queue_id_(0), depth_(0), cdev_idx_(0), cdev_registered_(false) {
    std::memset(&dev_info_, 0, sizeof(dev_info_));
}

UblkQueue::~UblkQueue() {
    // Rust unregisters the char device in `Drop`. Left behind, the slot stays
    // occupied for the ring's lifetime and a later queue on the same ring
    // runs out of fixed-file indices.
    if (cdev_registered_ && ring_) {
        ring_->UnregisterFd(cdev_idx_);
    }
}

core::Expected<std::unique_ptr<UblkQueue>, std::string> UblkQueue::Create(
    uint16_t queue_id, const abi::CtrlDevInfo& dev_info, int cdev_fd,
    std::shared_ptr<util::Ring> ring) {
    if (!ring) {
        return core::make_unexpected(std::string("ublk queue: null io_uring"));
    }
    // Every queue command is an `IORING_OP_URING_CMD` carrying a 16-byte
    // `ublksrv_io_cmd`, which does fit a 64-byte SQE -- but the ring is shared
    // with the control plane in practice, and a 64-byte ring would fail there
    // instead. Checked here so the mismatch is named at setup.
    if (!ring->Sqe128()) {
        return core::make_unexpected(std::string(
            "ublk queue: needs an sqe128 io_uring"));
    }
    if (queue_id >= dev_info.nr_hw_queues) {
        std::ostringstream why;
        why << "ublk queue: queue id " << queue_id << " is past nr_hw_queues "
            << dev_info.nr_hw_queues;
        return core::make_unexpected(why.str());
    }

    core::Expected<std::unique_ptr<QueueIoDescs>, std::string> descs =
        QueueIoDescs::Map(queue_id, dev_info.queue_depth, cdev_fd);
    if (!descs.ok()) return core::make_unexpected(descs.error());

    std::unique_ptr<UblkQueue> q(new UblkQueue());
    q->queue_id_ = queue_id;
    q->depth_ = dev_info.queue_depth;
    q->dev_info_ = dev_info;
    q->descs_.reset(descs.take_value().release());

    // Registered as a fixed file so commands can name it by index. Done after
    // the mapping, so a failed mmap does not leave a slot taken.
    const core::Expected<uint32_t, std::string> idx = ring->RegisterFd(cdev_fd);
    if (!idx.ok()) {
        return core::make_unexpected(std::string("register ublk char device: ") +
                                     idx.error());
    }
    q->cdev_idx_ = idx.value();
    q->cdev_registered_ = true;
    q->ring_ = ring;
    return std::move(q);
}

bool UblkQueue::IsZeroCopy() const {
    return (dev_info_.flags & abi::kFAutoBufReg) != 0;
}

abi::IoCmd UblkQueue::BuildIoCmd(uint16_t tag, const IoBuffer& buf, int32_t result) const {
    abi::IoCmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.q_id = queue_id_;
    cmd.tag = tag;
    cmd.result = result;
    // Zero under auto-registration, the buffer's address otherwise. The
    // buffer decides, not the device flags: a slot armed with a user buffer on
    // an auto-reg device still has to say where its memory is.
    cmd.addr = buf.CmdAddr();
    return cmd;
}

core::Expected<core::Unit, std::string> UblkQueue::SendIoCmd(const char* what,
                                                             uint32_t cmd_op,
                                                             const abi::IoCmd& cmd,
                                                             const IoBuffer& buf) {
    util::SubmissionEntry sqe =
        util::SubmissionEntry::UringCmd(static_cast<int>(cdev_idx_), cmd_op, &cmd,
                                        sizeof(cmd));
    // The fd is an index into the fixed-file table, so the SQE has to say so.
    sqe.UseFixedFile(true);

    uint64_t sqe_addr = 0;
    if (buf.SqeAddr(&sqe_addr)) {
        // Auto-registration carries the reserved slot here instead of in the
        // command payload. Overwriting `addr` is exactly Rust's
        // `override_sqe!(&mut sqe, addr, ...)`.
        sqe.OverrideAddr(sqe_addr);
    }
    // Identifies which slot a completion belongs to. A queue has `depth`
    // commands outstanding at once, so without this the results cannot be
    // matched to their tags.
    sqe.UserData((static_cast<uint64_t>(queue_id_) << 32) | cmd.tag);

    const core::Expected<uint32_t, std::string> sent = ring_->Submit(sqe);
    if (!sent.ok()) {
        return core::make_unexpected(std::string("send ") + what + ": " + sent.error());
    }
    // Not awaited: these commands stay outstanding until the kernel has work
    // for the slot. Waiting here would block the queue on its own first
    // request.
    return core::Unit();
}

core::Expected<core::Unit, std::string> UblkQueue::PrepSlot(uint16_t tag,
                                                            const IoBuffer& buf) {
    if (static_cast<uint32_t>(tag) >= depth_) {
        std::ostringstream why;
        why << "ublk queue: tag " << tag << " is past depth " << depth_;
        return core::make_unexpected(why.str());
    }
    // `result` is 0 and means nothing: FETCH_REQ has no previous request in
    // this slot to report on.
    const abi::IoCmd cmd = BuildIoCmd(tag, buf, 0);
    return SendIoCmd("FETCH_REQ", abi::IoCmdFetchReq(), cmd, buf);
}

core::Expected<core::Unit, std::string> UblkQueue::SubmitAndFetchForSlot(
    uint16_t tag, const IoBuffer& buf, int32_t result) {
    if (static_cast<uint32_t>(tag) >= depth_) {
        std::ostringstream why;
        why << "ublk queue: tag " << tag << " is past depth " << depth_;
        return core::make_unexpected(why.str());
    }
    const abi::IoCmd cmd = BuildIoCmd(tag, buf, result);
    return SendIoCmd("COMMIT_AND_FETCH_REQ", abi::IoCmdCommitAndFetchReq(), cmd, buf);
}

const abi::IoDesc* UblkQueue::DescAt(uint16_t tag) const {
    return descs_ ? descs_->At(tag) : NULL;
}

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
