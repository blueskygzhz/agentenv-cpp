// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/impls/overlaybd_target.rs — its `mod tests`.
#include "microtest.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/image_helper.h"
#include "agentenv/storage/overlaybd/image_service.h"
#include "agentenv/storage/ublk/overlaybd_target.h"

using namespace agentenv;                   // NOLINT
using namespace agentenv::storage::ublk;    // NOLINT

namespace obd = agentenv::storage::overlaybd;
namespace util = agentenv::storage::util;

namespace {

/// The image's own block size (`obd::kImageDefaultBlockSize`), which is what
/// becomes the device's *logical* block size -- hence a shift of 9, not 12.
/// Upstream's geometry test uses exactly this pairing: `(logical 9,
/// physical 12)`.
const uint64_t kSector = 512;
const uint64_t kBlk = 4096;
const uint64_t kVirtualSize = 64 * kBlk;
/// What the device advertises, in logical blocks.
const uint64_t kDevSectors = kVirtualSize / kSector;
/// A request size that fills the test buffer: 4096 bytes is 8 logical blocks.
const uint32_t kBlkInSectors = static_cast<uint32_t>(kBlk / kSector);

std::string MakeTempDir() {
    char tmpl[] = "/tmp/agentenv_ublk_obd_XXXXXX";
    char* made = ::mkdtemp(tmpl);
    if (made == 0) {
        std::fprintf(stderr, "  FAIL: mkdtemp\n");
        std::exit(1);
    }
    return std::string(made);
}

void RemoveTreeBestEffort(const std::string& dir) {
    const std::string cmd = "rm -rf '" + dir + "'";
    if (::system(cmd.c_str()) != 0) { /* ignored on purpose */ }
}

obd::GlobalConfig TestGlobalConfig(const std::string& dir) {
    obd::GlobalConfig cfg;
    cfg.cache_config.cache_type = "file";
    cfg.cache_config.cache_dir = core::fs::Join(dir, "cache");
    cfg.cache_config.cache_size_gb = 1;
    cfg.cache_config.refill_size = 65536;
    return cfg;
}

/// A writable image with a log-structured upper, pre-sized to `kVirtualSize`.
/// Upstream's own test builds the equivalent with a sparse upper.
std::shared_ptr<obd::ImageFile> OpenWritableImage(const std::string& dir) {
    const std::string data_path = core::fs::Join(dir, "upper.data");
    const std::string index_path = core::fs::Join(dir, "upper.index");
    if (!obd::PrepareRuntimeUpper(data_path, core::Optional<std::string>(index_path),
                                  kVirtualSize, obd::UpperMode::LogStructured)
             .ok()) {
        std::fprintf(stderr, "  FAIL: prepare upper\n");
        std::exit(1);
    }

    obd::ImageConfig cfg;
    cfg.upper.has_mode = true;
    cfg.upper.mode = obd::UpperMode::LogStructured;
    cfg.upper.data = data_path;
    cfg.upper.index = index_path;

    core::Expected<obd::ImageService, std::string> service =
        obd::ImageService::New(TestGlobalConfig(dir));
    if (!service.ok()) {
        std::fprintf(stderr, "  FAIL: build service: %s\n", service.error().c_str());
        std::exit(1);
    }
    core::Expected<std::shared_ptr<obd::ImageFile>, std::string> image =
        obd::ImageFile::Open(cfg, service.value(), core::Optional<std::string>());
    if (!image.ok()) {
        std::fprintf(stderr, "  FAIL: open image: %s\n", image.error().c_str());
        std::exit(1);
    }
    return image.value();
}

std::shared_ptr<OverlaybdTarget> OpenTarget(const std::string& dir, bool discard_supported) {
    core::Expected<std::shared_ptr<OverlaybdTarget>, std::string> target =
        OverlaybdTarget::FromOpenedImage(core::fs::Join(dir, "image.json"),
                                         OpenWritableImage(dir), discard_supported);
    if (!target.ok()) {
        std::fprintf(stderr, "  FAIL: build target: %s\n", target.error().c_str());
        std::exit(1);
    }
    return target.value();
}

/// `nr_sectors` is in logical blocks, matching what the kernel writes into a
/// descriptor; the target shifts it by `logical_bs_shift`.
abi::IoDesc Desc(uint8_t op, uint64_t start_sector, uint32_t nr_sectors) {
    abi::IoDesc desc;
    std::memset(&desc, 0, sizeof(desc));
    desc.op_flags = op;
    desc.start_sector = start_sector;
    desc.nr_sectors = nr_sectors;
    return desc;
}

/// The ring and buffer a read or write needs.
///
/// Null unless the tree was configured with `-DAGENTENV_WITH_LIBURING=ON`:
/// `UserBuffer::Create` registers the allocation with the ring, and
/// `Ring::Open` is a stub without liburing. The default build therefore
/// **skips** the two data-path tests below rather than failing them.
///
/// That is a real gap, not a formality: the `ReadAtIntoWithCtx` /
/// `WriteAtWithCtx` calls, the short-read and short-write checks, and the
/// byte-count return values are the part of this target that only these two
/// tests exercise. Run them with liburing enabled before trusting the data
/// plane. Everything else here -- dispatch, bounds, geometry, swap -- runs in
/// the default build.
struct RingBuffer {
    std::unique_ptr<util::Ring> ring;
    std::shared_ptr<UserBuffer> buffer;
    IoBuffer io;

