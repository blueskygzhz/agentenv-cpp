// SPDX-License-Identifier: MIT
// Tests for the io_uring submission/completion path the ublk data plane rides
// on. Rust: storage/util/src/io_ring/.
//
// Two kinds of assertion here, deliberately separated:
//
//   * ABI layout and entry construction are pure computation and are checked
//     unconditionally. These are what a wrong field offset or a truncated
//     command payload would break, and neither needs a kernel.
//   * Live submission needs a kernel with io_uring, and `sqe128` needs 5.19.
//     Those tests skip with a printed reason when the feature is absent --
//     never silently pass, because a skip that looks like a pass is how an
//     unsupported build gets shipped.
#include "microtest.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "agentenv/storage/util/io_ring.h"

using namespace agentenv::storage::util;
namespace core = agentenv::core;

namespace {

/// A ring shape this kernel accepts, or an empty config if io_uring itself is
/// unavailable.
///
/// The setup flags this port wants landed over several releases --
/// `IORING_SETUP_CQSIZE` in 5.5, `COOP_TASKRUN` and `SQE128` in 5.19,
/// `SINGLE_ISSUER` in 6.0 -- and a kernel that lacks one answers with a bare
/// EINVAL. Probing each separately matters: treating "this flag is too new" as
/// "io_uring is unavailable" would skip the submission and completion tests on
/// a kernel that runs them perfectly well, which is the half of this file that
/// could not otherwise be exercised at all.
bool ProbeConfig(IoRingConfig* out) {
    IoRingConfig full;
    full.sq_entries = 8;
    full.cq_entries = 16;
    full.coop_taskrun = true;
    full.single_issuer = true;
    if (Ring::Open(full).ok()) { *out = full; return true; }

    IoRingConfig no_hints;
    no_hints.sq_entries = 8;
    no_hints.cq_entries = 16;
    no_hints.coop_taskrun = false;
    no_hints.single_issuer = false;
    if (Ring::Open(no_hints).ok()) { *out = no_hints; return true; }

    // No explicit CQ size either: pre-5.5 derives it from the SQ depth.
    IoRingConfig bare;
    bare.sq_entries = 8;
    bare.cq_entries = 0;
    bare.coop_taskrun = false;
    bare.single_issuer = false;
    if (Ring::Open(bare).ok()) { *out = bare; return true; }

    return false;
}

/// `IoRingConfig` with the probed shape and a caller-chosen SQ depth.
bool ProbeConfigWithSq(uint32_t sq_entries, IoRingConfig* out) {
    if (!ProbeConfig(out)) return false;
    out->sq_entries = sq_entries;
    if (out->cq_entries != 0 && out->cq_entries < sq_entries) {
        out->cq_entries = sq_entries * 2;
    }
    return Ring::Open(*out).ok();
}

bool Sqe128Available(IoRingConfig* out) {
    if (!ProbeConfig(out)) return false;
    out->sqe128 = true;
    return Ring::Open(*out).ok();
}

void Skip(const char* what, const char* why) {
    std::printf("       SKIP %s: %s\n", what, why);
}

/// The byte at `offset` inside a built entry's raw SQE.
const uint8_t* RawBytes(const SubmissionEntry& e) {
    return reinterpret_cast<const uint8_t*>(&e.Raw());
}

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
uint64_t ReadU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

}  // namespace

// ============================================================
// ABI layout — no kernel needed, and the most dangerous to get wrong
// ============================================================

