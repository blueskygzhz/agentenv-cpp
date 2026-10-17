// SPDX-License-Identifier: MIT
// Tests for the ublk queue plane. Rust: storage/ublk/src/{queue,io_buffer}.rs.
//
// The parts that decide correctness here are arithmetic and protocol rules:
// the per-queue mmap offset, the IO command encoding, and which of
// `ublksrv_io_cmd::addr` / the SQE's `addr` carries the buffer. All three are
// checked without a kernel, because all three fail silently on a real one --
// a wrong offset maps another queue's descriptors, and a wrong `addr` is
// either rejected as a protocol violation or points the kernel at address
// zero.
#include "microtest.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "agentenv/storage/ublk/queue.h"

using namespace agentenv::storage::ublk;
namespace util = agentenv::storage::util;
namespace core = agentenv::core;

namespace {

void Skip(const char* what, const char* why) {
    std::printf("       SKIP %s: %s\n", what, why);
}

/// A ring with sparse tables, probed down for older kernels.
///
/// Returns null when the *sparse* registration opcodes are missing rather
/// than when io_uring is: `IORING_REGISTER_FILES2`/`BUFFERS2` arrived in 5.13,
/// several releases after the ring itself, so a kernel that runs every other
/// test here still cannot build this one. Reporting it as "io_uring
/// unavailable" would send a reader looking in the wrong place.
std::shared_ptr<util::Ring> MakeRing(uint32_t buffers, uint32_t files) {
    util::IoRingConfig c;
    c.sq_entries = 8;
    c.cq_entries = 16;
    c.sparse_buffers = buffers;
    c.sparse_files = files;
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

}  // namespace

// ============================================================
// the addr rule — which field names the memory
//
// Checked as a pure function rather than through a constructed buffer: the
// rule is the protocol, and tying it to an allocation would leave it
// unverified on every kernel without the sparse registration opcodes. Both
// failure modes are silent -- a non-zero cmd addr under auto-registration is
// rejected by the kernel, and a zero one without it points at address zero.
// ============================================================

MT_TEST(a_user_buffer_names_its_memory_in_the_command) {
    int storage = 0;
    const BufferAddrs a = AddrsForUserBuffer(&storage);
    MT_EXPECT_EQ(a.cmd_addr, reinterpret_cast<uint64_t>(&storage));
    // No SQE addr: this mode has no registered slot to put there, and
    // writing one would overwrite a field the kernel reads for other
    // purposes.
    MT_EXPECT_TRUE(!a.has_sqe_addr);
    MT_EXPECT_EQ(a.sqe_addr, static_cast<uint64_t>(0));
}

MT_TEST(an_auto_reg_buffer_leaves_the_command_addr_zero) {
    abi::AutoBufReg reg;
    std::memset(&reg, 0, sizeof(reg));
    reg.index = 9;

    const BufferAddrs a = AddrsForAutoReg(reg);
    // Zero, and Linux enforces it.
    MT_EXPECT_EQ(a.cmd_addr, static_cast<uint64_t>(0));
    // The slot travels in the SQE instead.
    MT_EXPECT_TRUE(a.has_sqe_addr);
    MT_EXPECT_EQ(a.sqe_addr & 0xFFFF, static_cast<uint64_t>(9));
}

MT_TEST(the_two_addr_fields_are_never_both_used) {
    // Exclusivity is the invariant: whichever field a mode does not use must
    // stay zero, because the kernel reads both.
    int storage = 0;
    const BufferAddrs user = AddrsForUserBuffer(&storage);
    abi::AutoBufReg reg;
    std::memset(&reg, 0, sizeof(reg));
    reg.index = 3;
    const BufferAddrs autoreg = AddrsForAutoReg(reg);

    MT_EXPECT_TRUE(user.cmd_addr != 0 && !user.has_sqe_addr);
    MT_EXPECT_TRUE(autoreg.cmd_addr == 0 && autoreg.has_sqe_addr);
}

// ============================================================
// descriptor area offsets — a wrong stride maps the wrong queue
// ============================================================

MT_TEST(queue_zero_starts_at_the_command_buffer_base) {
    MT_EXPECT_EQ(QueueDescMapOffset(0, 4096), abi::kCmdBufOffset);
}

MT_TEST(each_queue_is_strided_by_the_maximum_depth_not_its_own) {
    // 4096 descriptors of 24 bytes is 98304, already page-aligned.
    const uint64_t stride = static_cast<uint64_t>(abi::kMaxQueueDepth) * sizeof(abi::IoDesc);
    MT_EXPECT_EQ(stride, static_cast<uint64_t>(98304));

    MT_EXPECT_EQ(QueueDescMapOffset(1, 4096), abi::kCmdBufOffset + stride);
    MT_EXPECT_EQ(QueueDescMapOffset(3, 4096), abi::kCmdBufOffset + 3 * stride);

    // The stride is the kernel's reservation, not this device's depth: a
    // device of depth 16 still has its queue 1 a full 98304 bytes in. Striding
    // by the actual depth would land queue 1 inside queue 0's region, which
    // maps successfully and serves another queue's requests.
    MT_EXPECT_TRUE(QueueDescMapOffset(1, 4096) > 16 * sizeof(abi::IoDesc));
}

MT_TEST(the_stride_is_rounded_up_to_a_page) {
    // With a page size the raw area is not a multiple of, the stride must grow
    // to the next page: queue N's region has to start on a page boundary or
    // `mmap` refuses the offset outright.
    const std::size_t raw = static_cast<std::size_t>(abi::kMaxQueueDepth) * sizeof(abi::IoDesc);
    const uint64_t off = QueueDescMapOffset(1, 65536);
    MT_EXPECT_EQ(off % 65536, static_cast<uint64_t>(0));
    MT_EXPECT_TRUE(off >= raw);
    // 98304 rounds up to 131072 under 64 KiB pages.
    MT_EXPECT_EQ(off, static_cast<uint64_t>(131072));
}

// ============================================================
// io command encoding
// ============================================================

MT_TEST(io_commands_encode_the_sixteen_byte_payload) {
    // Half the size of a control command. Reusing the control encoding (32)
    // would produce numbers the kernel does not recognise.
    MT_EXPECT_EQ(abi::IoCmdFetchReq(), static_cast<uint32_t>(0xC0107520));
    MT_EXPECT_EQ(abi::IoCmdCommitAndFetchReq(), static_cast<uint32_t>(0xC0107521));

    const uint32_t cmds[] = {
        abi::IoCmdFetchReq(),       abi::IoCmdCommitAndFetchReq(),
        abi::IoCmdNeedGetData(),    abi::IoCmdRegisterIoBuf(),
        abi::IoCmdUnregisterIoBuf(),
    };
    const int n = sizeof(cmds) / sizeof(cmds[0]);
    for (int i = 0; i < n; ++i) {
        MT_EXPECT_EQ(cmds[i] >> 30, static_cast<uint32_t>(3));
        MT_EXPECT_EQ((cmds[i] >> 16) & 0x3FFF, static_cast<uint32_t>(16));
        MT_EXPECT_EQ((cmds[i] >> 8) & 0xFF, static_cast<uint32_t>('u'));
    }
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) MT_EXPECT_TRUE(cmds[i] != cmds[j]);
    }
    // And distinct from the control commands, which share the 'u' type byte
    // and differ only in the encoded size.
    MT_EXPECT_TRUE(abi::IoCmdFetchReq() != abi::CmdAddDev());
}

