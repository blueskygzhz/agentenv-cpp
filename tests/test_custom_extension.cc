// SPDX-License-Identifier: MIT
// Tests for sandbox::custom_extension — Rust src/sandbox/custom_extension/
// client.rs #[cfg(test)] mod tests (the lifecycle hooks over a real socket)
// plus the guard semantics.
#include "microtest.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <cctype>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "agentenv/core/json.h"
#include "agentenv/sandbox/custom_extension.h"

namespace {

using namespace agentenv::sandbox;

/// Serves `count` canned HTTP responses, one per connection, and records each
/// request line and body.
class TestServer {
 public:
    TestServer(int status, const std::string& body, std::size_t count)
        : status_(status), body_(body), count_(count) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd_, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
        ::listen(fd_, 8);
        socklen_t len = sizeof(a);
        ::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this]() { Serve(); });
    }
    ~TestServer() {
        ::shutdown(fd_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        ::close(fd_);
    }
    std::string Base() const { return "http://127.0.0.1:" + std::to_string(port_); }
    void Join() {
        if (thread_.joinable()) thread_.join();
    }
    std::size_t size() const { return requests_.size(); }
    const std::string& request_line(std::size_t i) const { return requests_[i].first; }
    const std::string& body(std::size_t i) const { return requests_[i].second; }

 private:
    void Serve() {
        for (std::size_t i = 0; i < count_; ++i) {
            const int c = ::accept(fd_, NULL, NULL);
            if (c < 0) return;
            std::string raw;
            while (raw.find("\r\n\r\n") == std::string::npos) {
                char buf[4096];
                const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                raw.append(buf, static_cast<std::size_t>(n));
            }
            const std::string::size_type end = raw.find("\r\n\r\n");
            const std::string head =
                end == std::string::npos ? raw : raw.substr(0, end);
            const std::string::size_type content_length = ContentLength(head);
            std::string req_body =
                end == std::string::npos ? std::string() : raw.substr(end + 4);
            while (req_body.size() < content_length) {
                char buf[4096];
                const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req_body.append(buf, static_cast<std::size_t>(n));
            }
            requests_.push_back(std::make_pair(
                head.substr(0, head.find("\r\n")),
                req_body.substr(0, std::min(req_body.size(), content_length))));
            std::string response = "HTTP/1.1 " + std::to_string(status_) + " ";
            response += status_ == 200 ? "OK" : (status_ >= 500 ? "Internal Server Error"
                                                                : "Unknown");
            response += "\r\nContent-Type: application/json\r\nContent-Length: " +
                        std::to_string(body_.size()) +
                        "\r\nConnection: close\r\n\r\n" + body_;
            (void)::send(c, response.data(), response.size(), MSG_NOSIGNAL);
            ::close(c);
        }
    }
    static std::string::size_type ContentLength(const std::string& head) {
        std::string::size_type at = 0;
        while (at <= head.size()) {
            std::string::size_type stop = head.find("\r\n", at);
            if (stop == std::string::npos) stop = head.size();
            std::string line = head.substr(at, stop - at);
            for (std::size_t i = 0; i < line.size(); ++i) {
                line[i] = static_cast<char>(::tolower(line[i]));
            }
            if (line.compare(0, 15, "content-length:") == 0) {
                return static_cast<std::string::size_type>(
                    ::atol(Trim(line.substr(15)).c_str()));
            }
            if (stop == head.size()) break;
            at = stop + 2;
        }
        return 0;
    }
    static std::string Trim(const std::string& s) {
        const std::string ws = " \t\r\n";
        const std::string::size_type b = s.find_first_not_of(ws);
        if (b == std::string::npos) return std::string();
        return s.substr(b, s.find_last_not_of(ws) - b + 1);
    }

    int status_;
    std::string body_;
    std::size_t count_;
    int fd_;
    uint16_t port_ = 0;
    std::thread thread_;
    std::vector<std::pair<std::string, std::string> > requests_;
};

std::shared_ptr<custom_extension::HttpClient> TestClient(const std::string& base) {
    return custom_extension::HttpClient::FromConfig(
        agentenv::core::Optional<std::string>(base), 2000);
}

agentenv::core::Optional<custom_extension::Params> Params(const std::string& json) {
    custom_extension::Params p;
    p.json_bytes = json;
    return agentenv::core::Optional<custom_extension::Params>(p);
}

}  // namespace

