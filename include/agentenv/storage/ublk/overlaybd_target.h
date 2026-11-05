// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/impls/overlaybd_target.rs
#ifndef AGENTENV_STORAGE_UBLK_OVERLAYBD_TARGET_H_
#define AGENTENV_STORAGE_UBLK_OVERLAYBD_TARGET_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/image_file.h"
#include "agentenv/storage/overlaybd/image_service.h"
#include "agentenv/storage/ublk/ctrl.h"
#include "agentenv/storage/ublk/dev.h"

namespace agentenv {
namespace storage {
namespace ublk {

/// Rust `validate_block_size_shift` — the log2 of a power-of-two block size.
///
/// Exposed for the tests that cover it directly, as upstream's are.
core::Expected<uint8_t, std::string> ValidateBlockSizeShift(uint32_t block_size);

// Rust's `build_ublk_params` and `DEFAULT_PHYSICAL_BS_SHIFT` are not repeated
// here: `ctrl.h` already has them as `BuildParams` and
// `kDefaultPhysicalBsShift`, with the same geometry and the same
// `max(io_opt, physical)` rule. Upstream keeps a private copy per target;
// duplicating it would be two places to fix when the kernel's struct changes.

/// Rust `struct OverlaybdTarget` — serves a ublk device from an overlaybd
/// LSMT stack.
///
/// This is the real data plane. `impls.h`'s `MakeOverlaybdTarget` serves
/// zeroes and refuses writes against the superseded `Target` interface; this
/// implements `UblkTarget` (Rust `UVMUblkTarget`) and reads and writes the
/// actual image.
///
/// **Concurrency.** Every queue thread calls `HandleIoRequest` on one shared
/// instance. Upstream holds its state in an `ArcSwap` and takes a `load_full`
/// per request; C++11 has no atomic shared_ptr, so `LoadState` copies the
/// handle under a short mutex and the request then runs entirely outside the
/// lock. The effect is what matters and it is the same: a request that already
/// loaded a state keeps using it, so a concurrent `SwapState` cannot pull the
/// image out from under an in-flight read.
class OverlaybdTarget : public UblkTarget {
 public:
    /// Rust `open` — opens the image service from the global config, then the
    /// image from the image config.
    ///
    /// `discard_supported` is derived, not passed: upstream sets it from
    /// `!image.is_read_only()`, because advertising discard on a read-only
    /// image would have the guest issue TRIMs that can only ever fail.
    static core::Expected<std::shared_ptr<OverlaybdTarget>, std::string> Open(
        const std::string& global_config_path, const std::string& image_config_path);

    /// Rust `from_opened_image` — for a caller that already has the image,
    /// which is how the daemon's warm pool and the tests build one.
    static core::Expected<std::shared_ptr<OverlaybdTarget>, std::string> FromOpenedImage(
        const std::string& image_config_path,
        std::shared_ptr<overlaybd::ImageFile> image, bool discard_supported);

    /// Rust `swap_state` — replace the backing image and capacity atomically.
    ///
    /// Used by the daemon warm pool to reuse a ublk device across images
    /// without tearing the device down.
    ///
    /// Precondition: the device must be idle. Nothing here enforces that, as
    /// nothing upstream does either -- the pool owns that ordering, and a
    /// check here could not see in-flight requests anyway.
    core::Expected<core::Unit, std::string> SwapState(
        const std::string& image_config_path,
        std::shared_ptr<overlaybd::ImageFile> image, bool discard_supported);

    /// The config path of the live state. For logging and tests.
    std::string ImageConfigPath() const;
    /// The live capacity, in 512-byte-or-larger logical sectors.
    uint64_t DevSectors() const;
    /// The live image handle, as `swap_state`'s tests need to read it back.
    std::shared_ptr<overlaybd::ImageFile> Image() const;

    // ---- UblkTarget ------------------------------------------------------

    /// Rust `const DEV_NAME: &str = "overlaybd"`.
    std::string DevName() const override { return "overlaybd"; }

    /// Rust's `init` is `Ok(())` — the image is already open, and there is
    /// nothing to set up per queue.
    core::Expected<core::Unit, std::string> Init(uint16_t qid, util::Ring* ring) override;

    /// Rust returns `None`: reads and writes go straight through the slot's
    /// own buffer, so no scratch space is needed.
    core::Optional<std::size_t> PerSlotExtraBufLen() const override;

    abi::Params DevParams(const abi::CtrlDevInfo& dev_info) const override;

    int32_t HandleIoRequest(uint16_t qid, uint16_t tag, const abi::IoDesc& desc,
                            IoBuffer* buf, IoBuffer* extra, util::Ring* ring) override;

 private:
    /// Rust `struct TargetState` — an immutable snapshot, replaced wholesale.
    struct State {
        std::string image_config_path;
        std::shared_ptr<overlaybd::ImageFile> image;
        uint64_t dev_sectors = 0;
        uint8_t logical_bs_shift = 0;
        uint8_t physical_bs_shift = 0;
        bool discard_supported = false;

        /// Rust `TargetState::dev_bytes`.
        uint64_t DevBytes() const { return dev_sectors << logical_bs_shift; }
    };

    OverlaybdTarget() {}

    /// Builds the snapshot both constructors and `SwapState` share, so the
    /// zero-capacity and block-size checks cannot be applied in one path and
    /// skipped in another.
    static core::Expected<std::shared_ptr<State>, std::string> MakeState(
        const std::string& image_config_path,
        std::shared_ptr<overlaybd::ImageFile> image, bool discard_supported);

    /// Rust `self.state.load_full()`.
    std::shared_ptr<const State> LoadState() const;

    mutable std::mutex state_mutex_;
    std::shared_ptr<const State> state_;
};

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_OVERLAYBD_TARGET_H_
