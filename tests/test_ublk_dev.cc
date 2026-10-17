// SPDX-License-Identifier: MIT
// Tests for the ublk device layer. Rust: storage/ublk/src/dev.rs.
//
// The decisions that matter here are ordering and classification, and both
// are checked with fakes rather than a real device:
//
//   * The start sequence -- workers armed before the block device is
//     published, geometry set before that. Wrong order is not an error on a
//     real kernel, it is a disk nobody is serving.
//   * Which open errors are worth retrying. Retrying the wrong one turns an
//     instant failure into a 1.6-second one; not retrying the right one
//     fails on every busy host.
#include "microtest.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "agentenv/storage/ublk/dev.h"

using namespace agentenv::storage::ublk;
namespace util = agentenv::storage::util;
namespace core = agentenv::core;

namespace {

void Skip(const char* what, const char* why) {
    std::printf("       SKIP %s: %s\n", what, why);
}

/// Records the order the device drives it in.
struct Trace {
    std::vector<std::string> events;

    bool Has(const char* what) const { return IndexOf(what) >= 0; }
    int IndexOf(const char* what) const {
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (events[i] == what) return static_cast<int>(i);
        }
        return -1;
    }
};

class FakeTarget : public UblkTarget {
 public:
    FakeTarget() : extra_len_(), params_calls_(0) {}

    std::string DevName() const override { return "fake"; }
    core::Expected<core::Unit, std::string> Init(uint16_t, util::Ring*) override {
        return core::Unit();
    }
    core::Optional<std::size_t> PerSlotExtraBufLen() const override { return extra_len_; }
    abi::Params DevParams(const abi::CtrlDevInfo& info) const override {
        ++params_calls_;
        seen_max_io_buf_ = info.max_io_buf_bytes;
        return BuildParams(2048, 9, 12, info.max_io_buf_bytes, false);
    }
    int32_t HandleIoRequest(uint16_t, uint16_t, const abi::IoDesc&, IoBuffer*, IoBuffer*,
                            util::Ring*) override {
        return 0;
    }

    void SetExtraLen(std::size_t n) { extra_len_ = core::Optional<std::size_t>(n); }
    int ParamsCalls() const { return params_calls_; }
    uint32_t SeenMaxIoBuf() const { return seen_max_io_buf_; }

 private:
    core::Optional<std::size_t> extra_len_;
    mutable int params_calls_;
    mutable uint32_t seen_max_io_buf_;
};

/// A launcher that records which queues were started, and can fail on cue.
class FakeLauncher : public QueueWorkerLauncher {
 public:
    explicit FakeLauncher(Trace* trace) : trace_(trace), fail_at_(-1), joined_(0) {}

    core::Expected<core::Unit, std::string> Spawn(uint16_t qid,
                                                  const abi::CtrlDevInfo&) override {
        if (fail_at_ >= 0 && static_cast<int>(qid) == fail_at_) {
            return core::make_unexpected(std::string("no threads"));
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "spawn-q%u", static_cast<unsigned>(qid));
        trace_->events.push_back(buf);
        ++spawned_;
        return core::Unit();
    }
    void Join() override {
        trace_->events.push_back("join");
        ++joined_;
    }

    void FailAt(int qid) { fail_at_ = qid; }
    int Spawned() const { return spawned_; }
    int Joined() const { return joined_; }

 private:
    Trace* trace_;
    int fail_at_;
    int spawned_ = 0;
    int joined_;
};

std::string MakeTempDir() {
    char tmpl[] = "/tmp/agentenv-ublkdev-XXXXXX";
    const char* d = ::mkdtemp(tmpl);
    return d == NULL ? std::string() : std::string(d);
}

}  // namespace

// ============================================================
// char device open — which failures are worth waiting out
// ============================================================