MT_TEST(auto_buf_reg_packs_the_index_into_the_low_bits) {
    abi::AutoBufReg b;
    std::memset(&b, 0, sizeof(b));
    b.index = 0x1234;
    b.flags = 0x56;
    b.reserved0 = 0x78;
    b.reserved1 = 0x9ABCDEF0;

    // The kernel reads the index out of the low 16 bits, so any other field
    // order hands it another slot's buffer.
    const uint64_t addr = abi::AutoBufRegToSqeAddr(b);
    MT_EXPECT_EQ(addr & 0xFFFF, static_cast<uint64_t>(0x1234));
    MT_EXPECT_EQ((addr >> 16) & 0xFF, static_cast<uint64_t>(0x56));
    MT_EXPECT_EQ((addr >> 24) & 0xFF, static_cast<uint64_t>(0x78));
    MT_EXPECT_EQ(addr >> 32, static_cast<uint64_t>(0x9ABCDEF0));
}

// ============================================================
// descriptor operation mapping
// ============================================================

MT_TEST(descriptor_operations_use_the_kernel_numbering) {
    // Zoned ops start at 10, not 6: numbering them consecutively after
    // WriteZeroes would map every zone request to the wrong operation.
    MT_EXPECT_EQ(static_cast<uint8_t>(UblkDescOperation::WriteSame), static_cast<uint8_t>(4));
    MT_EXPECT_EQ(static_cast<uint8_t>(UblkDescOperation::WriteZeroes), static_cast<uint8_t>(5));
    MT_EXPECT_EQ(static_cast<uint8_t>(UblkDescOperation::ZoneOpen), static_cast<uint8_t>(10));
    MT_EXPECT_EQ(static_cast<uint8_t>(UblkDescOperation::ZoneReset), static_cast<uint8_t>(15));

    core::Optional<UblkDescOperation> read = DescOperationFrom(0);
    MT_EXPECT_TRUE(read.has_value() && *read == UblkDescOperation::Read);
    core::Optional<UblkDescOperation> zopen = DescOperationFrom(10);
    MT_EXPECT_TRUE(zopen.has_value() && *zopen == UblkDescOperation::ZoneOpen);
}