MT_TEST(sqe_fields_sit_at_the_offsets_the_kernel_reads) {
    // The kernel reads this struct directly, so a field at the wrong offset is
    // not a compile error -- it is a wrong fd or length handed to the kernel,
    // surfacing as an unexplained EINVAL or as I/O against another file.
    int dummy = 0;
    SubmissionEntry e = SubmissionEntry::Read(0x41424344, &dummy, 0x1234, 0x5678);
    e.UserData(0xDEADBEEFCAFEF00Dull);
    const uint8_t* b = RawBytes(e);

    MT_EXPECT_EQ(b[0], static_cast<uint8_t>(22));  // opcode: IORING_OP_READ@0
    MT_EXPECT_EQ(ReadU32(b + 4), static_cast<uint32_t>(0x41424344));  // fd@4
    MT_EXPECT_EQ(ReadU64(b + 8), static_cast<uint64_t>(0x5678));      // off@8
    MT_EXPECT_EQ(ReadU64(b + 16), reinterpret_cast<uint64_t>(&dummy));  // addr@16
    MT_EXPECT_EQ(ReadU32(b + 24), static_cast<uint32_t>(0x1234));     // len@24
    MT_EXPECT_EQ(ReadU64(b + 32), 0xDEADBEEFCAFEF00Dull);             // user_data@32
}

MT_TEST(uring_cmd_puts_the_opcode_where_off_would_be) {
    // `cmd_op` occupies the low half of the field the read/write form uses for
    // `off`; the high half is reserved and must stay zero. Writing the whole
    // 64 bits would set reserved bits the kernel may later define.
    uint8_t payload[8];
    std::memset(payload, 0xAB, sizeof(payload));
    SubmissionEntry e = SubmissionEntry::UringCmd(7, 0x12345678, payload, sizeof(payload));
    const uint8_t* b = RawBytes(e);

    MT_EXPECT_EQ(b[0], static_cast<uint8_t>(46));  // IORING_OP_URING_CMD
    MT_EXPECT_EQ(ReadU32(b + 4), static_cast<uint32_t>(7));
    MT_EXPECT_EQ(ReadU32(b + 8), static_cast<uint32_t>(0x12345678));  // cmd_op
    MT_EXPECT_EQ(ReadU32(b + 12), static_cast<uint32_t>(0));          // reserved half
    // The payload starts at offset 48, where `addr3` lives in the rw form.
    MT_EXPECT_EQ(b[48], static_cast<uint8_t>(0xAB));
    MT_EXPECT_EQ(b[55], static_cast<uint8_t>(0xAB));
    MT_EXPECT_EQ(e.CmdLen(), static_cast<std::size_t>(8));
}

MT_TEST(a_long_command_payload_spills_into_the_second_half) {
    // A 64-byte SQE leaves 16 bytes of `cmd`. Anything longer lives in the
    // second 64 bytes, which exist only on an sqe128 ring -- this is the whole
    // reason ublk requires one.
    uint8_t payload[80];
    for (std::size_t i = 0; i < sizeof(payload); ++i) payload[i] = static_cast<uint8_t>(i);

    SubmissionEntry e = SubmissionEntry::UringCmd(3, 1, payload, sizeof(payload));
    MT_EXPECT_EQ(e.CmdLen(), static_cast<std::size_t>(80));

    // First 16 bytes in the base SQE at offset 48...
    const uint8_t* b = RawBytes(e);
    for (int i = 0; i < 16; ++i) {
        MT_EXPECT_EQ(b[48 + i], static_cast<uint8_t>(i));
    }
    // ...and the remaining 64 in the tail, contiguous with them.
    for (int i = 0; i < 64; ++i) {
        MT_EXPECT_EQ(e.CmdTail()[i], static_cast<uint8_t>(16 + i));
    }
}

MT_TEST(sqe128_is_required_exactly_when_the_payload_exceeds_sixteen_bytes) {
    // The boundary, both sides. 16 bytes is what a 64-byte SQE has room for.
    MT_EXPECT_TRUE(!UringCmdNeedsSqe128(0));
    MT_EXPECT_TRUE(!UringCmdNeedsSqe128(16));
    MT_EXPECT_TRUE(UringCmdNeedsSqe128(17));
    // Every real ublk command is well past it.
    MT_EXPECT_TRUE(UringCmdNeedsSqe128(80));
}

