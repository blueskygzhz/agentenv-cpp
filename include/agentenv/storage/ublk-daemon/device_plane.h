// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/server.rs — the half of `UblkDaemonServer`
// that owns ublk devices across requests.
//
// `server.cc` already has the socket half: bind, accept, frame, dispatch, roll
// back an undelivered handoff. Everything it dispatches *to* lives here. Rust
// fuses the two into one struct; the seam is `DeviceHandler`, and this is the
// production implementation of it.
//
// What this holds, and why each piece has to outlive a request:
//
//   * **The device table** (`devices_`). A `CreateOverlaybd` reply hands the
//     client a device id and nothing else. The `UblkDev` — which owns the
//     open `/dev/ublkcN` and the queue worker threads serving it — must stay
//     alive until a later `Delete` names that id, on a different connection
//     and a different thread.
//   * **The active maps** (`active_exclusive_`, `active_shared_`). Same
//     lifetime, different ownership rule: a shared device is refcounted, so
//     the second `AcquireOverlaybd` for one image returns the *same* device
//     id and only the last `ReleaseOverlaybd` tears it down.
//   * **The idle pool** (`idle_`). A released device is not deleted; it is
//     swapped onto a daemon-owned placeholder image and parked. The next
//     acquire reuses it, skipping ADD_DEV/START_DEV entirely. Parking it on
//     the business image instead would pin that image's layers and its page
//     cache for as long as the device sits idle, which is the whole reason
//     `OverlaybdTarget::SwapState` exists.
//
// Two things differ from Rust by necessity rather than by choice, and both are
// load-bearing:
//
//   * **The control ring is serialized by a mutex.** Rust's `IoRingHandle`
//     funnels submissions through a task; `util::Ring` has no internal lock
//     and is documented as single-issuer. Two connection threads submitting a
//     `URING_CMD` to `/dev/ublk-control` at once would interleave in the
//     submission queue. So every control command — ADD_DEV, SET_PARAMS,
//     START_DEV, STOP_DEV, DEL_DEV, UPDATE_SIZE, GET_FEATURES — is taken
//     under `ctrl_mutex_`.
//
//     The consequence is that Rust's bounded-concurrency shutdown
//     (`SHUTDOWN_STOP_CONCURRENCY`) would buy nothing here: the RCU grace
//     period each STOP_DEV waits on is inside the critical section. Shutdown
//     is therefore sequential, and says so.
//
//   * **Lock order is `table_mutex_` then `ctrl_mutex_`, never the reverse.**
//     Device creation takes `ctrl_mutex_` and inserts into the table only
//     after releasing it; teardown removes from the table first and takes
//     `ctrl_mutex_` after. `UpdateSize` is the one path that holds both, in
//     that order, because it must not let the device be released out from
//     under the resize.
#ifndef AGENTENV_STORAGE_UBLK_DAEMON_DEVICE_PLANE_H_
#define AGENTENV_STORAGE_UBLK_DAEMON_DEVICE_PLANE_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/bk_download.h"
#include "agentenv/storage/overlaybd/image_file.h"
#include "agentenv/storage/overlaybd/image_service.h"
#include "agentenv/storage/ublk-daemon/config.h"
#include "agentenv/storage/ublk-daemon/runtime.h"
#include "agentenv/storage/ublk-daemon/server.h"
#include "agentenv/storage/ublk/dev.h"
#include "agentenv/storage/ublk/overlaybd_target.h"
#include "agentenv/storage/util/io_ring.h"
#include "agentenv/warm-pool/pool.h"