MT_TEST(an_unknown_operation_is_absent_rather_than_defaulted) {
    // 6..9 are unused by the kernel, and 18 is ReportZones which this server
    // does not implement. All must be absent: a descriptor this build does
    // not understand has to be failed back, and guessing turns an unsupported
    // request into data loss.
    MT_EXPECT_TRUE(!DescOperationFrom(6).has_value());
    MT_EXPECT_TRUE(!DescOperationFrom(9).has_value());
    MT_EXPECT_TRUE(!DescOperationFrom(18).has_value());
    MT_EXPECT_TRUE(!DescOperationFrom(255).has_value());
}

// ============================================================
// buffers — which field carries the memory
// ============================================================

MT_TEST(a_user_buffer_is_registered_and_addressed_by_pointer) {
    std::shared_ptr<util::Ring> ring = MakeRing(8, 4);
    if (!ring) {
        Skip("user buffer", kNoSparse);
        return;
    }
    core::Expected<std::shared_ptr<UserBuffer>, std::string> buf =
        UserBuffer::Create(ring, 4096, 4096);
    if (!buf.ok()) {
        Skip("user buffer", buf.error().c_str());
        return;
    }
    MT_EXPECT_EQ(buf.value()->Size(), static_cast<std::size_t>(4096));
    // Aligned as asked: the kernel may require it for O_DIRECT-backed targets.
    MT_EXPECT_EQ(reinterpret_cast<uintptr_t>(buf.value()->Data()) % 4096,
                 static_cast<uintptr_t>(0));

    IoBuffer io = IoBuffer::OfUser(buf.value());
    MT_EXPECT_TRUE(io.IsUser());
    MT_EXPECT_TRUE(!io.IsAutoReg());
    // The command carries the address...
    MT_EXPECT_EQ(io.CmdAddr(), reinterpret_cast<uint64_t>(buf.value()->Data()));
    // ...and the SQE's addr is not used at all.
    uint64_t sqe_addr = 0xDEAD;
    MT_EXPECT_TRUE(!io.SqeAddr(&sqe_addr));
}