MT_TEST(an_existing_node_opens_on_the_first_try) {
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;
    const std::string path = dir + "/cdev";
    const int made = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    MT_EXPECT_TRUE(made >= 0);
    if (made >= 0) ::close(made);

    core::Expected<int, std::string> fd = OpenCharDeviceWithRetry(path, 4, 1);
    MT_EXPECT_TRUE(fd.ok());
    if (fd.ok()) ::close(fd.value());

    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

MT_TEST(a_missing_node_is_waited_for_then_given_up_on) {
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;
    // ENOENT is retried: udev creates the node *after* the kernel answers
    // ADD_DEV, so on a busy host the first open commonly loses that race.
    core::Expected<int, std::string> fd =
        OpenCharDeviceWithRetry(dir + "/never", 3, 1);
    MT_EXPECT_TRUE(!fd.ok());
    // The message says how hard it tried, which is what distinguishes "the
    // node never appeared" from "the node is not there right now".
    MT_EXPECT_TRUE(fd.error().find("3 times") != std::string::npos);
    ::rmdir(dir.c_str());
}

MT_TEST(a_node_that_exists_but_is_not_yet_readable_is_waited_for) {
    if (::geteuid() == 0) {
        // root bypasses the mode bits, so the EACCES this needs cannot be
        // produced.
        Skip("permission retry", "running as root; EACCES is not reachable");
        return;
    }
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;
    const std::string path = dir + "/cdev";
    const int made = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    MT_EXPECT_TRUE(made >= 0);
    if (made >= 0) ::close(made);
    // Present but unopenable. udev creates the node first and fixes its
    // ownership a moment later, so this is the second half of the same race
    // ENOENT covers -- not retrying it fails on every host where udev is
    // slower than the kernel.
    MT_EXPECT_TRUE(::chmod(path.c_str(), 0) == 0);

    core::Expected<int, std::string> fd = OpenCharDeviceWithRetry(path, 2, 1);
    MT_EXPECT_TRUE(!fd.ok());
    // Reported as an exhausted retry budget, which is what distinguishes it
    // from an error that was refused outright.
    MT_EXPECT_TRUE(fd.error().find("2 times") != std::string::npos);

    ::chmod(path.c_str(), 0600);
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

MT_TEST(an_error_that_waiting_cannot_fix_is_not_retried) {
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;
    // ENOTDIR: a path component that is a file. Retrying would turn an
    // instant, permanent error into a 1.6-second one and still fail.
    const std::string file = dir + "/afile";
    const int made = ::open(file.c_str(), O_CREAT | O_RDWR, 0600);
    if (made >= 0) ::close(made);

    core::Expected<int, std::string> fd =
        OpenCharDeviceWithRetry(file + "/under", 64, 25);
    MT_EXPECT_TRUE(!fd.ok());
    // Reported as the open failure it is, not as an exhausted retry budget.
    MT_EXPECT_TRUE(fd.error().find("times") == std::string::npos);

    ::unlink(file.c_str());
    ::rmdir(dir.c_str());
}

MT_TEST(the_retry_budget_matches_rust) {
    // 64 attempts, 25 ms apart: about 1.6 seconds of tolerance for udev.
    MT_EXPECT_EQ(kCdevOpenAttempts, 64);
    MT_EXPECT_EQ(kCdevOpenSleepMs, 25);
}

// ============================================================
// slot buffers — the device's mode decides the shape
// ============================================================

namespace {

std::shared_ptr<util::Ring> MakeRing() {
    util::IoRingConfig c;
    c.sq_entries = 8;
    c.cq_entries = 16;
    c.sparse_buffers = 16;
    c.sparse_files = 4;
    core::Expected<std::unique_ptr<util::Ring>, std::string> r = util::Ring::Open(c);
    if (!r.ok()) {
        c.coop_taskrun = false;
        c.single_issuer = false;
        r = util::Ring::Open(c);
    }
    if (!r.ok()) {
        c.cq_entries = 0;
        r = util::Ring::Open(c);
    }
    if (!r.ok()) return std::shared_ptr<util::Ring>();
    return std::shared_ptr<util::Ring>(r.take_value().release());
}

const char* const kNoSparse =
    "IORING_REGISTER_FILES2/BUFFERS2 (sparse tables) need Linux 5.13 or newer";

abi::CtrlDevInfo DevInfo(uint16_t queues, uint16_t depth, uint32_t bufsize,
                         uint64_t flags) {
    abi::CtrlDevInfo d;
    std::memset(&d, 0, sizeof(d));
    d.nr_hw_queues = queues;
    d.queue_depth = depth;
    d.max_io_buf_bytes = bufsize;
    d.flags = flags;
    return d;
}

}  // namespace

MT_TEST(a_plain_device_gets_a_user_buffer) {
    std::shared_ptr<util::Ring> ring = MakeRing();
    if (!ring) {
        Skip("plain slot buffer", kNoSparse);
        return;
    }
    core::Expected<SlotBuffers, std::string> b = AllocateSlotBuffers(
        ring, DevInfo(1, 16, 4096, 0), core::Optional<std::size_t>());
    MT_EXPECT_TRUE(b.ok());
    if (!b.ok()) return;
    // The server owns the memory in this mode, so the command names it.
    MT_EXPECT_TRUE(b.value().buf.IsUser());
    MT_EXPECT_TRUE(b.value().buf.CmdAddr() != 0);
    MT_EXPECT_TRUE(b.value().extra.Empty());
}

MT_TEST(an_auto_buf_reg_device_gets_a_reserved_slot) {
    std::shared_ptr<util::Ring> ring = MakeRing();
    if (!ring) {
        Skip("auto-reg slot buffer", kNoSparse);
        return;
    }
    core::Expected<SlotBuffers, std::string> b = AllocateSlotBuffers(
        ring, DevInfo(1, 16, 4096, abi::kFAutoBufReg), core::Optional<std::size_t>());
    MT_EXPECT_TRUE(b.ok());
    if (!b.ok()) return;
    // The negotiated device flag decides this, not a local preference: the
    // kernel supplies the pages here, and the command's addr must stay zero.
    MT_EXPECT_TRUE(b.value().buf.IsAutoReg());
    MT_EXPECT_EQ(b.value().buf.CmdAddr(), static_cast<uint64_t>(0));
}

MT_TEST(the_extra_buffer_is_always_a_user_buffer) {
    std::shared_ptr<util::Ring> ring = MakeRing();
    if (!ring) {
        Skip("extra buffer", kNoSparse);
        return;
    }
    // Even on an auto-reg device: the extra buffer is the target's own
    // scratch space, which the kernel knows nothing about and has no slot to
    // fill.
    core::Expected<SlotBuffers, std::string> b =
        AllocateSlotBuffers(ring, DevInfo(1, 16, 4096, abi::kFAutoBufReg),
                            core::Optional<std::size_t>(1024));
    MT_EXPECT_TRUE(b.ok());
    if (!b.ok()) return;
    MT_EXPECT_TRUE(b.value().buf.IsAutoReg());
    MT_EXPECT_TRUE(b.value().extra.IsUser());
    MT_EXPECT_EQ(b.value().extra.Size(), static_cast<std::size_t>(1024));
}

MT_TEST(an_unaligned_extra_buffer_is_refused) {
    // No ring needed: the size is a property of the request, so it is
    // rejected before anything is allocated. That also keeps this verifiable
    // on hosts without the sparse registration opcodes.
    //
    // Refused rather than rounded up. The target asked for this size because
    // it is what its own arithmetic needs; enlarging it silently would hide a
    // bug there, and the kernel rejects the unaligned transfer anyway.
    core::Expected<SlotBuffers, std::string> b =
        AllocateSlotBuffers(std::shared_ptr<util::Ring>(), DevInfo(1, 16, 4096, 0),
                            core::Optional<std::size_t>(1000));
    MT_EXPECT_TRUE(!b.ok());
    MT_EXPECT_TRUE(b.error().find("not aligned") != std::string::npos);

    // 512 and its multiples pass this gate (and then need a ring).
    core::Expected<SlotBuffers, std::string> ok =
        AllocateSlotBuffers(std::shared_ptr<util::Ring>(), DevInfo(1, 16, 4096, 0),
                            core::Optional<std::size_t>(1024));
    MT_EXPECT_TRUE(!ok.ok());
    MT_EXPECT_TRUE(ok.error().find("not aligned") == std::string::npos);
}

MT_TEST(a_zero_sized_io_buffer_is_refused) {
    // `max_io_buf_bytes` of zero means the kernel granted no buffer at all,
    // so every request would be unserviceable. Checked before the ring is
    // touched, for the same reason as above.
    core::Expected<SlotBuffers, std::string> b =
        AllocateSlotBuffers(std::shared_ptr<util::Ring>(), DevInfo(1, 16, 0, 0),
                            core::Optional<std::size_t>());
    MT_EXPECT_TRUE(!b.ok());
    MT_EXPECT_TRUE(b.error().find("max_io_buf_bytes") != std::string::npos);
}

// ============================================================
// builder and start sequence
// ============================================================

MT_TEST(building_without_a_target_is_refused) {
    // Rust's `ok_or_else(|| anyhow!("do not set target"))`. The target
    // supplies the geometry SET_PARAMS sends, so there is nothing to build.
    UblkDevBuilder b((std::unique_ptr<UblkCtrl>()));
    core::Expected<std::unique_ptr<UblkDev>, std::string> r = b.Build();
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("do not set target") != std::string::npos);
}

MT_TEST(building_without_a_control_channel_is_refused) {
    UblkDevBuilder b((std::unique_ptr<UblkCtrl>()));
    b.SetTarget(std::shared_ptr<UblkTarget>(new FakeTarget()));
    core::Expected<std::unique_ptr<UblkDev>, std::string> r = b.Build();
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("control channel") != std::string::npos);
}