MT_TEST(params_is_empty) {
    custom_extension::Params p;
    MT_EXPECT_TRUE(custom_extension::ParamsIsEmpty(p));
    p.json_bytes = "{}";
    MT_EXPECT_TRUE(custom_extension::ParamsIsEmpty(p));
    p.json_bytes = "null";
    MT_EXPECT_TRUE(custom_extension::ParamsIsEmpty(p));
    p.json_bytes = "{\"a\":1}";
    MT_EXPECT_TRUE(!custom_extension::ParamsIsEmpty(p));
}

MT_TEST(instance_id_unique) {
    const custom_extension::SandboxInstanceId a = custom_extension::SandboxInstanceId::New();
    const custom_extension::SandboxInstanceId b = custom_extension::SandboxInstanceId::New();
    MT_EXPECT_TRUE(a.ToString() != b.ToString());
    MT_EXPECT_TRUE(a.ToString().size() > 0);
}

MT_TEST(start_fresh_sends_request_and_returns_extra_boot_args) {
    TestServer server(200, "{\"extraBootArgs\":\"foo=bar baz\"}", 1);
    std::shared_ptr<custom_extension::HttpClient> client = TestClient(server.Base());
    const agentenv::core::SandboxId sandbox_id = agentenv::core::SandboxId::Fresh();
    const custom_extension::SandboxInstanceId instance_id =
        custom_extension::SandboxInstanceId::New();

    const agentenv::core::Expected<agentenv::core::Optional<std::string>,
                                   agentenv::core::AnyError> extra =
        client->HookStartFresh(sandbox_id, instance_id, "/var/run/netns/agentenv-ns-test",
                               "10.11.0.123", Params("{\"team\":\"alpha\"}"));
    server.Join();
    MT_EXPECT_TRUE(extra.ok());
    MT_EXPECT_TRUE(extra.value().has_value());
    MT_EXPECT_TRUE(*extra.value() == "foo=bar baz");
    MT_EXPECT_TRUE(server.request_line(0) == "POST /sandbox-hook/start-fresh HTTP/1.1");
    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> json =
        agentenv::core::Json::Parse(server.body(0));
    MT_EXPECT_TRUE(json.ok());
    MT_EXPECT_TRUE(json.value()["sandboxId"].as_string() == sandbox_id.ToString());
    MT_EXPECT_TRUE(json.value()["sandboxInstanceId"].as_string() == instance_id.ToString());
    MT_EXPECT_TRUE(json.value()["networkNamespacePath"].as_string() ==
                   "/var/run/netns/agentenv-ns-test");
    MT_EXPECT_TRUE(json.value()["hostInteractionIp"].as_string() == "10.11.0.123");
    MT_EXPECT_TRUE(json.value()["customExtensionParams"]["team"].as_string() == "alpha");
}

MT_TEST(start_fresh_sends_empty_object_when_params_absent) {
    TestServer server(200, "{}", 1);
    std::shared_ptr<custom_extension::HttpClient> client = TestClient(server.Base());
    const agentenv::core::Expected<agentenv::core::Optional<std::string>,
                                   agentenv::core::AnyError> extra =
        client->HookStartFresh(agentenv::core::SandboxId::Fresh(),
                               custom_extension::SandboxInstanceId::New(), "/ns", "10.11.0.124",
                               agentenv::core::Optional<custom_extension::Params>());
    server.Join();
    MT_EXPECT_TRUE(extra.ok());
    MT_EXPECT_TRUE(!extra.value().has_value());
    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> json =
        agentenv::core::Json::Parse(server.body(0));
    MT_EXPECT_TRUE(json.ok());
    MT_EXPECT_TRUE(json.value()["customExtensionParams"].kind() ==
                   agentenv::core::Json::Kind::Object);
    MT_EXPECT_TRUE(json.value()["customExtensionParams"].as_object().empty());
}

MT_TEST(start_fresh_non_2xx_fails) {
    TestServer server(500, "{}", 1);
    std::shared_ptr<custom_extension::HttpClient> client = TestClient(server.Base());
    const agentenv::core::Expected<agentenv::core::Optional<std::string>,
                                   agentenv::core::AnyError> extra =
        client->HookStartFresh(agentenv::core::SandboxId::Fresh(),
                               custom_extension::SandboxInstanceId::New(), "/ns", "10.11.0.125",
                               agentenv::core::Optional<custom_extension::Params>());
    server.Join();
    MT_EXPECT_TRUE(!extra.ok());
    MT_EXPECT_TRUE(extra.error().chain().find("start-fresh hook failed") != std::string::npos);
}

