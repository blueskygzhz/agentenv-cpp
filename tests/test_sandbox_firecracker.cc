// SPDX-License-Identifier: MIT
// Tests for firecracker config validation + the real HTTP-over-unix-socket
// client + FirecrackerBackend pre-boot validation.
#include "microtest.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "agentenv/sandbox/firecracker/config.h"
#include "agentenv/sandbox/firecracker/socket.h"
#include "agentenv/sandbox/firecracker/sandbox.h"

using namespace agentenv::sandbox::firecracker;

static ExtraDriveSpec Drive(const std::string& id, const std::string& mount) {
    ExtraDriveSpec d;
    d.drive_id = id;
    d.mount_path = mount;
    return d;
}

MT_TEST(default_boot_args_contains_damon) {
    std::string args = kDefaultBootArgs;
    MT_EXPECT_TRUE(args.find("console=ttyS0") != std::string::npos);
    MT_EXPECT_TRUE(args.find("damon_reclaim.enabled=Y") != std::string::npos);
    MT_EXPECT_TRUE(args.find("panic=1") != std::string::npos);
}

MT_TEST(extra_drives_ok) {
    std::vector<ExtraDriveSpec> ds;
    ds.push_back(Drive("data", "/mnt/data"));
    ds.push_back(Drive("logs", "/mnt/logs"));
    MT_EXPECT_TRUE(ValidateExtraDriveSet(ds, false).ok());
}

MT_TEST(extra_drives_duplicate_id) {
    std::vector<ExtraDriveSpec> ds;
    ds.push_back(Drive("data", "/mnt/a"));
    ds.push_back(Drive("data", "/mnt/b"));
    auto r = ValidateExtraDriveSet(ds, false);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("duplicate extra drive id") != std::string::npos);
}

MT_TEST(extra_drives_duplicate_mount) {
    std::vector<ExtraDriveSpec> ds;
    ds.push_back(Drive("a", "/mnt/x"));
    ds.push_back(Drive("b", "/mnt/x"));
    auto r = ValidateExtraDriveSet(ds, false);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("duplicate extra drive mount path") != std::string::npos);
}

MT_TEST(extra_drives_too_many) {
    std::vector<ExtraDriveSpec> ds;
    for (int i = 0; i < 25; ++i) {  // MAX is 24
        char id[16]; std::snprintf(id, sizeof(id), "d%d", i);
        char mp[24]; std::snprintf(mp, sizeof(mp), "/mnt/%d", i);
        ds.push_back(Drive(id, mp));
    }
    auto r = ValidateExtraDriveSet(ds, false);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("too many extra drives") != std::string::npos);
}

MT_TEST(extra_drive_zero_virtual_size) {
    std::vector<ExtraDriveSpec> ds;
    ExtraDriveSpec d = Drive("data", "/mnt/data");
    d.has_virtual_size = true;
    d.virtual_size = 0;
    ds.push_back(d);
    auto r = ValidateExtraDriveSet(ds, false);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("must be non-zero") != std::string::npos);
}

MT_TEST(resolve_serial_output_dir) {
    MT_EXPECT_TRUE(ResolveSerialOutputDir("").value().empty());
    MT_EXPECT_TRUE(ResolveSerialOutputDir("/abs/path").value() == "/abs/path");
    std::string rel = ResolveSerialOutputDir("rel/dir").value();
    MT_EXPECT_TRUE(!rel.empty() && rel[0] == '/');
    MT_EXPECT_TRUE(rel.find("rel/dir") != std::string::npos);
}

MT_TEST(validate_common_config_missing_binary) {
    CommonConfig cfg;
    cfg.firecracker_bin = "/nonexistent/firecracker";
    auto r = ValidateCommonConfig(cfg);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("firecracker binary not found") != std::string::npos);
}

MT_TEST(validate_common_config_ok_binary_missing_kernel) {
    CommonConfig cfg;
    cfg.firecracker_bin = "/bin/sh";        // exists
    cfg.kernel_image_path = "/nonexistent/vmlinux";
    auto r = ValidateCommonConfig(cfg);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("kernel image not found") != std::string::npos);
}

// ---- real HTTP-over-unix-socket round trip ----
namespace {
struct ServerArgs {
    std::string path;
    std::string reply;
};
void* http_server_thread(void* arg) {
    ServerArgs* sa = static_cast<ServerArgs*>(arg);
    int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return nullptr;
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sa->path.c_str(), sizeof(addr.sun_path) - 1);
    ::unlink(sa->path.c_str());
    if (::bind(srv, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(srv); return nullptr;
    }
    ::listen(srv, 1);
    int c = ::accept(srv, nullptr, nullptr);
    if (c >= 0) {
        char buf[2048];
        ::read(c, buf, sizeof(buf));  // drain request
        ::write(c, sa->reply.data(), sa->reply.size());
        ::close(c);
    }
    ::close(srv);
    ::unlink(sa->path.c_str());
    return nullptr;
}
}  // namespace

MT_TEST(unix_socket_http_roundtrip) {
    ServerArgs sa;
    sa.path = "/tmp/agentenv_fc_test.sock";
    sa.reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
               "Connection: close\r\n\r\n{\"state\":\"Running\"}";

    pthread_t th;
    pthread_create(&th, nullptr, http_server_thread, &sa);
    // Give the server a moment to bind/listen.
    struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 100* 1000000L; nanosleep(&ts, nullptr);

    std::shared_ptr<Connector> conn = MakeUnixConnector();
    auto sock = conn->Connect(sa.path);
    MT_EXPECT_TRUE(sock.ok());
    if (sock.ok()) {
        auto resp = sock.value()->Request("GET", "/", "");
        MT_EXPECT_TRUE(resp.ok());
        MT_EXPECT_TRUE(resp.value() == "{\"state\":\"Running\"}");
    }
    pthread_join(th, nullptr);
}

MT_TEST(unix_socket_connect_missing) {
    std::shared_ptr<Connector> conn = MakeUnixConnector();
    auto sock = conn->Connect("/tmp/agentenv_no_such.sock");
    // `Connect` is lazy, matching Rust's infallible `UnixSocketClient::new`:
    // no connection is attempted until a request needs one, so a missing
    // socket surfaces on first use rather than here.
    MT_EXPECT_TRUE(sock.ok());
    auto resp = sock.value()->Request("GET", "/", "");
    MT_EXPECT_TRUE(!resp.ok());
    MT_EXPECT_TRUE(resp.error().find("connect") != std::string::npos);
}

// ---- FirecrackerBackend pre-boot validation ----
MT_TEST(firecracker_boot_rejects_missing_binary) {
    Config cfg;
    cfg.firecracker_bin = "/nonexistent/firecracker";
    FirecrackerBackend be(cfg);
    agentenv::sandbox::LaunchPlan plan;
    plan.sandbox_id = agentenv::core::SandboxId::Fresh();
    plan.kernel_path = "/bin/sh";
    auto r = be.Boot(plan);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(std::string(r.error().chain()).find("firecracker binary not found")
                   != std::string::npos);
}

MT_MAIN
