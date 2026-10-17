// SPDX-License-Identifier: MIT
// Tests for the ublk control plane. Rust: storage/ublk/src/ctrl.rs and the
// `ublk_caps` tests.
//
// The command numbers and `BuildParams` are pure computation and are checked
// unconditionally: a wrong ioctl encoding or a wrong `len` is answered by the
// kernel with a bare errno, so getting them right is not something a live test
// would diagnose any better. Anything that opens /dev/ublk-control skips with
// a printed reason when the module is absent.
#include "microtest.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "agentenv/storage/ublk/ctrl.h"
#include "agentenv/storage/ublk/caps.h"

using namespace agentenv::storage::ublk;
namespace util = agentenv::storage::util;
namespace core = agentenv::core;

namespace {

void Skip(const char* what, const char* why) {
    std::printf("       SKIP %s: %s\n", what, why);
}

/// An sqe128 ring, or null when this kernel cannot make one.
std::shared_ptr<util::Ring> MakeCtrlRing() {
    util::IoRingConfig c;
    c.sq_entries = 8;
    c.cq_entries = 16;
    c.sqe128 = true;
    // Probed down the same way the io_ring tests do: these setup flags landed
    // across several releases and a kernel without one answers EINVAL.
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

/// A 64-byte-SQE ring, used to prove the builder refuses one.
std::shared_ptr<util::Ring> MakePlainRing() {
    util::IoRingConfig c;
    c.sq_entries = 8;
    c.cq_entries = 16;
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

}  // namespace

// ============================================================
// command encoding — a wrong number is answered with a bare errno
// ============================================================

MT_TEST(update_size_matches_the_kernel_encoding) {
    // Rust asserts this exact literal. The size is part of the encoding, so
    // this is simultaneously a check that `sizeof(ublksrv_ctrl_cmd)` is 32:
    // 0x20 appears in the middle of the number.
    MT_EXPECT_EQ(abi::CmdUpdateSize(), static_cast<uint32_t>(0xC0207515));
    // And the ported constant in caps.h agrees with the ABI header, which are
    // two independent derivations of the same number.
    MT_EXPECT_EQ(abi::CmdUpdateSize(), kUblkUCmdUpdateSize);
}

MT_TEST(every_control_command_is_distinct_and_well_formed) {
    const uint32_t cmds[] = {
        abi::CmdGetDevInfo(), abi::CmdAddDev(),      abi::CmdDelDev(),
        abi::CmdStartDev(),   abi::CmdStopDev(),     abi::CmdSetParams(),
        abi::CmdGetFeatures(), abi::CmdUpdateSize(),
    };
    const int n = sizeof(cmds) / sizeof(cmds[0]);

    for (int i = 0; i < n; ++i) {
        // _IOC_READ|_IOC_WRITE in the top two bits: every one of these both
        // sends and receives, so a command built as write-only would be
        // rejected by the kernel's own direction check.
        MT_EXPECT_EQ(cmds[i] >> 30, static_cast<uint32_t>(3));
        // The size field is what ties the number to the struct.
        MT_EXPECT_EQ((cmds[i] >> 16) & 0x3FFF, static_cast<uint32_t>(32));
        // The 'u' type byte.
        MT_EXPECT_EQ((cmds[i] >> 8) & 0xFF, static_cast<uint32_t>('u'));
    }
    // All eight distinct. A collision would silently send one command where
    // another was meant -- DEL_DEV for STOP_DEV, say.
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            MT_EXPECT_TRUE(cmds[i] != cmds[j]);
        }
    }
}

MT_TEST(feature_bits_are_the_kernel_values) {
    // Bit 10, not bit 0: bit 0 is zero-copy, and conflating them would make
    // every zero-copy-capable kernel look like it supports resizing.
    MT_EXPECT_EQ(abi::kFUpdateSize, 1ull << 10);
    MT_EXPECT_EQ(abi::kFSupportZeroCopy, 1ull << 0);
    MT_EXPECT_EQ(abi::kFAutoBufReg, 1ull << 11);
    // caps.h and abi.h agree.
    MT_EXPECT_EQ(abi::kFUpdateSize, kUblkFUpdateSize);
    MT_EXPECT_EQ(abi::kFSupportZeroCopy, kUblkFSupportZeroCopy);
}