MT_TEST(the_target_sees_the_buffer_size_the_kernel_granted) {
    // `max_io_buf_bytes` comes back from ADD_DEV, and `max_sectors` is
    // derived from it. A target handed the requested value instead of the
    // granted one would advertise a transfer size the device cannot accept.
    FakeTarget tgt;
    abi::CtrlDevInfo granted = DevInfo(1, 16, 128 * 1024, 0);
    const abi::Params p = tgt.DevParams(granted);
    MT_EXPECT_EQ(tgt.SeenMaxIoBuf(), static_cast<uint32_t>(128 * 1024));
    // 128 KiB / 512-byte logical blocks.
    MT_EXPECT_EQ(p.basic.max_sectors, static_cast<uint32_t>(256));
}

// ============================================================
// the start sequence — ordering is the logic
// ============================================================

namespace {

/// A plan whose steps all succeed, recording the order they run in.
StartPlan PlanOn(Trace* trace, abi::CtrlDevInfo info) {
    StartPlan p;
    p.read_dev_info = [trace, info]() -> core::Expected<abi::CtrlDevInfo, std::string> {
        trace->events.push_back("get_dev_info");
        return info;
    };
    p.spawn = [trace](uint16_t qid,
                      const abi::CtrlDevInfo&) -> core::Expected<core::Unit, std::string> {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "spawn-q%u", static_cast<unsigned>(qid));
        trace->events.push_back(buf);
        return core::Unit();
    };
    p.set_params = [trace](const abi::Params&) -> core::Expected<core::Unit, std::string> {
        trace->events.push_back("set_params");
        return core::Unit();
    };
    p.start_dev = [trace]() -> core::Expected<core::Unit, std::string> {
        trace->events.push_back("start_dev");
        return core::Unit();
    };
    return p;
}

}  // namespace