MT_TEST(a_command_longer_than_the_sqe_can_hold_is_capped_not_wrapped) {
    // 80 bytes is all a 128-byte SQE has. A longer payload is truncated at the
    // boundary rather than writing past the entry, which would corrupt the
    // next slot in the array.
    uint8_t payload[200];
    std::memset(payload, 0x5A, sizeof(payload));
    SubmissionEntry e = SubmissionEntry::UringCmd(3, 1, payload, sizeof(payload));
    MT_EXPECT_EQ(e.CmdLen(), static_cast<std::size_t>(80));
}

// ============================================================
// configuration — rejected with the field named
// ============================================================

MT_TEST(ring_config_limits_are_rejected_before_the_syscall) {
    // `io_uring_setup` answers all of these with a bare EINVAL, which cannot
    // be told from a kernel that simply lacks sqe128. Each is named here.
    IoRingConfig too_many_sq;
    too_many_sq.sq_entries = 8192;
    core::Expected<std::unique_ptr<Ring>, std::string> r1 = Ring::Open(too_many_sq);
    MT_EXPECT_TRUE(!r1.ok());
    MT_EXPECT_TRUE(r1.error().find("sq_entries") != std::string::npos);

    IoRingConfig zero_sq;
    zero_sq.sq_entries = 0;
    MT_EXPECT_TRUE(!Ring::Open(zero_sq).ok());

    IoRingConfig too_many_cq;
    too_many_cq.cq_entries = 16384;
    core::Expected<std::unique_ptr<Ring>, std::string> r2 = Ring::Open(too_many_cq);
    MT_EXPECT_TRUE(!r2.ok());
    MT_EXPECT_TRUE(r2.error().find("cq_entries") != std::string::npos);

    IoRingConfig too_many_files;
    too_many_files.sparse_files = 9000;
    core::Expected<std::unique_ptr<Ring>, std::string> r3 = Ring::Open(too_many_files);
    MT_EXPECT_TRUE(!r3.ok());
    MT_EXPECT_TRUE(r3.error().find("sparse_files") != std::string::npos);

    IoRingConfig too_many_bufs;
    too_many_bufs.sparse_buffers = 5000;
    core::Expected<std::unique_ptr<Ring>, std::string> r4 = Ring::Open(too_many_bufs);
    MT_EXPECT_TRUE(!r4.ok());
    MT_EXPECT_TRUE(r4.error().find("sparse_buffers") != std::string::npos);
}

MT_TEST(register_files_rejects_an_empty_or_oversized_table) {
    IoRingConfig c;
    if (!ProbeConfigWithSq(4, &c)) {
        Skip("register_files", "io_uring not available on this kernel");
        return;
    }
    core::Expected<std::unique_ptr<Ring>, std::string> ring = Ring::Open(c);
    MT_EXPECT_TRUE(ring.ok());
    if (!ring.ok()) return;

    MT_EXPECT_TRUE(!ring.value()->RegisterFiles(std::vector<int>()).ok());
    // Rust's cap of 64: a table this code did not size itself is one it
    // cannot index safely.
    MT_EXPECT_TRUE(!ring.value()->RegisterFiles(std::vector<int>(65, 0)).ok());
}

// ============================================================
// live ring — needs a kernel with io_uring
// ============================================================

