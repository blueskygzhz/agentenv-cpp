// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/instance.rs
//
// Real host-side process + API-socket management for a Firecracker microVM.
// Everything except an actually-running Firecracker (which needs KVM) is real:
//   - spawn / wait_for_ready / stop / Drop process lifecycle
//   - log stdio open, stderr tail, path resolution, log-level parsing
//   - the microVM API calls issue real HTTP/1.1 PUTs over the api unix socket
// This mirrors the Rust unit tests, which also use /bin/true + socket markers.
#include "agentenv/sandbox/firecracker/instance.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

namespace agentenv {
namespace sandbox {
namespace firecracker {

namespace {
int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
void sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, nullptr);
}
bool path_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
std::string to_lower_trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
  size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    std::string out = s.substr(b, e - b + 1);
 for (size_t i = 0; i < out.size(); ++i) {
        char c = out[i];
        if (c >= 'A' && c <= 'Z') out[i] = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}
// Recursively create parent dirs of `path` (like Rust create_dir_all(parent)).
void make_parent_dirs(const std::string& path) {
    std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return;
    std::string dir = path.substr(0, slash);
    std::string acc;
    std::string::size_type i = 0;
 if (!dir.empty() && dir[0] == '/') { acc = "/"; i = 1; }
    while (i <= dir.size()) {
    if (i == dir.size() || dir[i] == '/') {
      if (!acc.empty() && acc != "/") ::mkdir(acc.c_str(), 0755);
            if (i < dir.size()) acc += "/";
        } else {
            acc += dir[i];
        }
    ++i;
    }
}
}  // namespace

// ---- free helpers (Rust module fns) ----
core::Expected<std::string, std::string> ParseLogLevel(const std::string& level) {
    std::string l = to_lower_trim(level);
    if (l == "error") return std::string("Error");
    if (l == "warning" || l == "warn") return std::string("Warning");
    if (l == "info") return std::string("Info");
    if (l == "debug") return std::string("Debug");
  if (l == "trace") return std::string("Trace");
  return core::make_unexpected(
        std::string("invalid firecracker log level '") + level +
        "'; expected one of Error, Warning, Info, Debug, Trace");
}

core::Expected<int, std::string> OpenLogStdio(const std::string& path) {
    if (path.empty()) {
        return -1;  // caller maps to /dev/null
    }
    make_parent_dirs(path);
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
  return core::make_unexpected(std::string("open firecracker log ") + path +
  ": " + std::strerror(errno));
    }
    return fd;
}

core::Expected<std::string, std::string> ReadStderrTail(const std::string& path) {
    const long kMax = 4096;
    std::ifstream f(path.c_str(), std::ios::binary | std::ios::ate);
    if (!f) return core::make_unexpected(std::string("cannot open ") + path);
    std::streamoff size = f.tellg();
    std::streamoff to_read = size < kMax ? size : kMax;
    f.seekg(size - to_read, std::ios::beg);
    std::string data;
    data.resize(static_cast<size_t>(to_read));
    if (to_read > 0) f.read(&data[0], to_read);
    // trim
    size_t b = data.find_first_not_of(" \t\r\n");
    size_t e = data.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    return data.substr(b, e - b + 1);
}

// ---- FirecrackerInstance ----
FirecrackerInstance::FirecrackerInstance(std::string work_dir,
               std::shared_ptr<Connector> connector)
    : work_dir_(work_dir),
    socket_path_(work_dir + "/firecracker.socket"),
      connector_(connector),
      child_pid_(-1) {}

FirecrackerInstance::~FirecrackerInstance() {
    // Rust Drop: if we own a child, kill it and remove the socket.
    if (child_pid_ > 0) {
        ::kill(static_cast<pid_t>(child_pid_), SIGKILL);
        int status = 0;
   ::waitpid(static_cast<pid_t>(child_pid_), &status, 0);
        ::unlink(socket_path_.c_str());  // only remove when we owned the process
    }
}

core::Expected<int64_t, std::string> FirecrackerInstance::Pid() const {
    if (child_pid_ <= 0) {
     return core::make_unexpected(std::string("firecracker process is not running"));
    }
    return child_pid_;
}