    bool ok() const { return buffer != NULL; }
};

RingBuffer MakeRingBuffer(std::size_t bytes) {
    RingBuffer out;
    util::IoRingConfig cfg;
    cfg.sq_entries = 8;
    cfg.cq_entries = 16;
    cfg.sparse_buffers = 4;
    cfg.sparse_files = 4;
    core::Expected<std::unique_ptr<util::Ring>, std::string> ring = util::Ring::Open(cfg);
    if (!ring.ok()) return out;
    out.ring = std::move(ring.value());

    core::Expected<std::shared_ptr<UserBuffer>, std::string> buffer =
        UserBuffer::Create(std::shared_ptr<util::Ring>(out.ring.get(), [](util::Ring*) {}),
                           bytes, 512);
    if (!buffer.ok()) return out;
    out.buffer = buffer.value();
    out.io = IoBuffer::OfUser(out.buffer);
    return out;
}

}  // namespace

// ---- validate_block_size_shift ---------------------------------------------

MT_TEST(overlaybd_block_size_shift_validation) {
    // Rust `test_overlaybd_block_size_shift_validation`.
    MT_EXPECT_TRUE(ValidateBlockSizeShift(512).ok());
    MT_EXPECT_EQ(static_cast<int>(ValidateBlockSizeShift(512).value()), 9);
    MT_EXPECT_TRUE(ValidateBlockSizeShift(4096).ok());
    MT_EXPECT_EQ(static_cast<int>(ValidateBlockSizeShift(4096).value()), 12);

    // Zero and non-power-of-two are refused: the shift is the device's
    // logical block size, and a bogus one makes every offset wrong.
    MT_EXPECT_TRUE(!ValidateBlockSizeShift(0).ok());
    MT_EXPECT_TRUE(!ValidateBlockSizeShift(768).ok());
}

// ---- geometry ---------------------------------------------------------------

