// SPDX-License-Identifier: MIT
// Rust: src/sandbox/process.rs — process spawning primitive.
//
// The Rust `Executor` runs commands inside the guest via envd gRPC. This C++11
// port provides a host-local executor with the same ProcessOpts/ProcessOutput
// contract: environment overrides, working directory, stdout/stderr capture,
// an optional timeout that SIGKILLs the child, and the collected exit code.
#include "agentenv/sandbox/process.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <vector>

#include "agentenv/core/fs.h"

namespace agentenv {
namespace sandbox {

namespace {

// 10 MiB — mirrors Rust MAX_OUTPUT_BYTES.
static const size_t kMaxOutputBytes = 10 * 1024 * 1024;

int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void set_nonblock(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class LocalProcessHandle : public ProcessHandle {
 public:
    LocalProcessHandle(pid_t pid, int out_fd, int err_fd, int32_t timeout_sec)
        : pid_(pid), out_fd_(out_fd), err_fd_(err_fd), timeout_sec_(timeout_sec) {}

    ~LocalProcessHandle() {
        if (out_fd_ >= 0) ::close(out_fd_);
        if (err_fd_ >= 0) ::close(err_fd_);
    }

    int64_t Pid() const override { return static_cast<int64_t>(pid_); }

    core::Expected<core::Optional<ProcessOutput>, std::string> TryWait() override {
        if (reaped_) return core::Optional<ProcessOutput>(reaped_output_);

        // WNOHANG is the whole point: a start command is *expected* to still
        // be running, and blocking here would wedge the ready-check loop.
        int status = 0;
        const pid_t done = ::waitpid(pid_, &status, WNOHANG);
        if (done == 0) return core::Optional<ProcessOutput>();
        if (done < 0) {
            return core::make_unexpected(std::string("waitpid failed: ") +
                                         std::strerror(errno));
        }

        ProcessOutput out;
        // Drain whatever the process left behind; it has exited, so the pipes
        // will not fill again and a short non-blocking read is enough.
        set_nonblock(out_fd_);
        set_nonblock(err_fd_);
        DrainInto(out_fd_, &out.stdout_data);
        DrainInto(err_fd_, &out.stderr_data);
        out.exit_code = ExitCodeFrom(status);

        reaped_        = true;
        reaped_output_ = out;
        return core::Optional<ProcessOutput>(out);
    }

    core::Expected<ProcessOutput, std::string> Wait() override {
        if (reaped_) return reaped_output_;
        ProcessOutput out;
        set_nonblock(out_fd_);
        set_nonblock(err_fd_);

        const int64_t deadline = timeout_sec_ > 0 ? now_ms() + timeout_sec_ * 1000 : 0;
        bool timed_out = false;
        bool out_open = true, err_open = true;
        char buf[8192];

        while (out_open || err_open) {
            fd_set rfds;
            FD_ZERO(&rfds);
            int maxfd = -1;
            if (out_open) { FD_SET(out_fd_, &rfds); if (out_fd_ > maxfd) maxfd = out_fd_; }
            if (err_open) { FD_SET(err_fd_, &rfds); if (err_fd_ > maxfd) maxfd = err_fd_; }
            if (maxfd < 0) break;

            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200 * 1000;  // 200ms poll tick
            int rv = ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv);

            if (rv > 0) {
                if (out_open && FD_ISSET(out_fd_, &rfds)) {
                    ssize_t n = ::read(out_fd_, buf, sizeof(buf));
                    if (n > 0) {
                        if (out.stdout_data.size() + static_cast<size_t>(n) <= kMaxOutputBytes)
                            out.stdout_data.append(buf, static_cast<size_t>(n));
                    } else if (n == 0) {
                        out_open = false;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        out_open = false;
                    }
                }
                if (err_open && FD_ISSET(err_fd_, &rfds)) {
                    ssize_t n = ::read(err_fd_, buf, sizeof(buf));
                    if (n > 0) {
                        if (out.stderr_data.size() + static_cast<size_t>(n) <= kMaxOutputBytes)
                            out.stderr_data.append(buf, static_cast<size_t>(n));
                    } else if (n == 0) {
                        err_open = false;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        err_open = false;
                    }
                }
            }

            if (deadline != 0 && now_ms() >= deadline) {
                timed_out = true;
                Kill(SIGKILL);
                break;
            }
        }

        int status = 0;
        ::waitpid(pid_, &status, 0);

        if (timed_out) {
            return core::make_unexpected(std::string("process timed out"));
        }
        out.exit_code  = ExitCodeFrom(status);
        reaped_        = true;
        reaped_output_ = out;
        return out;
    }

    core::Expected<core::Unit, std::string> Kill(int signal) override {
        if (::kill(pid_, signal) != 0 && errno != ESRCH) {
            return core::make_unexpected(std::string("kill failed: ") + std::strerror(errno));
        }
        return core::Unit{};
    }

 private:
    static int32_t ExitCodeFrom(int status) {
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        // Shell convention, so a signalled process is distinguishable from an
        // ordinary non-zero exit.
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return -1;
    }

    static void DrainInto(int fd, std::string* sink) {
        if (fd < 0) return;
        char buf[8192];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            if (sink->size() + static_cast<size_t>(n) > kMaxOutputBytes) break;
            sink->append(buf, static_cast<size_t>(n));
        }
    }

