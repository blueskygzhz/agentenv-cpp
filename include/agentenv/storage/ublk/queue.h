// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/queue.rs — UVMUblkQueue + UblkQueueIoDesc.
//
// One hardware queue of a ublk device. Requests are not delivered by a
// syscall: the kernel writes descriptors into a shared mapping and the server
// claims slots in it with io_uring commands. A slot is "armed" by FETCH_REQ
// and re-armed by COMMIT_AND_FETCH_REQ, so a queue of depth N is N standing
// commands the kernel completes as work arrives.
#ifndef AGENTENV_STORAGE_UBLK_QUEUE_H_
#define AGENTENV_STORAGE_UBLK_QUEUE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/ublk/abi.h"
#include "agentenv/storage/ublk/io_buffer.h"
#include "agentenv/storage/util/io_ring.h"

namespace agentenv {
namespace storage {
namespace ublk {

/// Rust enum `UblkDescOperation` — what the kernel is asking for.
///
/// The values are the kernel's `req_op`, not a dense sequence: zoned ops start
/// at 10 and `ReportZones` is 18. Renumbering them to be contiguous would map
/// every zone request onto the wrong operation.
enum class UblkDescOperation : uint8_t {
    Read = abi::kIoOpRead,
    Write = abi::kIoOpWrite,
    Flush = abi::kIoOpFlush,
    Discard = abi::kIoOpDiscard,
    WriteSame = abi::kIoOpWriteSame,
    WriteZeroes = abi::kIoOpWriteZeroes,
    ZoneOpen = abi::kIoOpZoneOpen,
    ZoneClose = abi::kIoOpZoneClose,
    ZoneFinish = abi::kIoOpZoneFinish,
    ZoneAppend = abi::kIoOpZoneAppend,
    ZoneResetAll = abi::kIoOpZoneResetAll,
    ZoneReset = abi::kIoOpZoneReset,
};

/// Rust `TryFrom<u32> for UblkDescOperation`.
///
/// Absent for an unknown opcode rather than defaulting to one. A descriptor
/// this build does not understand must be failed back to the kernel, not
/// guessed at -- guessing turns an unsupported request into data loss.
core::Optional<UblkDescOperation> DescOperationFrom(uint8_t op);

/// Rust `UblkQueueIoDesc` — the queue's descriptor array, mapped read-only.
///
/// The kernel writes here and the server only reads, which is why the mapping
/// is `PROT_READ`: a descriptor is a statement of what was asked for, and
/// nothing on this side has standing to change it.
class QueueIoDescs {
 public:
    /// Rust `UblkQueueIoDesc::new(queue_id, depth, cdev)`.
    static core::Expected<std::unique_ptr<QueueIoDescs>, std::string> Map(
        uint16_t queue_id, uint32_t queue_depth, int cdev_fd);

    ~QueueIoDescs();
    QueueIoDescs(const QueueIoDescs&) = delete;
    QueueIoDescs& operator=(const QueueIoDescs&) = delete;

    /// The descriptor for `tag`, or null when out of range.
    const abi::IoDesc* At(uint16_t tag) const;
    uint32_t Depth() const { return depth_; }

 private:
    QueueIoDescs();

    const abi::IoDesc* descs_;
    std::size_t map_len_;
    uint32_t depth_;
};

/// Rust `UblkQueueIoDesc::new`'s offset arithmetic, exposed for testing.
///
/// Each queue's descriptor area is laid out at a stride of
/// `UBLK_MAX_QUEUE_DEPTH` descriptors rounded up to a page -- the maximum,
/// not this device's depth. Using the actual depth as the stride lands queue
/// 1 and up inside the previous queue's region, which reads as a working
/// mapping serving another queue's requests.
uint64_t QueueDescMapOffset(uint16_t queue_id, std::size_t page_size);

/// Rust `UVMUblkQueue`.
///
/// Single-threaded: Rust confines one queue to one thread (its ring is an
/// `Rc`/`RefCell`), and the ring underneath is created `single_issuer`, which
/// is a promise to the kernel rather than a hint.
class UblkQueue {
 public:
    /// Rust `UVMUblkQueue::new`.
    ///
    /// Takes the char-device fd, maps the descriptor array, and registers the
    /// fd as a fixed file: every queue command afterwards refers to it by
    /// index, which is what lets the kernel skip the fd lookup per request.
    static core::Expected<std::unique_ptr<UblkQueue>, std::string> Create(
        uint16_t queue_id, const abi::CtrlDevInfo& dev_info, int cdev_fd,
        std::shared_ptr<util::Ring> ring);

    ~UblkQueue();
    UblkQueue(const UblkQueue&) = delete;
    UblkQueue& operator=(const UblkQueue&) = delete;

    uint16_t QueueId() const { return queue_id_; }
    /// Rust `iodepth`.
    uint32_t Depth() const { return depth_; }
    /// Rust `is_zero_copy` — whether the device negotiated AUTO_BUF_REG.
    bool IsZeroCopy() const;
    uint32_t CdevIndex() const { return cdev_idx_; }

    /// Rust `prep_slot` — arm `tag` with `buf` and wait for a request.
    ///
    /// Sends FETCH_REQ, whose `result` field is meaningless: there is no
    /// previous request in this slot to report on. That is the whole
    /// difference from the call below.
    core::Expected<core::Unit, std::string> PrepSlot(uint16_t tag, const IoBuffer& buf);

    /// Rust `submit_and_fetch_for_slot` — report `result` and re-arm `tag`.
    ///
    /// One command rather than a commit followed by a fetch: two would leave
    /// a window where the slot is neither serving nor waiting, and the queue
    /// would lose a slot's worth of depth on every round.
    core::Expected<core::Unit, std::string> SubmitAndFetchForSlot(uint16_t tag,
                                                                  const IoBuffer& buf,
                                                                  int32_t result);

    /// Rust `io_desc(tag)` — the request the kernel put in this slot.
    const abi::IoDesc* DescAt(uint16_t tag) const;

    /// The `ublksrv_io_cmd` either call above would send. Exposed so the
    /// payload can be checked without a device: `addr` is the field Linux
    /// rejects a non-zero value in under auto-registration, and getting it
    /// wrong is silent until a real kernel refuses the command.
    abi::IoCmd BuildIoCmd(uint16_t tag, const IoBuffer& buf, int32_t result) const;

 private:
    UblkQueue();

    /// Rust `send_io_cmd`.
    core::Expected<core::Unit, std::string> SendIoCmd(const char* what, uint32_t cmd_op,
                                                      const abi::IoCmd& cmd,
                                                      const IoBuffer& buf);

    uint16_t queue_id_;
    uint32_t depth_;
    abi::CtrlDevInfo dev_info_;
    std::shared_ptr<util::Ring> ring_;
    std::unique_ptr<QueueIoDescs> descs_;
    uint32_t cdev_idx_;
    bool cdev_registered_;
};

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_QUEUE_H_
