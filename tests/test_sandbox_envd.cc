// SPDX-License-Identifier: MIT
// Tests for sandbox::EnvdInstance::WaitForReady + HttpHealthProbe.
// (The custom extension hooks have their own file: test_custom_extension.cc.)
#include "microtest.h"

#include <ctime>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <thread>

#include "agentenv/sandbox/envd.h"

using namespace agentenv::sandbox;

static int64_t mono_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

MT_TEST(envd_instance_addresses) {
    EnvdInstance e("http://10.0.0.5:50051");
    MT_EXPECT_TRUE(e.BasePath() == "http://10.0.0.5:50051");
    MT_EXPECT_TRUE(e.GrpcAddress() == "http://10.0.0.5:50051");
}

MT_TEST(envd_ready_immediately) {
    EnvdInstance e("http://x");
    int probes = 0;
    auto r = e.WaitForReady(2000, 10, [&](int32_t) { ++probes; return true; });
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(probes, 1);
}

MT_TEST(envd_ready_after_n_probes) {
    EnvdInstance e("http://x");
    int probes = 0;
    auto r = e.WaitForReady(5000, 5, [&](int32_t) { ++probes; return probes >= 3; });
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(probes, 3);
}

MT_TEST(envd_deadline_bounds_hung_probe) {
    // A probe that "hangs" for its whole timeout (simulated by sleeping the
    // probe_timeout) must still let WaitForReady return by the deadline.
    EnvdInstance e("http://x");
    const int64_t start = mono_ms();
    auto r = e.WaitForReady(/*timeout*/ 50, /*retry*/ 1, [](int32_t probe_timeout_ms) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = static_cast<long>(probe_timeout_ms) * 1000000L;
        nanosleep(&ts, nullptr);
        return false;  // never healthy
    });
    const int64_t elapsed = mono_ms() - start;
    MT_EXPECT_TRUE(!r.ok());
    // Rust asserts it finishes well under 500ms.
    MT_EXPECT_TRUE(elapsed < 500);
}

MT_TEST(envd_null_probe_times_out) {
    EnvdInstance e("http://x");
    auto r = e.WaitForReady(20, 1, EnvdInstance::HealthProbe());
    MT_EXPECT_TRUE(!r.ok());
}

// ---- HttpHealthProbe ----

namespace {

/// Serves one canned HTTP response per connection on a loopback port and
/// records the request line of each.
class OneShotHttpServer {
 public:
    explicit OneShotHttpServer(const std::string& status_line) : status_(status_line) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd_, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
        ::listen(fd_, 4);
        socklen_t len = sizeof(a);
        ::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this]() {
            const int c = ::accept(fd_, NULL, NULL);
            if (c < 0) return;
            char buf[1024];
            const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            if (n > 0) request_.assign(buf, static_cast<size_t>(n));
            const std::string resp = status_ + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
            ::close(c);
        });
    }
    ~OneShotHttpServer() {
        ::shutdown(fd_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        ::close(fd_);
    }
    uint16_t port() const { return port_; }
    std::string Base() const { return "http://127.0.0.1:" + std::to_string(port_); }
    void Join() {
        if (thread_.joinable()) thread_.join();
    }
    const std::string& request() const { return request_; }

 private:
    std::string status_;
    int fd_;
    uint16_t port_;
    std::thread thread_;
    std::string request_;
};

}  // namespace

MT_TEST(http_health_probe_gets_health_and_accepts_success) {
    OneShotHttpServer server("HTTP/1.1 204 No Content");
    EnvdInstance envd(server.Base(), agentenv::core::Optional<EnvdAccessToken>());
    MT_EXPECT_TRUE(envd.HttpHealthProbe()(1000));
    server.Join();
    MT_EXPECT_TRUE(server.request().compare(0, 16, "GET /health HTTP") == 0);
}

MT_TEST(http_health_probe_rejects_a_server_error) {
    OneShotHttpServer server("HTTP/1.1 503 Service Unavailable");
    EnvdInstance envd(server.Base(), agentenv::core::Optional<EnvdAccessToken>());
    MT_EXPECT_TRUE(!envd.HttpHealthProbe()(1000));
}

MT_TEST(http_health_probe_rejects_an_unreachable_or_malformed_base) {
    MT_EXPECT_TRUE(!EnvdInstance("http://127.0.0.1:1").HttpHealthProbe()(200));
    MT_EXPECT_TRUE(!EnvdInstance("unix:///tmp/x").HttpHealthProbe()(200));
    MT_EXPECT_TRUE(!EnvdInstance("http://:80").HttpHealthProbe()(200));
}

MT_TEST(wait_for_ready_uses_the_http_probe) {
    OneShotHttpServer server("HTTP/1.1 200 OK");
    EnvdInstance envd(server.Base(), agentenv::core::Optional<EnvdAccessToken>(
                                         EnvdAccessToken("secret")));
    MT_EXPECT_TRUE(envd.WaitForReady(2000, 10).ok());
    MT_EXPECT_EQ(envd.AccessToken()->Expose(), std::string("secret"));
}

MT_MAIN