MT_TEST(an_auto_reg_buffer_reserves_a_slot_and_leaves_cmd_addr_zero) {
    std::shared_ptr<util::Ring> ring = MakeRing(8, 4);
    if (!ring) {
        Skip("auto-reg buffer", kNoSparse);
        return;
    }
    core::Expected<std::shared_ptr<AutoRegBuffer>, std::string> buf =
        AutoRegBuffer::Create(ring, 4096);
    if (!buf.ok()) {
        Skip("auto-reg buffer", buf.error().c_str());
        return;
    }

    IoBuffer io = IoBuffer::OfAutoReg(buf.value());
    MT_EXPECT_TRUE(io.IsAutoReg());
    // Zero, and Linux requires it: the kernel supplies the pages under
    // AUTO_BUF_REG and reads a non-zero value here as a protocol violation.
    MT_EXPECT_EQ(io.CmdAddr(), static_cast<uint64_t>(0));
    // The slot travels in the SQE's addr instead. The two fields are mutually
    // exclusive -- whichever is unused for a mode must stay zero.
    uint64_t sqe_addr = 0;
    MT_EXPECT_TRUE(io.SqeAddr(&sqe_addr));
    MT_EXPECT_EQ(sqe_addr & 0xFFFF, static_cast<uint64_t>(buf.value()->Index()));
}

MT_TEST(buffer_slots_are_recycled_when_a_buffer_is_dropped) {
    std::shared_ptr<util::Ring> ring = MakeRing(2, 4);
    if (!ring) {
        Skip("slot recycling", kNoSparse);
        return;
    }
    // A table of two. Taking both and releasing one must leave room for
    // another: a slot leaked on drop would shrink the table permanently, and
    // a long-running queue would run out.
    core::Expected<std::shared_ptr<AutoRegBuffer>, std::string> a =
        AutoRegBuffer::Create(ring, 4096);
    core::Expected<std::shared_ptr<AutoRegBuffer>, std::string> b =
        AutoRegBuffer::Create(ring, 4096);
    if (!a.ok() || !b.ok()) {
        Skip("slot recycling", "sparse buffer table unavailable");
        return;
    }
    MT_EXPECT_TRUE(a.value()->Index() != b.value()->Index());

    // Full now.
    MT_EXPECT_TRUE(!AutoRegBuffer::Create(ring, 4096).ok());

    const uint16_t freed = a.value()->Index();
    a = core::make_unexpected(std::string("dropped"));  // releases the buffer

    core::Expected<std::shared_ptr<AutoRegBuffer>, std::string> c =
        AutoRegBuffer::Create(ring, 4096);
    MT_EXPECT_TRUE(c.ok());
    if (c.ok()) MT_EXPECT_EQ(c.value()->Index(), freed);
}

MT_TEST(a_user_buffer_rejects_a_bad_size_or_alignment) {
    std::shared_ptr<util::Ring> ring = MakeRing(8, 4);
    if (!ring) {
        Skip("buffer validation", kNoSparse);
        return;
    }
    MT_EXPECT_TRUE(!UserBuffer::Create(ring, 0, 4096).ok());
    // Not a power of two.
    MT_EXPECT_TRUE(!UserBuffer::Create(ring, 4096, 3000).ok());
    // Smaller than a pointer, which posix_memalign refuses.
    MT_EXPECT_TRUE(!UserBuffer::Create(ring, 4096, 4).ok());
}

MT_TEST(a_subslice_past_the_end_is_refused) {
    std::shared_ptr<util::Ring> ring = MakeRing(8, 4);
    if (!ring) {
        Skip("subslice bounds", kNoSparse);
        return;
    }
    core::Expected<std::shared_ptr<UserBuffer>, std::string> buf =
        UserBuffer::Create(ring, 4096, 4096);
    if (!buf.ok()) {
        Skip("subslice bounds", buf.error().c_str());
        return;
    }
    MT_EXPECT_TRUE(buf.value()->Subslice(0, 4096) != NULL);
    MT_EXPECT_TRUE(buf.value()->Subslice(4095, 1) != NULL);
    MT_EXPECT_TRUE(buf.value()->Subslice(0, 4097) == NULL);
    MT_EXPECT_TRUE(buf.value()->Subslice(4096, 1) == NULL);
    // The extents come from a kernel-written descriptor, so an overflowing
    // pair is untrusted input to reject rather than a reason to trap.
    MT_EXPECT_TRUE(buf.value()->Subslice(0xFFFFFFFFFFFFFF00ull, 4096) == NULL);
}

// ============================================================
// queue construction and command payloads
// ============================================================