// ============================================================
// BuildParams — the geometry the kernel publishes
// ============================================================

MT_TEST(build_params_fills_len_and_types) {
    abi::Params p = BuildParams(2048, 9, 12, 256 * 1024, false);

    // `len` must equal the struct size: the kernel compares it with its own
    // and refuses a mismatch, which is how an ABI skew is caught at
    // SET_PARAMS instead of becoming a disk with wrong geometry.
    MT_EXPECT_EQ(p.len, static_cast<uint32_t>(sizeof(abi::Params)));
    MT_EXPECT_EQ(p.len, static_cast<uint32_t>(120));

    // Only BASIC, because discard was not requested. Setting the DISCARD bit
    // with a zeroed discard struct would advertise a zero-granularity discard.
    MT_EXPECT_EQ(p.types, abi::kParamTypeBasic);
    MT_EXPECT_EQ(p.discard.discard_granularity, static_cast<uint32_t>(0));
    MT_EXPECT_EQ(p.discard.max_discard_segments, static_cast<uint16_t>(0));
}

MT_TEST(build_params_computes_basic_geometry) {
    // 512-byte logical, 4 KiB physical, 1 MiB of device, 256 KiB buffers.
    abi::Params p = BuildParams(2048, 9, 12, 256 * 1024, false);

    MT_EXPECT_EQ(p.basic.logical_bs_shift, static_cast<uint8_t>(9));
    MT_EXPECT_EQ(p.basic.physical_bs_shift, static_cast<uint8_t>(12));
    // io_min tracks the logical block: it is the smallest transfer the device
    // can accept at all.
    MT_EXPECT_EQ(p.basic.io_min_shift, static_cast<uint8_t>(9));
    // io_opt is max(default, physical). Both are 12 here.
    MT_EXPECT_EQ(p.basic.io_opt_shift, static_cast<uint8_t>(12));
    // max_sectors is in logical blocks: 256 KiB / 512.
    MT_EXPECT_EQ(p.basic.max_sectors, static_cast<uint32_t>(512));
    MT_EXPECT_EQ(p.basic.dev_sectors, static_cast<uint64_t>(2048));
}

MT_TEST(io_opt_never_falls_below_the_physical_block) {
    // A 64 KiB physical block, above the 4 KiB default. An optimal transfer
    // smaller than the device's own write granularity would have the kernel
    // issue sub-block writes it then has to read-modify-write.
    abi::Params p = BuildParams(2048, 9, 16, 256 * 1024, false);
    MT_EXPECT_EQ(p.basic.io_opt_shift, static_cast<uint8_t>(16));

    // And below it, the default wins.
    abi::Params small = BuildParams(2048, 9, 9, 256 * 1024, false);
    MT_EXPECT_EQ(small.basic.io_opt_shift, kDefaultIoOptShift);
    MT_EXPECT_EQ(kDefaultIoOptShift, static_cast<uint8_t>(12));
}

MT_TEST(discard_is_described_only_when_supported) {
    abi::Params p = BuildParams(4096, 12, 12, 256 * 1024, true);

    MT_EXPECT_EQ(p.types, abi::kParamTypeBasic | abi::kParamTypeDiscard);
    // Alignment and granularity are one logical block: 1 << 12.
    MT_EXPECT_EQ(p.discard.discard_alignment, static_cast<uint32_t>(4096));
    MT_EXPECT_EQ(p.discard.discard_granularity, static_cast<uint32_t>(4096));
    MT_EXPECT_EQ(p.discard.max_discard_sectors, static_cast<uint32_t>(64));
    MT_EXPECT_EQ(p.discard.max_discard_segments, static_cast<uint16_t>(1));
    // Zero, deliberately: write-zeroes is not implemented, and advertising it
    // would have the kernel send requests the target answers with an error.
    MT_EXPECT_EQ(p.discard.max_write_zeroes_sectors, static_cast<uint32_t>(0));
}

