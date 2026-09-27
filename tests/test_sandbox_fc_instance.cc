// SPDX-License-Identifier: MIT
// Tests for firecracker::FirecrackerInstance — ported 1:1 from the Rust
// instance.rs #[cfg(test)] module (process lifecycle + file helpers), plus a
// real HTTP-over-unix-socket API call test.
#include "microtest.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "agentenv/sandbox/firecracker/instance.h"
#include "agentenv/sandbox/firecracker/socket.h"

using namespace agentenv::sandbox::firecracker;

static std::string mkdtemp_dir() {
    char tmpl[] = "/tmp/agentenv_fc_XXXXXX";
    char* d = ::mkdtemp(tmpl);
    return d ? std::string(d) : std::string("/tmp");
}

// Rust: wait_for_ready_times_out_when_socket_never_appears
MT_TEST(wait_for_ready_times_out) {
    std::string dir = mkdtemp_dir();
    FirecrackerInstance inst(dir, nullptr);
    auto r = inst.WaitForReady(30, 10);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("did not appear") != std::string::npos);
    ::rmdir(dir.c_str());
}

// Rust: wait_for_ready_succeeds_when_socket_file_appears
MT_TEST(wait_for_ready_succeeds_when_socket_appears) {
    std::string dir = mkdtemp_dir();
  FirecrackerInstance inst(dir, nullptr);
    // Create the socket marker right away; WaitForReady should see it.
    std::ofstream(inst.SocketPath().c_str()) << "socket";
    auto r = inst.WaitForReady(100, 10);
    MT_EXPECT_TRUE(r.ok());
    ::unlink(inst.SocketPath().c_str());
    ::rmdir(dir.c_str());
}

// Rust: stop_without_process_removes_stale_socket_file
MT_TEST(stop_removes_stale_socket) {
    std::string dir = mkdtemp_dir();
    FirecrackerInstance inst(dir, nullptr);
    std::ofstream(inst.SocketPath().c_str()) << "stale";
    auto r = inst.Stop(10);
    MT_EXPECT_TRUE(r.ok());
    struct stat st;
    MT_EXPECT_TRUE(::stat(inst.SocketPath().c_str(), &st) != 0);  // removed
  ::rmdir(dir.c_str());
}

// Rust: spawn_with_netns_rejects_duplicate_start
MT_TEST(spawn_rejects_duplicate_start) {
    std::string dir = mkdtemp_dir();
    FirecrackerInstance inst(dir, nullptr);
auto r1 = inst.Spawn("/bin/true", "", "");
    MT_EXPECT_TRUE(r1.ok());
    auto r2 = inst.Spawn("/bin/true", "", "");
    MT_EXPECT_TRUE(!r2.ok());
    MT_EXPECT_TRUE(r2.error().find("already started") != std::string::npos);
    inst.Stop(10);
    ::rmdir(dir.c_str());
}

// Rust: resolve_host_path_uses_firecracker_work_dir_for_relative_paths
MT_TEST(resolve_host_path) {
    std::string dir = mkdtemp_dir();
    FirecrackerInstance inst(dir, nullptr);
    MT_EXPECT_TRUE(inst.ResolveHostPath("mem.bin") == dir + "/mem.bin");
    MT_EXPECT_TRUE(inst.ResolveHostPath("/tmp/mem.bin") == "/tmp/mem.bin");
::rmdir(dir.c_str());
}

// Rust: open_log_stdio_creates_parent_dirs_and_appends_existing_files
MT_TEST(open_log_stdio_creates_dirs_and_appends) {
    std::string dir = mkdtemp_dir();
  std::string log = dir + "/logs/firecracker.log";
    auto a = OpenLogStdio(log);
    MT_EXPECT_TRUE(a.ok());
    if (a.ok()) { const char* s = "first"; ssize_t w = ::write(a.value(), s, 5); (void)w; ::close(a.value()); }
    auto b = OpenLogStdio(log);
    MT_EXPECT_TRUE(b.ok());
    if (b.ok()) { const char* s = "second"; ssize_t w = ::write(b.value(), s, 6); (void)w; ::close(b.value()); }
  std::ifstream f(log.c_str());
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    MT_EXPECT_TRUE(content == "firstsecond");
    ::unlink(log.c_str());
    std::string logdir = dir + "/logs";
    ::rmdir(logdir.c_str());
    ::rmdir(dir.c_str());
}

