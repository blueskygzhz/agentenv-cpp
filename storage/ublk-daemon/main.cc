// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/main.rs
//
// What this binary does and does not do:
//
// The process-level half of `main` is ported in full -- argument parsing,
// tracing, the descriptor limit, config resolution, the parent pidfd, and the
// three ways the daemon is asked to stop (SIGTERM, SIGINT, parent exit). The
// socket half of `UblkDaemonServer` is ported too, in `server.cc`: it binds,
// accepts, frames, dispatches, and rolls back a device handoff the client
// never received.
//
// What is still missing is the *device plane* -- the io_uring control ring and
// a real overlaybd ublk target (`storage/ublk/impls.cc` is a skeleton). So
// this binary installs a handler that refuses every device request by name.
//
// That is deliberately different from the previous version, which refused to
// start at all. A daemon that does not listen makes every client block on a
// socket nobody is accepting on, and the operator learns nothing; a daemon
// that listens and answers "the overlaybd ublk target is not ported" tells
// them exactly what is wrong, at the request that needed it. What it must
// never do is the third option -- answer success -- and it does not.
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/storage/overlaybd/download_gate.h"
#include "agentenv/storage/ublk-daemon/config.h"
#include "agentenv/storage/ublk-daemon/lifecycle.h"
#include "agentenv/storage/ublk_daemon/server.h"

namespace ud = agentenv::storage::ublk_daemon;
namespace core = agentenv::core;

namespace {

/// The device plane, as far as it exists.
///
/// Every request that would touch a ublk device is refused with the specific
/// missing piece named, rather than with a generic "unimplemented": the
/// protocol, the client, the device control commands and the runtime
/// materialization are all ported, and an operator chasing a sandbox failure
/// needs to know it is the data plane that is absent.
///
/// `NotifySandboxReady` is answered for real. It releases held background
/// downloads and touches no device at all -- Rust handles it in
/// `handle_connection` for the same reason -- so refusing it would be
/// inventing a failure.
class UnportedDeviceHandler : public ud::DeviceHandler {
 public:
    explicit UnportedDeviceHandler(bool has_pool) : has_pool_(has_pool) {}

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
            "ublk device plane is not ported in this build: it needs an io_uring "
            "control ring and a real overlaybd ublk target (storage/ublk/impls.cc "
            "is a skeleton that serves zeroes and refuses writes). The daemon "
            "protocol, client, device control commands and runtime materialization "
            "are ported; only the data plane is not."));
    }

    /// Reported truthfully even though no device can be created, so the
    /// rollback path cannot start answering differently once it can.
    bool HasPool() const { return has_pool_; }

    void StopAllDevices() {}

 private:
    bool has_pool_;
};

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

    // 7. Serve. The device plane is absent, and the handler says so per
    //    request; see the file header for why that beats refusing to start.
    AGENTENV_WARN(
        "this build serves the daemon protocol but has no ublk device plane: "
        "every create/acquire/delete/restack request will be answered with an "
        "error naming storage/ublk/impls.cc. Only notify_sandbox_ready and "
        "shutdown are fully functional.");

    UnportedDeviceHandler handler(pool_config.value().has_value());
    ud::UblkDaemonServer server(cli.value().socket_path, &handler);
    const core::Expected<core::Unit, std::string> ran = server.Run(parent_pidfd, PrintReady);
    if (!ran.ok()) {
        AGENTENV_ERROR(ran.error());
        return 1;
    }
    return 0;
}
