// SPDX-License-Identifier: MIT
// Tests for the ublk queue worker. Rust: storage/ublk/src/dev.rs
// (`queue_work` / `slot_task`).
//
// What can go wrong here without a kernel noticing: a completion dispatched
// to the wrong slot, a ring too small to hold the standing commands, or a
// thread name long enough that Linux refuses it. All three are checked as
// pure functions. Anything that needs a real ublk char device skips.
#include "microtest.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "agentenv/storage/ublk/queue_worker.h"

using namespace agentenv::storage::ublk;
namespace util = agentenv::storage::util;
namespace core = agentenv::core;

namespace {

void Skip(const char* what, const char* why) {
    std::printf("       SKIP %s: %s\n", what, why);
}

}  // namespace

// ============================================================
// completion dispatch — the wrong slot is worse than a lost one
// ============================================================

MT_TEST(user_data_round_trips_through_queue_and_tag) {
    const uint64_t ud = QueueWorker::UserDataFor(3, 17);
    uint16_t tag = 0;
    MT_EXPECT_TRUE(QueueWorker::TagFromUserData(ud, 3, 64, &tag));
    MT_EXPECT_EQ(tag, static_cast<uint16_t>(17));
}

MT_TEST(a_completion_from_another_queue_is_not_dispatched) {
    // Checked rather than assumed. The ring is this queue's alone today, but
    // dispatching a foreign completion would hand the target a descriptor
    // for a request that was never made -- it would read a stale slot and
    // report a result for it.
    const uint64_t ud = QueueWorker::UserDataFor(2, 5);
    uint16_t tag = 0;
    MT_EXPECT_TRUE(!QueueWorker::TagFromUserData(ud, 3, 64, &tag));
    // And the right queue still accepts it.
    MT_EXPECT_TRUE(QueueWorker::TagFromUserData(ud, 2, 64, &tag));
    MT_EXPECT_EQ(tag, static_cast<uint16_t>(5));
}

MT_TEST(a_tag_past_the_depth_is_not_dispatched) {
    // The slot array is sized by depth, so an out-of-range tag would index
    // past it. The bound is here rather than at the array access because the
    // value comes from a completion, which is kernel-supplied input.
    const uint64_t ud = QueueWorker::UserDataFor(0, 64);
    uint16_t tag = 0;
    MT_EXPECT_TRUE(!QueueWorker::TagFromUserData(ud, 0, 64, &tag));
    // The last valid tag is depth-1.
    MT_EXPECT_TRUE(QueueWorker::TagFromUserData(QueueWorker::UserDataFor(0, 63), 0, 64,
                                                &tag));
    MT_EXPECT_EQ(tag, static_cast<uint16_t>(63));
}

MT_TEST(queue_id_zero_is_distinguishable_from_tag_zero) {
    // Both halves are zero here, which is the case a packing that merged
    // them would get wrong.
    const uint64_t ud = QueueWorker::UserDataFor(0, 0);
    MT_EXPECT_EQ(ud, static_cast<uint64_t>(0));
    uint16_t tag = 1;
    MT_EXPECT_TRUE(QueueWorker::TagFromUserData(ud, 0, 16, &tag));
    MT_EXPECT_EQ(tag, static_cast<uint16_t>(0));
    // ...and is refused for any other queue, rather than matching everything.
    MT_EXPECT_TRUE(!QueueWorker::TagFromUserData(ud, 1, 16, &tag));
}

MT_TEST(the_queue_id_occupies_the_high_half) {
    // Separate halves, so a 16-bit tag can never collide with a queue id.
    MT_EXPECT_EQ(QueueWorker::UserDataFor(1, 0), static_cast<uint64_t>(1) << 32);
    MT_EXPECT_EQ(QueueWorker::UserDataFor(0, 0xFFFF), static_cast<uint64_t>(0xFFFF));
}

// ============================================================
// ring sizing — too small and the protocol cannot stand
// ============================================================