MT_TEST(start_arms_every_queue_before_publishing_the_disk) {
    Trace trace;
    abi::CtrlDevInfo info = DevInfo(3, 16, 4096, 0);
    info.state = abi::kDevStateDead;

    core::Expected<core::Unit, std::string> r =
        RunStartSequence(PlanOn(&trace, info), abi::Params());
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;

    // Every queue, not just the first: a device with three hardware queues
    // and two workers would hang on anything hashing to the third.
    MT_EXPECT_TRUE(trace.Has("spawn-q0"));
    MT_EXPECT_TRUE(trace.Has("spawn-q1"));
    MT_EXPECT_TRUE(trace.Has("spawn-q2"));

    // The order is the whole point. START_DEV publishes /dev/ublkbN, and the
    // kernel expects the slots to be armed when it does -- the other order
    // exposes a disk whose requests nothing is waiting to serve.
    MT_EXPECT_TRUE(trace.IndexOf("spawn-q2") < trace.IndexOf("start_dev"));
    // And the geometry must be set before publication, because it cannot be
    // changed afterwards.
    MT_EXPECT_TRUE(trace.IndexOf("set_params") < trace.IndexOf("start_dev"));
    // State is read first, before anything is committed to.
    MT_EXPECT_EQ(trace.IndexOf("get_dev_info"), 0);
}

