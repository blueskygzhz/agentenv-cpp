// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/server.rs — the device plane.
#include "agentenv/storage/ublk-daemon/device_plane.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <system_error>

#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/download_gate.h"
#include "agentenv/storage/overlaybd/image_helper.h"
#include "agentenv/storage/overlaybd/startup_pack.h"
#include "agentenv/storage/ublk/caps.h"
#include "agentenv/storage/ublk/ctrl.h"

namespace agentenv {
namespace storage {
namespace ublk_daemon {

namespace {

/// Rust `NewDeviceOptions::ctrl_builder`'s `name("overlaybd-blk")`.
///
/// The kernel stores it with the device and it is what `ublk list` shows, so
/// it is the only way an operator can tell this daemon's devices from another
/// ublk server's on the same host.
const char kDeviceName[] = "overlaybd-blk";

/// `BLKFLSBUF`. Linux encodes `_IO(0x12, 97)` as `(0x12 << 8) | 97`.
const unsigned long kBlkFlsBuf = 0x1261;

/// How long a stop waits for the queue workers before giving up on them.
///
/// Rust wraps `wait_for_bg_tasks` in a 5 s timeout. `UblkDev::WaitForQueueWorkers`
/// has no timeout, and cannot grow one without a cancellation path into the
/// worker loop. The wait is bounded in practice by STOP_DEV: the kernel fails
/// the workers' pending `FETCH_REQ`s, which is what makes them return.
///
/// Named so the divergence is visible rather than implied by its absence.
const bool kQueueWorkerJoinIsUnbounded = true;

/// `errno` as `Errno()` in `ctrl.cc` renders it. Matching the numeric tail
/// rather than `strerror`'s text, which is locale-dependent.
bool IsErrno(const std::string& error, int err) {
    std::ostringstream marker;
    marker << "(errno " << err << ")";
    return error.find(marker.str()) != std::string::npos;
}

/// Rust `MaterializedOverlaybdRuntime`'s failure context, as one string.
std::string WithContext(const std::string& context, const std::string& error) {
    return context + ": " + error;
}

core::Json DescriptorToJson(const overlaybd::LayerDescriptor& descriptor) {
    core::Json out;
    out["digest"] = core::Json(descriptor.digest);
    out["size"] = core::Json(static_cast<int64_t>(descriptor.size));
    return out;
}

core::Json DataStatToJson(const overlaybd::DataStat& stat) {
    core::Json out;
    out["total_data_size"] = core::Json(static_cast<int64_t>(stat.total_data_size));
    out["valid_data_size"] = core::Json(static_cast<int64_t>(stat.valid_data_size));
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> WaitForUblkDev(uint32_t dev_id) {
    std::ostringstream path_os;
    path_os << "/dev/ublkb" << dev_id;
    const std::string path = path_os.str();

    // Rust's exact schedule: ~30 s total, front-loaded because the common
    // case is that udev is only microseconds behind START_DEV.
    static const int kIntervalMs[4] = {10, 100, 500, 1000};
    static const int kRetries[4] = {5, 10, 10, 24};

    bool warned_permission = false;
    for (int band = 0; band < 4; ++band) {
        for (int attempt = 0; attempt < kRetries[band]; ++attempt) {
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                ::close(fd);
                return core::Unit();
            }
            const int err = errno;
            if (err == EACCES) {
                // Not a failure: udev applies the rules that grant access
                // after it creates the node, so this is the same "not ready
                // yet" as ENOENT.
                if (!warned_permission) {
                    AGENTENV_WARN("ublk device is not readable yet; udev rules may still be "
                                  "applying: " << path);
                    warned_permission = true;
                }
            } else if (err != ENOENT) {
                std::ostringstream why;
                why << "open " << path << ": " << std::strerror(err) << " (errno " << err
                    << ")";
                return core::make_unexpected(why.str());
            }
            ::usleep(static_cast<useconds_t>(kIntervalMs[band]) * 1000);
        }
    }

    std::ostringstream why;
    why << "ublk device " << dev_id << " did not become accessible at " << path;
    return core::make_unexpected(why.str());
}

void ClearPageCache(const std::string& device_path) {
    // O_NONBLOCK so a device whose queues are wedged cannot make the open
    // itself hang; this is a cache hint, not a correctness step.
    const int fd = ::open(device_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        AGENTENV_WARN("failed to open device for cache clear: " << device_path << ": "
                                                                << std::strerror(errno));
        return;
    }
    const int rc = ::ioctl(fd, kBlkFlsBuf);
    const int err = errno;
    ::close(fd);
    if (rc < 0) {
        AGENTENV_WARN("failed to clear page cache: " << device_path << ": "
                                                     << std::strerror(err));
    } else {
        AGENTENV_DEBUG("cleared page cache: " << device_path);
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DevicePlane::DevicePlane(const DevicePlaneOptions& options)
    : options_(options), features_(0), refill_inflight_(false), stopping_(false) {}

DevicePlane::~DevicePlane() {
    // Idempotent: `main` calls it too, through the server's shutdown path.
    StopAllDevices();
}

core::Expected<std::shared_ptr<util::Ring>, std::string> OpenControlRing() {
    // `single_issuer` is cleared unconditionally rather than as a fallback:
    // connection threads all submit here, and the flag is a promise to the
    // kernel that only one ever does. `ctrl_mutex_` serializes them, but the
    // kernel's fast paths take the flag at its word.
    util::IoRingConfig config;
    config.sq_entries = 32;
    config.cq_entries = 64;
    config.sqe128 = true;
    config.single_issuer = false;

    core::Expected<std::unique_ptr<util::Ring>, std::string> ring = util::Ring::Open(config);
    if (!ring.ok()) {
        config.coop_taskrun = false;
        ring = util::Ring::Open(config);
    }
    if (!ring.ok()) {
        config.cq_entries = 0;
        ring = util::Ring::Open(config);
    }
    if (!ring.ok()) {
        return core::make_unexpected(
            WithContext("open an sqe128 io_uring for the ublk control plane", ring.error()));
    }
    return std::shared_ptr<util::Ring>(ring.take_value().release());
}

core::Expected<uint64_t, std::string> DetectUblkFeatures(
    const std::shared_ptr<util::Ring>& ctrl_ring) {
    // GET_FEATURES creates no device, so the fixed id is only a command field.
    ublk::UblkCtrlBuilder builder;
    builder.Name(kDeviceName).DevId(0);
    core::Expected<std::unique_ptr<ublk::UblkCtrl>, std::string> ctrl =
        builder.Build(ctrl_ring);
    if (!ctrl.ok()) {
        return core::make_unexpected(
            WithContext("build ctrl for feature detection", ctrl.error()));
    }

    const core::Expected<uint64_t, std::string> detected = ctrl.value()->GetFeatures();
    if (detected.ok()) return detected.value();

    if (IsErrno(detected.error(), ENOTTY) || IsErrno(detected.error(), EINVAL) ||
        IsErrno(detected.error(), EOPNOTSUPP)) {
        AGENTENV_WARN("ublk feature query unsupported; continuing without dynamic resize "
                      "support: " << detected.error());
        return static_cast<uint64_t>(0);
    }
    return core::make_unexpected(WithContext("get ublk features", detected.error()));
}

std::unique_ptr<DevicePlane> DevicePlane::WithControlRing(
    const DevicePlaneOptions& options, std::shared_ptr<util::Ring> ctrl_ring,
    uint64_t features) {
    std::unique_ptr<DevicePlane> plane(new DevicePlane(options));
    plane->ctrl_ring_ = ctrl_ring;
    plane->features_ = features;
    plane->image_service_cache_.reset(new ImageServiceCache(options.p2p_publish_url));
    if (options.pool_config.has_value()) {
        plane->idle_.reset(new warmpool::WarmPool<PooledDevice>(*options.pool_config));
    }
    return plane;
}

core::Expected<std::unique_ptr<DevicePlane>, std::string> DevicePlane::Open(
    const DevicePlaneOptions& options) {
    const core::Expected<std::shared_ptr<util::Ring>, std::string> ring = OpenControlRing();
    if (!ring.ok()) return core::make_unexpected(ring.error());

    const core::Expected<uint64_t, std::string> features = DetectUblkFeatures(ring.value());
    if (!features.ok()) return core::make_unexpected(features.error());

    return WithControlRing(options, ring.value(), features.value());
}

bool DevicePlane::HasPool() const { return static_cast<bool>(idle_); }

bool DevicePlane::SupportsUpdateSize() const {
    return (features_ & ublk::kUblkFUpdateSize) != 0;
}

std::string DevicePlane::EffectiveGlobalConfig(const std::string& requested) const {
    return requested.empty() ? options_.global_config : requested;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

core::Expected<DaemonResponse, HandlerError> DevicePlane::Handle(
    const DaemonRequest& request) {
    switch (request.kind) {
        case DaemonRequest::Kind::GetFeatures:
            return HandleGetFeatures();
        case DaemonRequest::Kind::NotifySandboxReady:
            return HandleNotifySandboxReady(request);
        case DaemonRequest::Kind::CreateOverlaybd:
            return HandleCreateRawOverlaybd(request);
        case DaemonRequest::Kind::CreateOverlaybdRuntimeDevice:
            return HandleCreateRuntimeDevice(request);
        case DaemonRequest::Kind::Delete:
            return HandleDelete(request);
        case DaemonRequest::Kind::RestackSnapshot:
            return HandleRestackSnapshot(request);
        case DaemonRequest::Kind::AcquireOverlaybd:
            return HandleAcquireOverlaybd(request.image_config,
                                          EffectiveGlobalConfig(request.global_config),
                                          request.virtual_size, request.access_mode);
        case DaemonRequest::Kind::ReleaseOverlaybd:
            return HandleReleaseOverlaybd(request.dev_id);
        case DaemonRequest::Kind::UpdateSize:
            return HandleUpdateSize(request.dev_id, request.new_sectors);
        case DaemonRequest::Kind::PrefetchStartupPack:
            return HandlePrefetchStartupPack(request);

        case DaemonRequest::Kind::StartPackRecording:
            return HandleStartPackRecording(request);
        case DaemonRequest::Kind::PackRecordingStatus:
            return HandlePackRecordingStatus(request.dev_id);
        case DaemonRequest::Kind::AbortPackRecording:
            return HandleAbortPackRecording(request.dev_id);

        case DaemonRequest::Kind::Shutdown:
            // `ServeConnection` answers this itself; reaching here means the
            // dispatch there changed.
            return core::make_unexpected(
                HandlerError::Internal("shutdown must be handled by the connection loop"));
    }
    return core::make_unexpected(HandlerError::InvalidRequest("unknown request kind"));
}

core::Expected<DaemonResponse, HandlerError> DevicePlane::HandleGetFeatures() {
    DaemonResponse out;
    out.status = DaemonResponse::Status::Features;
    // Rust reports the features it detected only when the pool is on
    // (`pool_state.map(|p| p.features).unwrap_or(0)`). Kept as-is: the one
    // caller reads the bitmap to decide whether it may ask for `UpdateSize`,
    // and `UpdateSize` is a pool-only operation on both sides.
    out.flags = idle_ ? features_ : 0;
    return out;
}

core::Expected<DaemonResponse, HandlerError> DevicePlane::HandleNotifySandboxReady(
    const DaemonRequest& request) {
    AGENTENV_INFO("sandbox reported envd-ready; releasing held downloads: "
                  << request.device_key);
    overlaybd::DownloadGate::NotifySandboxReady(request.device_key);
    DaemonResponse out;
    out.status = DaemonResponse::Status::Ok;
    return out;
}

// ---------------------------------------------------------------------------
// Image opening
// ---------------------------------------------------------------------------

std::shared_ptr<std::mutex> DevicePlane::ImageLock(const std::string& image_config) {
    // Canonicalized when possible, matching `ImageServiceCache`: two
    // spellings of one path must share a lock, or a restack through one would
    // not be excluded from an open through the other. A path that does not
    // exist yet falls back to its raw form, which is the same compromise Rust
    // documents -- aliases of a missing file are not serialized until it
    // exists, and nothing can be restacking a file that is not there.
    std::string key = image_config;
    char resolved[PATH_MAX];
    if (::realpath(image_config.c_str(), resolved) != NULL) {
        key.assign(resolved);
    }

    std::lock_guard<std::mutex> guard(image_locks_mutex_);
    std::map<std::string, std::shared_ptr<std::mutex> >::iterator it = image_locks_.find(key);
    if (it != image_locks_.end()) return it->second;
    std::shared_ptr<std::mutex> created(new std::mutex());
    image_locks_[key] = created;
    return created;
}

core::Expected<std::shared_ptr<overlaybd::ImageFile>, std::string> DevicePlane::OpenImage(
    const std::string& image_config, const std::string& global_config) {
    core::Expected<overlaybd::ImageService, std::string> service =
        image_service_cache_->GetOrCreate(global_config);
    if (!service.ok()) {
        return core::make_unexpected(
            WithContext("resolve image service for " + global_config, service.error()));
    }

    const std::shared_ptr<std::mutex> image_lock = ImageLock(image_config);
    std::lock_guard<std::mutex> guard(*image_lock);
    core::Expected<std::shared_ptr<overlaybd::ImageFile>, std::string> image =
        service.value().CreateImageFile(image_config);
    if (!image.ok()) {
        return core::make_unexpected(
            WithContext("open overlaybd image: " + image_config, image.error()));
    }
    return image.value();
}

// ---------------------------------------------------------------------------
// Device lifecycle
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> DevicePlane::CreateNewDevice(
    const std::string& image_config, const std::shared_ptr<overlaybd::ImageFile>& image,
    std::unique_ptr<ublk::UblkDev>* out_dev,
    std::shared_ptr<ublk::OverlaybdTarget>* out_target) {
    // The single gate for every device creation, so one check covers the raw
    // create, the pool miss and the prewarm. A plane built without a ring --
    // see `WithControlRing` -- can answer lookups but cannot add a device.
    if (!ctrl_ring_) {
        return core::make_unexpected(
            std::string("no ublk control ring: this device plane cannot create devices"));
    }

    // Derived, not configured: advertising discard on a read-only image would
    // have the guest issue TRIMs that can only ever fail.
    const bool discard_supported = !image->IsReadOnly();
    core::Expected<std::shared_ptr<ublk::OverlaybdTarget>, std::string> target =
        ublk::OverlaybdTarget::FromOpenedImage(image_config, image, discard_supported);
    if (!target.ok()) {
        return core::make_unexpected(WithContext("create overlaybd target", target.error()));
    }

    std::unique_ptr<ublk::UblkDev> dev;
    uint32_t dev_id = 0;
    {
        std::lock_guard<std::mutex> ring_guard(ctrl_mutex_);

        ublk::UblkCtrlBuilder builder;
        builder.Name(kDeviceName);
        core::Expected<std::unique_ptr<ublk::UblkCtrl>, std::string> ctrl =
            builder.Build(ctrl_ring_);
        if (!ctrl.ok()) {
            return core::make_unexpected(WithContext("build ublk ctrl", ctrl.error()));
        }

        ublk::UblkDevBuilder dev_builder(ctrl.take_value());
        dev_builder.SetTarget(target.value());
        core::Expected<std::unique_ptr<ublk::UblkDev>, std::string> built = dev_builder.Build();
        if (!built.ok()) {
            return core::make_unexpected(WithContext("build ublk dev", built.error()));
        }
        dev = built.take_value();
        // Read after ADD_DEV: with no id requested the kernel assigns one and
        // writes it back, so this is the first point it is knowable.
        dev_id = dev->DevId();

        const core::Expected<core::Unit, std::string> started = dev->Start();
        if (!started.ok()) {
            const std::string why = WithContext("start ublk dev", started.error());
            // Still under `ctrl_mutex_`: the cleanup is itself a sequence of
            // control commands, and dropping the lock between the failure and
            // the cleanup would let another thread's ADD_DEV interleave with
            // a device that is half up.
            CleanupFailedStart(std::move(dev), dev_id);
            return core::make_unexpected(why);
        }
    }

    // Outside the control lock: this waits on udev, not on the kernel's ublk
    // control path, and it can take seconds on a busy host.
    const core::Expected<core::Unit, std::string> ready = WaitForUblkDev(dev_id);
    if (!ready.ok()) {
        const std::string why = WithContext("wait for ublk device", ready.error());
        std::lock_guard<std::mutex> ring_guard(ctrl_mutex_);
        CleanupFailedStart(std::move(dev), dev_id);
        return core::make_unexpected(why);
    }

    AGENTENV_DEBUG("created new ublk device dev_id=" << dev_id << " path="
                                                      << dev->DevicePath());
    *out_dev = std::move(dev);
    *out_target = target.value();
    return core::Unit();
}

void DevicePlane::CleanupFailedStart(std::unique_ptr<ublk::UblkDev> dev, uint32_t dev_id) {
    // Caller holds `ctrl_mutex_`.
    if (dev) {
        const core::Expected<core::Unit, std::string> stopped = dev->Ctrl()->StopDev();
        if (!stopped.ok()) {
            if (IsErrno(stopped.error(), ENODEV)) {
                AGENTENV_INFO("dev " << dev_id
                                     << ": ublk device disappeared before startup cleanup");
                dev.reset();
                return;
            }
            AGENTENV_WARN("dev " << dev_id << ": failed to stop ublk device after startup "
                                              "failure: " << stopped.error());
        }
        // The destructor joins the queue workers and closes `/dev/ublkcN`.
        // Both must happen before DEL_DEV: the kernel waits for every
        // reference to the char device to go away, so deleting first blocks
        // on a reference this scope is holding.
        (void)kQueueWorkerJoinIsUnbounded;
        dev.reset();
    }

    ublk::UblkCtrlBuilder builder;
    builder.Name(kDeviceName).DevId(dev_id);
    core::Expected<std::unique_ptr<ublk::UblkCtrl>, std::string> ctrl =
        builder.Build(ctrl_ring_);
    if (!ctrl.ok()) {
        AGENTENV_WARN("dev " << dev_id
                             << ": failed to build ublk ctrl for startup cleanup; kernel "
                                "device may remain active: " << ctrl.error());
        return;
    }
    const core::Expected<core::Unit, std::string> deleted = ctrl.value()->DelDev();
    if (deleted.ok()) {
        AGENTENV_INFO("dev " << dev_id << ": deleted ublk device after startup failure");
    } else if (IsErrno(deleted.error(), ENODEV)) {
        AGENTENV_INFO("dev " << dev_id << ": ublk device already deleted after startup "
                                          "failure");
    } else {
        AGENTENV_WARN("dev " << dev_id
                             << ": failed to delete ublk device after startup failure; kernel "
                                "device may remain active: " << deleted.error());
    }
}

core::Expected<core::Unit, std::string> DevicePlane::DeleteDevById(uint32_t dev_id) {
    if (!ctrl_ring_) {
        // Unreachable in production, since nothing could have created the
        // device being deleted. Checked anyway because the alternative is a
        // null dereference on the teardown path, where it would surface as a
        // crash at shutdown.
        return core::make_unexpected(
            std::string("no ublk control ring: cannot delete devices"));
    }

    std::lock_guard<std::mutex> ring_guard(ctrl_mutex_);

    ublk::UblkCtrlBuilder builder;
    builder.Name(kDeviceName).DevId(dev_id);
    core::Expected<std::unique_ptr<ublk::UblkCtrl>, std::string> ctrl =
        builder.Build(ctrl_ring_);
    if (!ctrl.ok()) {
        return core::make_unexpected(WithContext("build ctrl", ctrl.error()));
    }

    const core::Expected<core::Unit, std::string> stopped = ctrl.value()->StopDev();
    if (!stopped.ok()) {
        if (IsErrno(stopped.error(), ENODEV)) {
            // Nothing to delete. Reported as success because the caller's
            // goal -- the device is gone -- already holds.
            AGENTENV_INFO("dev " << dev_id << ": stop device got ENODEV");
            return core::Unit();
        }
        return core::make_unexpected(stopped.error());
    }
    return ctrl.value()->DelDev();
}

void DevicePlane::StopAndDeleteDevice(DetachedDevice detached) {
    const uint32_t dev_id = detached.dev_id;

    if (detached.dev) {
        {
            std::lock_guard<std::mutex> ring_guard(ctrl_mutex_);
            const core::Expected<core::Unit, std::string> stopped =
                detached.dev->Ctrl()->StopDev();
            if (!stopped.ok() && !IsErrno(stopped.error(), ENODEV)) {
                AGENTENV_WARN("dev " << dev_id << ": failed to stop ublk device before delete: "
                                     << stopped.error());
            }
        }
        // Deliberately outside `ctrl_mutex_`: joining the queue workers can
        // take as long as the kernel needs to fail their pending fetches, and
        // nothing in that wait touches the control ring. Holding the lock
        // across it would serialize every other connection behind one
        // teardown.
        detached.dev->WaitForQueueWorkers();
        detached.dev.reset();
    }

    // Target and image released before DEL_DEV, not after: the target holds
    // the only other reference to the image, and an image still open keeps
    // its cache files mapped.
    detached.target.reset();
    detached.image.reset();

    const core::Expected<core::Unit, std::string> deleted = DeleteDevById(dev_id);
    if (!deleted.ok()) {
        AGENTENV_WARN("dev " << dev_id << ": failed to delete ublk device: "
                             << deleted.error());
    } else {
        AGENTENV_INFO("dev " << dev_id << ": device deleted");
    }
}

// ---------------------------------------------------------------------------
// Create (no pool) / create runtime device
// ---------------------------------------------------------------------------

core::Expected<DaemonResponse, HandlerError> DevicePlane::HandleCreateRawOverlaybd(
    const DaemonRequest& request) {
    const std::string global_config = EffectiveGlobalConfig(request.global_config);
    AGENTENV_INFO("creating raw overlaybd device image_config=" << request.image_config
                                                                << " global_config="
                                                                << global_config);

    core::Expected<std::shared_ptr<overlaybd::ImageFile>, std::string> image =
        OpenImage(request.image_config, global_config);
    if (!image.ok()) {
        return core::make_unexpected(HandlerError::Internal(image.error()));
    }

    std::unique_ptr<ublk::UblkDev> dev;
    std::shared_ptr<ublk::OverlaybdTarget> target;
    const core::Expected<core::Unit, std::string> created =
        CreateNewDevice(request.image_config, image.value(), &dev, &target);
    if (!created.ok()) {
        return core::make_unexpected(HandlerError::Internal(created.error()));
    }

    const uint32_t dev_id = dev->DevId();
    const std::string device_path = dev->DevicePath();
    {
        std::lock_guard<std::mutex> guard(table_mutex_);
        ManagedDevice managed;
        managed.dev = std::move(dev);
        managed.target = target;
        managed.image = image.value();
        managed.image_config = request.image_config;
        devices_[dev_id] = std::move(managed);
    }
    AGENTENV_INFO("dev " << dev_id << ": overlaybd device created at " << device_path);

    DaemonResponse out;
    out.status = DaemonResponse::Status::DeviceCreated;
    out.dev_id = dev_id;
    out.device_path = device_path;
    return out;
}

core::Expected<DaemonResponse, HandlerError> DevicePlane::HandleCreateRuntimeDevice(
    const DaemonRequest& request) {
    const std::string global_config = EffectiveGlobalConfig(request.global_config);

    MaterializeOverlaybdRuntimeRequest materialize;
    materialize.image_service_cache = image_service_cache_.get();
    materialize.source_image_config = request.source_image_config;
    materialize.global_config = global_config;
    materialize.runtime_dir = request.runtime_dir;
    materialize.read_only = request.read_only;
    materialize.runtime_upper_mode = request.runtime_upper_mode;
    materialize.requested_virtual_size = request.requested_virtual_size;
    materialize.known_source_virtual_size = request.known_source_virtual_size;
    materialize.resize_tool =
        options_.resize_tool.has_value() ? &(*options_.resize_tool) : NULL;
    materialize.resize_global_config = options_.resize_global_config;
    materialize.resize_permit = &resize_permit_;
    materialize.allow_shrink = request.allow_shrink;

    core::Expected<MaterializedOverlaybdRuntime, RuntimeError> runtime =
        MaterializeOverlaybdRuntime(materialize);
    if (!runtime.ok()) {
        const std::string why = WithContext(
            "materialize overlaybd runtime in " + request.runtime_dir,
            runtime.error().message);
        // The flavour is carried, not collapsed: a non-GiB-aligned size is
        // the caller's mistake and a retry fails identically, while a failed
        // image open is ours and a retry may work.
        return core::make_unexpected(runtime.error().invalid_request
                                         ? HandlerError::InvalidRequest(why)
                                         : HandlerError::Internal(why));
    }

    const std::string runtime_config = runtime.value().runtime_image_config_path;
    const uint64_t actual_virtual_size = runtime.value().actual_virtual_size;

    core::Expected<DaemonResponse, HandlerError> created =
        core::make_unexpected(HandlerError::Internal("unreachable"));
    if (idle_) {
        // With the pool on, a runtime device is an exclusive acquisition like
        // any other: that is what lets it come from a warm device instead of
        // a fresh ADD_DEV.
        created = HandleAcquireOverlaybd(runtime_config, global_config,
                                         core::Optional<uint64_t>(actual_virtual_size),
                                         AccessMode::Exclusive);
        if (created.ok() &&
            created.value().status != DaemonResponse::Status::DeviceAcquired) {
            created = core::make_unexpected(HandlerError::Internal(
                "unexpected acquire response for runtime device"));
        }
    } else {
        DaemonRequest raw;
        raw.kind = DaemonRequest::Kind::CreateOverlaybd;
        raw.image_config = runtime_config;
        raw.global_config = global_config;
        created = HandleCreateRawOverlaybd(raw);
    }

    if (!created.ok()) {
        // The create/acquire path has returned, so every transient image and
        // target local it built is already gone. Only now can the directory
        // be removed: unlinking files a device is still serving from would
        // leave that device reading through deleted inodes.
        MaterializedOverlaybdRuntime rollback = runtime.value();
        rollback.Rollback();
        return core::make_unexpected(HandlerError(
            created.error().kind == HandlerError::kInvalidRequest
                ? HandlerError::InvalidRequest(
                      WithContext("create overlaybd runtime device from " + runtime_config,
                                  created.error().message))
                : HandlerError::Internal(
                      WithContext("create overlaybd runtime device from " + runtime_config,
                                  created.error().message))));
    }

    DaemonResponse out;
    out.status = DaemonResponse::Status::OverlaybdRuntimeDeviceCreated;
    out.dev_id = created.value().dev_id;
    out.device_path = created.value().device_path;
    out.actual_virtual_size = actual_virtual_size;
    out.runtime_image_config_path = runtime_config;
    return out;
}

core::Expected<DaemonResponse, HandlerError> DevicePlane::HandleDelete(
    const DaemonRequest& request) {
    const uint32_t dev_id = request.dev_id;

    // Before the device leaves the table. The window thread holds a reference
    // to this device's target and would keep observing reads -- and could
    // finalize against an image whose device is being torn down -- so the
    // recording has to be stopped and joined first.
    //
    // Deliberately not conditional on the device existing: the recording is
    // what owns the output files, and a delete for an already-gone device
    // still has to clean them up.
    AbortRecordingHandle(TakeRecording(dev_id));

    DetachedDevice detached;
    {
        std::lock_guard<std::mutex> guard(table_mutex_);
        std::map<uint32_t, ManagedDevice>::iterator it = devices_.find(dev_id);
        if (it == devices_.end()) {
            std::ostringstream why;
            why << "device " << dev_id << " not found";
            return core::make_unexpected(HandlerError::Internal(why.str()));
        }
        detached.dev = std::move(it->second.dev);
        detached.target = std::move(it->second.target);
        detached.image = std::move(it->second.image);
        detached.dev_id = dev_id;
        devices_.erase(it);
    }

    AGENTENV_INFO("dev " << dev_id << ": deleting device");
    StopAndDeleteDevice(std::move(detached));

    DaemonResponse out;
    out.status = DaemonResponse::Status::Deleted;
    return out;
}

// ---------------------------------------------------------------------------
// Restack
// ---------------------------------------------------------------------------

core::Expected<DaemonResponse, HandlerError> DevicePlane::HandleRestackSnapshot(
    const DaemonRequest& request) {
    const uint32_t dev_id = request.dev_id;

    std::shared_ptr<overlaybd::ImageFile> image;
    std::string image_config;
    bool pooled = false;
    {
        std::lock_guard<std::mutex> guard(table_mutex_);
        std::map<uint32_t, ManagedDevice>::iterator managed = devices_.find(dev_id);
        if (managed != devices_.end()) {
            image = managed->second.image;
            image_config = managed->second.image_config;
        } else {
            std::map<uint32_t, ActiveDevice>::iterator exclusive =
                active_exclusive_.find(dev_id);
            if (exclusive != active_exclusive_.end()) {
                image = exclusive->second.image;
                image_config = exclusive->second.image_config;
                pooled = true;
            } else {
                // Through the reverse index, never by scanning
                // `active_shared_`: the scan is what Rust's comment warns
                // about, and the key here is not the device id.
                std::map<uint32_t, SharedKey>::iterator reverse =
                    shared_by_dev_id_.find(dev_id);
                if (reverse != shared_by_dev_id_.end()) {
                    std::map<SharedKey, ActiveDevice>::iterator shared =
                        active_shared_.find(reverse->second);
                    if (shared != active_shared_.end()) {
                        image = shared->second.image;
                        image_config = shared->second.image_config;
                        pooled = true;
                    }
                }
            }
        }
    }
    if (!image) {
        std::ostringstream why;
        why << "device " << dev_id << " not found for snapshot";
        return core::make_unexpected(HandlerError::Internal(why.str()));
    }

    AGENTENV_INFO("dev " << dev_id << ": creating restack snapshot at "
                         << request.output_layer_path);

    core::Expected<core::Optional<overlaybd::LayerDescriptor>, std::string> descriptor =
        core::make_unexpected(std::string("unreachable"));
    if (pooled) {
        // Held only for a pooled device, matching Rust: the lock exists to
        // exclude this mutation from a concurrent *acquire* of the same
        // image, and only the pool paths open images by that key.
        const std::shared_ptr<std::mutex> image_lock = ImageLock(image_config);
        std::lock_guard<std::mutex> guard(*image_lock);
        descriptor = image->CreateSnapshotAndRestack(request.output_layer_path);
    } else {
        descriptor = image->CreateSnapshotAndRestack(request.output_layer_path);
    }

    if (!descriptor.ok()) {
        const std::string why =
            WithContext("restack overlaybd snapshot for dev_id", descriptor.error());
        // A failure after the live layer stack was already mutated must not
        // be retried, and the caller reads that from the status rather than
        // from the message.
        if (overlaybd::IsRestackSnapshotTerminalFailure(descriptor.error())) {
            return core::make_unexpected(HandlerError::Terminal(why));
        }
        return core::make_unexpected(HandlerError::Internal(why));
    }

    DaemonResponse out;
    out.status = DaemonResponse::Status::RestackSnapshotCreated;
    if (descriptor.value().has_value()) {
        out.has_descriptor = true;
        out.descriptor = DescriptorToJson(*descriptor.value());
    }
    // Best effort, and never allowed to fail the restack: this is
    // observability of layer bloat, and the layer is already written.
    const core::Expected<overlaybd::DataStat, std::string> stat = image->GetDataStat();
    if (stat.ok()) {
        out.has_data_stat = true;
        out.data_stat = DataStatToJson(stat.value());
    }
    // `ext4_used_bytes` is left absent: Rust fills it from
    // `overlaybd::ext4_stat::ext4_used_bytes`, which is not ported. Absent is
    // a value the protocol already defines -- an older daemon omits it -- and
    // is not the same as reporting zero, so no caller is misled.

    AGENTENV_INFO("dev " << dev_id << ": ublk daemon restack snapshot completed");
    return out;
}

// ---------------------------------------------------------------------------
// Pool: acquire
// ---------------------------------------------------------------------------

bool DevicePlane::TakeIdleDevice(uint64_t dev_sectors, PooledDevice* out) {
    if (!idle_) return false;
    if (SupportsUpdateSize()) {
        // Any warm device will do: the kernel can be told the new size.
        return idle_->TryAcquire(out);
    }
    // Without `UBLK_F_UPDATE_SIZE` the kernel's size is fixed for the life of
    // the device, so only an exact match is reusable. A mismatched reuse
    // would publish a disk whose capacity disagrees with the image behind it.
    struct SameSize {
        uint64_t want;
        explicit SameSize(uint64_t w) : want(w) {}
        bool operator()(const PooledDevice& pooled) const {
            return pooled.dev_sectors == want;
        }
    };
    return idle_->TryAcquireWhere(SameSize(dev_sectors), out);
}

core::Expected<core::Unit, std::string> DevicePlane::PrepareOverlaybdDevice(
    const std::string& image_config, const std::shared_ptr<overlaybd::ImageFile>& image,
    const char* mode, std::unique_ptr<ublk::UblkDev>* out_dev,
    std::shared_ptr<ublk::OverlaybdTarget>* out_target, bool* out_reused) {
    const uint64_t new_sectors = image->NumLbas();

    PooledDevice pooled;
    if (TakeIdleDevice(new_sectors, &pooled)) {
        const uint32_t dev_id = pooled.dev->DevId();
        AGENTENV_DEBUG("dev " << dev_id << ": reusing warm " << mode
                              << " device from pool");

        const bool discard_supported = !image->IsReadOnly();
        const core::Expected<core::Unit, std::string> swapped =
            pooled.target->SwapState(image_config, image, discard_supported);
        if (!swapped.ok()) {
            // The device is still parked on its placeholder and still
            // serving, but it is no longer in the pool. Deleting it is the
            // only safe end: putting it back would hand the next acquire a
            // device whose state is unknown.
            DetachedDevice broken;
            broken.dev = std::move(pooled.dev);
            broken.target = std::move(pooled.target);
            broken.image = std::move(pooled.placeholder_image);
            broken.dev_id = dev_id;
            StopAndDeleteDevice(std::move(broken));
            return core::make_unexpected(
                WithContext(std::string("swap ") + mode + " target state", swapped.error()));
        }

        if (SupportsUpdateSize() && pooled.dev_sectors != new_sectors) {
            std::lock_guard<std::mutex> ring_guard(ctrl_mutex_);
            const core::Expected<core::Unit, std::string> resized =
                pooled.dev->Ctrl()->UpdateSize(new_sectors, features_);
            if (!resized.ok()) {
                std::ostringstream why;
                why << "update " << mode << " device size for dev_id=" << dev_id;
                return core::make_unexpected(WithContext(why.str(), resized.error()));
            }
        }

        *out_dev = std::move(pooled.dev);
        *out_target = pooled.target;
        *out_reused = true;
        return core::Unit();
    }

    AGENTENV_DEBUG("pool miss: creating new " << mode << " device");
    const core::Expected<core::Unit, std::string> created =
        CreateNewDevice(image_config, image, out_dev, out_target);
    if (!created.ok()) {
        std::ostringstream why;
        why << "create new " << mode << " ublk device on pool miss";
        return core::make_unexpected(WithContext(why.str(), created.error()));
    }
    *out_reused = false;
    return core::Unit();
}

core::Expected<DaemonResponse, HandlerError> DevicePlane::HandleAcquireOverlaybd(
    const std::string& image_config, const std::string& global_config,
    const core::Optional<uint64_t>& virtual_size, AccessMode access_mode) {
    if (!idle_) {
        return core::make_unexpected(HandlerError::Internal("warm pool not enabled"));
    }

    const SharedKey key(image_config, global_config);

    if (access_mode == AccessMode::Shared) {
        // The cheap path first, and without opening the image: a second
        // sandbox resuming from the same memory snapshot must get the same
        // device, and opening the image again would cost its whole layer
        // stack for nothing.
        std::lock_guard<std::mutex> guard(table_mutex_);
        std::map<SharedKey, ActiveDevice>::iterator it = active_shared_.find(key);
        if (it != active_shared_.end()) {
            it->second.refcount += 1;
            DaemonResponse out;
            out.status = DaemonResponse::Status::DeviceAcquired;
            out.dev_id = it->second.dev->DevId();
            out.device_path = it->second.dev->DevicePath();
            AGENTENV_INFO("dev " << out.dev_id << ": reusing shared overlaybd device "
                                                  "(refcount " << it->second.refcount << ")");
            return out;
        }
    }

    core::Expected<std::shared_ptr<overlaybd::ImageFile>, std::string> image =
        OpenImage(image_config, global_config);
    if (!image.ok()) {
        return core::make_unexpected(HandlerError::Internal(image.error()));
    }

    const uint64_t actual_virtual_size = image.value()->SizeBytes();
    if (virtual_size.has_value() && *virtual_size != actual_virtual_size) {
        // Checked rather than trusted: the caller derived the size from a
        // snapshot's metadata, and a device published at the wrong capacity
        // either truncates the guest's view or exposes unmapped space.
        std::ostringstream why;
        why << "requested overlaybd acquire virtual size " << *virtual_size
            << " does not match image virtual size " << actual_virtual_size << ": "
            << image_config;
        return core::make_unexpected(HandlerError::InvalidRequest(why.str()));
    }

    std::unique_ptr<ublk::UblkDev> dev;
    std::shared_ptr<ublk::OverlaybdTarget> target;
    bool reused = false;
    const core::Expected<core::Unit, std::string> prepared = PrepareOverlaybdDevice(
        image_config, image.value(),
        access_mode == AccessMode::Shared ? "shared" : "exclusive", &dev, &target, &reused);
    if (!prepared.ok()) {
        return core::make_unexpected(HandlerError::Internal(prepared.error()));
    }

    const uint32_t dev_id = dev->DevId();
    const std::string device_path = dev->DevicePath();

    if (access_mode == AccessMode::Exclusive) {
        {
            std::lock_guard<std::mutex> guard(table_mutex_);
            ActiveDevice active;
            active.dev = std::move(dev);
            active.target = target;
            active.image = image.value();
            active.image_config = image_config;
            active.global_config = global_config;
            active.refcount = 1;
            active_exclusive_[dev_id] = std::move(active);
        }
        AGENTENV_INFO("dev " << dev_id << ": acquired exclusive overlaybd device ("
                             << (reused ? "reused" : "new") << ") at " << device_path);
        if (options_.pool_config->startup_prewarm) {
            ScheduleIdlePoolRefill(actual_virtual_size);
        }

        DaemonResponse out;
        out.status = DaemonResponse::Status::DeviceAcquired;
        out.dev_id = dev_id;
        out.device_path = device_path;
        return out;
    }

    // Shared. The table is consulted a second time because the first look was
    // dropped before the image was opened and the device built, and another
    // connection may have inserted the same key in between. Whoever loses
    // tears its own device down -- two devices on one memory snapshot would
    // each believe they own its upper.
    bool lost_race = false;
    uint32_t winner_dev_id = dev_id;
    std::string winner_path = device_path;
    std::size_t winner_refcount = 1;
    {
        std::lock_guard<std::mutex> guard(table_mutex_);
        std::map<SharedKey, ActiveDevice>::iterator it = active_shared_.find(key);
        if (it != active_shared_.end()) {
            it->second.refcount += 1;
            winner_dev_id = it->second.dev->DevId();
            winner_path = it->second.dev->DevicePath();
            winner_refcount = it->second.refcount;
            lost_race = true;
        } else {
            ActiveDevice active;
            active.dev = std::move(dev);
            active.target = target;
            active.image = image.value();
            active.image_config = image_config;
            active.global_config = global_config;
            active.refcount = 1;
            active_shared_[key] = std::move(active);
            shared_by_dev_id_[dev_id] = key;
        }
    }

    if (lost_race) {
        // Deleted rather than idled: the pool must only ever hold devices
        // parked on a daemon-owned placeholder, and this one is bound to a
        // business image.
        DetachedDevice redundant;
        redundant.dev = std::move(dev);
        redundant.target = std::move(target);
        redundant.image = image.value();
        redundant.dev_id = dev_id;
        StopAndDeleteDevice(std::move(redundant));
        AGENTENV_INFO("dev " << winner_dev_id
                             << ": concurrent shared overlaybd acquire reused existing device "
                                "(refcount " << winner_refcount << ")");
        DaemonResponse out;
        out.status = DaemonResponse::Status::DeviceAcquired;
        out.dev_id = winner_dev_id;
        out.device_path = winner_path;
        return out;
    }

    AGENTENV_INFO("dev " << dev_id << ": acquired shared overlaybd device ("
                         << (reused ? "reused" : "new") << ") at " << device_path);
    if (options_.pool_config->startup_prewarm) {
        ScheduleIdlePoolRefill(actual_virtual_size);
    }

    DaemonResponse out;
    out.status = DaemonResponse::Status::DeviceAcquired;
    out.dev_id = dev_id;
    out.device_path = device_path;
    return out;
}

// ---------------------------------------------------------------------------
// Pool: release
// ---------------------------------------------------------------------------

core::Expected<DaemonResponse, HandlerError> DevicePlane::HandleReleaseOverlaybd(
    uint32_t dev_id) {
    if (!idle_) {
        return core::make_unexpected(HandlerError::Internal("warm pool not enabled"));
    }

    DetachedDevice detached;
    {
        std::lock_guard<std::mutex> guard(table_mutex_);

        std::map<uint32_t, ActiveDevice>::iterator exclusive = active_exclusive_.find(dev_id);
        if (exclusive != active_exclusive_.end()) {
            detached.dev = std::move(exclusive->second.dev);
            detached.target = std::move(exclusive->second.target);
            detached.image = std::move(exclusive->second.image);
            detached.dev_id = dev_id;
            active_exclusive_.erase(exclusive);
            AGENTENV_INFO("dev " << dev_id << ": releasing exclusive device");
        } else {
            std::map<uint32_t, SharedKey>::iterator reverse = shared_by_dev_id_.find(dev_id);
            if (reverse == shared_by_dev_id_.end()) {
                std::ostringstream why;
                why << "device " << dev_id << " not found in active pool";
                return core::make_unexpected(HandlerError::Internal(why.str()));
            }
            const SharedKey key = reverse->second;
            std::map<SharedKey, ActiveDevice>::iterator shared = active_shared_.find(key);
            if (shared == active_shared_.end()) {
                // Reverse index without an entry: the forward map was already
                // emptied, so the index is stale. Dropped rather than
                // reported, because nothing is left to release.
                shared_by_dev_id_.erase(reverse);
                std::ostringstream why;
                why << "device " << dev_id << " not found in active pool";
                return core::make_unexpected(HandlerError::Internal(why.str()));
            }

            // `saturating_sub`: a release of an already-zero refcount is a
            // client bug, and wrapping would make the device immortal.
            if (shared->second.refcount > 0) shared->second.refcount -= 1;
            if (shared->second.refcount > 0) {
                AGENTENV_INFO("dev " << dev_id << ": decremented shared refcount to "
                                     << shared->second.refcount);
                DaemonResponse out;
                out.status = DaemonResponse::Status::Released;
                return out;
            }

            AGENTENV_INFO("dev " << dev_id << ": releasing shared device (refcount reached 0)");
            detached.dev = std::move(shared->second.dev);
            detached.target = std::move(shared->second.target);
            detached.image = std::move(shared->second.image);
            detached.dev_id = dev_id;
            active_shared_.erase(shared);
            shared_by_dev_id_.erase(reverse);
        }
    }

    IdleReleasedDevice(std::move(detached));

    DaemonResponse out;
    out.status = DaemonResponse::Status::Released;
    return out;
}

void DevicePlane::IdleReleasedDevice(DetachedDevice detached) {
    const uint32_t dev_id = detached.dev_id;
    const uint64_t virtual_size = detached.image ? detached.image->SizeBytes() : 0;

    // Before the swap, while the device still points at the image whose
    // blocks are in the cache. After the swap those pages would be attributed
    // to the placeholder and the next tenant could read them.
    if (detached.dev) ClearPageCache(detached.dev->DevicePath());

    std::string placeholder_config;
    std::shared_ptr<overlaybd::ImageFile> placeholder_image;
    const core::Expected<core::Unit, std::string> placeholder =
        PlaceholderFor(virtual_size, &placeholder_config, &placeholder_image);
    if (!placeholder.ok()) {
        AGENTENV_WARN("dev " << dev_id
                             << ": failed to build pool placeholder; deleting device instead "
                                "of idling business image: " << placeholder.error());
        StopAndDeleteDevice(std::move(detached));
        return;
    }

    // Same-size by construction, so the kernel's capacity still matches and
    // no resize is needed -- which is what makes idling work on a kernel
    // without `UBLK_F_UPDATE_SIZE` at all.
    const bool discard = !placeholder_image->IsReadOnly();
    const core::Expected<core::Unit, std::string> swapped =
        detached.target->SwapState(placeholder_config, placeholder_image, discard);
    if (!swapped.ok()) {
        AGENTENV_WARN("dev " << dev_id
                             << ": failed to swap device to pool placeholder; deleting device "
                                "instead of idling business image: " << swapped.error());
        StopAndDeleteDevice(std::move(detached));
        return;
    }

    // The business image is dropped here and not before: until the swap
    // succeeded the target was still serving from it.
    detached.image.reset();

    PooledDevice pooled;
    pooled.dev = std::move(detached.dev);
    pooled.target = std::move(detached.target);
    pooled.dev_sectors = placeholder_image->NumLbas();
    pooled.placeholder_image = placeholder_image;

    if (!idle_->TryPushBounded(&pooled)) {
        AGENTENV_INFO("dev " << dev_id << ": idle overlaybd pool is full (high_watermark "
                             << idle_->Config().high_watermark
                             << "); stopping returned device");
        DetachedDevice excess;
        excess.dev = std::move(pooled.dev);
        excess.target = std::move(pooled.target);
        excess.image = std::move(pooled.placeholder_image);
        excess.dev_id = dev_id;
        StopAndDeleteDevice(std::move(excess));
        return;
    }

    if (options_.pool_config->startup_prewarm) {
        ScheduleIdlePoolRefill(virtual_size);
    }
    AGENTENV_INFO("dev " << dev_id << ": device returned to idle pool on placeholder");
}

// ---------------------------------------------------------------------------
// Pool: resize
// ---------------------------------------------------------------------------

core::Expected<DaemonResponse, HandlerError> DevicePlane::HandleUpdateSize(
    uint32_t dev_id, uint64_t new_sectors) {
    if (!idle_) {
        return core::make_unexpected(HandlerError::Internal("warm pool not enabled"));
    }
    if (!SupportsUpdateSize()) {
        return core::make_unexpected(
            HandlerError::Internal("UBLK_F_UPDATE_SIZE not supported by kernel"));
    }

    // Both locks, in the only order this file ever takes them. The table lock
    // is held across the resize on purpose: dropping it to take the control
    // lock would let a concurrent release park this device on a placeholder
    // and then resize the placeholder.
    std::lock_guard<std::mutex> guard(table_mutex_);

    ublk::UblkDev* dev = NULL;
    const char* what = NULL;
    std::map<uint32_t, ActiveDevice>::iterator exclusive = active_exclusive_.find(dev_id);
    if (exclusive != active_exclusive_.end()) {
        dev = exclusive->second.dev.get();
        what = "exclusive";
    } else {
        std::map<uint32_t, SharedKey>::iterator reverse = shared_by_dev_id_.find(dev_id);
        if (reverse != shared_by_dev_id_.end()) {
            std::map<SharedKey, ActiveDevice>::iterator shared =
                active_shared_.find(reverse->second);
            if (shared != active_shared_.end()) {
                dev = shared->second.dev.get();
                what = "shared";
            }
        }
    }
    if (dev == NULL) {
        std::ostringstream why;
        why << "device " << dev_id << " not found in active pool";
        return core::make_unexpected(HandlerError::Internal(why.str()));
    }

    core::Expected<core::Unit, std::string> updated =
        core::make_unexpected(std::string("unreachable"));
    {
        std::lock_guard<std::mutex> ring_guard(ctrl_mutex_);
        updated = dev->Ctrl()->UpdateSize(new_sectors, features_);
    }
    if (!updated.ok()) {
        std::ostringstream why;
        why << "update size for " << what << " dev_id=" << dev_id;
        return core::make_unexpected(
            HandlerError::Internal(WithContext(why.str(), updated.error())));
    }

    AGENTENV_INFO("dev " << dev_id << ": updated " << what << " device size to "
                         << new_sectors << " sectors");
    DaemonResponse out;
    out.status = DaemonResponse::Status::SizeUpdated;
    return out;
}

// ---------------------------------------------------------------------------
// Pool: placeholders and refill
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> DevicePlane::PlaceholderFor(
    uint64_t virtual_size, std::string* out_config,
    std::shared_ptr<overlaybd::ImageFile>* out_image) {
    // The lock is held across the build, not just across the lookup: two
    // first-time releases of the same size would otherwise each lay down
    // `<size>.data` and clobber the other's file -- while a device is about
    // to be bound to it.
    std::lock_guard<std::mutex> guard(placeholder_mutex_);

    std::map<uint64_t,
             std::pair<std::string, std::shared_ptr<overlaybd::ImageFile> > >::iterator it =
        placeholders_.find(virtual_size);
    if (it != placeholders_.end()) {
        *out_config = it->second.first;
        *out_image = it->second.second;
        return core::Unit();
    }

    const core::Expected<core::Unit, std::string> dir =
        core::fs::CreateDirAll(options_.placeholder_dir);
    if (!dir.ok()) {
        return core::make_unexpected(
            WithContext("create pool placeholder dir " + options_.placeholder_dir,
                        dir.error()));
    }

    std::ostringstream data_name;
    data_name << virtual_size << ".data";
    std::ostringstream config_name;
    config_name << virtual_size << ".json";
    const std::string data_path = core::fs::Join(options_.placeholder_dir, data_name.str());
    const std::string config_path =
        core::fs::Join(options_.placeholder_dir, config_name.str());

    // Sparse: a placeholder is never read, so it must not cost its virtual
    // size on disk. The other two layouts write an index up front.
    const core::Expected<core::Unit, std::string> upper = overlaybd::PrepareRuntimeUpper(
        data_path, core::Optional<std::string>(), virtual_size,
        overlaybd::UpperMode::Sparse);
    if (!upper.ok()) {
        return core::make_unexpected(
            WithContext("create placeholder upper " + data_path, upper.error()));
    }

    // No lowers at all, which is what keeps opening a placeholder local: it
    // never touches a registry, so parking a device cannot block on the
    // network.
    overlaybd::ImageConfig config;
    config.upper.has_mode = true;
    config.upper.mode = overlaybd::UpperMode::Sparse;
    config.upper.data = data_path;
    const core::Expected<core::Unit, std::string> written =
        core::fs::Write(config_path, overlaybd::ImageConfigToJson(config));
    if (!written.ok()) {
        return core::make_unexpected(
            WithContext("write placeholder config " + config_path, written.error()));
    }

    core::Expected<overlaybd::ImageService, std::string> service =
        image_service_cache_->GetOrCreate(options_.global_config);
    if (!service.ok()) {
        return core::make_unexpected(
            WithContext("resolve image service for placeholders", service.error()));
    }
    core::Expected<std::shared_ptr<overlaybd::ImageFile>, std::string> image =
        service.value().CreateImageFile(config_path);
    if (!image.ok()) {
        return core::make_unexpected(
            WithContext("open placeholder image " + config_path, image.error()));
    }

    placeholders_[virtual_size] = std::make_pair(config_path, image.value());
    *out_config = config_path;
    *out_image = image.value();
    return core::Unit();
}

void DevicePlane::ScheduleIdlePoolRefill(uint64_t virtual_size) {
    if (!idle_ || stopping_.load()) return;

    // Coalesced: concurrent acquires and releases all want the pool topped
    // up, and letting each of them create devices synchronously would make a
    // burst of sandbox starts pay for the whole watermark.
    bool expected = false;
    if (!refill_inflight_.compare_exchange_strong(expected, true)) return;

    std::lock_guard<std::mutex> guard(refill_mutex_);
    // The previous worker has already cleared `refill_inflight_`, so this
    // join is against a thread that has finished or is finishing.
    if (refill_thread_.joinable()) refill_thread_.join();
    try {
        refill_thread_ = std::thread(&DevicePlane::RefillLoop, this, virtual_size);
    } catch (const std::system_error& err) {
        refill_inflight_.store(false);
        AGENTENV_WARN("failed to start idle pool refill worker: " << err.what());
    }
}

void DevicePlane::RefillLoop(uint64_t virtual_size) {
    // Looped here rather than re-scheduled recursively, as Rust does: the
    // watermark can still call for devices after one pass, and a self-spawn
    // would hand the work to a thread this one has to be joined before.
    while (!stopping_.load()) {
        const core::Expected<core::Unit, std::string> filled =
            RefillIdlePoolOnce(virtual_size);
        if (!filled.ok()) {
            AGENTENV_WARN("failed to refill idle overlaybd pool (virtual_size "
                          << virtual_size << "): " << filled.error());
            // Backed off *before* clearing the in-flight flag, so a stream of
            // failing creates cannot be retried once per request.
            for (int i = 0; i < 20 && !stopping_.load(); ++i) {
                ::usleep(50 * 1000);
            }
            break;
        }
        if (idle_->ComputeMaintenanceAction(idle_->Len()).kind !=
            warmpool::PoolMaintenanceKind::Fill) {
            break;
        }
    }
    refill_inflight_.store(false);
}

core::Expected<core::Unit, std::string> DevicePlane::RefillIdlePoolOnce(
    uint64_t virtual_size) {
    const warmpool::PoolMaintenanceAction action =
        idle_->ComputeMaintenanceAction(idle_->Len());
    if (action.kind != warmpool::PoolMaintenanceKind::Fill) return core::Unit();

    std::string placeholder_config;
    std::shared_ptr<overlaybd::ImageFile> placeholder_image;
    const core::Expected<core::Unit, std::string> placeholder =
        PlaceholderFor(virtual_size, &placeholder_config, &placeholder_image);
    if (!placeholder.ok()) {
        return core::make_unexpected(
            WithContext("build pool placeholder for prewarm", placeholder.error()));
    }

    for (std::size_t i = 0; i < action.count; ++i) {
        if (stopping_.load()) return core::Unit();

        std::unique_ptr<ublk::UblkDev> dev;
        std::shared_ptr<ublk::OverlaybdTarget> target;
        const core::Expected<core::Unit, std::string> created =
            CreateNewDevice(placeholder_config, placeholder_image, &dev, &target);
        if (!created.ok()) {
            return core::make_unexpected(
                WithContext("prewarm overlaybd pool device", created.error()));
        }

        const uint32_t dev_id = dev->DevId();
        PooledDevice pooled;
        pooled.dev = std::move(dev);
        pooled.target = target;
        pooled.dev_sectors = placeholder_image->NumLbas();
        pooled.placeholder_image = placeholder_image;

        if (!idle_->TryPushBounded(&pooled)) {
            AGENTENV_INFO("dev " << dev_id
                                 << ": idle overlaybd pool filled during prewarm; stopping "
                                    "the device just created");
            DetachedDevice excess;
            excess.dev = std::move(pooled.dev);
            excess.target = std::move(pooled.target);
            excess.image = std::move(pooled.placeholder_image);
            excess.dev_id = dev_id;
            StopAndDeleteDevice(std::move(excess));
            break;
        }
    }
    return core::Unit();
}

// ---------------------------------------------------------------------------
// Startup pack prefetch
// ---------------------------------------------------------------------------

core::Expected<DaemonResponse, HandlerError> DevicePlane::HandlePrefetchStartupPack(
    const DaemonRequest& request) {
    DaemonResponse out;
    out.status = DaemonResponse::Status::Ok;

    // Every failure below is logged and answered `Ok`, matching Rust: the
    // prefetch is an accelerator and on-demand reads serve the same blocks,
    // so failing the request would turn a missed optimisation into a failed
    // sandbox resume.
    const std::string global_config = EffectiveGlobalConfig(request.global_config);
    core::Expected<overlaybd::ImageService, std::string> service =
        image_service_cache_->GetOrCreate(global_config);
    if (!service.ok()) {
        AGENTENV_WARN("startup pack prefetch: image service unavailable: " << service.error());
        return out;
    }

    overlaybd::StartupPackPrefetch pack;
    pack.url = request.url;
    pack.pack_size = request.pack_size;
    pack.index_sha256 = request.index_sha256;
    pack.mem_virtual_size = request.mem_virtual_size;
    pack.timeout_ms = request.timeout_secs * 1000;

    core::Expected<core::Optional<overlaybd::StartupPackHandle>, std::string> handle =
        service.value().PrefetchStartupPack(request.image_config, pack);
    if (!handle.ok()) {
        AGENTENV_WARN("startup pack prefetch registration failed: " << handle.error());
        return out;
    }
    if (!handle.value().has_value()) {
        AGENTENV_DEBUG("startup pack prefetch skipped: no bindable layers or cache: "
                       << request.image_config);
        return out;
    }

    {
        std::lock_guard<std::mutex> guard(startup_pack_mutex_);
        // Terminal handles are pruned here rather than on a timer, and a live
        // one is never dropped: a shared memory device's release must not
        // cancel a prefetch other sandboxes are still waiting on.
        std::vector<overlaybd::StartupPackHandle> live;
        live.reserve(startup_pack_handles_.size() + 1);
        for (std::size_t i = 0; i < startup_pack_handles_.size(); ++i) {
            const overlaybd::StartupPackPhase phase = startup_pack_handles_[i].Phase();
            if (phase != overlaybd::kStartupPackDone &&
                phase != overlaybd::kStartupPackFailed &&
                phase != overlaybd::kStartupPackCanceled) {
                live.push_back(startup_pack_handles_[i]);
            }
        }
        live.push_back(*handle.value());
        startup_pack_handles_.swap(live);
    }
    AGENTENV_INFO("startup pack prefetch registered: " << request.image_config);
    return out;
}

// ---------------------------------------------------------------------------
// Startup pack recording
// ---------------------------------------------------------------------------

std::shared_ptr<DevicePlane::PackRecordingHandle> DevicePlane::TakeRecording(
    uint32_t dev_id) {
    std::lock_guard<std::mutex> guard(recordings_mutex_);
    std::map<uint32_t, std::shared_ptr<PackRecordingHandle> >::iterator it =
        pack_recordings_.find(dev_id);
    if (it == pack_recordings_.end()) return std::shared_ptr<PackRecordingHandle>();
    const std::shared_ptr<PackRecordingHandle> handle = it->second;
    pack_recordings_.erase(it);
    return handle;
}

void DevicePlane::DetachRecorderFrom(uint32_t dev_id) {
    // The target handle is copied out under the lock and used after it: the
    // detach itself takes the target's own recorder mutex, and nesting the two
    // would put `table_mutex_` on the wrong side of a lock the read path holds.
    std::shared_ptr<ublk::OverlaybdTarget> target;
    {
        std::lock_guard<std::mutex> guard(table_mutex_);
        std::map<uint32_t, ManagedDevice>::iterator managed = devices_.find(dev_id);
        if (managed != devices_.end()) {
            target = managed->second.target;
        } else {
            std::map<uint32_t, ActiveDevice>::iterator exclusive =
                active_exclusive_.find(dev_id);
            if (exclusive != active_exclusive_.end()) {
                target = exclusive->second.target;
            } else {
                std::map<uint32_t, SharedKey>::iterator reverse =
                    shared_by_dev_id_.find(dev_id);
                if (reverse != shared_by_dev_id_.end()) {
                    std::map<SharedKey, ActiveDevice>::iterator shared =
                        active_shared_.find(reverse->second);
                    if (shared != active_shared_.end()) target = shared->second.target;
                }
            }
        }
    }
    if (target) target->SetRecorder(std::shared_ptr<ublk::StartupPackRecorder>());
}

void DevicePlane::AbortRecordingHandle(
    const std::shared_ptr<PackRecordingHandle>& handle) {
    if (!handle) return;

    // Set before the join, so a window thread that is about to finalize sees
    // it and bails before the rename rather than publishing a trace this call
    // is on its way to delete.
    handle->cancelled.store(true);

    // Rust aborts the task and then takes a `finalize_gate` so file removal
    // cannot race an in-flight rename. A join subsumes both: it returns only
    // once the window thread has left `Finalize`, so by the time the removals
    // below run there is nothing in flight. That is why no gate is ported --
    // `task.abort()` does not wait, and a join does.
    if (handle->window.joinable()) handle->window.join();

    bool done = false;
    {
        std::lock_guard<std::mutex> guard(handle->state_mutex);
        done = handle->state.kind == PackRecordingState::Kind::Done;
    }

    // The tmp file always goes: either the finalize never got to the rename,
    // or it did and the tmp name no longer exists.
    (void)core::fs::RemoveFile(ublk::TmpPackPath(handle->output));
    // The output only goes when this recording did not complete. A pack that
    // finished *before* the cancel arrived is a valid artifact someone may
    // already be consuming, and deleting it would turn a late abort into data
    // loss.
    if (!done) {
        (void)core::fs::RemoveFile(handle->output);
    }
}

void DevicePlane::AbortAllRecordings() {
    std::vector<std::shared_ptr<PackRecordingHandle> > handles;
    {
        std::lock_guard<std::mutex> guard(recordings_mutex_);
        for (std::map<uint32_t, std::shared_ptr<PackRecordingHandle> >::iterator it =
                 pack_recordings_.begin();
             it != pack_recordings_.end(); ++it) {
            handles.push_back(it->second);
        }
        pack_recordings_.clear();
    }
    // Joined outside the lock, one at a time: each is bounded by one tick plus
    // one finalize, and a connection thread must not block on the table lock
    // for the sum of them.
    for (std::size_t i = 0; i < handles.size(); ++i) {
        AbortRecordingHandle(handles[i]);
    }
}

core::Expected<DaemonResponse, HandlerError> DevicePlane::HandleStartPackRecording(
    const DaemonRequest& request) {
    const uint32_t dev_id = request.dev_id;

    // A finished handle is replaced; a live one is never interrupted. Arming
    // twice would detach the first recorder mid-window and leave its output
    // half-written, and the caller has no way to tell that happened.
    {
        std::shared_ptr<PackRecordingHandle> existing;
        {
            std::lock_guard<std::mutex> guard(recordings_mutex_);
            std::map<uint32_t, std::shared_ptr<PackRecordingHandle> >::iterator it =
                pack_recordings_.find(dev_id);
            if (it != pack_recordings_.end()) {
                if (!it->second->Finished()) {
                    std::ostringstream why;
                    why << "pack recording already active on device " << dev_id;
                    return core::make_unexpected(HandlerError::InvalidRequest(why.str()));
                }
                existing = it->second;
                pack_recordings_.erase(it);
            }
        }
        // The superseded handle's thread has already settled, so this only
        // reaps it. Done outside the lock all the same, since `join` is here.
        if (existing && existing->window.joinable()) existing->window.join();
    }

    if (request.max_pages == 0) {
        return core::make_unexpected(HandlerError::InvalidRequest("max_pages must be > 0"));
    }
    if (request.quiet_ms == 0) {
        // Zero would make the quiet check pass on the first tick, ending the
        // window before the guest has faulted anything.
        return core::make_unexpected(HandlerError::InvalidRequest("quiet_ms must be > 0"));
    }
    if (request.min_window_ms > request.max_window_ms) {
        std::ostringstream why;
        why << "min_window_ms " << request.min_window_ms << " exceeds max_window_ms "
            << request.max_window_ms;
        return core::make_unexpected(HandlerError::InvalidRequest(why.str()));
    }

    const core::Optional<std::string> parent = core::fs::Parent(request.output);
    if (!parent.has_value()) {
        return core::make_unexpected(
            HandlerError::InvalidRequest("pack output " + request.output +
                                         " has no parent dir"));
    }
    // Checked now rather than at finalize: the window runs for seconds and
    // the finalize is the last step, so a missing directory discovered there
    // would waste the whole recording and the throwaway VM that produced it.
    if (!core::fs::Exists(*parent)) {
        return core::make_unexpected(
            HandlerError::InvalidRequest("pack output dir " + *parent + " does not exist"));
    }

    // Only `devices_`, and deliberately. A recording VM is given a dedicated,
    // non-shared device for this exact reason: a shared device -- or the
    // block-device page cache behind one -- would answer the guest's first
    // faults without the read ever reaching this target, and those are the
    // reads the trace is made of. A pooled device reaching here means the
    // caller built the wrong kind, which is worth saying.
    std::shared_ptr<ublk::OverlaybdTarget> target;
    std::shared_ptr<overlaybd::ImageFile> image;
    {
        std::lock_guard<std::mutex> guard(table_mutex_);
        std::map<uint32_t, ManagedDevice>::iterator managed = devices_.find(dev_id);
        if (managed != devices_.end()) {
            target = managed->second.target;
            image = managed->second.image;
        }
    }
    if (!target || !image) {
        std::ostringstream why;
        why << "device " << dev_id << " not found";
        return core::make_unexpected(HandlerError::InvalidRequest(why.str()));
    }

    PackRecordingParams params;
    params.output = request.output;
    // Clamped to the trace format's own cap, so a caller asking for more
    // cannot produce a trace the decoder would reject.
    params.max_pages = request.max_pages < overlaybd::startup::kMaxPackPages
                           ? static_cast<std::size_t>(request.max_pages)
                           : static_cast<std::size_t>(overlaybd::startup::kMaxPackPages);
    params.min_window_nanos = request.min_window_ms * 1000000ull;
    params.quiet_nanos = request.quiet_ms * 1000000ull;
    params.max_window_nanos = request.max_window_ms * 1000000ull;

    const uint64_t mem_virtual_size = image->SizeBytes();
    const std::shared_ptr<ublk::StartupPackRecorder> recorder(
        new ublk::StartupPackRecorder(mem_virtual_size, params.max_pages));

    // Armed before the handle is published and before the window starts: the
    // caller arms this *before* loading the snapshot, so every read from here
    // on -- Firecracker's load-time reads included -- has to be observed.
    target->SetRecorder(recorder);

    const std::shared_ptr<PackRecordingHandle> handle(new PackRecordingHandle());
    handle->output = params.output;

    AGENTENV_INFO("dev " << dev_id << ": startup pack recording armed output="
                         << params.output << " mem_virtual_size=" << mem_virtual_size
                         << " max_pages=" << params.max_pages);

    try {
        handle->window = std::thread(&DevicePlane::PackRecordingWindow, this, recorder,
                                     target, params, handle);
    } catch (const std::system_error& err) {
        // Detached again, or the device would keep recording into a window
        // nobody will ever close or finalize.
        target->SetRecorder(std::shared_ptr<ublk::StartupPackRecorder>());
        return core::make_unexpected(HandlerError::Internal(
            std::string("start pack recording window thread: ") + err.what()));
    }

    {
        std::lock_guard<std::mutex> guard(recordings_mutex_);
        pack_recordings_[dev_id] = handle;
    }

    DaemonResponse out;
    out.status = DaemonResponse::Status::Ok;
    return out;
}

core::Expected<DaemonResponse, HandlerError> DevicePlane::HandlePackRecordingStatus(
    uint32_t dev_id) {
    std::shared_ptr<PackRecordingHandle> handle;
    {
        std::lock_guard<std::mutex> guard(recordings_mutex_);
        std::map<uint32_t, std::shared_ptr<PackRecordingHandle> >::iterator it =
            pack_recordings_.find(dev_id);
        if (it != pack_recordings_.end()) handle = it->second;
    }
    if (!handle) {
        // Not an internal error: the caller polls this, and "no recording"
        // is a legitimate answer to a poll for a device whose recording was
        // already aborted or never armed.
        std::ostringstream why;
        why << "no pack recording on device " << dev_id;
        return core::make_unexpected(HandlerError::InvalidRequest(why.str()));
    }

    DaemonResponse out;
    out.status = DaemonResponse::Status::PackRecording;
    {
        std::lock_guard<std::mutex> guard(handle->state_mutex);
        out.state = handle->state;
    }
    return out;
}

core::Expected<DaemonResponse, HandlerError> DevicePlane::HandleAbortPackRecording(
    uint32_t dev_id) {
    const std::shared_ptr<PackRecordingHandle> handle = TakeRecording(dev_id);
    if (!handle) {
        // Answered `Ok`, matching Rust: abort is what a failed snapshot flow
        // calls on its way out, and it must be safe to call for a device whose
        // recording already finished or was never armed.
        DaemonResponse out;
        out.status = DaemonResponse::Status::Ok;
        return out;
    }

    // Detached first, so no further read is observed while the window is
    // being torn down. The window thread detaches too; both are idempotent.
    DetachRecorderFrom(dev_id);
    AbortRecordingHandle(handle);

    AGENTENV_INFO("dev " << dev_id << ": startup pack recording aborted");
    DaemonResponse out;
    out.status = DaemonResponse::Status::Ok;
    return out;
}

void DevicePlane::PackRecordingWindow(std::shared_ptr<ublk::StartupPackRecorder> recorder,
                                      std::shared_ptr<ublk::OverlaybdTarget> target,
                                      PackRecordingParams params,
                                      std::shared_ptr<PackRecordingHandle> handle) {
    // Rust `PACK_RECORDING_TICK`.
    const uint64_t kTickMicros = 100 * 1000;

    ublk::RecordingVerdict verdict = ublk::RecordingVerdict::Continue;
    for (;;) {
        ::usleep(static_cast<useconds_t>(kTickMicros));
        // Cooperative, where Rust uses `task.abort()`. The cost is up to one
        // tick of latency on an abort; the benefit is that the cancellation
        // point is a known place rather than any await, so the detach and the
        // state write below cannot be skipped half-done.
        if (handle->cancelled.load() || stopping_.load()) {
            target->SetRecorder(std::shared_ptr<ublk::StartupPackRecorder>());
            return;
        }
        verdict = recorder->Verdict(params.min_window_nanos, params.quiet_nanos,
                                    params.max_window_nanos);
        if (verdict != ublk::RecordingVerdict::Continue) break;
    }

    // Detached before finalizing, so the page list cannot grow while it is
    // being encoded -- the trace's page count would then disagree with the
    // offsets written after it.
    target->SetRecorder(std::shared_ptr<ublk::StartupPackRecorder>());

    PackRecordingState settled;
    if (verdict == ublk::RecordingVerdict::Fail) {
        // `Verdict` does not produce this today, upstream included. Handled
        // rather than asserted so adding a quality guard stays a one-line
        // change in the recorder.
        AGENTENV_WARN("startup pack recording aborted by guard");
        settled.kind = PackRecordingState::Kind::Failed;
        settled.reason = "recording rejected by quality guard";
    } else {
        const core::Expected<ublk::FinalizeOutcome, std::string> outcome =
            recorder->Finalize(params.output, handle->cancelled);
        if (outcome.ok()) {
            AGENTENV_INFO("startup pack recorded output=" << params.output << " pages="
                          << outcome.value().pages << " bytes=" << outcome.value().bytes
                          << " remote_bytes=" << outcome.value().remote_bytes
                          << " skipped_high_pages=" << outcome.value().skipped_high_pages);
            settled.kind = PackRecordingState::Kind::Done;
            settled.pages = outcome.value().pages;
            settled.bytes = outcome.value().bytes;
            settled.remote_bytes = outcome.value().remote_bytes;
            settled.path = params.output;
        } else {
            AGENTENV_WARN("startup pack packaging failed: " << outcome.error());
            // The tmp file is this thread's to clean when it failed on its
            // own; the abort path only owns it once a cancel is in play.
            (void)core::fs::RemoveFile(ublk::TmpPackPath(params.output));
            settled.kind = PackRecordingState::Kind::Failed;
            settled.reason = outcome.error();
        }
    }

    // Published last. `PackRecordingStatus` reads this, and the caller treats
    // anything other than `Recording` as "the artifact question is settled",
    // so it must not be visible before the file is in place.
    std::lock_guard<std::mutex> guard(handle->state_mutex);
    handle->state = settled;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void DevicePlane::StopAllDevices() {
    stopping_.store(true);
    {
        std::lock_guard<std::mutex> guard(refill_mutex_);
        if (refill_thread_.joinable()) refill_thread_.join();
    }

    // Before the devices go. Every window thread holds a `shared_ptr` to a
    // target, so a device stopped underneath a live recording would be kept
    // alive by it; and a recording left running past shutdown would have no
    // one to report its result to.
    AbortAllRecordings();

    // Everything is moved out of the maps first, under one lock, so a request
    // racing shutdown either finds its device or does not -- it never finds a
    // device that is halfway torn down.
    std::vector<DetachedDevice> doomed;
    {
        std::lock_guard<std::mutex> guard(table_mutex_);
        for (std::map<uint32_t, ManagedDevice>::iterator it = devices_.begin();
             it != devices_.end(); ++it) {
            DetachedDevice d;
            d.dev = std::move(it->second.dev);
            d.target = std::move(it->second.target);
            d.image = std::move(it->second.image);
            d.dev_id = it->first;
            doomed.push_back(std::move(d));
        }
        devices_.clear();

        for (std::map<uint32_t, ActiveDevice>::iterator it = active_exclusive_.begin();
             it != active_exclusive_.end(); ++it) {
            DetachedDevice d;
            d.dev = std::move(it->second.dev);
            d.target = std::move(it->second.target);
            d.image = std::move(it->second.image);
            d.dev_id = it->first;
            doomed.push_back(std::move(d));
        }
        active_exclusive_.clear();

        for (std::map<SharedKey, ActiveDevice>::iterator it = active_shared_.begin();
             it != active_shared_.end(); ++it) {
            DetachedDevice d;
            d.dev_id = it->second.dev ? it->second.dev->DevId() : 0;
            d.dev = std::move(it->second.dev);
            d.target = std::move(it->second.target);
            d.image = std::move(it->second.image);
            doomed.push_back(std::move(d));
        }
        active_shared_.clear();
        shared_by_dev_id_.clear();
    }

    if (idle_) {
        // `DrainAll` also marks the pool shut down, so a release that arrives
        // after this cannot park a device nobody will ever stop.
        std::vector<PooledDevice> parked = idle_->DrainAll();
        for (std::size_t i = 0; i < parked.size(); ++i) {
            DetachedDevice d;
            d.dev_id = parked[i].dev ? parked[i].dev->DevId() : 0;
            d.dev = std::move(parked[i].dev);
            d.target = std::move(parked[i].target);
            d.image = std::move(parked[i].placeholder_image);
            doomed.push_back(std::move(d));
        }
    }

    // Sequential, where Rust overlaps 16 at a time. Its reason for the
    // concurrency -- each STOP_DEV/DEL_DEV waits on block-layer RCU grace
    // periods -- does not apply here: those commands go through the one
    // control ring under `ctrl_mutex_`, so the waits cannot overlap no matter
    // how many threads ask. Spawning threads would add contention and no
    // speedup.
    for (std::size_t i = 0; i < doomed.size(); ++i) {
        AGENTENV_INFO("dev " << doomed[i].dev_id << ": stopping device during shutdown");
        StopAndDeleteDevice(std::move(doomed[i]));
    }

    {
        std::lock_guard<std::mutex> guard(placeholder_mutex_);
        placeholders_.clear();
    }
    {
        std::lock_guard<std::mutex> guard(startup_pack_mutex_);
        startup_pack_handles_.clear();
    }
}

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