MT_TEST(kernel_filled_params_are_left_zero) {
    abi::Params p = BuildParams(2048, 9, 12, 256 * 1024, true);
    // `devt` is filled by the kernel and `zoned` is unused. Neither bit is in
    // `types`, so the kernel ignores both -- writing into them would be
    // claiming a major/minor this process does not get to choose.
    MT_EXPECT_EQ(p.devt.char_major, static_cast<uint32_t>(0));
    MT_EXPECT_EQ(p.devt.disk_major, static_cast<uint32_t>(0));
    MT_EXPECT_EQ(p.zoned.max_open_zones, static_cast<uint32_t>(0));
    MT_EXPECT_EQ(p.types & abi::kParamTypeDevt, static_cast<uint32_t>(0));
    MT_EXPECT_EQ(p.types & abi::kParamTypeZoned, static_cast<uint32_t>(0));
}

// ============================================================
// io descriptor accessors
// ============================================================

MT_TEST(io_desc_op_and_flags_unpack_from_one_word) {
    abi::IoDesc d;
    std::memset(&d, 0, sizeof(d));
    // op in the low 8 bits, flags above. Reading the whole word as the op
    // would turn every flagged request into an unknown opcode.
    d.op_flags = abi::kIoOpWrite | (1u << 16);
    MT_EXPECT_EQ(abi::IoDescOp(d), abi::kIoOpWrite);
    MT_EXPECT_EQ(abi::IoDescFlags(d), static_cast<uint32_t>(1u << 16));

    d.op_flags = abi::kIoOpDiscard;
    MT_EXPECT_EQ(abi::IoDescOp(d), abi::kIoOpDiscard);
    MT_EXPECT_EQ(abi::IoDescFlags(d), static_cast<uint32_t>(0));
}

// ============================================================
// builder — refusals that do not need a device
// ============================================================

MT_TEST(the_builder_refuses_a_ring_that_cannot_carry_a_control_command) {
    std::shared_ptr<util::Ring> plain = MakePlainRing();
    if (!plain) {
        Skip("sqe128 refusal", "io_uring not available on this kernel");
        return;
    }
    // Caught here, not at the first command. A `ublksrv_ctrl_cmd` is 32 bytes
    // and a 64-byte SQE holds 16, so the failure is certain -- and reported a
    // layer down it would read as a complaint about SQE sizes rather than
    // about how this ring was created.
    core::Expected<std::unique_ptr<UblkCtrl>, std::string> r =
        UblkCtrlBuilder().Name("t").Build(plain);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("sqe128") != std::string::npos);
}

MT_TEST(the_builder_refuses_a_null_ring) {
    core::Expected<std::unique_ptr<UblkCtrl>, std::string> r =
        UblkCtrlBuilder().Build(std::shared_ptr<util::Ring>());
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("null io_uring") != std::string::npos);
}

MT_TEST(zero_copy_can_be_turned_back_off) {
    // Cleared as well as set, so a builder reused with `false` does not keep
    // a flag an earlier call turned on. Checked on the projection rather than
    // on a built device: the flag is chosen entirely here, and needing a
    // loaded module to see it would leave it unverified on most hosts.
    abi::CtrlDevInfo on = UblkCtrlBuilder().Name("t").ZeroCopy(true).DevInfoProjection();
    MT_EXPECT_TRUE((on.flags & abi::kFAutoBufReg) != 0);

    abi::CtrlDevInfo off =
        UblkCtrlBuilder().Name("t").ZeroCopy(true).ZeroCopy(false).DevInfoProjection();
    MT_EXPECT_EQ(off.flags & abi::kFAutoBufReg, static_cast<uint64_t>(0));

    // And it does not disturb other bits.
    MT_EXPECT_EQ(off.flags, static_cast<uint64_t>(0));
}

MT_TEST(the_builder_projects_the_device_info_add_dev_will_carry) {
    abi::CtrlDevInfo d = UblkCtrlBuilder()
                             .Name("agentenv")
                             .NrQueues(2)
                             .Depth(32)
                             .MaxIoBufBytes(512 * 1024)
                             .DevId(11)
                             .DevInfoProjection();
    MT_EXPECT_EQ(d.nr_hw_queues, static_cast<uint16_t>(2));
    MT_EXPECT_EQ(d.queue_depth, static_cast<uint16_t>(32));
    MT_EXPECT_EQ(d.max_io_buf_bytes, static_cast<uint32_t>(512 * 1024));
    MT_EXPECT_EQ(d.dev_id, static_cast<uint32_t>(11));
    // The kernel attributes the device to this pid and tears it down if the
    // process dies, so a zero or wrong pid leaves an orphan device.
    MT_EXPECT_EQ(d.ublksrv_pid, static_cast<int32_t>(::getpid()));
    // Kernel-owned; asserting a state here would be claiming something this
    // process does not get to decide.
    MT_EXPECT_EQ(d.state, static_cast<uint16_t>(0));
}

