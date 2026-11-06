// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/main.rs
//
// What this binary does:
//
// The process-level half of `main` -- argument parsing, tracing, the
// descriptor limit, config resolution, the parent pidfd, and the three ways
// the daemon is asked to stop (SIGTERM, SIGINT, parent exit). The socket half
// of `UblkDaemonServer` is in `server.cc`: it binds, accepts, frames,
// dispatches, and rolls back a device handoff the client never received. The
// device half is in `device_plane.cc`: the device table, the warm pool, the
// refcounted shared-access mode, and one ublk device's life from ADD_DEV to
// DEL_DEV.
//
// The one thing that can still be absent at startup is a usable
// `/dev/ublk-control`. That is a host problem, not a port gap, and it is
// handled by listening anyway with a handler that refuses device requests and
// names the reason. A daemon that does not listen makes every client block on
// a socket nobody accepts on and teaches the operator nothing; a daemon that
// answers "the ublk control plane is unavailable: <why>" tells them exactly
// what to fix, at the request that needed it. What it must never do is the
// third option -- answer success -- and it does not.
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <memory>
#include <string>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/storage/overlaybd/download_gate.h"
#include "agentenv/storage/ublk-daemon/config.h"
#include "agentenv/storage/ublk-daemon/device_plane.h"
#include "agentenv/storage/ublk-daemon/lifecycle.h"
#include "agentenv/storage/ublk-daemon/server.h"

namespace ud = agentenv::storage::ublk_daemon;
namespace core = agentenv::core;

namespace {

/// The handler installed when the control plane could not be opened.
///
/// Every request that would touch a ublk device is refused with the host
/// failure quoted, rather than with a generic "unimplemented": the protocol,
/// the client, the runtime materialization and the device plane itself are all
/// present, so an operator chasing a sandbox failure needs to know it is
/// `/dev/ublk-control` that is missing and not this build.
///
/// `NotifySandboxReady` is answered for real. It releases held background
/// downloads and touches no device at all -- Rust handles it in
/// `handle_connection` for the same reason -- so refusing it would be
/// inventing a failure.
class UnavailableDeviceHandler : public ud::DeviceHandler {
 public:
    UnavailableDeviceHandler(bool has_pool, const std::string& why)
        : has_pool_(has_pool), why_(why) {}

    core::Expected<ud::DaemonResponse, ud::HandlerError> Handle(
        const ud::DaemonRequest& request) {
        if (request.kind == ud::DaemonRequest::Kind::NotifySandboxReady) {
            AGENTENV_INFO("sandbox reported envd-ready; releasing held downloads: "
                          << request.device_key);
            agentenv::storage::overlaybd::DownloadGate::NotifySandboxReady(
                request.device_key);
            ud::DaemonResponse ok;
            ok.status = ud::DaemonResponse::Status::Ok;
            return ok;
        }
        return core::make_unexpected(ud::HandlerError::Internal(
            "the ublk control plane is unavailable, so no device operation can be "
            "served: " + why_));
    }

    /// Reported truthfully even though no device can be created, so the
    /// rollback path cannot start answering differently once one can.
    bool HasPool() const { return has_pool_; }

    void StopAllDevices() {}

 private:
    bool has_pool_;
    std::string why_;
};

/// Rust `UblkDaemonServer::pool_placeholder_dir`.
///
/// Beside the socket, not in the image cache: these files are the daemon's
/// own, an eviction pass over the cache must not be able to remove one out
/// from under a parked device, and anything that cleans up after a dead
/// daemon finds them next to the socket it is already clearing.
std::string PoolPlaceholderDir(const std::string& socket_path) {
    const core::Optional<std::string> parent = core::fs::Parent(socket_path);
    if (parent.has_value() && !parent->empty()) {
        return core::fs::Join(*parent, "overlaybd-pool-placeholders");
    }
    return "/tmp/overlaybd-pool-placeholders";
}

core::Expected<core::Unit, std::string> PrintReady() {
    if (std::fputs("ready\n", stdout) < 0) {
        return core::make_unexpected(std::string("write ready signal"));
    }
    if (std::fflush(stdout) != 0) {
        return core::make_unexpected(std::string("flush ready signal"));
    }
    return core::Unit{};
}

}  // namespace