MT_TEST(overlaybd_target_params_report_the_image_geometry) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, false);

    abi::CtrlDevInfo info;
    std::memset(&info, 0, sizeof(info));
    info.max_io_buf_bytes = 512 * 1024;

    const abi::Params params = target->DevParams(info);
    // Read-only image: the discard bit must be absent, or the guest issues
    // TRIMs that can only ever be refused.
    MT_EXPECT_EQ(params.types, abi::kParamTypeBasic);
    // The image's 512-byte block size, as a shift.
    MT_EXPECT_EQ(static_cast<int>(params.basic.logical_bs_shift), 9);
    // Rust `DEFAULT_PHYSICAL_BS_SHIFT.max(logical_bs_shift)` — 12 wins.
    MT_EXPECT_EQ(static_cast<int>(params.basic.physical_bs_shift), 12);
    MT_EXPECT_EQ(static_cast<int>(params.basic.io_min_shift), 9);
    MT_EXPECT_EQ(static_cast<int>(params.basic.io_opt_shift), 12);
    MT_EXPECT_EQ(params.basic.dev_sectors, kDevSectors);
    // In logical blocks: this bounds one request against the buffer the queue
    // actually has for it.
    MT_EXPECT_EQ(params.basic.max_sectors, (512u * 1024u) >> 9);

    RemoveTreeBestEffort(dir);
}

MT_TEST(overlaybd_target_advertises_discard_only_when_supported) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, true);

    abi::CtrlDevInfo info;
    std::memset(&info, 0, sizeof(info));
    info.max_io_buf_bytes = 512 * 1024;

    const abi::Params params = target->DevParams(info);
    MT_EXPECT_EQ(params.types, abi::kParamTypeBasic | abi::kParamTypeDiscard);
    // The logical block size, i.e. `1 << logical_bs_shift`.
    MT_EXPECT_EQ(params.discard.discard_alignment, 512u);
    MT_EXPECT_EQ(params.discard.discard_granularity, 512u);
    // Zero: write-zeroes is answered as unsupported, so advertising a limit
    // would invite an op that always fails.
    MT_EXPECT_EQ(params.discard.max_write_zeroes_sectors, 0u);
    MT_EXPECT_EQ(params.discard.max_discard_segments, 1);

    RemoveTreeBestEffort(dir);
}

MT_TEST(overlaybd_target_names_itself_and_asks_for_no_extra_buffer) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, false);

    MT_EXPECT_EQ(target->DevName(), std::string("overlaybd"));
    // Rust returns `None`: reads and writes use the slot's own buffer.
    MT_EXPECT_TRUE(!target->PerSlotExtraBufLen().has_value());
    // Rust's `init` is `Ok(())`; there is nothing to register per queue.
    MT_EXPECT_TRUE(target->Init(0, NULL).ok());

    RemoveTreeBestEffort(dir);
}

// ---- construction guards ----------------------------------------------------

MT_TEST(overlaybd_target_refuses_a_null_image) {
    MT_EXPECT_TRUE(!OverlaybdTarget::FromOpenedImage(
                        "image.json", std::shared_ptr<obd::ImageFile>(), false).ok());
}

MT_TEST(overlaybd_target_open_reports_a_missing_global_config) {
    const core::Expected<std::shared_ptr<OverlaybdTarget>, std::string> target =
        OverlaybdTarget::Open("/nonexistent/global.json", "/nonexistent/image.json");
    MT_EXPECT_TRUE(!target.ok());
    // The message names which of the two configs failed, as upstream's
    // `with_context` does.
    MT_EXPECT_TRUE(target.error().find("image service") != std::string::npos);
}

// ---- request bounds ---------------------------------------------------------

MT_TEST(overlaybd_target_rejects_a_request_past_the_end) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, false);

    // One block past the last. Rust `request_meta`'s `end > dev_bytes` arm.
    const abi::IoDesc desc = Desc(abi::kIoOpRead, kDevSectors, 1);
    MT_EXPECT_EQ(target->HandleIoRequest(0, 0, desc, NULL, NULL, NULL), -EINVAL);

    RemoveTreeBestEffort(dir);
}

MT_TEST(overlaybd_target_rejects_an_overflowing_request) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, false);

    // `start_sector << 12` wraps. The descriptor is untrusted input read out
    // of a kernel-mapped array, so a wrapped end must not pass the bounds
    // check and then read outside the image.
    const abi::IoDesc desc = Desc(abi::kIoOpRead, 0xFFFFFFFFFFFFFFFFull, 8);
    MT_EXPECT_EQ(target->HandleIoRequest(0, 0, desc, NULL, NULL, NULL), -EINVAL);

    RemoveTreeBestEffort(dir);
}

