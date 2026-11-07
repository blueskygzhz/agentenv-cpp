// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/impls/overlaybd_target.rs
#include "agentenv/storage/ublk/overlaybd_target.h"

#include <cerrno>
#include <sstream>

#include "agentenv/core/logging.h"

namespace agentenv {
namespace storage {
namespace ublk {

namespace {

/// Rust's `anyhow_to_ublk_errno`, as far as this port can express it.
///
/// Upstream downcasts to `std::io::Error` and reads `raw_os_error()`, so a
/// failure deep in the layer stack reaches the guest as its own errno.
/// `ImageFile` here reports `Expected<T, std::string>` -- the errno is already
/// gone by the time a caller sees it, and parsing it back out of a message
/// would be guessing.
///
/// So the mapping is inverted: each call site that *knows* what went wrong
/// names the errno itself (EROFS for a read-only discard, EINVAL for a
/// malformed descriptor, EOPNOTSUPP for an unsupported op), and this is only
/// the fallback for a failure whose cause is genuinely opaque. It returns the
/// same `-EIO` upstream's own fallback arm does.
int32_t OpaqueFailureErrno() { return -EIO; }

}  // namespace

core::Expected<uint8_t, std::string> ValidateBlockSizeShift(uint32_t block_size) {
    // Rust `block_size == 0 || !block_size.is_power_of_two()`.
    if (block_size == 0 || (block_size & (block_size - 1)) != 0) {
        std::ostringstream os;
        os << "overlaybd block size must be power-of-two, got " << block_size;
        return core::make_unexpected(os.str());
    }
    uint8_t shift = 0;
    while ((block_size >> shift) > 1) ++shift;
    return shift;
}

core::Expected<std::shared_ptr<OverlaybdTarget::State>, std::string>
OverlaybdTarget::MakeState(const std::string& image_config_path,
                           std::shared_ptr<overlaybd::ImageFile> image,
                           bool discard_supported) {
    if (!image) {
        return core::make_unexpected(std::string("overlaybd target needs an open image"));
    }
    const core::Expected<uint8_t, std::string> shift =
        ValidateBlockSizeShift(image->BlockSize());
    if (!shift.ok()) return core::make_unexpected(shift.error());

    const uint64_t dev_sectors = image->NumLbas();
    if (dev_sectors == 0) {
        // Refused rather than allowed through as a zero-length device: the
        // kernel would create it, and the guest would see a disk it cannot
        // read a single sector from.
        return core::make_unexpected("overlaybd image has zero capacity: " + image_config_path);
    }

    std::shared_ptr<State> state(new State());
    state->image_config_path = image_config_path;
    state->image = image;
    state->dev_sectors = dev_sectors;
    state->logical_bs_shift = shift.value();
    // Rust `DEFAULT_PHYSICAL_BS_SHIFT.max(logical_bs_shift)`.
    state->physical_bs_shift = shift.value() > kDefaultPhysicalBsShift ? shift.value()
                                                                       : kDefaultPhysicalBsShift;
    state->discard_supported = discard_supported;
    return state;
}

core::Expected<std::shared_ptr<OverlaybdTarget>, std::string> OverlaybdTarget::FromOpenedImage(
    const std::string& image_config_path, std::shared_ptr<overlaybd::ImageFile> image,
    bool discard_supported) {
    const core::Expected<std::shared_ptr<State>, std::string> state =
        MakeState(image_config_path, image, discard_supported);
    if (!state.ok()) return core::make_unexpected(state.error());

    std::shared_ptr<OverlaybdTarget> target(new OverlaybdTarget());
    target->state_ = state.value();
    return target;
}

core::Expected<std::shared_ptr<OverlaybdTarget>, std::string> OverlaybdTarget::Open(
    const std::string& global_config_path, const std::string& image_config_path) {
    const core::Expected<overlaybd::ImageService, std::string> service =
        overlaybd::ImageService::FromConfigPath(global_config_path);
    if (!service.ok()) {
        return core::make_unexpected("open overlaybd image service failed: " +
                                     global_config_path + ": " + service.error());
    }
    // Non-const: `CreateImageFile` records the outcome in the config's
    // `resultFile`, so it is not a read-only operation on the service.
    overlaybd::ImageService opened = service.value();
    const core::Expected<std::shared_ptr<overlaybd::ImageFile>, std::string> image =
        opened.CreateImageFile(image_config_path);
    if (!image.ok()) {
        return core::make_unexpected("open overlaybd image file failed: " + image_config_path +
                                     ": " + image.error());
    }
    // Rust `let discard_supported = !image.is_read_only().await;`
    const bool discard_supported = !image.value()->IsReadOnly();
    return FromOpenedImage(image_config_path, image.value(), discard_supported);
}

core::Expected<core::Unit, std::string> OverlaybdTarget::SwapState(
    const std::string& image_config_path, std::shared_ptr<overlaybd::ImageFile> image,
    bool discard_supported) {
    const core::Expected<std::shared_ptr<State>, std::string> next =
        MakeState(image_config_path, image, discard_supported);
    // Validated before the swap, so a rejected image leaves the live one
    // serving rather than tearing down a working device.
    if (!next.ok()) return core::make_unexpected(next.error());

    {
        std::lock_guard<std::mutex> guard(state_mutex_);
        state_ = next.value();
    }
    // The recorder is per *image*: its bitmap is sized from the old image's
    // virtual size and its page numbers index the old address space. Carrying
    // it across a swap would append the new image's pages to the old image's
    // trace, which is worse than losing the recording -- the trace would look
    // valid and describe pages the pack never contains.
    //
    // Cleared after the state is live, so no read can be observed against the
    // new image by the old recorder.
    SetRecorder(std::shared_ptr<StartupPackRecorder>());
    return core::Unit();
}

std::shared_ptr<const OverlaybdTarget::State> OverlaybdTarget::LoadState() const {
    std::lock_guard<std::mutex> guard(state_mutex_);
    return state_;
}

void OverlaybdTarget::SetRecorder(std::shared_ptr<StartupPackRecorder> recorder) {
    // Swapped out under the lock and released after it. The outgoing recorder
    // may be the last reference, and its destructor walks a bitmap that can
    // be half a megabyte; holding the lock across that would stall every
    // queue thread's read.
    std::shared_ptr<StartupPackRecorder> previous;
    {
        std::lock_guard<std::mutex> guard(recorder_mutex_);
        previous = recorder_;
        recorder_ = recorder;
    }
}

std::shared_ptr<StartupPackRecorder> OverlaybdTarget::Recorder() const {
    std::lock_guard<std::mutex> guard(recorder_mutex_);
    return recorder_;
}

std::string OverlaybdTarget::ImageConfigPath() const {
    return LoadState()->image_config_path;
}

uint64_t OverlaybdTarget::DevSectors() const { return LoadState()->dev_sectors; }

std::shared_ptr<overlaybd::ImageFile> OverlaybdTarget::Image() const {
    return LoadState()->image;
}

core::Expected<core::Unit, std::string> OverlaybdTarget::Init(uint16_t qid, util::Ring* ring) {
    // Rust's `init` is `Ok(())`. The image is already open and registers
    // nothing per queue; the ring reaches the image through `IoCtx` on each
    // request instead.
    (void)qid;
    (void)ring;
    return core::Unit();
}

core::Optional<std::size_t> OverlaybdTarget::PerSlotExtraBufLen() const {
    // Rust returns `None` — reads and writes use the slot's own buffer.
    return core::Optional<std::size_t>();
}

abi::Params OverlaybdTarget::DevParams(const abi::CtrlDevInfo& dev_info) const {
    const std::shared_ptr<const State> state = LoadState();
    // `ctrl.h`'s `BuildParams` is the port of Rust's `build_ublk_params`.
    return BuildParams(state->dev_sectors, state->logical_bs_shift, state->physical_bs_shift,
                       dev_info.max_io_buf_bytes, state->discard_supported);
}

int32_t OverlaybdTarget::HandleIoRequest(uint16_t qid, uint16_t tag, const abi::IoDesc& desc,
                                         IoBuffer* buf, IoBuffer* extra, util::Ring* ring) {
    // One load per request, as Rust's `load_full` is: the state this request
    // sees stays fixed even if a swap lands while it runs.
    const std::shared_ptr<const State> state = LoadState();
    (void)extra;  // Rust ignores `_extra`; nothing asked for a second buffer.

    // ---- Rust `request_meta` --------------------------------------------
    const uint8_t op = abi::IoDescOp(desc);
    const uint64_t offset = desc.start_sector << state->logical_bs_shift;
    const uint64_t length = static_cast<uint64_t>(desc.nr_sectors) << state->logical_bs_shift;

    // Overflow-checked, as Rust's `checked_add` is. The descriptor comes from
    // the kernel's mapped array and is untrusted input: a wrapped `end` would
    // pass the bounds check below and then read outside the image.
    if (offset > UINT64_MAX - length) {
        AGENTENV_ERROR("invalid overlaybd ublk request qid=" << qid << " tag=" << tag
                       << " start_sector=" << desc.start_sector
                       << " nr_sectors=" << desc.nr_sectors << " (offset overflow)");
        return -EINVAL;
    }
    if (offset + length > state->DevBytes()) {
        AGENTENV_ERROR("invalid overlaybd ublk request qid=" << qid << " tag=" << tag
                       << " offset=" << offset << " length=" << length
                       << " exceeds device bytes " << state->DevBytes());
        return -EINVAL;
    }
    const std::size_t len = static_cast<std::size_t>(length);

    // Submitted on *this* queue's ring. Each queue owns a `single_issuer`
    // ring, so the context has to be threaded through rather than looked up.
    const overlaybd::IoCtx ctx(ring);

    switch (op) {
        case abi::kIoOpRead: {
            if (len == 0) return 0;
            if (!buf || !buf->IsUser()) {
                // Rust: `IOBuffer::AutoReg(_) => bail!("...does not support
                // zero-copy IO buffer")`.
                AGENTENV_ERROR("overlaybd target does not support zero-copy IO buffer"
                               << " qid=" << qid << " tag=" << tag);
                return -EOPNOTSUPP;
            }
            uint8_t* dst = buf->User()->Subslice(0, len);
            if (dst == NULL) return -EINVAL;

            // Startup-pack recorder: observe at submission, which is
            // approximately the guest's fault order, then let the guard settle
            // the completion when this scope ends. Both must bracket the read
            // itself -- the guard's latency measurement is the only input to
            // the remote-bytes estimate.
            //
            // Loaded once into a local `shared_ptr`, so a detach landing
            // mid-read cannot free the recorder this read is still counted by.
            // Costs one mutex acquisition per read when no recording is
            // armed, which is the common case; the alternative, an atomic
            // flag checked first, would be a second source of truth for
            // whether a recorder exists.
            StartupPackReadGuard record_guard;
            const std::shared_ptr<StartupPackRecorder> recorder = Recorder();
            if (recorder) {
                const bool touched_new = recorder->ObserveSubmit(offset, len);
                record_guard.Arm(recorder, static_cast<uint64_t>(len), touched_new);
            }

            const core::Expected<std::size_t, std::string> read =
                state->image->ReadAtIntoWithCtx(ctx, offset, dst, len);
            if (!read.ok()) {
                AGENTENV_ERROR("overlaybd request failed qid=" << qid << " tag=" << tag
                               << " op=read offset=" << offset << " len=" << len << ": "
                               << read.error());
                return OpaqueFailureErrno();
            }
            if (read.value() != len) {
                // A short read is a failure, not a partial success: the guest
                // would treat the untouched tail as valid data.
                AGENTENV_ERROR("overlaybd target short read at offset " << offset << ": expect "
                               << len << ", got " << read.value());
                return -EIO;
            }
            return static_cast<int32_t>(read.value());
        }
        case abi::kIoOpWrite: {
            if (len == 0) return 0;
            if (!buf || !buf->IsUser()) {
                AGENTENV_ERROR("overlaybd target does not support zero-copy IO buffer"
                               << " qid=" << qid << " tag=" << tag);
                return -EOPNOTSUPP;
            }
            const uint8_t* src = buf->User()->Subslice(0, len);
            if (src == NULL) return -EINVAL;
            const core::Expected<std::size_t, std::string> written =
                state->image->WriteAtWithCtx(ctx, offset, src, len);
            if (!written.ok()) {
                AGENTENV_ERROR("overlaybd request failed qid=" << qid << " tag=" << tag
                               << " op=write offset=" << offset << " len=" << len << ": "
                               << written.error());
                return OpaqueFailureErrno();
            }
            if (written.value() != len) {
                AGENTENV_ERROR("overlaybd target short write at offset " << offset << ": expect "
                               << len << ", wrote " << written.value());
                return -EIO;
            }
            return static_cast<int32_t>(written.value());
        }
        case abi::kIoOpFlush: {
            const core::Expected<core::Unit, std::string> synced = state->image->Sync();
            if (!synced.ok()) {
                AGENTENV_ERROR("overlaybd request failed qid=" << qid << " tag=" << tag
                               << " op=flush: " << synced.error());
                return OpaqueFailureErrno();
            }
            return 0;
        }
        case abi::kIoOpDiscard: {
            if (len == 0) return 0;
            if (!state->discard_supported) {
                // Rust raises `ErrorKind::PermissionDenied`, which its errno
                // mapping turns into EROFS. Named directly here, since this
                // port's errors carry no kind to map.
                AGENTENV_ERROR("overlaybd discard on read-only image qid=" << qid
                               << " tag=" << tag);
                return -EROFS;
            }
            const core::Expected<core::Unit, std::string> discarded =
                state->image->Discard(offset, length);
            if (!discarded.ok()) {
                AGENTENV_ERROR("overlaybd request failed qid=" << qid << " tag=" << tag
                               << " op=discard offset=" << offset << " len=" << len << ": "
                               << discarded.error());
                return OpaqueFailureErrno();
            }
            return 0;
        }
        default:
            // Rust bails for WRITE_ZEROES and everything else alike. Its
            // mapping has no arm for a plain `anyhow!`, so it lands on -EIO;
            // EOPNOTSUPP is the honest answer and is what the kernel expects
            // for an op a target does not implement.
            AGENTENV_ERROR("overlaybd target does not support op=" << static_cast<int>(op)
                           << " qid=" << qid << " tag=" << tag);
            return -EOPNOTSUPP;
    }
}

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