MT_TEST(start_fresh_4xx_preserves_client_error_status) {
    TestServer server(403, "{\"error\":\"denied\"}", 1);
    std::shared_ptr<custom_extension::HttpClient> client = TestClient(server.Base());
    const agentenv::core::Expected<agentenv::core::Optional<std::string>,
                                   agentenv::core::AnyError> extra =
        client->HookStartFresh(agentenv::core::SandboxId::Fresh(),
                               custom_extension::SandboxInstanceId::New(),
                               "/proc/1000/task/1001/ns/net", "10.11.0.131",
                               agentenv::core::Optional<custom_extension::Params>());
    server.Join();
    MT_EXPECT_TRUE(!extra.ok());
    const custom_extension::HookStatusError* hook_err =
        custom_extension::FindHookStatusError(extra.error());
    MT_EXPECT_TRUE(hook_err != NULL);
    if (hook_err != NULL) {
        MT_EXPECT_TRUE(hook_err->IsClientError());
        MT_EXPECT_TRUE(!hook_err->IsServerError());
        MT_EXPECT_TRUE(hook_err->ToString().find("403") != std::string::npos);
    }
}

MT_TEST(start_guard_stop_carries_matching_instance_id) {
    TestServer server(200, "{}", 2);
    std::shared_ptr<custom_extension::Client> client = TestClient(server.Base());
    const agentenv::core::SandboxId sandbox_id = agentenv::core::SandboxId::Fresh();

    custom_extension::HookGuard guard(client, sandbox_id);
    const agentenv::core::Expected<agentenv::core::Optional<std::string>,
                                    agentenv::core::AnyError> extra =
        guard.StartFresh("/var/run/netns/agentenv-ns-test", "10.11.0.127",
                         agentenv::core::Optional<custom_extension::Params>());
    MT_EXPECT_TRUE(extra.ok());
    MT_EXPECT_TRUE(!extra.value().has_value());
    guard.Stop();
    server.Join();

    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> start =
        agentenv::core::Json::Parse(server.body(0));
    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> stop =
        agentenv::core::Json::Parse(server.body(1));
    MT_EXPECT_TRUE(start.ok() && stop.ok());
    if (start.ok() && stop.ok()) {
        MT_EXPECT_TRUE(stop.value()["sandboxId"].as_string() == sandbox_id.ToString());
        MT_EXPECT_TRUE(start.value()["sandboxInstanceId"].kind() ==
                       agentenv::core::Json::Kind::String);
        MT_EXPECT_TRUE(stop.value()["sandboxInstanceId"].as_string() ==
                       start.value()["sandboxInstanceId"].as_string());
    }
}

MT_TEST(start_resume_sends_request) {
    TestServer server(200, "{}", 1);
    std::shared_ptr<custom_extension::HttpClient> client = TestClient(server.Base());
    const agentenv::core::SandboxId sandbox_id = agentenv::core::SandboxId::Fresh();
    const custom_extension::SandboxInstanceId instance_id =
        custom_extension::SandboxInstanceId::New();
    const agentenv::core::Expected<agentenv::core::Unit, agentenv::core::AnyError> r =
        client->HookStartResume(sandbox_id, instance_id, "/var/run/netns/agentenv-ns-resume",
                                "10.11.0.126",
                                agentenv::core::Optional<custom_extension::Params>());
    server.Join();
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(server.request_line(0) == "POST /sandbox-hook/start-resume HTTP/1.1");
    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> json =
        agentenv::core::Json::Parse(server.body(0));
    MT_EXPECT_TRUE(json.ok());
    if (json.ok()) {
        MT_EXPECT_TRUE(json.value()["sandboxId"].as_string() == sandbox_id.ToString());
        MT_EXPECT_TRUE(json.value()["sandboxInstanceId"].as_string() == instance_id.ToString());
        MT_EXPECT_TRUE(json.value()["networkNamespacePath"].as_string() ==
                       "/var/run/netns/agentenv-ns-resume");
        MT_EXPECT_TRUE(json.value()["hostInteractionIp"].as_string() == "10.11.0.126");
    }
}