MT_TEST(overlaybd_target_serves_an_empty_request_as_a_no_op) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, false);

    // Rust returns `Ok(0)` before touching the buffer, so a null one is fine.
    MT_EXPECT_EQ(target->HandleIoRequest(0, 0, Desc(abi::kIoOpRead, 0, 0), NULL, NULL, NULL), 0);
    MT_EXPECT_EQ(target->HandleIoRequest(0, 0, Desc(abi::kIoOpWrite, 0, 0), NULL, NULL, NULL), 0);
    MT_EXPECT_EQ(target->HandleIoRequest(0, 0, Desc(abi::kIoOpDiscard, 0, 0), NULL, NULL, NULL),
                 0);

    RemoveTreeBestEffort(dir);
}

// ---- op dispatch ------------------------------------------------------------

MT_TEST(overlaybd_target_refuses_a_zero_copy_buffer) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, false);

    // Rust bails with "does not support zero-copy IO buffer" for the
    // `AutoReg` arm. An absent buffer reaches the same refusal.
    MT_EXPECT_EQ(target->HandleIoRequest(0, 0, Desc(abi::kIoOpRead, 0, 1), NULL, NULL, NULL),
                 -EOPNOTSUPP);
    MT_EXPECT_EQ(target->HandleIoRequest(0, 0, Desc(abi::kIoOpWrite, 0, 1), NULL, NULL, NULL),
                 -EOPNOTSUPP);

    RemoveTreeBestEffort(dir);
}

MT_TEST(overlaybd_target_refuses_write_zeroes_and_unknown_ops) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, true);

    MT_EXPECT_EQ(
        target->HandleIoRequest(0, 0, Desc(abi::kIoOpWriteZeroes, 0, 1), NULL, NULL, NULL),
        -EOPNOTSUPP);
    MT_EXPECT_EQ(
        target->HandleIoRequest(0, 0, Desc(abi::kIoOpWriteSame, 0, 1), NULL, NULL, NULL),
        -EOPNOTSUPP);

    RemoveTreeBestEffort(dir);
}

MT_TEST(overlaybd_target_flushes_through_to_the_image) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, true);

    // Flush needs no buffer and no bounds: Rust forwards it to `image.sync()`.
    MT_EXPECT_EQ(target->HandleIoRequest(0, 0, Desc(abi::kIoOpFlush, 0, 0), NULL, NULL, NULL), 0);

    RemoveTreeBestEffort(dir);
}

MT_TEST(overlaybd_target_refuses_discard_on_a_read_only_image) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, false);

    // Rust raises `PermissionDenied`, which its errno mapping turns into
    // EROFS. Reported as EROFS here too, rather than as a generic EIO.
    MT_EXPECT_EQ(target->HandleIoRequest(0, 0, Desc(abi::kIoOpDiscard, 0, 1), NULL, NULL, NULL),
                 -EROFS);

    RemoveTreeBestEffort(dir);
}

MT_TEST(overlaybd_target_passes_discard_through_when_writable) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, true);

    // Rust `test_overlaybd_target_discard_passthrough_sparse_image`.
    MT_EXPECT_EQ(target->HandleIoRequest(0, 0, Desc(abi::kIoOpDiscard, 0, 8), NULL, NULL, NULL),
                 0);

    RemoveTreeBestEffort(dir);
}

// ---- the data path ----------------------------------------------------------