// Rust: parse_log_level
MT_TEST(parse_log_level) {
    MT_EXPECT_TRUE(ParseLogLevel("error").value() == "Error");
    MT_EXPECT_TRUE(ParseLogLevel("WARN").value() == "Warning");
    MT_EXPECT_TRUE(ParseLogLevel("  Info ").value() == "Info");
    MT_EXPECT_TRUE(ParseLogLevel("debug").value() == "Debug");
    MT_EXPECT_TRUE(ParseLogLevel("TRACE").value() == "Trace");
    MT_EXPECT_TRUE(!ParseLogLevel("bogus").ok());
}

// read_stderr_tail returns the last <=4096 bytes.
MT_TEST(read_stderr_tail) {
    std::string dir = mkdtemp_dir();
    std::string path = dir + "/err.log";
    { std::ofstream o(path.c_str()); o << "  hello error tail  "; }
    auto r = ReadStderrTail(path);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value() == "hello error tail");
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

// Drop after spawn removes the owned socket file.
MT_TEST(drop_after_spawn_removes_owned_socket) {
    std::string dir = mkdtemp_dir();
    std::string socket_path;
    {
        FirecrackerInstance inst(dir, nullptr);
        auto r = inst.Spawn("/bin/true", "", "");
        MT_EXPECT_TRUE(r.ok());
  socket_path = inst.SocketPath();
std::ofstream(socket_path.c_str()) << "owned";
        // destructor runs here: kills child + removes socket
    }
    struct stat st;
    MT_EXPECT_TRUE(::stat(socket_path.c_str(), &st) != 0);
    ::rmdir(dir.c_str());
}

// ---- real HTTP API call over unix socket ----
namespace {
struct SrvArgs { std::string path; std::string* captured; };
void* api_server(void* arg) {
    SrvArgs* sa = static_cast<SrvArgs*>(arg);
    int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return nullptr;
    struct sockaddr_un addr; std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
 std::strncpy(addr.sun_path, sa->path.c_str(), sizeof(addr.sun_path) - 1);
    ::unlink(sa->path.c_str());
    if (::bind(srv, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) { ::close(srv); return nullptr; }
    ::listen(srv, 1);
  int c = ::accept(srv, nullptr, nullptr);
    if (c >= 0) {
        char buf[2048];
        ssize_t n = ::read(c, buf, sizeof(buf));
        if (n > 0 && sa->captured) sa->captured->assign(buf, static_cast<size_t>(n));
        const char* reply = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
        ssize_t w = ::write(c, reply, std::strlen(reply)); (void)w;
        ::close(c);
    }
    ::close(srv);
    ::unlink(sa->path.c_str());
    return nullptr;
}
}  // namespace

MT_TEST(set_machine_config_issues_real_put) {
    std::string dir = mkdtemp_dir();
    std::string sock = dir + "/firecracker.socket";
    std::string captured;
 SrvArgs sa; sa.path = sock; sa.captured = &captured;
    pthread_t th; pthread_create(&th, nullptr, api_server, &sa);
  struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 100 * 1000000L; nanosleep(&ts, nullptr);

    FirecrackerInstance inst(dir, MakeUnixConnector());
    auto r = inst.SetMachineConfig(512, 2, false, true);
    pthread_join(th, nullptr);

    MT_EXPECT_TRUE(r.ok());
    // The server captured a real HTTP PUT /machine-config with the JSON body.
    MT_EXPECT_TRUE(captured.find("PUT /machine-config HTTP/1.1") != std::string::npos);
    MT_EXPECT_TRUE(captured.find("\"vcpu_count\":2") != std::string::npos);
    MT_EXPECT_TRUE(captured.find("\"mem_size_mib\":512") != std::string::npos);
    ::rmdir(dir.c_str());
}

MT_TEST(api_without_connector_errors) {
  std::string dir = mkdtemp_dir();
    FirecrackerInstance inst(dir, nullptr);
 auto r = inst.Start();
    MT_EXPECT_TRUE(!r.ok());
    ::rmdir(dir.c_str());
}

MT_MAIN
