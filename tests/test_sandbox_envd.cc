// SPDX-License-Identifier: MIT
// Tests for sandbox::EnvdInstance::WaitForReady + HttpHealthProbe.
// (The custom extension hooks have their own file: test_custom_extension.cc.)
#include "microtest.h"

#include <ctime>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

// ---- liveness (Rust `live: Arc<AtomicBool>`) ------------------------------

// Rust `invalidated_runtime_rejects_new_envd_clients`
MT_TEST(invalidated_runtime_rejects_new_envd_clients) {
    EnvdInstance envd("http://127.0.0.1:1");
    // Rust clones the handle and invalidates the original; the *copy* must be
    // dead too, which is what `Arc<AtomicBool>` buys and a per-copy flag
    // would not. A stale copy that kept working would reach whatever VM has
    // since been given this address.
    EnvdInstance stale = envd;
    MT_EXPECT_TRUE(envd.EnsureLive().ok());
    MT_EXPECT_TRUE(stale.EnsureLive().ok());

    envd.Invalidate();

    MT_EXPECT_TRUE(!stale.EnsureLive().ok());
    MT_EXPECT_EQ(stale.EnsureLive().error(),
                 std::string("sandbox runtime is no longer active"));
    MT_EXPECT_TRUE(!stale.ProcessClient().ok());
    MT_EXPECT_TRUE(!stale.FilesystemClient().ok());
}

MT_TEST(liveness_is_checked_before_the_transport_is_reported_missing) {
    // The two failures are not interchangeable: "no longer active" is a
    // sandbox that is gone (terminal), a missing transport is a property of
    // this build. Collapsing them makes a torn-down sandbox look like a
    // deployment problem.
    EnvdInstance live("http://127.0.0.1:1");
    const auto before = live.ProcessClient();
    MT_EXPECT_TRUE(!before.ok());
    MT_EXPECT_TRUE(before.error().find("not available in this build") != std::string::npos);

    live.Invalidate();
    const auto after = live.ProcessClient();
    MT_EXPECT_TRUE(!after.ok());
    MT_EXPECT_EQ(after.error(), std::string("sandbox runtime is no longer active"));
}

// ---- InitPostRequest wire format ------------------------------------------

MT_TEST(init_body_omits_unset_fields_rather_than_writing_null) {
    // serde `skip_serializing_if = "Option::is_none"`. envd reads an absent
    // `defaultUser` as "keep what you have" and a null one as a value, so a
    // null here would clear the account the resolution sequence just created.
    InitPostRequest empty;
    MT_EXPECT_EQ(empty.ToJson().ToString(), std::string("{}"));

    InitPostRequest req;
    req.access_token = std::string("tok");
    req.default_user = std::string("root");
    const std::string body = req.ToJson().ToString();
    MT_EXPECT_TRUE(body.find("\"accessToken\":\"tok\"") != std::string::npos);
    MT_EXPECT_TRUE(body.find("\"defaultUser\":\"root\"") != std::string::npos);
    // Every field the caller did not set stays out of the body entirely.
    MT_EXPECT_TRUE(body.find("null") == std::string::npos);
    MT_EXPECT_TRUE(body.find("envVars") == std::string::npos);
    MT_EXPECT_TRUE(body.find("defaultWorkdir") == std::string::npos);
    MT_EXPECT_TRUE(body.find("timestamp") == std::string::npos);
    // The three fields this call path never sets are absent structurally.
    MT_EXPECT_TRUE(body.find("volumeMounts") == std::string::npos);
    MT_EXPECT_TRUE(body.find("hyperloopIP") == std::string::npos);
    MT_EXPECT_TRUE(body.find("caBundle") == std::string::npos);
}

