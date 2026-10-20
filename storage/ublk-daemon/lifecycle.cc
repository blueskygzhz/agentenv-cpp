// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/main.rs
#include "agentenv/storage/ublk-daemon/lifecycle.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <sstream>

namespace agentenv {
namespace storage {
namespace ublk_daemon {
namespace {

/// Set by the signal handler, read by `WaitForShutdown`.
///
/// `volatile sig_atomic_t` is the only thing a handler may touch here: the
/// handler must not log or allocate, so it records the fact and lets the loop
/// report it.
volatile sig_atomic_t g_shutdown_signal = 0;

void OnShutdownSignal(int sig) { g_shutdown_signal = sig; }

std::string Errno(const char* what, int err) {
    std::ostringstream o;
    o << what << ": " << ::strerror(err) << " (errno " << err << ")";
    return o.str();
}

}  // namespace

core::Expected<LogLevelSpec, std::string> ParseLogLevel(const std::string& text) {
    LogLevelSpec out;
    if (text == "off") {
        out.off = true;
        // Still carries a level, so a caller that ignores `off` is at least
        // quiet rather than verbose.
        out.level = core::LogLevel::Error;
        return out;
    }
    if (text == "error") {
        out.level = core::LogLevel::Error;
    } else if (text == "warn") {
        out.level = core::LogLevel::Warn;
    } else if (text == "info") {
        out.level = core::LogLevel::Info;
    } else if (text == "debug") {
        out.level = core::LogLevel::Debug;
    } else if (text == "trace") {
        out.level = core::LogLevel::Trace;
    } else {
        return core::make_unexpected(std::string("invalid log level `") + text +
                                     "`: expected off, error, warn, info, debug or trace");
    }
    return out;
}

core::Expected<core::Unit, std::string> SetupTracing(
    const core::Optional<std::string>& log_file, const LogLevelSpec& level) {
    core::Logger::Get().SetLevel(level.level);

    if (!log_file.has_value() || log_file->empty()) return core::Unit{};

    // Append, not truncate: see the header.
    const int fd = ::open(log_file->c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return core::make_unexpected(Errno((std::string("open log file ") + *log_file).c_str(),
                                           errno));
    }
    if (::dup2(fd, STDERR_FILENO) < 0) {
        const int err = errno;
        ::close(fd);
        return core::make_unexpected(Errno("redirect stderr to log file", err));
    }
    // STDERR_FILENO now holds the description; the original is redundant. Not
    // closing it would leak one descriptor per start.
    ::close(fd);
    return core::Unit{};
}

core::Expected<core::Unit, std::string> RaiseNoFileLimit(uint64_t target) {
    struct rlimit lim;
    lim.rlim_cur = static_cast<rlim_t>(target);
    lim.rlim_max = static_cast<rlim_t>(target);
    if (::setrlimit(RLIMIT_NOFILE, &lim) != 0) {
        std::ostringstream o;
        o << "failed to raise RLIMIT_NOFILE to " << target << ": " << ::strerror(errno);
        return core::make_unexpected(o.str());
    }
    return core::Unit{};
}

core::Expected<int, std::string> PidfdOpen(int32_t pid) {
#ifdef SYS_pidfd_open
    const long ret = ::syscall(SYS_pidfd_open, static_cast<int>(pid), 0u);
#else
    const long ret = -1;
    errno = ENOSYS;
#endif
    if (ret < 0) {
        std::ostringstream o;
        o << "pidfd_open(" << pid << "): " << ::strerror(errno);
        return core::make_unexpected(o.str());
    }
    return static_cast<int>(ret);
}

const char* ShutdownCauseName(ShutdownCause cause) {
    switch (cause) {
        case kShutdownSigterm:
            return "received SIGTERM";
        case kShutdownSigint:
            return "received SIGINT";
        case kShutdownParentExited:
            return "parent process exited, shutting down";
        case kShutdownParentError:
            return "parent pidfd error, shutting down";
    }
    return "unknown shutdown cause";
}

core::Expected<core::Unit, std::string> InstallSignalHandlers() {
    struct sigaction sa;
    ::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &OnShutdownSignal;
    // No SA_RESTART: the point of these handlers is to interrupt the poll
    // below. With restart, the poll would resume and the signal would only be
    // noticed at the next timeout.
    sa.sa_flags = 0;
    ::sigemptyset(&sa.sa_mask);

    if (::sigaction(SIGTERM, &sa, 0) != 0) {
        return core::make_unexpected(Errno("install SIGTERM handler", errno));
    }
    if (::sigaction(SIGINT, &sa, 0) != 0) {
        return core::make_unexpected(Errno("install SIGINT handler", errno));
    }
    return core::Unit{};
}

bool ShutdownRequested() { return g_shutdown_signal != 0; }

core::Expected<ShutdownCause, std::string> WaitForShutdown(int parent_pidfd) {
    for (;;) {
        // Checked first, so a signal delivered before the poll is not missed.
        if (g_shutdown_signal == SIGTERM) {
            if (parent_pidfd >= 0) ::close(parent_pidfd);
            return kShutdownSigterm;
        }
        if (g_shutdown_signal == SIGINT) {
            if (parent_pidfd >= 0) ::close(parent_pidfd);
            return kShutdownSigint;
        }

        if (parent_pidfd < 0) {
            // No parent to watch. A short sleep keeps this from spinning while
            // still noticing a signal promptly.
            struct pollfd none;
            none.fd = -1;
            none.events = 0;
            none.revents = 0;
            ::poll(&none, 0, 50);
            continue;
        }

        struct pollfd pfd;
        pfd.fd = parent_pidfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int ready = ::poll(&pfd, 1, 50);
        if (ready < 0) {
            if (errno == EINTR) continue;  // a signal arrived; re-check above
            const int err = errno;
            ::close(parent_pidfd);
            return core::make_unexpected(Errno("poll parent pidfd", err));
        }
        if (ready == 0) continue;

        ::close(parent_pidfd);
        if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) return kShutdownParentError;
        return kShutdownParentExited;
    }
}

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