MT_TEST(a_nop_completes_and_echoes_its_token) {
    IoRingConfig c;
    if (!ProbeConfigWithSq(8, &c)) {
        Skip("nop", "io_uring not available on this kernel");
        return;
    }
    core::Expected<std::unique_ptr<Ring>, std::string> ring = Ring::Open(c);
    MT_EXPECT_TRUE(ring.ok());
    if (!ring.ok()) return;

    SubmissionEntry e = SubmissionEntry::Nop();
    e.UserData(0xA5A5A5A5ull);
    core::Expected<uint32_t, std::string> sub = ring.value()->SubmitAndWait(e, 1);
    MT_EXPECT_TRUE(sub.ok());
    if (!sub.ok()) return;
    MT_EXPECT_EQ(sub.value(), static_cast<uint32_t>(1));

    core::Expected<std::vector<CompletionEntry>, std::string> done =
        ring.value()->WaitAndReap(1);
    MT_EXPECT_TRUE(done.ok());
    if (!done.ok()) return;
    MT_EXPECT_EQ(done.value().size(), static_cast<std::size_t>(1));
    if (done.value().empty()) return;
    // The token is how a caller matches a completion to its request; a ring
    // that lost or reordered it would complete the wrong operation.
    MT_EXPECT_EQ(done.value()[0].user_data, 0xA5A5A5A5ull);
    MT_EXPECT_EQ(done.value()[0].res, 0);
    // No completion was discarded.
    MT_EXPECT_EQ(ring.value()->CompletionOverflow(), static_cast<uint32_t>(0));
    MT_EXPECT_EQ(ring.value()->DroppedCompletions(), static_cast<uint32_t>(0));
}

MT_TEST(reads_and_writes_go_through_the_ring) {
    IoRingConfig c;
    if (!ProbeConfigWithSq(8, &c)) {
        Skip("read/write", "io_uring not available on this kernel");
        return;
    }
    core::Expected<std::unique_ptr<Ring>, std::string> ring = Ring::Open(c);
    MT_EXPECT_TRUE(ring.ok());
    if (!ring.ok()) return;

    char path[] = "/tmp/agentenv-ioring-XXXXXX";
    const int fd = ::mkstemp(path);
    MT_EXPECT_TRUE(fd >= 0);
    if (fd < 0) return;

    const char payload[] = "startup-pack";
    SubmissionEntry w = SubmissionEntry::Write(fd, payload, sizeof(payload) - 1, 0);
    w.UserData(1);
    MT_EXPECT_TRUE(ring.value()->SubmitAndWait(w, 1).ok());
    core::Expected<std::vector<CompletionEntry>, std::string> wd = ring.value()->WaitAndReap(1);
    MT_EXPECT_TRUE(wd.ok());
    if (!wd.ok() || wd.value().empty()) {
        ::close(fd);
        ::unlink(path);
        return;
    }

    // `IORING_OP_WRITE` / `_READ` arrived in 5.6; before that only the vectored
    // forms existed and the kernel answers EINVAL. Detected from the
    // completion rather than from a version check, and skipped rather than
    // worked around: Rust issues these same two opcodes, so substituting
    // READV/WRITEV here would test something the port does not do.
    if (wd.value()[0].res == -EINVAL) {
        Skip("read/write", "IORING_OP_READ/WRITE need Linux 5.6 or newer");
        ::close(fd);
        ::unlink(path);
        return;
    }

    // `res` is the byte count, not a status: a short write is a success the
    // caller has to notice, which is why it is passed through raw.
    MT_EXPECT_EQ(wd.value()[0].res, static_cast<int32_t>(sizeof(payload) - 1));
    MT_EXPECT_EQ(wd.value()[0].user_data, static_cast<uint64_t>(1));

    char buf[32];
    std::memset(buf, 0, sizeof(buf));
    SubmissionEntry r = SubmissionEntry::Read(fd, buf, sizeof(payload) - 1, 0);
    r.UserData(2);
    MT_EXPECT_TRUE(ring.value()->SubmitAndWait(r, 1).ok());
    core::Expected<std::vector<CompletionEntry>, std::string> rd = ring.value()->WaitAndReap(1);
    MT_EXPECT_TRUE(rd.ok());
    if (rd.ok() && !rd.value().empty()) {
        MT_EXPECT_EQ(rd.value()[0].user_data, static_cast<uint64_t>(2));
        MT_EXPECT_EQ(rd.value()[0].res, static_cast<int32_t>(sizeof(payload) - 1));
    }
    // The bytes actually made the round trip, which is what proves the `addr`
    // and `len` fields landed where the kernel reads them.
    MT_EXPECT_TRUE(std::string(buf) == std::string(payload));

    ::close(fd);
    ::unlink(path);
}