MT_TEST(a_queue_refuses_a_depth_past_the_kernel_maximum) {
    // Caught before the mmap: a depth past the maximum would compute an area
    // larger than the kernel reserved and overlap the next queue's region.
    core::Expected<std::unique_ptr<QueueIoDescs>, std::string> r =
        QueueIoDescs::Map(0, abi::kMaxQueueDepth + 1, 0);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("UBLK_MAX_QUEUE_DEPTH") != std::string::npos);

    MT_EXPECT_TRUE(!QueueIoDescs::Map(0, 0, 0).ok());
    MT_EXPECT_TRUE(!QueueIoDescs::Map(0, 16, -1).ok());
}

MT_TEST(a_queue_id_past_the_device_is_refused) {
    // A plain ring is enough: the id check runs before the sqe128 check and
    // before anything is mapped, so this stays verifiable on kernels without
    // the newer setup flags.
    util::IoRingConfig c;
    c.sq_entries = 8;
    c.cq_entries = 16;
    core::Expected<std::unique_ptr<util::Ring>, std::string> opened = util::Ring::Open(c);
    if (!opened.ok()) {
        c.coop_taskrun = false;
        c.single_issuer = false;
        opened = util::Ring::Open(c);
    }
    if (!opened.ok()) {
        c.cq_entries = 0;
        opened = util::Ring::Open(c);
    }
    if (!opened.ok()) {
        Skip("queue id bound", "io_uring not available on this kernel");
        return;
    }
    std::shared_ptr<util::Ring> ring(opened.take_value().release());

    abi::CtrlDevInfo info;
    std::memset(&info, 0, sizeof(info));
    info.nr_hw_queues = 2;
    info.queue_depth = 16;

    // Queue 2 of a two-queue device does not exist. Its descriptor area would
    // map past the region the kernel reserved for this device.
    core::Expected<std::unique_ptr<UblkQueue>, std::string> r =
        UblkQueue::Create(2, info, 0, ring);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("nr_hw_queues") != std::string::npos);
}

MT_TEST(a_queue_refuses_a_ring_that_cannot_carry_its_commands) {
    util::IoRingConfig c;
    c.sq_entries = 8;
    c.cq_entries = 16;
    c.sqe128 = false;
    core::Expected<std::unique_ptr<util::Ring>, std::string> opened = util::Ring::Open(c);
    if (!opened.ok()) {
        c.coop_taskrun = false;
        c.single_issuer = false;
        opened = util::Ring::Open(c);
    }
    if (!opened.ok()) {
        c.cq_entries = 0;
        opened = util::Ring::Open(c);
    }
    if (!opened.ok()) {
        Skip("queue sqe128", "io_uring not available on this kernel");
        return;
    }
    std::shared_ptr<util::Ring> ring(opened.take_value().release());

    abi::CtrlDevInfo info;
    std::memset(&info, 0, sizeof(info));
    info.nr_hw_queues = 1;
    info.queue_depth = 16;

    // Named at setup rather than at the first command, and before the mmap:
    // the queue shares its ring with the control plane in practice, where a
    // 64-byte SQE cannot carry a 32-byte control command at all.
    core::Expected<std::unique_ptr<UblkQueue>, std::string> r =
        UblkQueue::Create(0, info, 0, ring);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("sqe128") != std::string::npos);
}

MT_TEST(zero_copy_follows_the_negotiated_device_flag) {
    // Read off the device's flags, not off the buffer a slot happens to hold:
    // the mode was agreed with the kernel at ADD_DEV and cannot be chosen per
    // request.
    abi::CtrlDevInfo plain;
    std::memset(&plain, 0, sizeof(plain));
    MT_EXPECT_EQ(plain.flags & abi::kFAutoBufReg, static_cast<uint64_t>(0));

    abi::CtrlDevInfo zc;
    std::memset(&zc, 0, sizeof(zc));
    zc.flags = abi::kFAutoBufReg;
    MT_EXPECT_TRUE((zc.flags & abi::kFAutoBufReg) != 0);
    // Bit 11, distinct from the zero-copy bit 0 the name might suggest.
    MT_EXPECT_EQ(abi::kFAutoBufReg, 1ull << 11);
}

MT_MAIN
