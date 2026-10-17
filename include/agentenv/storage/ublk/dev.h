// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/dev.rs — UVMUblkDev + UVMUblkTarget.
//
// The piece that joins the control plane to the queue plane. A device is
// added, its char device opened, one worker thread started per hardware
// queue, and only then is the block device published.
//
// The ordering is the whole design: `START_DEV` is what makes /dev/ublkbN
// appear, and the kernel expects every queue to already have its slots armed
// when it does. Publishing first would expose a disk whose requests nothing
// is waiting to serve.
#ifndef AGENTENV_STORAGE_UBLK_DEV_H_
#define AGENTENV_STORAGE_UBLK_DEV_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/ublk/abi.h"
#include "agentenv/storage/ublk/ctrl.h"
#include "agentenv/storage/ublk/io_buffer.h"
#include "agentenv/storage/ublk/queue.h"

namespace agentenv {
namespace storage {
namespace ublk {

/// Rust `trait UVMUblkTarget` — what a device actually serves.
///
/// Called from every queue thread concurrently, hence the const methods: one
/// target backs all queues, and Rust's `&self` here says the same thing.
class UblkTarget {
 public:
    virtual ~UblkTarget() {}

    /// Rust `DEV_NAME`.
    virtual std::string DevName() const = 0;

    /// Rust `init(qid, io_ring)` — called once per queue, on that queue's own
    /// thread, before any slot is armed.
    ///
    /// This is where a target registers its own fds with the queue's ring.
    /// It cannot be done earlier: each queue has a separate ring, and an
    /// index from one is meaningless in another.
    virtual core::Expected<core::Unit, std::string> Init(uint16_t qid,
                                                         util::Ring* ring) = 0;

    /// Rust `per_slot_extra_buf_len` — an optional second buffer per slot.
    ///
    /// Must be a multiple of 512 when present; a target that needs scratch
    /// space for a read-modify-write cycle asks for it here rather than
    /// allocating per request.
    virtual core::Optional<std::size_t> PerSlotExtraBufLen() const = 0;

    /// Rust `ublk_dev_params` — the geometry sent in SET_PARAMS.
    ///
    /// Takes the device info because the kernel decides `max_io_buf_bytes`,
    /// and `max_sectors` has to be derived from what it actually granted.
    virtual abi::Params DevParams(const abi::CtrlDevInfo& dev_info) const = 0;

    /// Rust `handle_io_request` — serve one request, return its result.
    ///
    /// The return is the value that goes into the next
    /// `COMMIT_AND_FETCH_REQ`: bytes transferred when non-negative, `-errno`
    /// when not. It is never an error type, because a failed request is
    /// reported to the guest rather than to this process.
    virtual int32_t HandleIoRequest(uint16_t qid, uint16_t tag,
                                    const abi::IoDesc& desc, IoBuffer* buf,
                                    IoBuffer* extra, util::Ring* ring) = 0;
};

/// How a queue worker thread is started.
///
/// Injected so the start sequence can be exercised without a real device:
/// a worker needs a ublk char device to mmap, which only exists after a real
/// ADD_DEV.
class QueueWorkerLauncher {
 public:
    virtual ~QueueWorkerLauncher() {}
    /// Rust `spawn_queue_worker_thread`. Returns once the thread is running,
    /// not once the queue is armed.
    virtual core::Expected<core::Unit, std::string> Spawn(
        uint16_t qid, const abi::CtrlDevInfo& dev_info) = 0;
    /// Waits for every started worker to finish.
    virtual void Join() = 0;
};

/// Rust `UVMUblkDev`.
class UblkDev {
 public:
    ~UblkDev();
    UblkDev(const UblkDev&) = delete;
    UblkDev& operator=(const UblkDev&) = delete;

    /// Rust `start`.
    ///
    /// Refreshes the device info, requires the device to be `DEAD`, starts
    /// the queue workers, then SET_PARAMS and START_DEV in that order.
    ///
    /// The state check is not a formality: starting a live device would spawn
    /// a second set of workers against queues the first set is already
    /// serving, and both would fetch from the same slots.
    core::Expected<core::Unit, std::string> Start();

    /// Rust `del` — DEL_DEV, releasing the device id.
    ///
    /// The kernel waits for every reference to /dev/ublkcN to be dropped
    /// before it returns, so this blocks forever unless the queue workers
    /// have exited first. Callers stop the device (or the process) before
    /// calling it.
    core::Expected<core::Unit, std::string> Del();

    /// Rust `wait_for_bg_tasks`.
    void WaitForQueueWorkers();

    /// Rust `device_path` — `/dev/ublkbN`, valid only after `Start`.
    std::string DevicePath() const;
    uint32_t DevId() const;
    std::string TargetName() const { return tgt_name_; }
    const abi::Params& Params() const { return params_; }
    UblkCtrl* Ctrl() { return ctrl_.get(); }
    /// The open `/dev/ublkcN`. Queue workers mmap their descriptor areas from
    /// it, so it must outlive every worker.
    int CharDevFd() const { return cdev_fd_; }

 private:
    friend class UblkDevBuilder;
    UblkDev();

    std::unique_ptr<UblkCtrl> ctrl_;
    std::shared_ptr<UblkTarget> tgt_;
    std::string tgt_name_;
    abi::Params params_;
    int cdev_fd_;
    std::shared_ptr<QueueWorkerLauncher> launcher_;
};

/// Rust `UVMUblkDevBuilder`.
class UblkDevBuilder {
 public:
    explicit UblkDevBuilder(std::unique_ptr<UblkCtrl> ctrl);

    UblkDevBuilder& SetTarget(std::shared_ptr<UblkTarget> target);
    /// Overrides how queue workers are started.
    UblkDevBuilder& SetLauncher(std::shared_ptr<QueueWorkerLauncher> launcher);

    /// Rust `build` — ADD_DEV, then open the char device.
    ///
    /// Opening retries on `ENOENT`/`EACCES`: the node is created by udev
    /// after the kernel answers ADD_DEV, so the first open commonly loses
    /// that race on a busy host. Rust retries 64 times at 25 ms.
    core::Expected<std::unique_ptr<UblkDev>, std::string> Build();

 private:
    std::unique_ptr<UblkCtrl> ctrl_;
    std::shared_ptr<UblkTarget> tgt_;
    std::shared_ptr<QueueWorkerLauncher> launcher_;
};

/// Rust's open loop in `build`, as a testable function.
///
/// Returns the fd, or the reason it gave up. `sleep_ms` is injectable so the
/// retry policy can be checked without waiting 1.6 seconds.
core::Expected<int, std::string> OpenCharDeviceWithRetry(const std::string& path,
                                                         int max_attempts,
                                                         int sleep_ms);

/// Rust's retry budget: 64 attempts, 25 ms apart.
const int kCdevOpenAttempts = 64;
const int kCdevOpenSleepMs = 25;

/// Rust `queue_work`'s body for one slot, minus the loop.
///
/// Allocates the slot's buffer (auto-registered or user, per the device's
/// negotiated mode) and the target's optional extra buffer. Separated out
/// because the choice is a protocol decision, while the loop around it is
/// just scheduling.
struct SlotBuffers {
    IoBuffer buf;
    IoBuffer extra;
};

core::Expected<SlotBuffers, std::string> AllocateSlotBuffers(
    std::shared_ptr<util::Ring> ring, const abi::CtrlDevInfo& dev_info,
    const core::Optional<std::size_t>& extra_len);

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_DEV_H_
