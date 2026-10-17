// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/dev.rs — `queue_work` / `slot_task`, and the thread
// they run on.
//
// Structural difference worth stating up front. Rust gives each slot its own
// async task on a current-thread runtime: a task arms its slot, awaits the
// completion, calls the target, and commits. There are no coroutines here, so
// the same shape is expressed as one event loop per queue that dispatches
// completions to slots by tag. The externally visible behaviour is identical
// -- `depth` slots armed at once, each serving independently -- but the loop
// is explicit rather than implied by the executor.
//
// What is *not* a free choice: one thread and one io_uring per queue. Rust's
// comment on `spawn_queue_workers` records why sharing a ring across devices
// deadlocks DEL_DEV -- unregistering an fd only drops a refcount on the
// kernel's resource node, and another device holding that node keeps the ublk
// char device from being released. The same applies here.
#ifndef AGENTENV_STORAGE_UBLK_QUEUE_WORKER_H_
#define AGENTENV_STORAGE_UBLK_QUEUE_WORKER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/ublk/abi.h"
#include "agentenv/storage/ublk/dev.h"
#include "agentenv/storage/util/io_ring.h"

namespace agentenv {
namespace storage {
namespace ublk {

/// Rust `AsyncIoRingBuilder` sizes in `UVMUblkQueue::new`, as a function of
/// queue depth.
///
/// Each is a multiple of the depth rather than a constant: a queue of depth N
/// has N commands outstanding at all times, so a ring sized below that would
/// refuse submissions the protocol requires to be standing.
util::IoRingConfig QueueRingConfig(uint32_t depth);

/// Why a queue worker stopped.
///
/// Separated from the error string because the two are acted on differently:
/// a stopped queue is normal at shutdown and a failure is not, and the device
/// teardown path needs to tell them apart without parsing text.
enum class QueueExitReason {
    /// `Stop()` was asked for and every slot wound down.
    Requested,
    /// The kernel answered a queue command with an error.
    CommandFailed,
    /// Setup never completed -- the ring, the mapping or the target's `Init`.
    SetupFailed,
};

struct QueueExit {
    QueueExitReason reason;
    /// Empty when `Requested`.
    std::string error;
    /// Requests served before stopping. Zero on a setup failure, which is how
    /// a queue that never started is told from one that stopped early.
    uint64_t served;

    QueueExit() : reason(QueueExitReason::Requested), served(0) {}
};

/// One queue's event loop, exposed so it can be driven without a thread.
///
/// Owns the ring, the queue and the slot buffers. Not copyable and not
/// movable across threads once started: the ring is created `single_issuer`,
/// which is a promise to the kernel that only one task submits.
class QueueWorker {
 public:
    /// Rust `queue_work`'s setup half: build the ring, map the descriptors,
    /// call the target's `Init`, allocate one buffer set per slot.
    ///
    /// Does not arm anything. Arming is `ArmAllSlots`, kept separate because
    /// a half-armed queue has to be distinguishable from one that never
    /// started -- the device is published on the strength of this having
    /// succeeded.
    static core::Expected<std::unique_ptr<QueueWorker>, std::string> Create(
        uint16_t qid, const abi::CtrlDevInfo& dev_info, int cdev_fd,
        std::shared_ptr<UblkTarget> target);

    ~QueueWorker();
    QueueWorker(const QueueWorker&) = delete;
    QueueWorker& operator=(const QueueWorker&) = delete;

    /// Rust's `for tag in 0..iodepth { spawn_local(slot_task) }` — send
    /// FETCH_REQ for every slot.
    core::Expected<core::Unit, std::string> ArmAllSlots();

    /// Runs until `Stop()` is called or a command fails.
    ///
    /// Each completion names its slot through `user_data`; the target is
    /// called for that slot and the result goes back in the same
    /// COMMIT_AND_FETCH_REQ that re-arms it.
    QueueExit Run();

    /// Processes whatever completions are already pending, without blocking.
    ///
    /// Returns the number of requests served. Exposed for tests and for a
    /// caller that drives several queues from one thread; `Run` is this in a
    /// loop with a blocking wait.
    core::Expected<uint64_t, std::string> PollOnce();

    /// Asks `Run` to return. Safe from another thread -- it only sets a flag
    /// the loop reads; it does not touch the ring, which belongs to the
    /// loop's own thread.
    void Stop();

    uint16_t QueueId() const;
    uint32_t Depth() const;
    uint64_t Served() const { return served_; }
    util::Ring* Ring() { return ring_.get(); }

    /// Rust's slot-task body for one completion: look up the descriptor, call
    /// the target, commit the result and re-arm.
    ///
    /// Separated from the loop so the dispatch can be checked without a
    /// kernel: which slot a completion belongs to, and what happens when the
    /// descriptor or the result is bad.
    core::Expected<core::Unit, std::string> HandleCompletion(
        const util::CompletionEntry& cqe);

    /// The tag encoded in a completion's `user_data`, or absent when it does
    /// not belong to this queue.
    ///
    /// Checked rather than assumed: a ring shared with anything else would
    /// otherwise have its completions dispatched to a slot, and the target
    /// would be handed a descriptor for a request that was never made.
    static bool TagFromUserData(uint64_t user_data, uint16_t expect_qid,
                                uint32_t depth, uint16_t* tag);
    /// The inverse, used when submitting.
    static uint64_t UserDataFor(uint16_t qid, uint16_t tag);

 private:
    QueueWorker();

    uint16_t qid_;
    uint32_t depth_;
    abi::CtrlDevInfo dev_info_;
    std::shared_ptr<UblkTarget> target_;
    std::shared_ptr<util::Ring> ring_;
    std::unique_ptr<UblkQueue> queue_;
    /// One per slot, indexed by tag.
    std::vector<SlotBuffers> slots_;
    uint64_t served_;
    /// Read by the loop, written by `Stop` from any thread.
    volatile bool stopping_;
};

/// Rust's `ublk-q-{dev_id}-{qid}` thread name, truncated to what Linux takes.
///
/// `pthread_setname_np` accepts 15 characters plus a NUL and fails outright
/// on anything longer -- it does not truncate. A large dev_id would therefore
/// leave the thread unnamed, which is exactly the case where a host has
/// enough devices for the name to matter.
std::string QueueThreadName(uint32_t dev_id, uint16_t qid);

/// Rust `spawn_queue_worker_thread` — the real launcher.
///
/// One thread per queue, named so that a thread stuck in a queue loop can be
/// attributed to the device it belongs to.
class ThreadedQueueWorkerLauncher : public QueueWorkerLauncher {
 public:
    ThreadedQueueWorkerLauncher(int cdev_fd, std::shared_ptr<UblkTarget> target);
    ~ThreadedQueueWorkerLauncher();

    core::Expected<core::Unit, std::string> Spawn(
        uint16_t qid, const abi::CtrlDevInfo& dev_info) override;
    void Join() override;

    /// Asks every worker to stop, without waiting.
    ///
    /// Separate from `Join` because DEL_DEV blocks until the char device has
    /// no references left: the caller has to stop the workers, wait for them,
    /// and only then delete.
    void StopAll();

    /// How each worker ended. Valid after `Join`.
    std::vector<QueueExit> Exits() const;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_QUEUE_WORKER_H_