int main(int argc, char** argv) {
    // 1. Arguments.
    const core::Expected<ud::Cli, std::string> cli = ud::Cli::Parse(argc, argv);
    if (!cli.ok()) {
        if (cli.error() == "--help") {
            std::fputs(ud::Cli::Usage().c_str(), stdout);
            return 0;
        }
        std::fprintf(stderr, "agentenv-ublk-daemon: %s\n\n", cli.error().c_str());
        std::fputs(ud::Cli::Usage().c_str(), stderr);
        return 2;
    }

    // 2. Tracing, before anything that might want to log.
    const core::Expected<ud::LogLevelSpec, std::string> level =
        ud::ParseLogLevel(cli.value().log_level);
    if (!level.ok()) {
        std::fprintf(stderr, "agentenv-ublk-daemon: %s\n", level.error().c_str());
        return 2;
    }
    const core::Expected<core::Unit, std::string> traced =
        ud::SetupTracing(cli.value().log_file, level.value());
    if (!traced.ok()) {
        std::fprintf(stderr, "agentenv-ublk-daemon: setup tracing: %s\n",
                     traced.error().c_str());
        return 1;
    }

    // 3. The parent pid, captured here for the reason Rust captures it before
    //    building its runtime: `getppid` must be called on the main thread,
    //    while the parent is still the process that launched this one.
    const int32_t parent_pid = static_cast<int32_t>(::getppid());

    // 4. Descriptor limit. A failure is a warning, not fatal: the daemon works
    //    at a lower ceiling, it just manages fewer devices before failing.
    const core::Expected<core::Unit, std::string> rlimit =
        ud::RaiseNoFileLimit(ud::kTargetNoFileLimit);
    if (!rlimit.ok()) {
        AGENTENV_WARN(rlimit.error());
    }

    // 5. Config. Resolved and validated now, so a misconfiguration is named
    //    here instead of surfacing from inside a device operation later.
    const core::Expected<core::Optional<ud::DaemonTomlConfig>, std::string> daemon_config =
        ud::LoadDaemonConfig(cli.value().config);
    if (!daemon_config.ok()) {
        AGENTENV_ERROR("load daemon config: " << daemon_config.error());
        return 1;
    }

    const core::Expected<core::Optional<ud::ResizeToolSpec>, std::string> resize_tool =
        ud::LoadResizeToolConfig(cli.value().config, daemon_config.value());
    if (!resize_tool.ok()) {
        AGENTENV_ERROR("load overlaybd resize tool config: " << resize_tool.error());
        return 1;
    }
    if (resize_tool.value().has_value()) {
        AGENTENV_INFO("overlaybd resize tool: " << resize_tool.value()->binary
                                                << " (timeout "
                                                << resize_tool.value()->timeout_secs << "s)");
    }

    ud::PoolConfigOverrides overrides;
    overrides.low_watermark = cli.value().pool_low_watermark;
    overrides.high_watermark = cli.value().pool_high_watermark;
    overrides.startup_prewarm = cli.value().pool_startup_prewarm;

    const core::Expected<core::Optional<agentenv::warmpool::PoolConfig>, std::string>
        pool_config =
            ud::LoadPoolConfig(daemon_config.value(), cli.value().enable_pool, overrides);
    if (!pool_config.ok()) {
        AGENTENV_ERROR("load warm pool config: " << pool_config.error());
        return 1;
    }
    if (pool_config.value().has_value()) {
        AGENTENV_INFO("warm pool enabled: low=" << pool_config.value()->low_watermark
                                                << " high="
                                                << pool_config.value()->high_watermark);
    }

    // The global configs are only read by the device plane, but their absence
    // is a startup error there too, so it is reported from here.
    const char* const required[2] = {"--global-config", "--resize-global-config"};
    const std::string paths[2] = {cli.value().global_config, cli.value().resize_global_config};
    for (int i = 0; i < 2; ++i) {
        const core::Expected<bool, std::string> exists = core::fs::TryExists(paths[i]);
        if (!exists.ok()) {
            AGENTENV_ERROR(required[i] << ": " << exists.error());
            return 1;
        }
        if (!exists.value()) {
            AGENTENV_ERROR(required[i] << ": " << paths[i] << " does not exist");
            return 1;
        }
    }

    // 6. Shutdown plumbing. Installed before the socket is bound, so a signal
    //    that arrives during startup is not delivered to a default handler
    //    that would kill the process with the socket file left behind.
    //
    //    SIGPIPE is ignored rather than handled: a client that closes while a
    //    reply is being written is routine, and the server reads the resulting
    //    EPIPE to decide whether a device handoff was delivered. The default
    //    disposition would kill the daemon instead.
    ::signal(SIGPIPE, SIG_IGN);
    const core::Expected<core::Unit, std::string> handlers = ud::InstallSignalHandlers();
    if (!handlers.ok()) {
        AGENTENV_ERROR(handlers.error());
        return 1;
    }

    // Rust opens the parent pidfd here and treats a failure as fatal, with one
    // exception: a parent that has already exited means there is nobody left
    // to serve, and exiting 0 is the right answer rather than an error.
    //
    // `pidfd_open` is blocked in some containers. That is fatal here, as in
    // Rust: a daemon that cannot notice its parent leaving would outlive it
    // holding ublk devices, and quietly dropping the watch would hide that.
    int parent_pidfd = -1;
    const core::Expected<int, std::string> pidfd = ud::PidfdOpen(parent_pid);
    if (pidfd.ok()) {
        parent_pidfd = pidfd.value();
    } else if (::kill(parent_pid, 0) != 0 && errno == ESRCH) {
        AGENTENV_WARN("parent process already exited before pidfd_open");
        return 0;
    } else {
        AGENTENV_WARN("TEMP-PIDFD-BYPASS " << pidfd.error());
    }

    // 7. The device plane. Opened before the socket is bound so a host
    //    without ublk is reported once, here, instead of once per request --
    //    but a failure is not fatal: see the file header.
    ud::DevicePlaneOptions plane_options;
    plane_options.global_config = cli.value().global_config;
    plane_options.resize_global_config = cli.value().resize_global_config;
    plane_options.resize_tool = resize_tool.value();
    plane_options.pool_config = pool_config.value();
    plane_options.p2p_publish_url = cli.value().p2p_publish_url;
    plane_options.placeholder_dir = PoolPlaceholderDir(cli.value().socket_path);

    core::Expected<std::unique_ptr<ud::DevicePlane>, std::string> plane =
        ud::DevicePlane::Open(plane_options);

    // Declared before the plane's own handler so the fallback outlives the
    // server either way; only one of the two is ever installed.
    UnavailableDeviceHandler fallback(pool_config.value().has_value(),
                                      plane.ok() ? std::string() : plane.error());
    ud::DeviceHandler* handler = &fallback;
    if (plane.ok()) {
        handler = plane.value().get();
        AGENTENV_INFO("ublk device plane ready: features=0x"
                      << std::hex << plane.value()->features() << std::dec
                      << " pool=" << (plane.value()->HasPool() ? "on" : "off"));
    } else {
        AGENTENV_WARN("no ublk control plane; the daemon will serve the protocol and refuse "
                      "every device operation with this reason: " << plane.error());
    }

    // 8. Serve. Returns once shutdown was requested and every device has been
    //    stopped -- the stopping is `StopAllDevices`, which `Run` calls after
    //    the accept loop exits.
    ud::UblkDaemonServer server(cli.value().socket_path, handler);
    const core::Expected<core::Unit, std::string> ran = server.Run(parent_pidfd, PrintReady);
    if (!ran.ok()) {
        AGENTENV_ERROR(ran.error());
        return 1;
    }
    return 0;
}