namespace agentenv {
namespace storage {
namespace ublk_daemon {

/// What `main` resolved before the device plane could exist.
struct DevicePlaneOptions {
    /// `--global-config`. The default overlaybd global config: it backs the
    /// placeholder images and is the fallback for a request that omits one.
    std::string global_config;
    /// `--resize-global-config`. Only `overlaybd-resize` children read it.
    std::string resize_global_config;
    /// Absent when no resize tool is configured, which makes a request for a
    /// non-base virtual size an invalid request rather than a silent no-op.
    core::Optional<ResizeToolSpec> resize_tool;
    /// Absent disables the warm pool. `HasPool` reports this truthfully
    /// because the server's rollback path picks `ReleaseOverlaybd` versus
    /// `Delete` from it, and the two are not interchangeable.
    core::Optional<warmpool::PoolConfig> pool_config;
    core::Optional<std::string> p2p_publish_url;
    /// Where the daemon's same-size sparse placeholder images are built.
    /// Daemon-owned, never the image cache: an idle device binds one of these
    /// so the pool stops pinning the business image it just served.
    std::string placeholder_dir;
};

/// Rust `UblkDaemonServer`'s device half.
class DevicePlane : public DeviceHandler {
 public:
    /// Opens the control ring, detects the driver's features, and builds the
    /// plane around them.
    ///
    /// Fails when there is no usable `/dev/ublk-control` or the kernel cannot
    /// make an `sqe128` ring. That is the one startup condition worth
    /// reporting up front: every device request would fail anyway, and naming
    /// the host problem once beats naming it once per sandbox.
    static core::Expected<std::unique_ptr<DevicePlane>, std::string> Open(
        const DevicePlaneOptions& options);

    /// Rust `UblkDaemonServer::new`, which takes its `ctrl_ring` as an
    /// argument rather than opening one.
    ///
    /// Kept as the real entry point, with `Open` as the wrapper that supplies
    /// the ring, for the reason upstream injects it: every decision the plane
    /// makes *before* it reaches a device -- which requests need a pool, which
    /// device ids it knows, which request families are refused -- is otherwise
    /// unreachable on a host without ublk, which is exactly where getting one
    /// wrong is hardest to notice.
    ///
    /// `ctrl_ring` may be null. A plane built that way serves every lookup and
    /// classification path and refuses the ones that would submit a control
    /// command, instead of dereferencing nothing.
    static std::unique_ptr<DevicePlane> WithControlRing(
        const DevicePlaneOptions& options, std::shared_ptr<util::Ring> ctrl_ring,
        uint64_t features);

    ~DevicePlane();

    // ---- DeviceHandler ---------------------------------------------------

    core::Expected<DaemonResponse, HandlerError> Handle(const DaemonRequest& request);
    bool HasPool() const;
    void StopAllDevices();

    /// The driver feature bitmap read at startup, for logging.
    uint64_t features() const { return features_; }

 private:
    /// Rust `ManagedDevice` — a device created by `CreateOverlaybd` or
    /// `CreateOverlaybdRuntimeDevice` with the pool off. Not refcounted and
    /// not poolable: it exists until `Delete`.
    struct ManagedDevice {
        std::unique_ptr<ublk::UblkDev> dev;
        std::shared_ptr<ublk::OverlaybdTarget> target;
        std::shared_ptr<overlaybd::ImageFile> image;
        std::string image_config;

        ManagedDevice() {}
        ManagedDevice(ManagedDevice&& o)
            : dev(std::move(o.dev)),
              target(std::move(o.target)),
              image(std::move(o.image)),
              image_config(std::move(o.image_config)) {}
        ManagedDevice& operator=(ManagedDevice&& o) {
            dev = std::move(o.dev);
            target = std::move(o.target);
            image = std::move(o.image);
            image_config = std::move(o.image_config);
            return *this;
        }

     private:
        ManagedDevice(const ManagedDevice&);
        ManagedDevice& operator=(const ManagedDevice&);
    };

    /// Rust `ActiveExclusive` / `ActiveShared`, merged.
    ///
    /// One struct for both because they differ only in `refcount`, which is
    /// fixed at 1 for an exclusive device. Keeping two types would mean two
    /// copies of every teardown path, and the maps already carry the
    /// distinction: one is keyed by device id, the other by image identity.
    struct ActiveDevice {
        std::unique_ptr<ublk::UblkDev> dev;
        std::shared_ptr<ublk::OverlaybdTarget> target;
        std::shared_ptr<overlaybd::ImageFile> image;
        std::string image_config;
        std::string global_config;
        std::size_t refcount;

