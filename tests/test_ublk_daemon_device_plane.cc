// SPDX-License-Identifier: MIT
// Tests for storage/ublk-daemon's device plane, ported against the device half
// of `storage/ublk-daemon/src/server.rs`.
//
// Adding a device needs `/dev/ublk-control` and an `sqe128` io_uring, so the
// cases that genuinely require one are guarded. But most of what the plane
// decides happens *before* it reaches a device -- which request families need
// a pool, which device ids it knows, which are refused, what a lookup miss is
// classified as -- and all of that is reachable anywhere, because
// `WithControlRing` takes the ring as an argument the way Rust's
// `UblkDaemonServer::new` does. Those are the decisions where being wrong is
// hardest to notice, since the error a client sees looks the same either way.
#include "agentenv/storage/ublk-daemon/device_plane.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <memory>
#include <string>

#include "agentenv/core/fs.h"
#include "microtest.h"

namespace ud = agentenv::storage::ublk_daemon;
namespace core = agentenv::core;

namespace {

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

struct Scratch {
    std::string dir;

    Scratch() {
        const core::Expected<std::string, std::string> made =
            core::fs::CreateTempDir("agentenv-device-plane-test");
        if (made.ok()) dir = made.value();
        core::fs::Write(dir + "/global.json", "{}");
        core::fs::Write(dir + "/resize.json", "{}");
    }
    ~Scratch() {
        if (!dir.empty()) core::fs::RemoveDirAll(dir);
    }
};

ud::DevicePlaneOptions Options(const std::string& dir, bool pool) {
    ud::DevicePlaneOptions options;
    options.global_config = dir + "/global.json";
    options.resize_global_config = dir + "/resize.json";
    options.placeholder_dir = dir + "/placeholders";
    if (pool) {
        agentenv::warmpool::PoolConfig config;
        config.low_watermark = 1;
        config.high_watermark = 2;
        // Always false, as `LoadPoolConfig` sets it: the daemon refills inline
        // from acquire and release, and the generic background worker would
        // fight that.
        config.maintenance_enabled = false;
        options.pool_config = core::Optional<agentenv::warmpool::PoolConfig>(config);
    }
    return options;
}

/// A plane with no control ring: it can look devices up and classify requests,
/// and refuses anything that would submit a control command.
std::unique_ptr<ud::DevicePlane> RinglessPlane(const std::string& dir, bool pool) {
    return ud::DevicePlane::WithControlRing(Options(dir, pool),
                                            std::shared_ptr<agentenv::storage::util::Ring>(), 0);
}

bool HaveUblk() { return ud::OpenControlRing().ok(); }

void Skip(const char* what, const char* why) {
    std::printf("       SKIP %s: %s\n", what, why);
}

}  // namespace

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------

MT_TEST(open_either_succeeds_or_names_the_host_facility) {
    Scratch scratch;
    const core::Expected<std::unique_ptr<ud::DevicePlane>, std::string> plane =
        ud::DevicePlane::Open(Options(scratch.dir, false));

    if (plane.ok()) {
        MT_EXPECT_TRUE(!plane.value()->HasPool());
        return;
    }

    // The failure must say which host facility is missing. This error is what
    // an operator sees on every request for the lifetime of the daemon, and a
    // bare errno from a path that opens a ring, a control node and a char
    // device names none of the three.
    MT_EXPECT_TRUE(Contains(plane.error(), "ublk"));
    MT_EXPECT_TRUE(Contains(plane.error(), "io_uring") ||
                   Contains(plane.error(), "control"));
}

MT_TEST(has_pool_follows_the_configuration) {
    Scratch off;
    MT_EXPECT_TRUE(!RinglessPlane(off.dir, false)->HasPool());

    Scratch on;
    // Not cosmetic: `ServeConnection`'s rollback reads this to choose between
    // releasing one acquisition and deleting the device outright, and the two
    // are not interchangeable.
    MT_EXPECT_TRUE(RinglessPlane(on.dir, true)->HasPool());
}