MT_TEST(init_body_uses_the_camel_case_wire_names) {
    // These are serde `rename` values, not field names: envd ignores anything
    // it does not recognise, so a snake_case key would be silently dropped
    // and the guest would come up with no environment and the wrong user.
    InitPostRequest req;
    std::map<std::string, std::string> envs;
    envs["HOME"] = "/app";
    req.env_vars = envs;
    req.default_workdir = std::string("/work");
    req.timestamp = std::string("2026-11-06T00:00:00.000000000Z");
    const std::string body = req.ToJson().ToString();
    MT_EXPECT_TRUE(body.find("\"envVars\":{\"HOME\":\"/app\"}") != std::string::npos);
    MT_EXPECT_TRUE(body.find("\"defaultWorkdir\":\"/work\"") != std::string::npos);
    MT_EXPECT_TRUE(body.find("\"timestamp\":\"2026-11-06T00:00:00.000000000Z\"") !=
                   std::string::npos);
    MT_EXPECT_TRUE(body.find("env_vars") == std::string::npos);
    MT_EXPECT_TRUE(body.find("default_workdir") == std::string::npos);
}

MT_TEST(an_empty_env_map_is_sent_and_is_not_the_same_as_unset) {
    // `Some({})` asks envd to set no variables; `None` leaves the image's
    // environment alone. Folding one into the other changes what the guest
    // sees.
    InitPostRequest req;
    req.env_vars = std::map<std::string, std::string>();
    MT_EXPECT_EQ(req.ToJson().ToString(), std::string("{\"envVars\":{}}"));
}

// ---- POST /init and GET /files over a recording server --------------------

namespace {

/// Serves a scripted sequence of responses, recording each full request.
class RecordingHttpServer {
 public:
    RecordingHttpServer(const std::vector<std::string>& bodies, int status)
        : bodies_(bodies), status_(status) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int one = 1;
        (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd_, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
        ::listen(fd_, 8);
        socklen_t len = sizeof(a);
        ::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this]() {
            for (size_t i = 0; i < bodies_.size(); ++i) {
                const int c = ::accept(fd_, NULL, NULL);
                if (c < 0) return;
                std::string req;
                for (;;) {
                    char buf[4096];
                    const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    req.append(buf, static_cast<size_t>(n));
                    const std::string::size_type head = req.find("\r\n\r\n");
                    if (head == std::string::npos) continue;
                    // Read the declared body, if any, then stop.
                    const std::string::size_type cl = req.find("Content-Length:");
                    if (cl == std::string::npos) break;
                    const size_t want = static_cast<size_t>(
                        std::strtoul(req.c_str() + cl + 15, NULL, 10));
                    if (req.size() >= head + 4 + want) break;
                }
                requests_.push_back(req);
                std::ostringstream resp;
                resp << "HTTP/1.1 " << status_ << " X\r\nContent-Length: "
                     << bodies_[i].size() << "\r\nConnection: close\r\n\r\n" << bodies_[i];
                const std::string out = resp.str();
                (void)::send(c, out.data(), out.size(), MSG_NOSIGNAL);
                ::close(c);
            }
        });
    }
    ~RecordingHttpServer() {
        ::shutdown(fd_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        ::close(fd_);
    }
    std::string Base() const { return "http://127.0.0.1:" + std::to_string(port_); }
    void Join() {
        if (thread_.joinable()) thread_.join();
    }
    const std::vector<std::string>& requests() const { return requests_; }

 private:
    std::vector<std::string> bodies_;
    int status_;
    int fd_;
    uint16_t port_;
    std::thread thread_;
    std::vector<std::string> requests_;
};

bool Has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// Rust `init_sends_access_token_in_header_and_body`
MT_TEST(init_sends_access_token_in_header_and_body) {
    RecordingHttpServer server(std::vector<std::string>(1, std::string()), 204);
    EnvdInstance envd(server.Base(), agentenv::core::Optional<EnvdAccessToken>(
                                         EnvdAccessToken("tok-abc")));
    MT_EXPECT_TRUE(envd.Init(agentenv::core::Optional<std::map<std::string, std::string> >(),
                             agentenv::core::Optional<std::string>(),
                             agentenv::core::Optional<std::string>())
                       .ok());
    server.Join();
    MT_EXPECT_EQ(server.requests().size(), static_cast<size_t>(1));
    const std::string& req = server.requests()[0];
    MT_EXPECT_TRUE(req.compare(0, 14, "POST /init HTTP") == 0 ||
                   Has(req, "POST /init HTTP"));
    // The header authenticates *this* call; the body is what envd stores and
    // checks on every later one. Sending only one of the two breaks a
    // different half of the lifecycle, so both are asserted.
    MT_EXPECT_TRUE(Has(req, "X-Access-Token: tok-abc"));
    MT_EXPECT_TRUE(Has(req, "\"accessToken\":\"tok-abc\""));
}