        ActiveDevice() : refcount(0) {}
        ActiveDevice(ActiveDevice&& o)
            : dev(std::move(o.dev)),
              target(std::move(o.target)),
              image(std::move(o.image)),
              image_config(std::move(o.image_config)),
              global_config(std::move(o.global_config)),
              refcount(o.refcount) {}
        ActiveDevice& operator=(ActiveDevice&& o) {
            dev = std::move(o.dev);
            target = std::move(o.target);
            image = std::move(o.image);
            image_config = std::move(o.image_config);
            global_config = std::move(o.global_config);
            refcount = o.refcount;
            return *this;
        }

     private:
        ActiveDevice(const ActiveDevice&);
        ActiveDevice& operator=(const ActiveDevice&);
    };

    /// Rust `PooledDevice` — a warm device parked on a placeholder.
    ///
    /// `dev_sectors` is the *kernel's* current size for this device, which is
    /// the placeholder's. Without `UBLK_F_UPDATE_SIZE` a reuse must match it
    /// exactly, so the pool is searched by this value rather than taken from
    /// the front.
    struct PooledDevice {
        std::unique_ptr<ublk::UblkDev> dev;
        std::shared_ptr<ublk::OverlaybdTarget> target;
        /// Kept alive only so the parked device's target has something to
        /// serve. Never read by the pool itself.
        std::shared_ptr<overlaybd::ImageFile> placeholder_image;
        uint64_t dev_sectors;

        PooledDevice() : dev_sectors(0) {}
        PooledDevice(PooledDevice&& o)
            : dev(std::move(o.dev)),
              target(std::move(o.target)),
              placeholder_image(std::move(o.placeholder_image)),
              dev_sectors(o.dev_sectors) {}
        PooledDevice& operator=(PooledDevice&& o) {
            dev = std::move(o.dev);
            target = std::move(o.target);
            placeholder_image = std::move(o.placeholder_image);
            dev_sectors = o.dev_sectors;
            return *this;
        }

     private:
        PooledDevice(const PooledDevice&);
        PooledDevice& operator=(const PooledDevice&);
    };

    /// Rust `SharedKey` — `(image_config, global_config)`.
    ///
    /// Both halves, not just the image: the same `image.json` opened through
    /// two different global configs has two different caches and two
    /// different io pools, so one device cannot serve both.
    typedef std::pair<std::string, std::string> SharedKey;

    /// A device plus everything that has to be released with it, moved out of
    /// a map before the long teardown runs so no lock is held across it.
    struct DetachedDevice {
        std::unique_ptr<ublk::UblkDev> dev;
        std::shared_ptr<ublk::OverlaybdTarget> target;
        std::shared_ptr<overlaybd::ImageFile> image;
        uint32_t dev_id;

        DetachedDevice() : dev_id(0) {}
    };

    explicit DevicePlane(const DevicePlaneOptions& options);
    DevicePlane(const DevicePlane&);
    DevicePlane& operator=(const DevicePlane&);

    // ---- request handlers ------------------------------------------------

    core::Expected<DaemonResponse, HandlerError> HandleGetFeatures();
    core::Expected<DaemonResponse, HandlerError> HandleNotifySandboxReady(
        const DaemonRequest& request);
    core::Expected<DaemonResponse, HandlerError> HandleCreateRawOverlaybd(
        const DaemonRequest& request);
    core::Expected<DaemonResponse, HandlerError> HandleCreateRuntimeDevice(
        const DaemonRequest& request);
    core::Expected<DaemonResponse, HandlerError> HandleDelete(const DaemonRequest& request);
    core::Expected<DaemonResponse, HandlerError> HandleRestackSnapshot(
        const DaemonRequest& request);
    core::Expected<DaemonResponse, HandlerError> HandleAcquireOverlaybd(
        const std::string& image_config, const std::string& global_config,
        const core::Optional<uint64_t>& virtual_size, AccessMode access_mode);
    core::Expected<DaemonResponse, HandlerError> HandleReleaseOverlaybd(uint32_t dev_id);
    core::Expected<DaemonResponse, HandlerError> HandleUpdateSize(uint32_t dev_id,
                                                                  uint64_t new_sectors);
    core::Expected<DaemonResponse, HandlerError> HandlePrefetchStartupPack(
        const DaemonRequest& request);