MT_TEST(patch_params_passes_patch_and_returns_full_params) {
    TestServer server(200, "{\"customExtensionParams\":{\"team\":\"beta\",\"rev\":2}}", 1);
    std::shared_ptr<custom_extension::HttpClient> client = TestClient(server.Base());
    const agentenv::core::SandboxId sandbox_id = agentenv::core::SandboxId::Fresh();
    const agentenv::core::Expected<agentenv::core::Optional<custom_extension::Params>,
                                   agentenv::core::AnyError> patched =
        client->HookPatchParams(sandbox_id, *Params("{\"op\":\"set\",\"team\":\"beta\"}"));
    server.Join();
    MT_EXPECT_TRUE(patched.ok());
    MT_EXPECT_TRUE(patched.value().has_value());
    if (patched.ok() && patched.value().has_value()) {
        MT_EXPECT_TRUE(patched.value()->json_bytes.find("\"team\":\"beta\"") !=
                       std::string::npos);
    }
    MT_EXPECT_TRUE(server.request_line(0) == "POST /sandbox-hook/patch-params HTTP/1.1");
    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> json =
        agentenv::core::Json::Parse(server.body(0));
    MT_EXPECT_TRUE(json.ok());
    if (json.ok()) {
        MT_EXPECT_TRUE(json.value()["sandboxId"].as_string() == sandbox_id.ToString());
        MT_EXPECT_TRUE(json.value()["patch"]["team"].as_string() == "beta");
    }
}

MT_TEST(patch_params_maps_empty_response_to_empty_params) {
    TestServer server(200, "{}", 1);
    std::shared_ptr<custom_extension::HttpClient> client = TestClient(server.Base());
    const agentenv::core::Expected<agentenv::core::Optional<custom_extension::Params>,
                                   agentenv::core::AnyError> patched =
        client->HookPatchParams(agentenv::core::SandboxId::Fresh(), *Params("{\"op\":\"clear\"}"));
    server.Join();
    MT_EXPECT_TRUE(patched.ok());
    MT_EXPECT_TRUE(!patched.value().has_value());
}

MT_TEST(patch_params_5xx_preserves_server_error_status) {
    TestServer server(502, "{\"error\":\"bad gateway\"}", 1);
    std::shared_ptr<custom_extension::HttpClient> client = TestClient(server.Base());
    const agentenv::core::Expected<agentenv::core::Optional<custom_extension::Params>,
                                   agentenv::core::AnyError> patched =
        client->HookPatchParams(agentenv::core::SandboxId::Fresh(), *Params("{}"));
    server.Join();
    MT_EXPECT_TRUE(!patched.ok());
    const custom_extension::HookStatusError* hook_err =
        custom_extension::FindHookStatusError(patched.error());
    MT_EXPECT_TRUE(hook_err != NULL);
    if (hook_err != NULL) {
        MT_EXPECT_TRUE(hook_err->IsServerError());
        MT_EXPECT_TRUE(!hook_err->IsClientError());
        MT_EXPECT_TRUE(hook_err->ToString().find("502") != std::string::npos);
    }
}

MT_TEST(stop_sends_sandbox_and_instance_ids) {
    TestServer server(200, "{}", 1);
    std::shared_ptr<custom_extension::HttpClient> client = TestClient(server.Base());
    const agentenv::core::SandboxId sandbox_id = agentenv::core::SandboxId::Fresh();
    const custom_extension::SandboxInstanceId instance_id =
        custom_extension::SandboxInstanceId::New();
    client->HookStop(sandbox_id, instance_id);
    server.Join();
    MT_EXPECT_TRUE(server.request_line(0) == "POST /sandbox-hook/stop HTTP/1.1");
    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> json =
        agentenv::core::Json::Parse(server.body(0));
    MT_EXPECT_TRUE(json.ok());
    if (json.ok()) {
        MT_EXPECT_TRUE(json.value()["sandboxId"].as_string() == sandbox_id.ToString());
        MT_EXPECT_TRUE(json.value()["sandboxInstanceId"].as_string() == instance_id.ToString());
        MT_EXPECT_TRUE(json.value()["networkNamespacePath"].kind() ==
                       agentenv::core::Json::Kind::Null);
    }
}