MT_TEST(the_submission_queue_reports_when_it_is_full) {
    // A shallow SQ, so it fills before the CQ does.
    IoRingConfig c;
    if (!ProbeConfigWithSq(1, &c)) {
        Skip("sq full", "io_uring not available on this kernel");
        return;
    }
    core::Expected<std::unique_ptr<Ring>, std::string> ring = Ring::Open(c);
    MT_EXPECT_TRUE(ring.ok());
    if (!ring.ok()) return;

    // The kernel consumes entries as they are submitted, so filling the queue
    // from a single-threaded caller is not reliable. What must hold is that a
    // full queue is *reported* rather than blocking -- blocking on the thread
    // that also reaps would deadlock against the completions that free a slot.
    bool saw_full = false;
    for (int i = 0; i < 256; ++i) {
        SubmissionEntry e = SubmissionEntry::Nop();
        e.UserData(static_cast<uint64_t>(i));
        core::Expected<uint32_t, std::string> sub = ring.value()->Submit(e);
        if (!sub.ok()) {
            MT_EXPECT_TRUE(sub.error().find("full") != std::string::npos);
            saw_full = true;
            break;
        }
    }
    // Either it filled and said so, or the kernel kept up; both are correct.
    // What would not be is hanging, which a failing run shows as a timeout.
    if (!saw_full) {
        core::Expected<std::vector<CompletionEntry>, std::string> drained =
            ring.value()->WaitAndReap(1);
        MT_EXPECT_TRUE(drained.ok());
    }
}

MT_TEST(an_sqe128_ring_carries_a_full_eighty_byte_command) {
    IoRingConfig c;
    if (!Sqe128Available(&c)) {
        Skip("sqe128", "IORING_SETUP_SQE128 needs Linux 5.19 or newer");
        return;
    }
    core::Expected<std::unique_ptr<Ring>, std::string> ring = Ring::Open(c);
    MT_EXPECT_TRUE(ring.ok());
    if (!ring.ok()) return;
    MT_EXPECT_TRUE(ring.value()->Sqe128());

    // A nop on an sqe128 ring still completes: this proves the doubled SQE
    // stride is right. With the mapping or stride sized for 64-byte entries
    // the kernel would read the wrong slot and the nop would not come back.
    SubmissionEntry e = SubmissionEntry::Nop();
    e.UserData(0x5151515151515151ull);
    MT_EXPECT_TRUE(ring.value()->SubmitAndWait(e, 1).ok());
    core::Expected<std::vector<CompletionEntry>, std::string> done =
        ring.value()->WaitAndReap(1);
    MT_EXPECT_TRUE(done.ok());
    if (done.ok() && !done.value().empty()) {
        MT_EXPECT_EQ(done.value()[0].user_data, 0x5151515151515151ull);
    }
}

MT_TEST(a_long_command_is_refused_on_a_sixty_four_byte_ring) {
    IoRingConfig c;
    if (!ProbeConfigWithSq(4, &c)) {
        Skip("payload refusal", "io_uring not available on this kernel");
        return;
    }
    c.sqe128 = false;
    core::Expected<std::unique_ptr<Ring>, std::string> ring = Ring::Open(c);
    MT_EXPECT_TRUE(ring.ok());
    if (!ring.ok()) return;

    uint8_t payload[80];
    std::memset(payload, 0x11, sizeof(payload));
    SubmissionEntry e = SubmissionEntry::UringCmd(0, 1, payload, sizeof(payload));

    // Refused, not truncated. A cut command is still a structurally valid SQE,
    // so the kernel would act on whatever its tail fields happened to hold --
    // and for a ublk command those fields are device ids and addresses.
    core::Expected<uint32_t, std::string> sub = ring.value()->Submit(e);
    MT_EXPECT_TRUE(!sub.ok());
    MT_EXPECT_TRUE(sub.error().find("sqe128") != std::string::npos);
}