    // ---- device lifecycle ------------------------------------------------

    /// Rust `create_new_device` — target, ADD_DEV, START_DEV, wait for the
    /// node. Cleans up the half-built device on any failure.
    core::Expected<core::Unit, std::string> CreateNewDevice(
        const std::string& image_config,
        const std::shared_ptr<overlaybd::ImageFile>& image,
        std::unique_ptr<ublk::UblkDev>* out_dev,
        std::shared_ptr<ublk::OverlaybdTarget>* out_target);

    /// Rust `prepare_overlaybd_device` — reuse a warm device or create one.
    ///
    /// `*out_reused` tells the caller which happened, because the two are
    /// worth distinguishing in the log: a pool miss means the next acquire
    /// paid for ADD_DEV/START_DEV.
    core::Expected<core::Unit, std::string> PrepareOverlaybdDevice(
        const std::string& image_config,
        const std::shared_ptr<overlaybd::ImageFile>& image, const char* mode,
        std::unique_ptr<ublk::UblkDev>* out_dev,
        std::shared_ptr<ublk::OverlaybdTarget>* out_target, bool* out_reused);

    /// Rust `quiesce_ublk_device` + `drop(dev)` + `delete_dev`.
    ///
    /// Takes the device by value: the open `/dev/ublkcN` has to be gone
    /// before DEL_DEV, or the kernel blocks waiting for the reference this
    /// very call is holding.
    void StopAndDeleteDevice(DetachedDevice detached);

    /// Rust `cleanup_failed_ublk_start`.
    void CleanupFailedStart(std::unique_ptr<ublk::UblkDev> dev, uint32_t dev_id);

    /// Rust `delete_dev` — STOP_DEV then DEL_DEV through a fresh control
    /// channel, with `ENODEV` from the stop treated as "already gone".
    core::Expected<core::Unit, std::string> DeleteDevById(uint32_t dev_id);

    /// Rust `idle_released_device` — park a released device on a placeholder,
    /// or delete it when that cannot be done.
    void IdleReleasedDevice(DetachedDevice detached);

    // ---- pool ------------------------------------------------------------

    bool SupportsUpdateSize() const;
    /// Rust `take_idle_device`.
    bool TakeIdleDevice(uint64_t dev_sectors, PooledDevice* out);
    /// Rust `placeholder_for` — the daemon-owned same-size sparse image for
    /// `virtual_size`, built once per size.
    core::Expected<core::Unit, std::string> PlaceholderFor(
        uint64_t virtual_size, std::string* out_config,
        std::shared_ptr<overlaybd::ImageFile>* out_image);
    /// Rust `schedule_idle_pool_refill` — coalesced background prewarm.
    void ScheduleIdlePoolRefill(uint64_t virtual_size);
    void RefillLoop(uint64_t virtual_size);
    core::Expected<core::Unit, std::string> RefillIdlePoolOnce(uint64_t virtual_size);

    // ---- helpers ---------------------------------------------------------

    /// Rust `PoolState::image_lock`.
    ///
    /// Rust uses an `RwLock` so concurrent opens of one image do not
    /// serialize; C++11 has no reader/writer mutex, so this is exclusive.
    /// The safety property is the one that matters and it is unchanged: a
    /// restack, which mutates the live layer stack, cannot run while the same
    /// image is being opened. Unrelated images are still independent.
    std::shared_ptr<std::mutex> ImageLock(const std::string& image_config);

