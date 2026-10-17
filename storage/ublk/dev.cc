// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/dev.rs
#include "agentenv/storage/ublk/dev.h"

#include "agentenv/storage/ublk/queue_worker.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>

namespace agentenv {
namespace storage {
namespace ublk {

namespace {

void SleepMs(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    // Resumed across signals: a short sleep cut by an unrelated signal would
    // otherwise shorten the retry budget rather than the wait.
    while (::nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

}  // namespace

core::Expected<int, std::string> OpenCharDeviceWithRetry(const std::string& path,
                                                         int max_attempts, int sleep_ms) {
    int attempt = 0;
    while (true) {
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;

        const int err = errno;
        // Only these three are retried. The node is created by udev *after*
        // the kernel answers ADD_DEV, so on a busy host the first open
        // commonly loses that race -- ENOENT while the node is still
        // missing, EACCES/EPERM in the window before udev fixes its
        // ownership. Anything else -- ENODEV, EBUSY -- is a real failure
        // that waiting cannot fix, and retrying it would turn an instant
        // error into a 1.6-second one.
        //
        // NOTE: the EACCES/EPERM half is only exercised by a test that needs
        // a non-root user, so it is unverified on a root-only CI host.
        if (err != ENOENT && err != EACCES && err != EPERM) {
            std::ostringstream why;
            why << "open " << path << " failed: " << std::strerror(err) << " (errno " << err
                << ")";
            return core::make_unexpected(why.str());
        }
        if (attempt >= max_attempts) {
            std::ostringstream why;
            why << "tried to open " << path << " " << attempt
                << " times but it is still unavailable: " << std::strerror(err);
            return core::make_unexpected(why.str());
        }
        ++attempt;
        SleepMs(sleep_ms);
    }
}

core::Expected<SlotBuffers, std::string> AllocateSlotBuffers(
    std::shared_ptr<util::Ring> ring, const abi::CtrlDevInfo& dev_info,
    const core::Optional<std::size_t>& extra_len) {
    const std::size_t bufsize = static_cast<std::size_t>(dev_info.max_io_buf_bytes);
    // Both arguments are checked before anything is allocated. They are
    // properties of the request, not of the ring, so failing on them first
    // keeps the diagnosis away from allocation errors -- and means a bad
    // size is reported identically whatever the host supports.
    if (bufsize == 0) {
        return core::make_unexpected(std::string("slot buffers: zero max_io_buf_bytes"));
    }
    if (extra_len.has_value() && *extra_len % 512 != 0) {
        // Rejected rather than rounded. The target asked for this size
        // because it is what its own arithmetic needs; silently enlarging it
        // would hide a bug there, and the kernel would reject the unaligned
        // transfer anyway.
        std::ostringstream why;
        why << "extra buffer length (" << *extra_len << ") not aligned";
        return core::make_unexpected(why.str());
    }
    if (!ring) {
        return core::make_unexpected(std::string("slot buffers: null ring"));
    }

    SlotBuffers out;
    // The device's negotiated mode decides the shape, not a local preference:
    // under AUTO_BUF_REG the kernel supplies the pages and the server must
    // only reserve a slot, while without it the server owns the memory.
    //
    // NOTE: which branch is taken is only observable with a ring that has
    // sparse buffer tables (Linux 5.13+), so on older hosts the test suite
    // skips it and an inverted condition here would go unnoticed. The
    // consequence on a real device is severe -- a zero `cmd_addr` on a plain
    // device points the kernel at address zero -- so treat changes to this
    // condition as unverified until run on 5.13 or newer.
    if ((dev_info.flags & abi::kFAutoBufReg) != 0) {
        const core::Expected<std::shared_ptr<AutoRegBuffer>, std::string> b =
            AutoRegBuffer::Create(ring, bufsize);
        if (!b.ok()) {
            return core::make_unexpected(std::string("allocate io auto_reg buffer: ") +
                                         b.error());
        }
        out.buf = IoBuffer::OfAutoReg(b.value());
    } else {
        // 512-byte alignment, as Rust does: the backing file may be opened
        // O_DIRECT, which rejects unaligned buffers outright.
        const core::Expected<std::shared_ptr<UserBuffer>, std::string> b =
            UserBuffer::Create(ring, bufsize, 512);
        if (!b.ok()) {
            return core::make_unexpected(std::string("allocate io user buffer: ") + b.error());
        }
        out.buf = IoBuffer::OfUser(b.value());
    }

    if (extra_len.has_value()) {
        // Always a user buffer, even on an auto-reg device: the extra buffer
        // is the target's own scratch space and the kernel knows nothing
        // about it, so there is no slot for it to fill.
        const core::Expected<std::shared_ptr<UserBuffer>, std::string> e =
            UserBuffer::Create(ring, *extra_len, 512);
        if (!e.ok()) {
            return core::make_unexpected(std::string("allocate extra buffer: ") + e.error());
        }
        out.extra = IoBuffer::OfUser(e.value());
    }
    return out;
}

// ---------------------------------------------------------------------------
// UblkDev
// ---------------------------------------------------------------------------

UblkDev::UblkDev() : cdev_fd_(-1) {
    std::memset(&params_, 0, sizeof(params_));
}

UblkDev::~UblkDev() {
    // Workers first: they hold mappings taken from this fd, and the kernel
    // keeps /dev/ublkcN alive until every reference is gone. Closing it with
    // workers still running would leave the device undeletable.
    if (launcher_) launcher_->Join();
    if (cdev_fd_ >= 0) ::close(cdev_fd_);
}

core::Expected<core::Unit, std::string> RunStartSequence(const StartPlan& plan,
                                                          const abi::Params& params) {
    if (!plan.read_dev_info || !plan.spawn || !plan.set_params || !plan.start_dev) {
        return core::make_unexpected(std::string("ublk dev: incomplete start plan"));
    }

    // Refreshed from the kernel rather than trusted: `state` is kernel-owned
    // and the local copy is from ADD_DEV, which says nothing about what has
    // happened since.
    const core::Expected<abi::CtrlDevInfo, std::string> info = plan.read_dev_info();
    if (!info.ok()) {
        return core::make_unexpected(std::string("read dev info when starting: ") +
                                     info.error());
    }
    const abi::CtrlDevInfo dev_info = info.value();
    if (dev_info.state != abi::kDevStateDead) {
        // Not a formality: starting a live device spawns a second set of
        // workers against queues the first set is already serving, and both
        // would fetch from the same slots.
        std::ostringstream why;
        why << "device state is not dead: " << dev_info.state;
        return core::make_unexpected(why.str());
    }

    // Workers before START_DEV. The kernel publishes /dev/ublkbN on that
    // command and expects the slots to be armed already; the other order
    // exposes a disk nothing is waiting to serve.
    for (uint16_t qid = 0; qid < dev_info.nr_hw_queues; ++qid) {
        const core::Expected<core::Unit, std::string> spawned = plan.spawn(qid, dev_info);
        if (!spawned.ok()) {
            std::ostringstream why;
            why << "spawn ublk queue worker dev_id=" << dev_info.dev_id << " qid=" << qid
                << " nr_hw_queues=" << dev_info.nr_hw_queues
                << " queue_depth=" << dev_info.queue_depth << ": " << spawned.error();
            // Already-started workers are left running and joined by the
            // caller's teardown. Killing them here would race their own
            // setup, and the device was never published, so they have
            // nothing to serve and exit on their own.
            return core::make_unexpected(why.str());
        }
    }

    // Geometry before publication: once /dev/ublkbN exists its parameters
    // cannot be changed, so a SET_PARAMS after START_DEV would be refused.
    const core::Expected<core::Unit, std::string> set = plan.set_params(params);
    if (!set.ok()) {
        return core::make_unexpected(std::string("set params when starting: ") + set.error());
    }
    const core::Expected<core::Unit, std::string> started = plan.start_dev();
    if (!started.ok()) {
        return core::make_unexpected(std::string("start the device: ") + started.error());
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> UblkDev::Start() {
    if (!ctrl_) {
        return core::make_unexpected(std::string("ublk dev: no control channel"));
    }
    if (!launcher_) {
        return core::make_unexpected(std::string("ublk dev: no queue worker launcher"));
    }

    UblkCtrl* ctrl = ctrl_.get();
    QueueWorkerLauncher* launcher = launcher_.get();

    StartPlan plan;
    plan.read_dev_info =
        [ctrl]() -> core::Expected<abi::CtrlDevInfo, std::string> {
            const core::Expected<core::Unit, std::string> r = ctrl->GetDevInfo();
            if (!r.ok()) return core::make_unexpected(r.error());
            return ctrl->DevInfo();
        };
    plan.spawn = [launcher](uint16_t qid, const abi::CtrlDevInfo& info) {
        return launcher->Spawn(qid, info);
    };
    plan.set_params = [ctrl](const abi::Params& p) { return ctrl->SetParams(p); };
    plan.start_dev = [ctrl]() { return ctrl->StartDev(); };

    return RunStartSequence(plan, params_);
}

core::Expected<core::Unit, std::string> UblkDev::Del() {
    if (!ctrl_) {
        return core::make_unexpected(std::string("ublk dev: no control channel"));
    }
    return ctrl_->DelDev();
}

void UblkDev::WaitForQueueWorkers() {
    if (launcher_) launcher_->Join();
}

std::string UblkDev::DevicePath() const {
    return ctrl_ ? ctrl_->BlockDevPath() : std::string();
}

uint32_t UblkDev::DevId() const {
    return ctrl_ ? ctrl_->DevId() : 0;
}

// ---------------------------------------------------------------------------
// UblkDevBuilder
// ---------------------------------------------------------------------------

UblkDevBuilder::UblkDevBuilder(std::unique_ptr<UblkCtrl> ctrl) : ctrl_(std::move(ctrl)) {}

UblkDevBuilder& UblkDevBuilder::SetTarget(std::shared_ptr<UblkTarget> target) {
    tgt_ = target;
    return *this;
}

UblkDevBuilder& UblkDevBuilder::SetLauncher(std::shared_ptr<QueueWorkerLauncher> launcher) {
    launcher_ = launcher;
    return *this;
}

core::Expected<std::unique_ptr<UblkDev>, std::string> UblkDevBuilder::Build() {
    if (!tgt_) {
        return core::make_unexpected(std::string("do not set target"));
    }
    if (!ctrl_) {
        return core::make_unexpected(std::string("ublk dev: no control channel"));
    }

    const core::Expected<core::Unit, std::string> added = ctrl_->AddDev();
    if (!added.ok()) {
        return core::make_unexpected(std::string("add dev when building: ") + added.error());
    }

    // Read *after* ADD_DEV: with `dev_id` left unset the kernel assigns one
    // and writes it back, so the path below is only knowable now.
    const abi::CtrlDevInfo dev_info = ctrl_->DevInfo();
    const abi::Params params = tgt_->DevParams(dev_info);

    const core::Expected<int, std::string> fd = OpenCharDeviceWithRetry(
        ctrl_->CharDevPath(), kCdevOpenAttempts, kCdevOpenSleepMs);
    if (!fd.ok()) {
        // The device exists in the kernel but is unusable. Released here so a
        // failed build does not leak the id for the module's lifetime.
        ctrl_->DelDev();
        return core::make_unexpected(fd.error());
    }

    std::unique_ptr<UblkDev> dev(new UblkDev());
    dev->tgt_ = tgt_;
    dev->tgt_name_ = tgt_->DevName();
    dev->params_ = params;
    dev->cdev_fd_ = fd.value();
    // The real launcher unless a caller supplied one. Rust has exactly one
    // implementation, so defaulting to anything else -- including nothing --
    // would make a built device silently unable to start.
    //
    // Constructed here rather than in the builder because it needs the char
    // device fd, which does not exist until the open above succeeds.
    dev->launcher_ = launcher_ ? launcher_
                               : std::shared_ptr<QueueWorkerLauncher>(
                                     new ThreadedQueueWorkerLauncher(fd.value(), tgt_));
    dev->ctrl_.reset(ctrl_.release());
    return std::move(dev);
}

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