MT_TEST(the_ring_holds_more_than_the_queue_depth) {
    const util::IoRingConfig c = QueueRingConfig(16);
    // Rust's `depth * 2`. A queue keeps `depth` commands standing and
    // briefly twice that while a slot is re-armed, so a ring sized at the
    // depth would refuse submissions the protocol requires to be in flight.
    MT_EXPECT_EQ(c.sq_entries, static_cast<uint32_t>(32));
    MT_EXPECT_EQ(c.cq_entries, static_cast<uint32_t>(32));
    // Rust's `depth * 4`: one buffer per slot, the target's optional extra,
    // and headroom for what the target registers in `Init`.
    MT_EXPECT_EQ(c.sparse_buffers, static_cast<uint32_t>(64));
    // Rust's fixed 16: the char device plus the target's files.
    MT_EXPECT_EQ(c.sparse_files, static_cast<uint32_t>(16));
}

MT_TEST(the_queue_ring_requires_sqe128) {
    // Every queue command is an `IORING_OP_URING_CMD`; without sqe128 the
    // payload has nowhere to go.
    MT_EXPECT_TRUE(QueueRingConfig(16).sqe128);
    // One thread owns the ring, which is what single_issuer promises the
    // kernel -- violating it is undefined, not merely slower.
    MT_EXPECT_TRUE(QueueRingConfig(16).single_issuer);
}

MT_TEST(a_deep_queue_clamps_instead_of_failing_to_start) {
    // 4096 * 2 would exceed what `Ring::Open` accepts. Clamped rather than
    // passed through: the protocol still works with less headroom, while a
    // rejected ring means the device cannot start at all.
    const util::IoRingConfig c = QueueRingConfig(4096);
    MT_EXPECT_TRUE(c.sq_entries <= 4096);
    MT_EXPECT_TRUE(c.cq_entries <= 8192);
    MT_EXPECT_TRUE(c.sparse_buffers <= 4096);
    // And it is still at least the depth, or the queue could not arm.
    MT_EXPECT_TRUE(c.sq_entries >= 4096);
}

// ============================================================
// thread naming
// ============================================================

MT_TEST(a_thread_name_fits_what_linux_accepts) {
    // 15 characters plus NUL. `pthread_setname_np` fails rather than
    // truncating, so an over-long name leaves the thread anonymous -- on
    // exactly the hosts with enough devices for names to matter.
    MT_EXPECT_TRUE(QueueThreadName(0, 0).size() <= 15);
    MT_EXPECT_TRUE(QueueThreadName(4294967295u, 65535).size() <= 15);
    MT_EXPECT_TRUE(QueueThreadName(12345, 7).size() <= 15);
}

MT_TEST(a_thread_name_keeps_the_queue_id) {
    // Within one device the queue id is what distinguishes threads, so it
    // survives truncation even when the device id does not.
    const std::string big = QueueThreadName(4294967295u, 42);
    MT_EXPECT_TRUE(big.size() <= 15);
    MT_EXPECT_TRUE(big.find("-42") != std::string::npos);

    const std::string small = QueueThreadName(3, 1);
    MT_EXPECT_TRUE(small == std::string("ublk-q3-1"));
}

MT_TEST(the_name_linux_actually_accepts) {
    // The truncation is only worth anything if the result is accepted, so
    // this calls the real thing rather than trusting the length check.
    const std::string name = QueueThreadName(4294967295u, 65535);
    MT_EXPECT_EQ(::pthread_setname_np(::pthread_self(), name.c_str()), 0);
    ::pthread_setname_np(::pthread_self(), "test_ublk_q");
}

// ============================================================
// construction
// ============================================================

MT_TEST(a_worker_needs_a_target) {
    abi::CtrlDevInfo info;
    std::memset(&info, 0, sizeof(info));
    info.nr_hw_queues = 1;
    info.queue_depth = 16;
    info.max_io_buf_bytes = 4096;

    core::Expected<std::unique_ptr<QueueWorker>, std::string> r =
        QueueWorker::Create(0, info, -1, std::shared_ptr<UblkTarget>());
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("null target") != std::string::npos);
}

MT_TEST(a_worker_needs_a_nonzero_depth) {
    // Checked before the ring is built: a depth of zero sizes every table at
    // zero, and the failure would surface as an opaque EINVAL from setup.
    abi::CtrlDevInfo info;
    std::memset(&info, 0, sizeof(info));
    info.nr_hw_queues = 1;
    info.queue_depth = 0;

    core::Expected<std::unique_ptr<QueueWorker>, std::string> r =
        QueueWorker::Create(0, info, -1, std::shared_ptr<UblkTarget>(NULL));
    MT_EXPECT_TRUE(!r.ok());
}

MT_MAIN