MT_TEST(overlaybd_target_round_trips_a_write_through_a_read) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, true);

    RingBuffer rb = MakeRingBuffer(static_cast<std::size_t>(kBlk));
    if (!rb.ok()) {
        // No io_uring here. Skipped rather than failed: the dispatch and
        // bounds tests above cover the target's own logic, and this one is
        // about the bytes reaching the image.
        std::fprintf(stderr, "  (skipped: needs -DAGENTENV_WITH_LIBURING=ON)\n");
        RemoveTreeBestEffort(dir);
        return;
    }

    for (std::size_t i = 0; i < kBlk; ++i) {
        rb.buffer->Data()[i] = static_cast<uint8_t>(0x7A + (i % 251));
    }
    const int32_t wrote = target->HandleIoRequest(
        0, 0, Desc(abi::kIoOpWrite, 0, kBlkInSectors), &rb.io, NULL, rb.ring.get());
    MT_EXPECT_EQ(wrote, static_cast<int32_t>(kBlk));

    std::memset(rb.buffer->Data(), 0, kBlk);
    const int32_t read = target->HandleIoRequest(
        0, 0, Desc(abi::kIoOpRead, 0, kBlkInSectors), &rb.io, NULL, rb.ring.get());
    MT_EXPECT_EQ(read, static_cast<int32_t>(kBlk));

    bool same = true;
    for (std::size_t i = 0; i < kBlk; ++i) {
        if (rb.buffer->Data()[i] != static_cast<uint8_t>(0x7A + (i % 251))) same = false;
    }
    MT_EXPECT_TRUE(same);

    RemoveTreeBestEffort(dir);
}

MT_TEST(overlaybd_target_reads_an_unwritten_block_as_zeroes) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, true);

    RingBuffer rb = MakeRingBuffer(static_cast<std::size_t>(kBlk));
    if (!rb.ok()) {
        std::fprintf(stderr, "  (skipped: needs -DAGENTENV_WITH_LIBURING=ON)\n");
        RemoveTreeBestEffort(dir);
        return;
    }

    std::memset(rb.buffer->Data(), 0xFF, kBlk);
    // Nothing was ever written here, so the stack has no mapping for it. That
    // is a hole, not an error.
    const int32_t read = target->HandleIoRequest(
        0, 0, Desc(abi::kIoOpRead, 2 * kBlkInSectors, kBlkInSectors), &rb.io, NULL,
        rb.ring.get());
    MT_EXPECT_EQ(read, static_cast<int32_t>(kBlk));

    bool zeroed = true;
    for (std::size_t i = 0; i < kBlk; ++i) {
        if (rb.buffer->Data()[i] != 0) zeroed = false;
    }
    MT_EXPECT_TRUE(zeroed);

    RemoveTreeBestEffort(dir);
}

// ---- swap_state -------------------------------------------------------------

MT_TEST(overlaybd_target_swaps_its_backing_image) {
    const std::string first_dir = MakeTempDir();
    const std::string second_dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(first_dir, true);

    const std::shared_ptr<obd::ImageFile> replacement = OpenWritableImage(second_dir);
    const std::string next_path = core::fs::Join(second_dir, "image.json");
    MT_EXPECT_TRUE(target->SwapState(next_path, replacement, true).ok());

    MT_EXPECT_EQ(target->ImageConfigPath(), next_path);
    MT_EXPECT_TRUE(target->Image() == replacement);
    MT_EXPECT_EQ(target->DevSectors(), kDevSectors);

    RemoveTreeBestEffort(first_dir);
    RemoveTreeBestEffort(second_dir);
}

MT_TEST(overlaybd_target_keeps_serving_when_a_swap_is_rejected) {
    const std::string dir = MakeTempDir();
    const std::shared_ptr<OverlaybdTarget> target = OpenTarget(dir, true);
    const std::string original = target->ImageConfigPath();
    const std::shared_ptr<obd::ImageFile> live = target->Image();

    // Validated before the swap, so a rejected image leaves the live one in
    // place rather than tearing down a working device.
    MT_EXPECT_TRUE(!target->SwapState("other.json", std::shared_ptr<obd::ImageFile>(), true).ok());
    MT_EXPECT_EQ(target->ImageConfigPath(), original);
    MT_EXPECT_TRUE(target->Image() == live);

    RemoveTreeBestEffort(dir);
}

MT_MAIN
