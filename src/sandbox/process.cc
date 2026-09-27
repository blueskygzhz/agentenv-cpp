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
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>

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

    core::Expected<ProcessOutput, std::string> Wait() override {
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
        if (WIFEXITED(status)) {
            out.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            out.exit_code = 128 + WTERMSIG(status);
        } else {
            out.exit_code = -1;
        }
        return out;
    }

    core::Expected<core::Unit, std::string> Kill(int signal) override {
        if (::kill(pid_, signal) != 0 && errno != ESRCH) {
            return core::make_unexpected(std::string("kill failed: ") + std::strerror(errno));
        }
        return core::Unit{};
    }

 private:
    pid_t   pid_;
    int     out_fd_;
    int     err_fd_;
    int32_t timeout_sec_;
};

class LocalExecutor : public Executor {
 public:
    core::Expected<ProcessOutput, std::string> Run(const ProcessOpts& opts) override {
        if (opts.argv.empty()) {
            return core::make_unexpected(std::string("empty argv"));
        }

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

            if (!opts.cwd.empty()) {
                if (::chdir(opts.cwd.c_str()) != 0) {
                    ::_exit(127);
                }
            }
            for (size_t i = 0; i < opts.env_vars.size(); ++i) {
                ::putenv(const_cast<char*>(opts.env_vars[i].c_str()));
            }

            std::vector<char*> argv;
            argv.reserve(opts.argv.size() + 1);
            for (size_t i = 0; i < opts.argv.size(); ++i) {
                argv.push_back(const_cast<char*>(opts.argv[i].c_str()));
            }
            argv.push_back(nullptr);

            ::execvp(argv[0], &argv[0]);
            ::_exit(127);  // exec failed
        }

        // ---- parent ----
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);
        LocalProcessHandle handle(pid, out_pipe[0], err_pipe[0], opts.timeout_sec);
        // Handle owns the read fds now.
        out_pipe[0] = -1;
        err_pipe[0] = -1;
        return handle.Wait();
    }
};

}  // namespace

std::unique_ptr<Executor> MakeLocalExecutor() {
    return std::unique_ptr<Executor>(new LocalExecutor());
}

}  // namespace sandbox
}  // namespace agentenv
