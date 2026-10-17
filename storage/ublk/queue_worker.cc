// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/dev.rs — `queue_work` / `slot_task`.
#include "agentenv/storage/ublk/queue_worker.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sstream>
#include <thread>
#include <vector>

namespace agentenv {
namespace storage {
namespace ublk {

util::IoRingConfig QueueRingConfig(uint32_t depth) {
    util::IoRingConfig c;
    // Rust's `sqe_entries(depth * 2)` / `cqe_entries(depth * 2)`. A queue has
    // `depth` commands standing at all times and briefly twice that while a
    // slot is being re-armed, so a ring sized at `depth` would refuse
    // submissions the protocol requires to be outstanding.
    c.sq_entries = depth * 2;
    c.cq_entries = depth * 2;
    // Rust's `nr_sparse_buffer(depth * 4)`: one buffer per slot, plus the
    // target's optional extra, with headroom for the target's own
    // registrations in `Init`.
    c.sparse_buffers = depth * 4;
    // Rust's `nr_sparse_file(16)`: the queue's own char device plus whatever
    // files the target registers.
    c.sparse_files = 16;
    // Required: every queue command is an `IORING_OP_URING_CMD`.
    c.sqe128 = true;
    // One thread owns this ring, which is what `single_issuer` promises.
    c.single_issuer = true;
    c.coop_taskrun = true;

    // The caps `Ring::Open` enforces. Clamped rather than passed through, so
    // a deep queue degrades to a smaller ring instead of failing to start --
    // the protocol still works, there is simply less headroom.
    if (c.sq_entries > 4096) c.sq_entries = 4096;
    if (c.cq_entries > 8192) c.cq_entries = 8192;
    if (c.sparse_buffers > 4096) c.sparse_buffers = 4096;
    return c;
}

std::string QueueThreadName(uint32_t dev_id, uint16_t qid) {
    // Linux caps this at 15 characters plus a NUL, and `pthread_setname_np`
    // *fails* rather than truncating -- so an over-long name leaves the
    // thread anonymous, precisely on the hosts with enough devices for names
    // to matter. Truncated from the left of the device id, keeping the queue
    // id intact: within one device the queue is what distinguishes threads.
    std::ostringstream os;
    os << "ublk-q" << dev_id << "-" << qid;
    std::string name = os.str();
    if (name.size() > 15) {
        std::ostringstream tail;
        tail << "-" << qid;
        const std::string suffix = tail.str();
        const std::size_t room = suffix.size() < 15 ? 15 - suffix.size() : 0;
        name = name.substr(0, room) + suffix;
    }
    return name;
}

// ---------------------------------------------------------------------------
// QueueWorker
// ---------------------------------------------------------------------------

QueueWorker::QueueWorker()
    : qid_(0), depth_(0), served_(0), stopping_(false) {
    std::memset(&dev_info_, 0, sizeof(dev_info_));
}

QueueWorker::~QueueWorker() {
    // Order matters: the queue unregisters the char device from the ring in
    // its own destructor, so it has to go before the ring does. The slot
    // buffers likewise hold registered indices.
    slots_.clear();
    queue_.reset();
    ring_.reset();
}

uint64_t QueueWorker::UserDataFor(uint16_t qid, uint16_t tag) {
    return (static_cast<uint64_t>(qid) << 32) | static_cast<uint64_t>(tag);
}

bool QueueWorker::TagFromUserData(uint64_t user_data, uint16_t expect_qid,
                                  uint32_t depth, uint16_t* tag) {
    const uint32_t qid = static_cast<uint32_t>(user_data >> 32);
    // Checked, not assumed. A completion from anything else sharing this ring
    // would otherwise be dispatched to a slot, and the target handed a
    // descriptor for a request that was never made.
    if (qid != static_cast<uint32_t>(expect_qid)) return false;
    const uint64_t raw = user_data & 0xFFFFFFFFull;
    if (raw >= depth) return false;
    if (tag != NULL) *tag = static_cast<uint16_t>(raw);
    return true;
}

core::Expected<std::unique_ptr<QueueWorker>, std::string> QueueWorker::Create(
    uint16_t qid, const abi::CtrlDevInfo& dev_info, int cdev_fd,
    std::shared_ptr<UblkTarget> target) {
    if (!target) {
        return core::make_unexpected(std::string("queue worker: null target"));
    }
    if (dev_info.queue_depth == 0) {
        return core::make_unexpected(std::string("queue worker: zero queue depth"));
    }

    std::unique_ptr<QueueWorker> w(new QueueWorker());
    w->qid_ = qid;
    w->depth_ = dev_info.queue_depth;
    w->dev_info_ = dev_info;
    w->target_ = target;

    // A ring of this queue's own. Sharing one across queues or devices
    // deadlocks DEL_DEV: unregistering an fd only drops a refcount on the
    // kernel's resource node, and another holder keeps the ublk char device
    // from being released.
    core::Expected<std::unique_ptr<util::Ring>, std::string> ring =
        util::Ring::Open(QueueRingConfig(dev_info.queue_depth));
    if (!ring.ok()) {
        std::ostringstream why;
        why << "create io uring for queue " << qid << ": " << ring.error();
        return core::make_unexpected(why.str());
    }
    w->ring_.reset(ring.take_value().release());

    core::Expected<std::unique_ptr<UblkQueue>, std::string> queue =
        UblkQueue::Create(qid, dev_info, cdev_fd, w->ring_);
    if (!queue.ok()) {
        return core::make_unexpected(std::string("create ublk queue: ") + queue.error());
    }
    w->queue_.reset(queue.take_value().release());

    // Before any buffer is allocated, as Rust does: the target may register
    // its own files, and an index from another queue's ring is meaningless
    // here.
    const core::Expected<core::Unit, std::string> init =
        target->Init(qid, w->ring_.get());
    if (!init.ok()) {
        return core::make_unexpected(std::string("init target for queue: ") + init.error());
    }

    const core::Optional<std::size_t> extra = target->PerSlotExtraBufLen();
    w->slots_.resize(dev_info.queue_depth);
    for (uint32_t tag = 0; tag < dev_info.queue_depth; ++tag) {
        core::Expected<SlotBuffers, std::string> bufs =
            AllocateSlotBuffers(w->ring_, dev_info, extra);
        if (!bufs.ok()) {
            std::ostringstream why;
            why << "allocate buffers for queue " << qid << " slot " << tag << ": "
                << bufs.error();
            return core::make_unexpected(why.str());
        }
        w->slots_[tag] = bufs.value();
    }
    return std::move(w);
}

core::Expected<core::Unit, std::string> QueueWorker::ArmAllSlots() {
    for (uint32_t tag = 0; tag < depth_; ++tag) {
        const core::Expected<core::Unit, std::string> armed =
            queue_->PrepSlot(static_cast<uint16_t>(tag), slots_[tag].buf);
        if (!armed.ok()) {
            std::ostringstream why;
            why << "prep slot (" << tag << "): " << armed.error();
            // Slots armed so far are left armed. They are standing commands
            // the kernel owns now; cancelling them would need the device to
            // be stopped, which is the caller's decision rather than this
            // function's.
            return core::make_unexpected(why.str());
        }
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> QueueWorker::HandleCompletion(
    const util::CompletionEntry& cqe) {
    uint16_t tag = 0;
    if (!TagFromUserData(cqe.user_data, qid_, depth_, &tag)) {
        // Not this queue's. Ignored rather than failed: the ring is this
        // queue's alone today, but dispatching a foreign completion to a slot
        // would be far worse than skipping one.
        return core::Unit();
    }

    // Negative means the kernel refused the command itself -- not a failed
    // guest request, which is reported through the descriptor. The queue
    // cannot continue: the slot is no longer armed and nothing will re-arm
    // it.
    if (cqe.res < 0) {
        std::ostringstream why;
        why << "queue " << qid_ << " slot " << tag << ": "
            << std::strerror(-cqe.res) << " (errno " << -cqe.res << ")";
        return core::make_unexpected(why.str());
    }

    const abi::IoDesc* desc = queue_->DescAt(tag);
    if (desc == NULL) {
        std::ostringstream why;
        why << "queue " << qid_ << ": no descriptor at tag " << tag;
        return core::make_unexpected(why.str());
    }

    // The target's return is the value the kernel gets: bytes transferred, or
    // `-errno`. A failed request is reported to the guest, not to this
    // process, which is why this is not an error type.
    const int32_t result = target_->HandleIoRequest(
        qid_, tag, *desc, &slots_[tag].buf,
        slots_[tag].extra.Empty() ? NULL : &slots_[tag].extra, ring_.get());

    // One command, not a commit then a fetch: two would leave the slot
    // neither serving nor waiting, and the queue would lose depth each round.
    const core::Expected<core::Unit, std::string> committed =
        queue_->SubmitAndFetchForSlot(tag, slots_[tag].buf, result);
    if (!committed.ok()) {
        std::ostringstream why;
        why << "commit and fetch for queue " << qid_ << " slot " << tag << ": "
            << committed.error();
        return core::make_unexpected(why.str());
    }
    ++served_;
    return core::Unit();
}

core::Expected<uint64_t, std::string> QueueWorker::PollOnce() {
    const std::vector<util::CompletionEntry> ready = ring_->Reap();
    uint64_t handled = 0;
    for (std::size_t i = 0; i < ready.size(); ++i) {
        const core::Expected<core::Unit, std::string> r = HandleCompletion(ready[i]);
        if (!r.ok()) return core::make_unexpected(r.error());
        ++handled;
    }
    return handled;
}

QueueExit QueueWorker::Run() {
    QueueExit exit;
    while (!stopping_) {
        // Blocks until at least one completion. The slots are standing
        // commands, so there is nothing to poll for in between -- a spin
        // here would burn a core per queue.
        const core::Expected<std::vector<util::CompletionEntry>, std::string> ready =
            ring_->WaitAndReap(1);
        if (!ready.ok()) {
            exit.reason = QueueExitReason::CommandFailed;
            exit.error = ready.error();
            exit.served = served_;
            return exit;
        }
        for (std::size_t i = 0; i < ready.value().size(); ++i) {
            const core::Expected<core::Unit, std::string> r =
                HandleCompletion(ready.value()[i]);
            if (!r.ok()) {
                exit.reason = QueueExitReason::CommandFailed;
                exit.error = r.error();
                exit.served = served_;
                return exit;
            }
        }
    }
    exit.reason = QueueExitReason::Requested;
    exit.served = served_;
    return exit;
}

void QueueWorker::Stop() {
    // Only a flag. Touching the ring from another thread would break the
    // `single_issuer` promise this ring was created under.
    stopping_ = true;
}

uint16_t QueueWorker::QueueId() const { return qid_; }
uint32_t QueueWorker::Depth() const { return depth_; }

// ---------------------------------------------------------------------------
// ThreadedQueueWorkerLauncher
// ---------------------------------------------------------------------------

struct ThreadedQueueWorkerLauncher::Impl {
    int cdev_fd;
    std::shared_ptr<UblkTarget> target;
    std::vector<std::thread> threads;
    /// Held so `StopAll` can reach a running worker. Each is owned by its own
    /// thread for everything else.
    std::vector<QueueWorker*> workers;
    std::vector<QueueExit> exits;
    std::mutex mu;
    bool joined;

    Impl(int fd, std::shared_ptr<UblkTarget> t)
        : cdev_fd(fd), target(t), joined(false) {}
};

ThreadedQueueWorkerLauncher::ThreadedQueueWorkerLauncher(
    int cdev_fd, std::shared_ptr<UblkTarget> target)
    : impl_(new Impl(cdev_fd, target)) {}

ThreadedQueueWorkerLauncher::~ThreadedQueueWorkerLauncher() {
    // A running thread holds a mapping taken from the char device, and the
    // kernel keeps /dev/ublkcN alive until every reference is gone. Leaving
    // one running would make the device undeletable for the process's life.
    StopAll();
    Join();
}

core::Expected<core::Unit, std::string> ThreadedQueueWorkerLauncher::Spawn(
    uint16_t qid, const abi::CtrlDevInfo& dev_info) {
    // Created on the caller's thread so a setup failure is reported here,
    // where the start sequence can stop before publishing the device. Doing
    // it inside the thread would surface as a worker that exits immediately,
    // after /dev/ublkbN already exists.
    core::Expected<std::unique_ptr<QueueWorker>, std::string> worker =
        QueueWorker::Create(qid, dev_info, impl_->cdev_fd, impl_->target);
    if (!worker.ok()) return core::make_unexpected(worker.error());

    const core::Expected<core::Unit, std::string> armed = worker.value()->ArmAllSlots();
    if (!armed.ok()) return core::make_unexpected(armed.error());

    QueueWorker* raw = worker.take_value().release();
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->workers.push_back(raw);
    }

    const uint32_t dev_id = dev_info.dev_id;
    Impl* impl = impl_.get();
    impl_->threads.push_back(std::thread([impl, raw, dev_id, qid]() {
        const std::string name = QueueThreadName(dev_id, qid);
        ::pthread_setname_np(::pthread_self(), name.c_str());

        const QueueExit exit = raw->Run();
        {
            std::lock_guard<std::mutex> lock(impl->mu);
            impl->exits.push_back(exit);
        }
        delete raw;
    }));
    return core::Unit();
}

void ThreadedQueueWorkerLauncher::StopAll() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    for (std::size_t i = 0; i < impl_->workers.size(); ++i) {
        impl_->workers[i]->Stop();
    }
    // The pointers are cleared here, not deleted: each worker is freed by its
    // own thread once `Run` returns, which is the only point at which the
    // ring it owns is no longer in use.
    impl_->workers.clear();
}

void ThreadedQueueWorkerLauncher::Join() {
    if (impl_->joined) return;
    for (std::size_t i = 0; i < impl_->threads.size(); ++i) {
        if (impl_->threads[i].joinable()) impl_->threads[i].join();
    }
    impl_->threads.clear();
    impl_->joined = true;
}

std::vector<QueueExit> ThreadedQueueWorkerLauncher::Exits() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->exits;
}

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