MT_TEST(a_plain_user_name_does_a_single_init_with_no_bootstrap) {
    // `needs_resolution` is false, so there is no account read and no
    // bootstrap call. A second `init` here would overwrite the configuration
    // this one just set.
    RecordingHttpServer server(std::vector<std::string>(1, std::string()), 204);
    EnvdInstance envd(server.Base(), agentenv::core::Optional<EnvdAccessToken>());
    std::map<std::string, std::string> envs;
    envs["A"] = "1";
    MT_EXPECT_TRUE(envd.Init(envs, agentenv::core::Optional<std::string>(std::string("/w")),
                             agentenv::core::Optional<std::string>(std::string("user")))
                       .ok());
    server.Join();
    MT_EXPECT_EQ(server.requests().size(), static_cast<size_t>(1));
    MT_EXPECT_TRUE(Has(server.requests()[0], "\"defaultUser\":\"user\""));
    MT_EXPECT_TRUE(Has(server.requests()[0], "\"defaultWorkdir\":\"/w\""));
    MT_EXPECT_TRUE(Has(server.requests()[0], "\"envVars\":{\"A\":\"1\"}"));
}

// Rust `init_resolves_numeric_default_user_and_preserves_image_environment`
MT_TEST(init_resolves_numeric_default_user_and_preserves_image_environment) {
    // 1) bootstrap init, 2) GET /etc/passwd, 3) final init.
    std::vector<std::string> bodies;
    bodies.push_back(std::string());
    bodies.push_back(
        "root:x:0:0:r\xff"
        "ot:/root:/bin/sh\nother:x:1000:1000:\xff:/home/other:/bin/sh\n");
    bodies.push_back(std::string());
    RecordingHttpServer server(bodies, 200);

    EnvdInstance envd(server.Base(), agentenv::core::Optional<EnvdAccessToken>(
                                         EnvdAccessToken("tok")));
    std::map<std::string, std::string> envs;
    envs["HOME"] = "/app";
    const auto r = envd.Init(envs, agentenv::core::Optional<std::string>(std::string("/work")),
                             agentenv::core::Optional<std::string>(std::string("0")));
    MT_EXPECT_TRUE(r.ok());
    server.Join();
    MT_EXPECT_EQ(server.requests().size(), static_cast<size_t>(3));

    // The bootstrap call comes *first* and carries `/` + `root`, which is what
    // makes the account read below authenticate. It must not carry the
    // caller's env: publishing the sandbox's configuration under a user that
    // does not exist yet is the failure this ordering avoids.
    const std::string& bootstrap = server.requests()[0];
    MT_EXPECT_TRUE(Has(bootstrap, "POST /init"));
    MT_EXPECT_TRUE(Has(bootstrap, "\"defaultUser\":\"root\""));
    MT_EXPECT_TRUE(Has(bootstrap, "\"defaultWorkdir\":\"/\""));
    MT_EXPECT_TRUE(!Has(bootstrap, "envVars"));

    // The account read is a GET with the path and username as query
    // parameters and the token as a header.
    // (The expected line is held in a variable because the micro-test macro
    // stringifies its condition into a printf format string, where a `%`
    // would be read as a conversion.)
    const std::string& read = server.requests()[1];
    const std::string want_read = "GET /files?path=%2Fetc%2Fpasswd&username=root";
    MT_EXPECT_TRUE(Has(read, want_read));
    MT_EXPECT_TRUE(Has(read, "X-Access-Token: tok"));

    // Only `defaultUser` was replaced; the caller's env and workdir survive.
    const std::string& final_init = server.requests()[2];
    MT_EXPECT_TRUE(Has(final_init, "\"defaultUser\":\"root\""));
    MT_EXPECT_TRUE(Has(final_init, "\"defaultWorkdir\":\"/work\""));
    MT_EXPECT_TRUE(Has(final_init, "\"envVars\":{\"HOME\":\"/app\"}"));
}