MT_TEST(every_staged_entry_reaches_the_kernel_with_its_own_token) {
    // The SQ index array is what tells the kernel *which* SQE slot each
    // submission refers to. A constant or stale index there still produces
    // valid-looking completions -- just all for the same entry -- so this
    // stages several distinct requests without draining in between and
    // requires every token back exactly once.
    //
    // A single-entry round trip cannot see this: with one in flight, the
    // wrong index and the right one name the same slot.
    IoRingConfig c;
    if (!ProbeConfigWithSq(8, &c)) {
        Skip("sq index", "io_uring not available on this kernel");
        return;
    }
    core::Expected<std::unique_ptr<Ring>, std::string> ring = Ring::Open(c);
    MT_EXPECT_TRUE(ring.ok());
    if (!ring.ok()) return;

    const int kCount = 6;
    for (int i = 0; i < kCount; ++i) {
        SubmissionEntry e = SubmissionEntry::Nop();
        // Distinct, and distinguishable from a zeroed or repeated field.
        e.UserData(0x1000ull + static_cast<uint64_t>(i));
        core::Expected<uint32_t, std::string> sub = ring.value()->Submit(e);
        MT_EXPECT_TRUE(sub.ok());
        if (!sub.ok()) return;
    }

    std::vector<CompletionEntry> all;
    while (static_cast<int>(all.size()) < kCount) {
        core::Expected<std::vector<CompletionEntry>, std::string> batch =
            ring.value()->WaitAndReap(1);
        MT_EXPECT_TRUE(batch.ok());
        if (!batch.ok()) return;
        all.insert(all.end(), batch.value().begin(), batch.value().end());
    }
    MT_EXPECT_EQ(all.size(), static_cast<std::size_t>(kCount));

    // Each token exactly once. Completions may arrive in any order, so this
    // counts rather than compares positionally.
    int seen[kCount];
    for (int i = 0; i < kCount; ++i) seen[i] = 0;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const uint64_t token = all[i].user_data;
        MT_EXPECT_TRUE(token >= 0x1000ull && token < 0x1000ull + kCount);
        if (token >= 0x1000ull && token < 0x1000ull + kCount) {
            ++seen[token - 0x1000ull];
        }
        MT_EXPECT_EQ(all[i].res, 0);
    }
    for (int i = 0; i < kCount; ++i) {
        MT_EXPECT_EQ(seen[i], 1);
    }
    MT_EXPECT_EQ(ring.value()->DroppedCompletions(), static_cast<uint32_t>(0));
}

MT_TEST(reaping_twice_does_not_replay_completions) {
    IoRingConfig c;
    if (!ProbeConfigWithSq(8, &c)) {
        Skip("reap idempotence", "io_uring not available on this kernel");
        return;
    }
    core::Expected<std::unique_ptr<Ring>, std::string> ring = Ring::Open(c);
    MT_EXPECT_TRUE(ring.ok());
    if (!ring.ok()) return;

    for (int i = 0; i < 3; ++i) {
        SubmissionEntry e = SubmissionEntry::Nop();
        e.UserData(static_cast<uint64_t>(100 + i));
        MT_EXPECT_TRUE(ring.value()->Submit(e).ok());
    }
    core::Expected<std::vector<CompletionEntry>, std::string> first =
        ring.value()->WaitAndReap(3);
    MT_EXPECT_TRUE(first.ok());
    if (!first.ok()) return;
    MT_EXPECT_EQ(first.value().size(), static_cast<std::size_t>(3));

    // Advancing the CQ head is what frees the slots. A second reap seeing the
    // same entries would mean the head never moved -- and the kernel would
    // start dropping completions once the ring wrapped.
    MT_EXPECT_TRUE(ring.value()->Reap().empty());
}

MT_MAIN