MT_TEST(hook_guard_fires_stop_on_drop) {
    TestServer server(200, "{}", 2);
    std::shared_ptr<custom_extension::Client> client = TestClient(server.Base());
    const agentenv::core::SandboxId sandbox_id = agentenv::core::SandboxId::Fresh();
    {
        custom_extension::HookGuard guard(client, sandbox_id);
        const agentenv::core::Expected<agentenv::core::Optional<std::string>,
                                        agentenv::core::AnyError> extra =
            guard.StartFresh("/var/run/netns/agentenv-ns-test", "10.11.0.130",
                             agentenv::core::Optional<custom_extension::Params>());
        MT_EXPECT_TRUE(extra.ok());
        MT_EXPECT_TRUE(guard.InstanceId().has_value());
    }  // dropped: fires the stop hook fire-and-forget
    // The notification runs on the shared executor; the server thread waits
    // for it, so joining is what makes it observable here.
    server.Join();
    MT_EXPECT_TRUE(server.size() == 2);
    MT_EXPECT_TRUE(server.request_line(1) == "POST /sandbox-hook/stop HTTP/1.1");
    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> start =
        agentenv::core::Json::Parse(server.body(0));
    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> stop =
        agentenv::core::Json::Parse(server.body(1));
    MT_EXPECT_TRUE(start.ok() && stop.ok());
    if (start.ok() && stop.ok()) {
        MT_EXPECT_TRUE(stop.value()["sandboxInstanceId"].as_string() ==
                       start.value()["sandboxInstanceId"].as_string());
    }
}

MT_TEST(failed_start_still_stops_on_drop_with_matching_instance_id) {
    // The instance id is recorded before the start hook is delivered, so even
    // a failed start gets a matching stop on teardown.
    TestServer server(500, "{}", 2);
    std::shared_ptr<custom_extension::Client> client = TestClient(server.Base());
    const agentenv::core::SandboxId sandbox_id = agentenv::core::SandboxId::Fresh();
    {
        custom_extension::HookGuard guard(client, sandbox_id);
        const agentenv::core::Expected<agentenv::core::Optional<std::string>,
                                        agentenv::core::AnyError> extra =
            guard.StartFresh("/var/run/netns/agentenv-ns-test", "10.11.0.129",
                             agentenv::core::Optional<custom_extension::Params>());
        MT_EXPECT_TRUE(!extra.ok());
    }
    server.Join();
    MT_EXPECT_TRUE(server.size() == 2);
    MT_EXPECT_TRUE(server.request_line(1) == "POST /sandbox-hook/stop HTTP/1.1");
}

MT_TEST(hook_guard_stop_delivers_once) {
    TestServer server(200, "{}", 2);
    std::shared_ptr<custom_extension::Client> client = TestClient(server.Base());
    custom_extension::HookGuard guard(client, agentenv::core::SandboxId::Fresh());
    const agentenv::core::Expected<agentenv::core::Unit, agentenv::core::AnyError> r =
        guard.StartResume("/var/run/netns/agentenv-ns-resume", "10.11.0.128",
                          agentenv::core::Optional<custom_extension::Params>());
    MT_EXPECT_TRUE(r.ok());
    guard.Stop();
    server.Join();
    MT_EXPECT_TRUE(server.size() == 2);
    MT_EXPECT_TRUE(server.request_line(1) == "POST /sandbox-hook/stop HTTP/1.1");
    // Cleared: destroying the guard must not fire again.
}

MT_TEST(from_config_rejects_unusable_urls) {
    MT_EXPECT_TRUE(!custom_extension::HttpClient::FromConfig(
                        agentenv::core::Optional<std::string>(), 5000));
    MT_EXPECT_TRUE(!custom_extension::HttpClient::FromConfig(
                       agentenv::core::Optional<std::string>(std::string("  ")), 5000));
    // No TLS client in this build.
    MT_EXPECT_TRUE(!custom_extension::HttpClient::FromConfig(
                       agentenv::core::Optional<std::string>(
                           std::string("https://127.0.0.1:1/hooks/")), 5000));
    std::shared_ptr<custom_extension::HttpClient> ok =
        custom_extension::HttpClient::FromConfig(
            agentenv::core::Optional<std::string>(std::string("http://127.0.0.1:9/ext/")), 5000);
    MT_EXPECT_TRUE(ok);
    if (ok) {
        MT_EXPECT_TRUE(ok->path_prefix() == "/ext");
        MT_EXPECT_TRUE(ok->target().host == "127.0.0.1");
        MT_EXPECT_TRUE(ok->target().port == 9);
    }
}

MT_MAIN