    pid_t   pid_;
    int     out_fd_;
    int     err_fd_;
    int32_t timeout_sec_;
    /// Once reaped, the result is remembered: `waitpid` on an already-reaped
    /// pid would fail, and both `Wait` and `TryWait` may be called more than
    /// once on the same handle.
    bool          reaped_ = false;
    ProcessOutput reaped_output_;
};

class LocalExecutor : public Executor {
 public:
    core::Expected<ProcessOutput, std::string> RunCommandWithOpts(
        const std::string& cmd, const std::vector<std::string>& args,
        const ProcessOpts& opts) override {
        core::Expected<std::unique_ptr<ProcessHandle>, std::string> handle =
            StartProcess(cmd, args, opts);
        if (!handle.ok()) return core::make_unexpected(handle.error());
        return handle.value()->Wait();
    }

    core::Expected<core::Unit, std::string> CreateDirAll(const std::string& path) override {
        // Upstream this is an envd filesystem RPC so it works in images with
        // no userland; the host-local executor uses the host filesystem for
        // the same reason — no `mkdir` binary is required.
        return core::fs::CreateDirAll(path);
    }

    core::Expected<std::unique_ptr<ProcessHandle>, std::string> StartProcess(
        const std::string& cmd, const std::vector<std::string>& args,
        const ProcessOpts& opts) override {
        if (cmd.empty()) {
            return core::make_unexpected(std::string("empty command"));
        }

        // argv[0] is the command itself, then its arguments.
        std::vector<std::string> spliced;
        spliced.reserve(args.size() + 1);
        spliced.push_back(cmd);
        for (std::size_t i = 0; i < args.size(); ++i) spliced.push_back(args[i]);

        int out_pipe[2] = {-1, -1};
        int err_pipe[2] = {-1, -1};
        if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
            return core::make_unexpected(std::string("pipe() failed"));
        }

        pid_t pid = ::fork();
        if (pid < 0) {
            ::close(out_pipe[0]); ::close(out_pipe[1]);
            ::close(err_pipe[0]); ::close(err_pipe[1]);
            return core::make_unexpected(std::string("fork() failed"));
        }

        if (pid == 0) {
            // ---- child ----
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::dup2(err_pipe[1], STDERR_FILENO);
            ::close(out_pipe[0]); ::close(out_pipe[1]);
            ::close(err_pipe[0]); ::close(err_pipe[1]);

            if (opts.cwd.has_value() && !opts.cwd->empty()) {
                if (::chdir(opts.cwd->c_str()) != 0) {
                    ::_exit(127);
                }
            }
            // `envs` is a map, so a duplicate key cannot reach the process
            // twice. `setenv` overwrites, matching the map's own semantics.
            for (std::map<std::string, std::string>::const_iterator it = opts.envs.begin();
                 it != opts.envs.end(); ++it) {
                ::setenv(it->first.c_str(), it->second.c_str(), 1);
            }

            std::vector<char*> argv;
            argv.reserve(spliced.size() + 1);
            for (size_t i = 0; i < spliced.size(); ++i) {
                argv.push_back(const_cast<char*>(spliced[i].c_str()));
            }
            argv.push_back(nullptr);

            ::execvp(argv[0], &argv[0]);
            ::_exit(127);  // exec failed
        }

        // ---- parent ----
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);
        // Rust carries a `Duration`; the handle polls in whole seconds, so a
        // sub-second timeout still yields at least one tick.
        const int32_t timeout_sec =
            opts.timeout_ms.has_value()
                ? static_cast<int32_t>((*opts.timeout_ms + 999) / 1000)
                : 0;
        // The handle owns the read fds from here on.
        std::unique_ptr<ProcessHandle> handle(
            new LocalProcessHandle(pid, out_pipe[0], err_pipe[0], timeout_sec));
        out_pipe[0] = -1;
        err_pipe[0] = -1;
        return std::move(handle);
    }
};

}  // namespace

ProcessOpts& ProcessOpts::WithEnvs(const std::map<std::string, std::string>& envs_in) {
    envs = envs_in;
    return *this;
}

ProcessOpts& ProcessOpts::WithCwd(const std::string& cwd_in) {
    cwd = core::Optional<std::string>(cwd_in);
    return *this;
}

ProcessOpts& ProcessOpts::WithTimeoutMs(int64_t timeout_in) {
    timeout_ms = core::Optional<int64_t>(timeout_in);
    return *this;
}

core::Expected<ProcessOutput, std::string> Executor::RunCommand(
    const std::string& cmd, const std::vector<std::string>& args) {
    return RunCommandWithOpts(cmd, args, ProcessOpts());
}

std::unique_ptr<Executor> MakeLocalExecutor() {
    return std::unique_ptr<Executor>(new LocalExecutor());
}

}  // namespace sandbox
}  // namespace agentenv