MT_TEST(a_resolution_needing_an_append_fails_without_a_command_runner) {
    // uid 1234 has no account, so a record must be appended. With no process
    // client there is no way to append it -- and returning the name anyway
    // would hand envd a default user that names nothing, turning every later
    // SDK call into an unrelated-looking file error.
    std::vector<std::string> bodies;
    bodies.push_back(std::string());
    bodies.push_back("root:x:0:0::/root:/bin/sh\n");
    RecordingHttpServer server(bodies, 200);
    EnvdInstance envd(server.Base(), agentenv::core::Optional<EnvdAccessToken>());
    const auto r = envd.Init(agentenv::core::Optional<std::map<std::string, std::string> >(),
                             agentenv::core::Optional<std::string>(),
                             agentenv::core::Optional<std::string>(std::string("1234")));
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(Has(r.error(), "1234"));
    MT_EXPECT_TRUE(Has(r.error(), "not available in this build"));
    server.Join();
}

MT_TEST(the_append_runs_the_script_with_busybox_and_the_resolved_entry) {
    std::vector<std::string> bodies;
    bodies.push_back(std::string());
    bodies.push_back("root:x:0:0::/root:/bin/sh\n");
    bodies.push_back(std::string());
    RecordingHttpServer server(bodies, 200);
    EnvdInstance envd(server.Base(), agentenv::core::Optional<EnvdAccessToken>());

    std::string seen_cmd;
    std::vector<std::string> seen_args;
    agentenv::sandbox::ProcessOpts seen_opts;
    envd.SetRootCommandRunner(
        [&](const std::string& cmd, const std::vector<std::string>& args,
            const agentenv::sandbox::ProcessOpts& opts)
            -> agentenv::core::Expected<agentenv::sandbox::ProcessOutput, std::string> {
            seen_cmd = cmd;
            seen_args = args;
            seen_opts = opts;
            agentenv::sandbox::ProcessOutput out;
            out.exit_code = 0;
            return out;
        });

    const auto r = envd.Init(agentenv::core::Optional<std::map<std::string, std::string> >(),
                             agentenv::core::Optional<std::string>(),
                             agentenv::core::Optional<std::string>(std::string("1234")));
    MT_EXPECT_TRUE(r.ok());
    server.Join();

    MT_EXPECT_EQ(seen_cmd, std::string("/agentenv/bin/busybox"));
    MT_EXPECT_EQ(seen_args.size(), static_cast<size_t>(7));
    MT_EXPECT_EQ(seen_args[0], std::string("sh"));
    MT_EXPECT_EQ(seen_args[1], std::string("-c"));
    MT_EXPECT_EQ(seen_args[3], std::string("sh"));
    MT_EXPECT_EQ(seen_args[4], std::string("/agentenv/bin/busybox"));
    MT_EXPECT_EQ(seen_args[5], std::string("/etc/passwd"));
    MT_EXPECT_EQ(seen_args[6], std::string("aenv-1234-0:x:1234:0::/:/bin/sh"));
    // Rust pins both: `/` as the cwd (the guest's default may not exist) and
    // a 10 s timeout, because the script itself waits on a lock.
    MT_EXPECT_TRUE(seen_opts.cwd.has_value() && *seen_opts.cwd == "/");
    MT_EXPECT_TRUE(seen_opts.timeout_ms.has_value() && *seen_opts.timeout_ms == 10000);
    // The name the appended record declares is what reaches envd.
    MT_EXPECT_TRUE(Has(server.requests()[2], "\"defaultUser\":\"aenv-1234-0\""));
}