core::Expected<core::Unit, std::string>
FirecrackerInstance::Spawn(const std::string& firecracker_binary,
     const std::string& stdout_path,
  const std::string& stderr_path) {
    if (child_pid_ > 0) {
        return core::make_unexpected(std::string("firecracker process already started"));
  }
    if (path_exists(socket_path_)) {
        ::unlink(socket_path_.c_str());
    }

    int out_fd = -1, err_fd = -1;
    {
        auto o = OpenLogStdio(stdout_path);
        if (!o.ok()) return core::make_unexpected(o.error());
        out_fd = o.value();
        auto e = OpenLogStdio(stderr_path);
     if (!e.ok()) { if (out_fd >= 0) ::close(out_fd); return core::make_unexpected(e.error()); }
    err_fd = e.value();
    }
    stderr_path_ = stderr_path;

    pid_t pid = ::fork();
    if (pid < 0) {
        if (out_fd >= 0) ::close(out_fd);
   if (err_fd >= 0) ::close(err_fd);
    return core::make_unexpected(std::string("fork() failed"));
 }
    if (pid == 0) {
        // child
        if (chdir(work_dir_.c_str()) != 0) { /* best effort */ }
     int devnull = -1;
        if (out_fd < 0 || err_fd < 0) devnull = ::open("/dev/null", O_WRONLY);
        ::dup2(out_fd >= 0 ? out_fd : devnull, STDOUT_FILENO);
        ::dup2(err_fd >= 0 ? err_fd : devnull, STDERR_FILENO);
        int nullin = ::open("/dev/null", O_RDONLY);
        if (nullin >= 0) ::dup2(nullin, STDIN_FILENO);

        std::string sizelimit;
        {
      char b[32]; std::snprintf(b, sizeof(b), "%zu", kMmdsSizeLimit); sizelimit = b;
 }
        const char* argv[] = {
       firecracker_binary.c_str(),
 "--api-sock", socket_path_.c_str(),
            "--mmds-size-limit", sizelimit.c_str(),
            nullptr
        };
        ::execvp(argv[0], const_cast<char* const*>(argv));
        ::_exit(127);
    }
    // parent
    if (out_fd >= 0) ::close(out_fd);
    if (err_fd >= 0) ::close(err_fd);
    child_pid_ = pid;

    // Rust: set oom_score_adj = 1000 (best effort).
    {
        char p[64];
        std::snprintf(p, sizeof(p), "/proc/%d/oom_score_adj", static_cast<int>(pid));
        std::ofstream oom(p);
        if (oom) oom << "1000";
    }
    return core::Unit{};
}

std::string FirecrackerInstance::StderrSummary() const {
    if (stderr_path_.empty()) return "not captured";
    auto r = ReadStderrTail(stderr_path_);
    if (!r.ok()) return std::string("unavailable (") + r.error() + ")";
    return r.value().empty() ? "empty" : r.value();
}

core::Expected<core::Unit, std::string>
FirecrackerInstance::WaitForReady(int64_t timeout_ms, int64_t poll_interval_ms) {
    const int64_t start = now_ms();
    while (!path_exists(socket_path_)) {
        // Detect child early-exit.
        if (child_pid_ > 0) {
     int status = 0;
     pid_t r = ::waitpid(static_cast<pid_t>(child_pid_), &status, WNOHANG);
    if (r == child_pid_) {
        child_pid_ = -1;
       return core::make_unexpected(
   std::string("firecracker exited before its API socket appeared at ") +
   socket_path_ + "; stderr: " + StderrSummary());
            }
        }
     if (now_ms() - start > timeout_ms) {
            return core::make_unexpected(
std::string("firecracker api socket did not appear at ") + socket_path_ +
       " within timeout; stderr: " + StderrSummary());
   }
        sleep_ms(poll_interval_ms);
    }
    return core::Unit{};
}

core::Expected<core::Unit, std::string>
FirecrackerInstance::Stop(int64_t timeout_ms) {
    if (child_pid_ > 0) {
        pid_t pid = static_cast<pid_t>(child_pid_);
        ::kill(pid, SIGTERM);
   const int64_t deadline = now_ms() + timeout_ms;
        bool exited = false;
        while (now_ms() < deadline) {
            int status = 0;
            pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) { exited = true; break; }
         sleep_ms(5);
        }
 if (!exited) {
  ::kill(pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
 }
        child_pid_ = -1;
    }
    if (path_exists(socket_path_)) {
        ::unlink(socket_path_.c_str());
    }
    return core::Unit{};
}

std::string FirecrackerInstance::ResolveHostPath(const std::string& path) const {
    if (!path.empty() && path[0] == '/') return path;
    return work_dir_ + "/" + path;
}