MT_TEST(get_features_reports_zero_until_a_pool_can_use_them) {
    Scratch no_pool;
    const std::unique_ptr<ud::DevicePlane> without = RinglessPlane(no_pool.dir, false);
    ud::DaemonRequest request;
    request.kind = ud::DaemonRequest::Kind::GetFeatures;

    const core::Expected<ud::DaemonResponse, ud::HandlerError> answer =
        without->Handle(request);
    MT_EXPECT_TRUE(answer.ok());
    MT_EXPECT_TRUE(answer.value().status == ud::DaemonResponse::Status::Features);
    // Zero with the pool off, matching Rust. The one caller reads the bitmap
    // to decide whether it may ask for `UpdateSize`, which is a pool-only
    // operation -- so advertising the kernel's real features here would invite
    // a request that is refused anyway.
    MT_EXPECT_EQ(answer.value().flags, static_cast<uint64_t>(0));

    Scratch pooled;
    const std::unique_ptr<ud::DevicePlane> with = ud::DevicePlane::WithControlRing(
        Options(pooled.dir, true), std::shared_ptr<agentenv::storage::util::Ring>(), 0x40);
    const core::Expected<ud::DaemonResponse, ud::HandlerError> pooled_answer =
        with->Handle(request);
    MT_EXPECT_TRUE(pooled_answer.ok());
    MT_EXPECT_EQ(pooled_answer.value().flags, static_cast<uint64_t>(0x40));
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

MT_TEST(shutdown_is_refused_by_the_device_plane) {
    Scratch scratch;
    const std::unique_ptr<ud::DevicePlane> plane = RinglessPlane(scratch.dir, false);

    // `ServeConnection` answers `Shutdown` itself, because stopping the accept
    // loop is not a device operation. Reaching the plane with one means that
    // dispatch changed, and answering it here would stop nothing while
    // reporting success.
    ud::DaemonRequest request;
    request.kind = ud::DaemonRequest::Kind::Shutdown;
    const core::Expected<ud::DaemonResponse, ud::HandlerError> answer =
        plane->Handle(request);
    MT_EXPECT_TRUE(!answer.ok());
    MT_EXPECT_TRUE(Contains(answer.error().message, "connection loop"));
}

MT_TEST(pool_only_requests_are_refused_when_the_pool_is_off) {
    Scratch scratch;
    const std::unique_ptr<ud::DevicePlane> plane = RinglessPlane(scratch.dir, false);

    // Acquire, release and resize are all pool operations. With no pool they
    // must be refused rather than quietly falling back to the raw create path:
    // a client that acquired would later release, and a raw device returned to
    // a pool that never held it is lost.
    const ud::DaemonRequest::Kind pool_only[3] = {
        ud::DaemonRequest::Kind::AcquireOverlaybd,
        ud::DaemonRequest::Kind::ReleaseOverlaybd,
        ud::DaemonRequest::Kind::UpdateSize,
    };
    for (int i = 0; i < 3; ++i) {
        ud::DaemonRequest request;
        request.kind = pool_only[i];
        request.dev_id = 7;
        request.image_config = "/nonexistent/image.json";
        const core::Expected<ud::DaemonResponse, ud::HandlerError> answer =
            plane->Handle(request);
        MT_EXPECT_TRUE(!answer.ok());
        MT_EXPECT_TRUE(Contains(answer.error().message, "warm pool not enabled"));
    }
}

MT_TEST(update_size_is_refused_without_the_kernel_feature) {
    Scratch scratch;
    // Pool on, but a kernel that reports no features.
    const std::unique_ptr<ud::DevicePlane> plane = RinglessPlane(scratch.dir, true);

    ud::DaemonRequest request;
    request.kind = ud::DaemonRequest::Kind::UpdateSize;
    request.dev_id = 7;
    request.new_sectors = 2048;
    const core::Expected<ud::DaemonResponse, ud::HandlerError> answer =
        plane->Handle(request);
    MT_EXPECT_TRUE(!answer.ok());
    // Refused on the feature, not on the unknown device id: the check has to
    // come first, or the message would send the operator looking for a device
    // on a kernel that could never have resized it anyway.
    MT_EXPECT_TRUE(Contains(answer.error().message, "UBLK_F_UPDATE_SIZE"));
}

MT_TEST(unknown_device_ids_are_errors_not_successes) {
    Scratch scratch;
    const std::unique_ptr<ud::DevicePlane> plane = RinglessPlane(scratch.dir, true);

    // Answering `Deleted` or `Released` for a device the tables never had
    // would tell the caller its device is gone while any real device stayed
    // behind. Every lookup miss is reported.
    const ud::DaemonRequest::Kind by_id[2] = {
        ud::DaemonRequest::Kind::Delete,
        ud::DaemonRequest::Kind::ReleaseOverlaybd,
    };
    for (int i = 0; i < 2; ++i) {
        ud::DaemonRequest request;
        request.kind = by_id[i];
        request.dev_id = 4242;
        const core::Expected<ud::DaemonResponse, ud::HandlerError> answer =
            plane->Handle(request);
        MT_EXPECT_TRUE(!answer.ok());
        MT_EXPECT_TRUE(Contains(answer.error().message, "not found"));
    }
}

MT_TEST(restacking_an_unknown_device_is_recoverable_not_terminal) {
    Scratch scratch;
    const std::unique_ptr<ud::DevicePlane> plane = RinglessPlane(scratch.dir, false);

    ud::DaemonRequest request;
    request.kind = ud::DaemonRequest::Kind::RestackSnapshot;
    request.dev_id = 4242;
    request.output_layer_path = scratch.dir + "/out.lsmt";
    const core::Expected<ud::DaemonResponse, ud::HandlerError> answer =
        plane->Handle(request);

    MT_EXPECT_TRUE(!answer.ok());
    MT_EXPECT_TRUE(Contains(answer.error().message, "not found"));
    // `kInternal`, not `kTerminal`. The distinction is the caller's retry
    // decision: terminal means the live layer stack was already mutated and
    // retrying would compound it, which is not what a lookup miss is.
    MT_EXPECT_TRUE(answer.error().kind == ud::HandlerError::kInternal);
    // And nothing was written, so there is no half-built layer for a retry to
    // trip over.
    MT_EXPECT_TRUE(!core::fs::Exists(request.output_layer_path));
}

MT_TEST(notify_sandbox_ready_needs_no_device) {
    Scratch scratch;
    const std::unique_ptr<ud::DevicePlane> plane = RinglessPlane(scratch.dir, false);

    // It releases held background downloads and touches no device, so even a
    // plane that cannot create one must answer it. Refusing would invent a
    // failure, and the sandbox would wait out the download fallback timeout.
    ud::DaemonRequest request;
    request.kind = ud::DaemonRequest::Kind::NotifySandboxReady;
    request.device_key = "sandbox-under-test";
    const core::Expected<ud::DaemonResponse, ud::HandlerError> answer =
        plane->Handle(request);
    MT_EXPECT_TRUE(answer.ok());
    MT_EXPECT_TRUE(answer.value().status == ud::DaemonResponse::Status::Ok);
}

MT_TEST(prefetch_startup_pack_never_fails_the_request) {
    Scratch scratch;
    const std::unique_ptr<ud::DevicePlane> plane = RinglessPlane(scratch.dir, false);

    // The image does not exist, so every step of the prefetch will fail. It
    // still answers `Ok`: on-demand reads serve the same blocks, so failing
    // the request would turn a missed optimisation into a failed resume.
    ud::DaemonRequest request;
    request.kind = ud::DaemonRequest::Kind::PrefetchStartupPack;
    request.image_config = scratch.dir + "/nonexistent.json";
    request.url = "http://127.0.0.1:1/pack";
    request.pack_size = 4096;
    request.mem_virtual_size = 1 << 20;
    request.timeout_secs = 1;
    const core::Expected<ud::DaemonResponse, ud::HandlerError> answer =
        plane->Handle(request);
    MT_EXPECT_TRUE(answer.ok());
    MT_EXPECT_TRUE(answer.value().status == ud::DaemonResponse::Status::Ok);
}

MT_TEST(pack_recording_is_refused_by_name) {
    Scratch scratch;
    const std::unique_ptr<ud::DevicePlane> plane = RinglessPlane(scratch.dir, true);

    // The one request family still unported. It must be refused with the
    // missing piece named, and must never answer success: arming nothing and
    // then reporting a pack that does not exist is worse than refusing, since
    // the caller would go looking for the artifact.
    const ud::DaemonRequest::Kind recording[3] = {
        ud::DaemonRequest::Kind::StartPackRecording,
        ud::DaemonRequest::Kind::PackRecordingStatus,
        ud::DaemonRequest::Kind::AbortPackRecording,
    };
    for (int i = 0; i < 3; ++i) {
        ud::DaemonRequest request;
        request.kind = recording[i];
        request.dev_id = 1;
        const core::Expected<ud::DaemonResponse, ud::HandlerError> answer =
            plane->Handle(request);
        MT_EXPECT_TRUE(!answer.ok());
        MT_EXPECT_TRUE(Contains(answer.error().message, "StartupPackRecorder"));
    }
}

MT_TEST(creating_from_a_missing_image_names_the_image) {
    Scratch scratch;
    const std::unique_ptr<ud::DevicePlane> plane = RinglessPlane(scratch.dir, false);

    // The image is opened before any control command is issued, so this is
    // refused without a device ever being added -- which is the ordering that
    // matters: a create that reached ADD_DEV first would have to unwind it.
    ud::DaemonRequest request;
    request.kind = ud::DaemonRequest::Kind::CreateOverlaybd;
    request.image_config = scratch.dir + "/nonexistent.json";
    const core::Expected<ud::DaemonResponse, ud::HandlerError> answer =
        plane->Handle(request);
    MT_EXPECT_TRUE(!answer.ok());
    // The path is in the message. The daemon serves many images and the
    // caller's request named only this one, so an error without it cannot be
    // traced back to a sandbox.
    MT_EXPECT_TRUE(Contains(answer.error().message, "nonexistent.json"));
}

MT_TEST(stop_all_devices_is_safe_and_idempotent) {
    Scratch scratch;
    std::unique_ptr<ud::DevicePlane> plane = RinglessPlane(scratch.dir, true);

    // Called by `Run` after the accept loop exits *and* by the destructor, so
    // it has to be idempotent. A second pass that re-walked emptied maps, or
    // re-joined a joined thread, would abort at shutdown -- the one moment a
    // crash looks like an unrelated bug.
    plane->StopAllDevices();
    plane->StopAllDevices();
    plane.reset();
}

// ---------------------------------------------------------------------------
// Best-effort helpers
// ---------------------------------------------------------------------------

MT_TEST(clear_page_cache_tolerates_what_it_cannot_flush) {
    Scratch scratch;

    // A regular file answers BLKFLSBUF with ENOTTY and a missing path fails
    // the open. Both run on the release path, and both must stay warnings: a
    // release that aborted over a cache hint would leave the device in neither
    // the active map nor the pool.
    const std::string regular = scratch.dir + "/not-a-block-device";
    MT_EXPECT_TRUE(core::fs::Write(regular, "x").ok());
    ud::ClearPageCache(regular);
    ud::ClearPageCache(scratch.dir + "/missing");
    ud::ClearPageCache("");

    // Reaching here at all is the assertion.
    MT_EXPECT_TRUE(core::fs::Exists(regular));
}

MT_TEST(wait_for_ublk_dev_reports_the_path_it_gave_up_on) {
    if (HaveUblk()) {
        // With a real ublk module an unused id may be claimed between the
        // check and the wait, and the negative path costs the full ~30 s
        // retry budget.
        Skip("wait_for_ublk_dev_reports_the_path_it_gave_up_on",
             "host has ublk; the negative path would wait out the full retry budget");
        return;
    }

    // No module, so the node cannot appear and the whole budget elapses. The
    // message has to name the node: the caller knows only a device id, and
    // "timed out" alone does not say whether the kernel or udev was the one
    // that never delivered.
    const core::Expected<core::Unit, std::string> waited = ud::WaitForUblkDev(4242);
    MT_EXPECT_TRUE(!waited.ok());
    MT_EXPECT_TRUE(Contains(waited.error(), "/dev/ublkb4242"));
}

MT_MAIN