MT_TEST(a_failed_append_is_an_error_naming_the_requested_user) {
    std::vector<std::string> bodies;
    bodies.push_back(std::string());
    bodies.push_back("root:x:0:0::/root:/bin/sh\n");
    RecordingHttpServer server(bodies, 200);
    EnvdInstance envd(server.Base(), agentenv::core::Optional<EnvdAccessToken>());
    envd.SetRootCommandRunner(
        [](const std::string&, const std::vector<std::string>&,
           const agentenv::sandbox::ProcessOpts&)
            -> agentenv::core::Expected<agentenv::sandbox::ProcessOutput, std::string> {
            agentenv::sandbox::ProcessOutput out;
            out.exit_code = 1;
            out.stderr_data = "timed out waiting for /etc/passwd.lock";
            return out;
        });
    const auto r = envd.Init(agentenv::core::Optional<std::map<std::string, std::string> >(),
                             agentenv::core::Optional<std::string>(),
                             agentenv::core::Optional<std::string>(std::string("1234")));
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(Has(r.error(), "failed to prepare Dockerfile USER 1234"));
    // The script's own diagnostic must survive into the message, or an
    // operator sees only that "something" failed.
    MT_EXPECT_TRUE(Has(r.error(), "timed out waiting for /etc/passwd.lock"));
    server.Join();
}

// ---- GET /files ------------------------------------------------------------

MT_TEST(an_absent_account_file_reads_as_empty_not_as_an_error) {
    // An image need not ship /etc/group. Refusing on 404 would reject every
    // such image; any *other* error status is still an error.
    RecordingHttpServer server(std::vector<std::string>(1, std::string("nope")), 404);
    EnvdInstance envd(server.Base(), agentenv::core::Optional<EnvdAccessToken>());
    const auto r = envd.ReadAccountFile("/etc/group");
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(r.value(), std::string());
    server.Join();
}

MT_TEST(a_server_error_reading_an_account_file_is_an_error) {
    RecordingHttpServer server(std::vector<std::string>(1, std::string("boom")), 500);
    EnvdInstance envd(server.Base(), agentenv::core::Optional<EnvdAccessToken>());
    const auto r = envd.ReadAccountFile("/etc/passwd");
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(Has(r.error(), "read guest /etc/passwd to resolve Dockerfile USER"));
    server.Join();
}

MT_TEST(the_account_file_path_is_query_encoded) {
    // The path is spliced into a query string. An unencoded `&` would end the
    // parameter early and envd would read a different file than the one asked
    // for -- here, the file the default user is derived from.
    RecordingHttpServer server(std::vector<std::string>(1, std::string()), 200);
    EnvdInstance envd(server.Base(), agentenv::core::Optional<EnvdAccessToken>());
    (void)envd.ReadAccountFile("/etc/a&b c#d");
    server.Join();
    const std::string want = "GET /files?path=%2Fetc%2Fa%26b+c%23d&username=root";
    MT_EXPECT_TRUE(Has(server.requests()[0], want));
    // The signed-URL pair must stay absent, or envd would validate a
    // signature that was never produced.
    MT_EXPECT_TRUE(!Has(server.requests()[0], "signature"));
}

// ---- guest log file names --------------------------------------------------

// Rust `guest_log_names_cannot_escape_directory`
MT_TEST(guest_log_names_cannot_escape_directory) {
    MT_EXPECT_TRUE(IsEnvdLogFilename("current"));
    MT_EXPECT_TRUE(IsEnvdLogFilename("@4000000069a0000000000000.s"));
    const char* bad[] = {
        "../current",
        "/etc/shadow",
        "config",
        "@4000000069a0000000000000.u",
        "@../../../../etc/shadow.s",
    };
    for (int i = 0; i < 5; ++i) {
        MT_EXPECT_TRUE(!IsEnvdLogFilename(bad[i]));
    }
    // The length is load-bearing on its own: without it the hex scan would
    // run past the end for a short name and accept a long one.
    MT_EXPECT_TRUE(!IsEnvdLogFilename("@4000000069a000000000000.s"));
    MT_EXPECT_TRUE(!IsEnvdLogFilename("@4000000069a00000000000000.s"));
    MT_EXPECT_TRUE(!IsEnvdLogFilename(""));
}

MT_MAIN
