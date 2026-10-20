// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/main.rs
//
// What this binary does and does not do:
//
// The process-level half of `main` is ported in full -- argument parsing,
// tracing, the descriptor limit, config resolution, and the three ways the
// daemon is asked to stop (SIGTERM, SIGINT, parent exit). The serving half is
// not: `UblkDaemonServer` drives real ublk devices through an io_uring control
// ring, and this port's ublk targets are skeletons (`storage/ublk/impls.cc`).
//
// So it refuses, loudly and with a non-zero status, at exactly the point where
// it would otherwise start pretending. The previous version of this file
// printed a line and returned 0, which is worse than useless: a supervisor
// reads that as a daemon that came up, and every client then blocks on a
// socket that nobody is listening to.
//
// Everything before the refusal still runs, because that is what makes the
// refusal useful: a bad flag, an unparsable TOML or a zero resize timeout is
// reported here rather than waiting for the serving half to exist.
#include <unistd.h>

#include <cstdio>
#include <string>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/storage/ublk-daemon/config.h"
#include "agentenv/storage/ublk-daemon/lifecycle.h"

namespace ud = agentenv::storage::ublk_daemon;
namespace core = agentenv::core;

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

    // The global configs are only read by the serving half, but their absence
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

    // 6. The refusal. Named precisely, because the gap is a specific missing
    //    piece and not a general "unimplemented": the protocol and client are
    //    ported (`protocol.cc`, `client.cc`), the device control path exists
    //    (`storage/ublk/ctrl.cc`), and what is absent is the server loop plus
    //    the overlaybd ublk target it would drive.
    //
    //    This comes before the shutdown plumbing on purpose. Rust opens the
    //    parent pidfd here and treats a failure as fatal, but it does so to
    //    hand the descriptor to a running server. With no server, opening one
    //    and closing it again would be a fatal check on a resource this
    //    process immediately discards -- and on a host without
    //    `pidfd_open` (containers often block it) that check would mask the
    //    message the operator actually needs. `PidfdOpen`,
    //    `InstallSignalHandlers` and `WaitForShutdown` are implemented and
    //    waiting in `lifecycle.cc` for the server that will use them.
    //
    //    No "ready" line is printed. Rust prints it on stdout once the socket
    //    is accepting, and a supervisor waits for it -- emitting it here would
    //    tell the supervisor to start sending requests.
    AGENTENV_ERROR(
        "agentenv-ublk-daemon cannot serve in this build: UblkDaemonServer is not "
        "ported (it needs the overlaybd ublk target and an io_uring control ring; "
        "see storage/ublk/impls.cc). Configuration was parsed and validated, and "
        "the socket at "
        << cli.value().socket_path << " was NOT created.");
    // `parent_pid` is captured above because that must happen on the main
    // thread before anything else runs; it is unused only because the server
    // that would watch it is absent.
    (void)parent_pid;
    return 1;
}