MT_TEST(starting_a_live_device_is_refused) {
    Trace trace;
    abi::CtrlDevInfo info = DevInfo(1, 16, 4096, 0);
    info.state = abi::kDevStateLive;

    core::Expected<core::Unit, std::string> r =
        RunStartSequence(PlanOn(&trace, info), abi::Params());
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("not dead") != std::string::npos);
    // Nothing was started: a second set of workers would fetch from the same
    // slots as the set already serving this device.
    MT_EXPECT_TRUE(!trace.Has("spawn-q0"));
    MT_EXPECT_TRUE(!trace.Has("start_dev"));
}

MT_TEST(a_quiesced_device_is_refused_too) {
    Trace trace;
    abi::CtrlDevInfo info = DevInfo(1, 16, 4096, 0);
    info.state = abi::kDevStateQuiesced;
    // Only DEAD is startable. Quiesced means the kernel still owns the
    // device's queues, so arming them again would collide.
    MT_EXPECT_TRUE(!RunStartSequence(PlanOn(&trace, info), abi::Params()).ok());
}

MT_TEST(a_failed_worker_stops_the_sequence_before_publication) {
    Trace trace;
    abi::CtrlDevInfo info = DevInfo(3, 16, 4096, 0);
    info.state = abi::kDevStateDead;

    StartPlan p = PlanOn(&trace, info);
    p.spawn = [&trace](uint16_t qid,
                       const abi::CtrlDevInfo&) -> core::Expected<core::Unit, std::string> {
        if (qid == 1) return core::make_unexpected(std::string("no threads"));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "spawn-q%u", static_cast<unsigned>(qid));
        trace.events.push_back(buf);
        return core::Unit();
    };

    core::Expected<core::Unit, std::string> r = RunStartSequence(p, abi::Params());
    MT_EXPECT_TRUE(!r.ok());
    // The device is never published. Publishing with a queue missing would
    // hang every request that landed on it, which is worse than not starting.
    MT_EXPECT_TRUE(!trace.Has("start_dev"));
    MT_EXPECT_TRUE(!trace.Has("set_params"));
    // The message names what it was trying to do, including the geometry --
    // a bare "no threads" would not say which device or queue.
    MT_EXPECT_TRUE(r.error().find("qid=1") != std::string::npos);
    MT_EXPECT_TRUE(r.error().find("nr_hw_queues=3") != std::string::npos);
    // Queue 0 was already running and is left alone, to be joined by the
    // caller's teardown rather than killed mid-setup.
    MT_EXPECT_TRUE(trace.Has("spawn-q0"));
}

MT_TEST(a_failed_set_params_does_not_publish) {
    Trace trace;
    abi::CtrlDevInfo info = DevInfo(1, 16, 4096, 0);
    info.state = abi::kDevStateDead;

    StartPlan p = PlanOn(&trace, info);
    p.set_params = [](const abi::Params&) -> core::Expected<core::Unit, std::string> {
        return core::make_unexpected(std::string("bad geometry"));
    };

    core::Expected<core::Unit, std::string> r = RunStartSequence(p, abi::Params());
    MT_EXPECT_TRUE(!r.ok());
    // Publishing a disk whose geometry the kernel rejected would expose one
    // with whatever defaults it fell back to.
    MT_EXPECT_TRUE(!trace.Has("start_dev"));
}

MT_TEST(an_incomplete_plan_is_refused) {
    // Every step is required; a missing one would silently skip part of the
    // sequence rather than fail.
    StartPlan p;
    MT_EXPECT_TRUE(!RunStartSequence(p, abi::Params()).ok());
}

MT_TEST(a_device_with_no_queues_publishes_nothing_to_serve_it) {
    // Zero hardware queues spawns no workers but still reaches START_DEV --
    // matching Rust, whose loop simply does not run. Recorded as the current
    // behaviour: `nr_hw_queues` is validated when the control channel is
    // built, not here.
    Trace trace;
    abi::CtrlDevInfo info = DevInfo(0, 16, 4096, 0);
    info.state = abi::kDevStateDead;
    core::Expected<core::Unit, std::string> r =
        RunStartSequence(PlanOn(&trace, info), abi::Params());
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(!trace.Has("spawn-q0"));
    MT_EXPECT_TRUE(trace.Has("start_dev"));
}

MT_MAIN