core::Expected<core::Unit, std::string>
FirecrackerInstance::ApiPut(const std::string& path, const std::string& json_body) {
    if (!connector_) {
        return core::make_unexpected(std::string("no api connector configured"));
    }
    auto sock = connector_->Connect(socket_path_);
    if (!sock.ok()) return core::make_unexpected(sock.error());
    auto resp = sock.value()->Request("PUT", path, json_body);
    if (!resp.ok()) return core::make_unexpected(resp.error());
    return core::Unit{};
}

core::Expected<core::Unit, std::string>
FirecrackerInstance::SetMachineConfig(uint32_t mem_mib, uint32_t vcpus, bool smt,
          bool track_dirty) {
    char body[192];
    std::snprintf(body, sizeof(body),
        "{\"mem_size_mib\":%u,\"vcpu_count\":%u,\"smt\":%s,\"track_dirty_pages\":%s}",
        mem_mib, vcpus, smt ? "true" : "false", track_dirty ? "true" : "false");
    return ApiPut("/machine-config", body);
}

core::Expected<core::Unit, std::string>
FirecrackerInstance::SetBootSource(const std::string& kernel_image,
           const std::string& boot_args) {
    std::string body = "{\"kernel_image_path\":\"" + kernel_image + "\"";
    if (!boot_args.empty()) body += ",\"boot_args\":\"" + boot_args + "\"";
    body += "}";
    return ApiPut("/boot-source", body);
}

core::Expected<core::Unit, std::string>
FirecrackerInstance::AddDrive(const std::string& drive_id, const std::string& path_on_host,
bool is_root, bool read_only) {
    std::string body = "{\"drive_id\":\"" + drive_id + "\",\"path_on_host\":\"" +
       path_on_host + "\",\"is_root_device\":" +
              (is_root ? "true" : "false") + ",\"is_read_only\":" +
         (read_only ? "true" : "false") + "}";
    return ApiPut("/drives/" + drive_id, body);
}

core::Expected<core::Unit, std::string> FirecrackerInstance::Start() {
    return ApiPut("/actions", "{\"action_type\":\"InstanceStart\"}");
}
core::Expected<core::Unit, std::string> FirecrackerInstance::Pause() {
    return ApiPut("/vm", "{\"state\":\"Paused\"}");
}
core::Expected<core::Unit, std::string> FirecrackerInstance::Resume() {
    return ApiPut("/vm", "{\"state\":\"Resumed\"}");
}

core::Expected<core::Unit, std::string> FirecrackerInstance::Boot() {
    return Start();
}
core::Expected<core::Unit, std::string> FirecrackerInstance::Shutdown() {
    return Stop(10000);
}
core::Expected<core::Unit, std::string>
FirecrackerInstance::SetMmds(const MmdsMetadata& metadata) {
    // Rust serializes first and measures *that*, because the limit applies to
    // the payload Firecracker receives. `imageConfigs` extras are the usual
    // reason this trips, so the error names the actual size and the cap.
    const std::string payload = metadata.ToJson();
    if (payload.size() > kMmdsSizeLimit) {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "MMDS metadata payload is %zu bytes, exceeds Firecracker MMDS size "
                      "limit %zu; imageConfigs may be too large",
                      payload.size(), kMmdsSizeLimit);
        return core::make_unexpected(std::string(msg));
    }
    return ApiPut("/mmds", payload);
}

// ---- ProcessVmReader ----
}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv

// process_vm_readv needs <sys/uio.h>; keep the impl in a fresh TU section.
#include <sys/uio.h>

namespace agentenv {
namespace sandbox {
namespace firecracker {
namespace {
class RealProcessVmReader : public ProcessVmReader {
 public:
    core::Expected<std::vector<uint8_t>, std::string>
    Read(int64_t pid, uint64_t addr, size_t len) override {
        std::vector<uint8_t> out(len, 0);
  struct iovec local;
        local.iov_base = out.empty() ? nullptr : &out[0];
        local.iov_len = len;
        struct iovec remote;
        remote.iov_base = reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
     remote.iov_len = len;
    ssize_t n = ::process_vm_readv(static_cast<pid_t>(pid), &local, 1, &remote, 1, 0);
        if (n < 0) {
  return core::make_unexpected(std::string("process_vm_readv failed: ") +
     std::strerror(errno));
      }
  out.resize(static_cast<size_t>(n));
        return out;
    }
};
}  // namespace

std::unique_ptr<ProcessVmReader> MakeProcessVmReader() {
    return std::unique_ptr<ProcessVmReader>(new RealProcessVmReader());
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