MT_TEST(an_unset_dev_id_asks_the_kernel_to_assign_one) {
    // Rust's default is `u32::MAX`, which is ADD_DEV's "pick one for me". A
    // default of 0 would instead collide with the first device on the host.
    abi::CtrlDevInfo d = UblkCtrlBuilder().Name("t").DevInfoProjection();
    MT_EXPECT_EQ(d.dev_id, static_cast<uint32_t>(0xFFFFFFFFu));
    // Rust's other defaults, which decide the queue geometry.
    MT_EXPECT_EQ(d.nr_hw_queues, static_cast<uint16_t>(1));
    MT_EXPECT_EQ(d.queue_depth, static_cast<uint16_t>(16));
    MT_EXPECT_EQ(d.max_io_buf_bytes, static_cast<uint32_t>(256 * 1024));
}

MT_TEST(update_size_is_refused_without_the_feature_bit) {
    if (!UblkModuleLoaded()) {
        Skip("update_size gate", "ublk_drv is not loaded");
        return;
    }
    std::shared_ptr<util::Ring> ring = MakeCtrlRing();
    if (!ring) {
        Skip("update_size gate", "IORING_SETUP_SQE128 needs Linux 5.19 or newer");
        return;
    }
    core::Expected<std::unique_ptr<UblkCtrl>, std::string> ctrl =
        UblkCtrlBuilder().Name("t").Build(ring);
    MT_EXPECT_TRUE(ctrl.ok());
    if (!ctrl.ok()) return;

    // Gated before anything is sent. An older kernel answers the unknown
    // command with EOPNOTSUPP, which reads identically to a device refusing
    // the new size -- and the caller would retry the wrong thing.
    core::Expected<core::Unit, std::string> r =
        ctrl.value()->UpdateSize(1024, abi::kFSupportZeroCopy);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("UBLK_F_UPDATE_SIZE") != std::string::npos);
}

MT_TEST(device_paths_follow_the_assigned_id) {
    // Two different nodes: `ublkc` is the queue-side char device the data
    // plane opens, `ublkb` is the block device the kernel publishes. Mixing
    // them up gets a working open and no I/O.
    MT_EXPECT_TRUE(UblkCtrlBuilder::CharDevPathFor(7) == std::string("/dev/ublkc7"));
    MT_EXPECT_TRUE(UblkCtrlBuilder::BlockDevPathFor(7) == std::string("/dev/ublkb7"));
    // Multi-digit ids are not truncated or padded.
    MT_EXPECT_TRUE(UblkCtrlBuilder::CharDevPathFor(123) == std::string("/dev/ublkc123"));
    MT_EXPECT_TRUE(UblkCtrlBuilder::BlockDevPathFor(0) == std::string("/dev/ublkb0"));
}

// ============================================================
// live control plane
// ============================================================

MT_TEST(get_features_answers_from_the_driver) {
    if (!UblkAvailable()) {
        Skip("get_features", "ublk control device is not available");
        return;
    }
    std::shared_ptr<util::Ring> ring = MakeCtrlRing();
    if (!ring) {
        Skip("get_features", "IORING_SETUP_SQE128 needs Linux 5.19 or newer");
        return;
    }
    core::Expected<std::unique_ptr<UblkCtrl>, std::string> ctrl =
        UblkCtrlBuilder().Name("agentenv-test").Build(ring);
    MT_EXPECT_TRUE(ctrl.ok());
    if (!ctrl.ok()) {
        Skip("get_features", ctrl.error().c_str());
        return;
    }

    core::Expected<uint64_t, std::string> features = ctrl.value()->GetFeatures();
    MT_EXPECT_TRUE(features.ok());
    if (!features.ok()) return;
    // A driver that answered at all reports something; an all-zero bitmap
    // would mean the `addr` buffer was never written, which is the failure
    // mode of a wrong `len` or a wrong command number.
    MT_EXPECT_TRUE(features.value() != 0);
    std::printf("       ublk features = 0x%llx\n",
                static_cast<unsigned long long>(features.value()));
}

MT_MAIN