    /// Opens an image through the cache, under its image lock.
    core::Expected<std::shared_ptr<overlaybd::ImageFile>, std::string> OpenImage(
        const std::string& image_config, const std::string& global_config);

    /// The global config a request should be served with — its own, or the
    /// daemon's default when it named none.
    std::string EffectiveGlobalConfig(const std::string& requested) const;

    DevicePlaneOptions options_;
    uint64_t features_;

    /// The one `/dev/ublk-control` ring, and the mutex that makes it safe to
    /// submit on from a connection thread. See the file header.
    std::shared_ptr<util::Ring> ctrl_ring_;
    mutable std::mutex ctrl_mutex_;

    std::unique_ptr<ImageServiceCache> image_service_cache_;

    /// Serializes the `overlaybd-resize` children, which all share the one
    /// isolated resize cacheDir.
    std::mutex resize_permit_;

    mutable std::mutex table_mutex_;
    std::map<uint32_t, ManagedDevice> devices_;
    std::map<uint32_t, ActiveDevice> active_exclusive_;
    std::map<SharedKey, ActiveDevice> active_shared_;
    std::map<uint32_t, SharedKey> shared_by_dev_id_;

    std::mutex image_locks_mutex_;
    std::map<std::string, std::shared_ptr<std::mutex> > image_locks_;

    std::mutex placeholder_mutex_;
    std::map<uint64_t, std::pair<std::string, std::shared_ptr<overlaybd::ImageFile> > >
        placeholders_;

    /// Null when the pool is disabled, which is also what `HasPool` reports.
    std::unique_ptr<warmpool::WarmPool<PooledDevice> > idle_;

    std::atomic<bool> refill_inflight_;
    std::atomic<bool> stopping_;
    std::mutex refill_mutex_;
    std::thread refill_thread_;

    std::mutex startup_pack_mutex_;
    std::vector<overlaybd::StartupPackHandle> startup_pack_handles_;
};

/// The `sqe128` io_uring every ublk control command is submitted on.
///
/// Separate from `DevicePlane` because it is the half that needs the kernel,
/// and because the probing is worth naming: `coop_taskrun` and
/// `single_issuer` landed across several releases and a kernel without one
/// answers `EINVAL`, which would otherwise read as "this host has no ublk".
core::Expected<std::shared_ptr<util::Ring>, std::string> OpenControlRing();

/// Rust `detect_ublk_features` — GET_FEATURES, with an old driver's refusal
/// read as "no features" rather than as a failure.
///
/// A driver that predates the command answers `ENOTTY`/`EINVAL`/`EOPNOTSUPP`,
/// which is not the same as a broken control plane: every feature is
/// optional, and the only cost of assuming none is that the pool loses
/// cross-size device reuse.
core::Expected<uint64_t, std::string> DetectUblkFeatures(
    const std::shared_ptr<util::Ring>& ctrl_ring);

/// Rust `wait_for_ublk_dev` — wait for `/dev/ublkbN` to become openable.
///
/// Exposed because the schedule is the part worth asserting: the node is
/// created by udev *after* START_DEV answers, and the permissions it ends up
/// with are applied later still. Rust backs off 10/100/500/1000 ms for
/// 5/10/10/24 tries — about 30 seconds — and treats `EACCES` as "not ready"
/// rather than as a failure for exactly that reason.
core::Expected<core::Unit, std::string> WaitForUblkDev(uint32_t dev_id);

/// Rust `clear_page_cache` — `BLKFLSBUF` on a block device, best effort.
///
/// Done when a device is returned to the pool, not when it is handed out: the
/// page cache belongs to the image that was being served, and leaving it
/// behind would let the next tenant read the previous tenant's blocks through
/// a device that is now pointing somewhere else.
void ClearPageCache(const std::string& device_path);

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_DAEMON_DEVICE_PLANE_H_
