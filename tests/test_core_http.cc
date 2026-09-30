// SPDX-License-Identifier: MIT
// Rust: hyper / hyper-util legacy Client, as used by
// src/sandbox/firecracker/socket.rs.
//
// The parsing tests are pure; the client tests run against a real AF_UNIX
// server in a background thread, so the connect/write/read path and the
// timeouts are exercised rather than mocked.
#include "agentenv/core/http.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/sandbox/firecracker/socket.h"
#include "microtest.h"

using namespace agentenv;              // NOLINT
using namespace agentenv::core::http;  // NOLINT

namespace {

/// A one-shot AF_UNIX server that replies with a canned response.
///
/// `hold_ms` delays the reply, which is how the read timeout is exercised
/// without depending on wall-clock luck.
class ScriptedUnixServer {
 public:
    ScriptedUnixServer(const std::string& path, const std::string& reply, int hold_ms = 0,
                       bool close_without_reply = false)
        : path_(path), reply_(reply), hold_ms_(hold_ms),
          close_without_reply_(close_without_reply), listen_fd_(-1), stop_(false) {
        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        MT_EXPECT_TRUE(listen_fd_ >= 0);

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        ::unlink(path_.c_str());
        MT_EXPECT_TRUE(::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                              sizeof(addr)) == 0);
        MT_EXPECT_TRUE(::listen(listen_fd_, 4) == 0);

        worker_ = std::thread([this]() { Serve(); });
    }

    ~ScriptedUnixServer() {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
        if (listen_fd_ >= 0) ::close(listen_fd_);
        ::unlink(path_.c_str());
    }

    const std::string& request() const { return request_; }

 private:
    /// Serves connections until the destructor asks to stop.
    ///
    /// More than one is needed because the Firecracker connector probes the
    /// socket (connect + close) before the real request, so a one-shot server
    /// would make the request itself find nothing listening.
    void Serve() {
        while (!stop_) {
            struct pollfd pfd;
            pfd.fd      = listen_fd_;
            pfd.events  = POLLIN;
            pfd.revents = 0;
            // Polled rather than blocking, so the destructor's stop flag is
            // actually observed.
            if (::poll(&pfd, 1, 20) <= 0) continue;

            const int fd = ::accept(listen_fd_, NULL, NULL);
            if (fd < 0) continue;

            // The tests send small bodies, so one read captures the request.
            char buffer[8192];
            const ssize_t got = ::read(fd, buffer, sizeof(buffer));
            // A bare probe reads as EOF; only a real request is recorded.
            if (got > 0) request_.assign(buffer, static_cast<std::size_t>(got));

            if (got > 0 && hold_ms_ > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms_));
            }
            if (got > 0 && !close_without_reply_) {
                ssize_t ignored = ::write(fd, reply_.data(), reply_.size());
                (void)ignored;
            }
            ::close(fd);
        }
    }

    std::string path_;
    std::string reply_;
    int         hold_ms_;
    bool        close_without_reply_;
    int               listen_fd_;
    std::atomic<bool> stop_;
    std::string       request_;
    std::thread       worker_;
};

struct TempRoot {
    std::string path;
    TempRoot() {
        const core::Expected<std::string, std::string> dir =
            core::fs::CreateTempDir("agentenv-http-");
        path = dir.ok() ? dir.value() : std::string("/tmp/agentenv-http-fallback");
        core::fs::CreateDirAll(path);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
    std::string Socket() const { return path + "/api.sock"; }
};

Headers HeadersWith(const std::string& name, const std::string& value) {
    Headers headers;
    headers[name] = value;
    return headers;
}

}  // namespace

// ---- response parsing ------------------------------------------------------

MT_TEST(http_parses_a_content_length_response) {
    const core::Expected<Response, std::string> response = ParseResponse(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 13\r\n\r\n"
        "{\"ok\":true}\r\n");
    MT_EXPECT_TRUE(response.ok());
    MT_EXPECT_EQ(response.value().status, 200);
    MT_EXPECT_TRUE(response.value().IsSuccess());
    MT_EXPECT_EQ(response.value().body, std::string("{\"ok\":true}\r\n"));
    MT_EXPECT_EQ(*FindHeader(response.value().headers, "content-type"),
                 std::string("application/json"));
}

MT_TEST(http_trims_a_body_past_the_declared_length) {
    // The declared framing is authoritative: a peer that sends more must not
    // leak the extra bytes into the body.
    const core::Expected<Response, std::string> response =
        ParseResponse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokEXTRA");
    MT_EXPECT_TRUE(response.ok());
    MT_EXPECT_EQ(response.value().body, std::string("ok"));
}

MT_TEST(http_rejects_a_truncated_content_length_body) {
    // Returning a short body as if it were complete would hand the caller
    // half a JSON document.
    MT_EXPECT_TRUE(
        !ParseResponse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort").ok());
}

MT_TEST(http_parses_a_chunked_response) {
    const core::Expected<Response, std::string> response =
        ParseResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
    MT_EXPECT_TRUE(response.ok());
    // The chunk headers must not survive into the body.
    MT_EXPECT_EQ(response.value().body, std::string("hello world"));
}

MT_TEST(http_chunked_decoding_edge_cases) {
    MT_EXPECT_EQ(DecodeChunked("0\r\n\r\n").value(), std::string(""));
    // Chunk extensions after ';' are not part of the size.
    MT_EXPECT_EQ(DecodeChunked("5;name=value\r\nhello\r\n0\r\n\r\n").value(),
                 std::string("hello"));
    // Hex sizes, including uppercase.
    MT_EXPECT_EQ(DecodeChunked("A\r\n0123456789\r\n0\r\n\r\n").value(),
                 std::string("0123456789"));

    MT_EXPECT_TRUE(!DecodeChunked("5\r\nhi\r\n0\r\n\r\n").ok());   // truncated chunk
    MT_EXPECT_TRUE(!DecodeChunked("zz\r\n").ok());                 // malformed size
    MT_EXPECT_TRUE(!DecodeChunked("5\r\nhello").ok());             // no terminator
}

MT_TEST(http_treats_204_and_304_as_bodyless) {
    // No framing headers and no body by definition; anything following must
    // not be reported as content.
    const core::Expected<Response, std::string> no_content =
        ParseResponse("HTTP/1.1 204 No Content\r\n\r\n");
    MT_EXPECT_TRUE(no_content.ok());
    MT_EXPECT_EQ(no_content.value().status, 204);
    MT_EXPECT_TRUE(no_content.value().body.empty());
    MT_EXPECT_TRUE(no_content.value().IsSuccess());

    MT_EXPECT_TRUE(ParseResponse("HTTP/1.1 304 Not Modified\r\n\r\n").value().body.empty());
}

MT_TEST(http_reads_an_unframed_body_to_the_end) {
    // No Content-Length and no chunking: the body runs to EOF.
    const core::Expected<Response, std::string> response =
        ParseResponse("HTTP/1.1 200 OK\r\n\r\nraw payload");
    MT_EXPECT_TRUE(response.ok());
    MT_EXPECT_EQ(response.value().body, std::string("raw payload"));
}

MT_TEST(http_surfaces_error_statuses_rather_than_the_body_alone) {
    const core::Expected<Response, std::string> response = ParseResponse(
        "HTTP/1.1 400 Bad Request\r\nContent-Length: 28\r\n\r\n"
        "{\"fault_message\":\"bad path\"}");
    MT_EXPECT_TRUE(response.ok());
    // A 400 is a valid *response*, not a transport error: the caller decides.
    MT_EXPECT_EQ(response.value().status, 400);
    MT_EXPECT_TRUE(!response.value().IsSuccess());
    MT_EXPECT_TRUE(response.value().body.find("bad path") != std::string::npos);
}

MT_TEST(http_rejects_malformed_framing) {
    MT_EXPECT_TRUE(!ParseResponse("").ok());
    MT_EXPECT_TRUE(!ParseResponse("HTTP/1.1 200 OK\r\n").ok());        // no terminator
    MT_EXPECT_TRUE(!ParseResponse("NOT-HTTP 200 OK\r\n\r\n").ok());    // wrong protocol
    MT_EXPECT_TRUE(!ParseResponse("HTTP/1.1 abc OK\r\n\r\n").ok());    // non-numeric status
    MT_EXPECT_TRUE(!ParseResponse("HTTP/1.1 99 X\r\n\r\n").ok());      // out of range
    MT_EXPECT_TRUE(!ParseResponse("HTTP/1.1 600 X\r\n\r\n").ok());
    MT_EXPECT_TRUE(
        !ParseResponse("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n").ok());
}

MT_TEST(http_header_lookup_is_case_insensitive) {
    const Headers headers = HeadersWith("Content-Length", "5");
    MT_EXPECT_TRUE(FindHeader(headers, "content-length").has_value());
    MT_EXPECT_TRUE(FindHeader(headers, "CONTENT-LENGTH").has_value());
    MT_EXPECT_TRUE(!FindHeader(headers, "content-type").has_value());
}

MT_TEST(http_parses_a_status_line_without_a_reason_phrase) {
    const core::Expected<Response, std::string> response = ParseResponse("HTTP/1.1 200\r\n\r\n");
    MT_EXPECT_TRUE(response.ok());
    MT_EXPECT_EQ(response.value().status, 200);
}

// ---- request serialization -------------------------------------------------

MT_TEST(http_serializes_a_request) {
    Request request;
    request.method            = "PUT";
    request.path              = "/machine-config";
    request.body              = "{\"vcpu_count\":2}";
    request.headers["Accept"] = "application/json";

    const std::string wire = SerializeRequest(request, Target::UnixSocket("/tmp/x.sock"));
    MT_EXPECT_TRUE(wire.find("PUT /machine-config HTTP/1.1\r\n") == 0);
    // A Unix socket has no authority, so the Host matches hyper's
    // `http://localhost{path}` URI.
    MT_EXPECT_TRUE(wire.find("Host: localhost\r\n") != std::string::npos);
    // A body must be framed: without a length the peer would read until close,
    // which never happens on a request.
    MT_EXPECT_TRUE(wire.find("Content-Length: 16\r\n") != std::string::npos);
    MT_EXPECT_TRUE(wire.find("Accept: application/json\r\n") != std::string::npos);
    MT_EXPECT_TRUE(wire.find("\r\n\r\n{\"vcpu_count\":2}") != std::string::npos);
}

MT_TEST(http_serialization_ignores_caller_host_and_connection) {
    Request request;
    request.headers["Host"]       = "evil.example";
    request.headers["Connection"] = "keep-alive";

    // These two are ours: a caller override would make the framing on the wire
    // disagree with what the client actually does.
    const std::string wire = SerializeRequest(request, Target::UnixSocket("/tmp/x.sock"));
    MT_EXPECT_TRUE(wire.find("evil.example") == std::string::npos);
    MT_EXPECT_TRUE(wire.find("keep-alive") == std::string::npos);
    MT_EXPECT_TRUE(wire.find("Connection: close\r\n") != std::string::npos);
}

MT_TEST(http_serializes_a_tcp_host_header) {
    Request request;
    MT_EXPECT_TRUE(SerializeRequest(request, Target::Tcp("example.test", 8080))
                       .find("Host: example.test:8080\r\n") != std::string::npos);
    // The default port is omitted, as a conforming client does.
    MT_EXPECT_TRUE(SerializeRequest(request, Target::Tcp("example.test", 80))
                       .find("Host: example.test\r\n") != std::string::npos);
}

MT_TEST(http_bodyless_request_has_no_content_length) {
    Request request;
    request.method = "GET";
    MT_EXPECT_TRUE(SerializeRequest(request, Target::UnixSocket("/tmp/x.sock"))
                       .find("Content-Length") == std::string::npos);
}

// ---- live client over AF_UNIX ---------------------------------------------

MT_TEST(http_client_round_trips_over_a_unix_socket) {
    TempRoot root;
    ScriptedUnixServer server(
        root.Socket(),
        "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n{\"ok\":true}");

    const HttpClient client(Target::UnixSocket(root.Socket()));
    const core::Expected<std::string, std::string> body =
        client.RequestJson("PUT", "/machine-config", "{\"vcpu_count\":2}");
    MT_EXPECT_TRUE(body.ok());
    MT_EXPECT_EQ(body.value(), std::string("{\"ok\":true}"));

    // The request really went out in the Firecracker dialect.
    MT_EXPECT_TRUE(server.request().find("PUT /machine-config HTTP/1.1") == 0);
    MT_EXPECT_TRUE(server.request().find("Content-Type: application/json") !=
                   std::string::npos);
}

MT_TEST(http_client_reports_a_firecracker_rejection) {
    TempRoot root;
    ScriptedUnixServer server(root.Socket(),
                              "HTTP/1.1 400 Bad Request\r\nContent-Length: 33\r\n\r\n"
                              "{\"fault_message\":\"invalid drive\"}");

    // This is the bug the previous implementation had: a 400 was reported as
    // success because only the body was returned.
    const HttpClient client(Target::UnixSocket(root.Socket()));
    const core::Expected<std::string, std::string> body = client.RequestJson("PUT", "/drives/1", "{}");
    MT_EXPECT_TRUE(!body.ok());
    MT_EXPECT_TRUE(body.error().find("400") != std::string::npos);
    // The fault body is what says *why*, so it must survive.
    MT_EXPECT_TRUE(body.error().find("invalid drive") != std::string::npos);
}

MT_TEST(http_client_accepts_an_empty_204) {
    TempRoot root;
    ScriptedUnixServer server(root.Socket(), "HTTP/1.1 204 No Content\r\n\r\n");

    const HttpClient client(Target::UnixSocket(root.Socket()));
    const core::Expected<std::string, std::string> body = client.RequestJson("PUT", "/actions", "{}");
    MT_EXPECT_TRUE(body.ok());
    MT_EXPECT_TRUE(body.value().empty());
}

MT_TEST(http_client_times_out_on_a_silent_peer) {
    TempRoot root;
    // Accepts, then never replies: exactly the wedged-VMM case.
    ScriptedUnixServer server(root.Socket(), "", 400, true);

    Timeouts timeouts;
    timeouts.connect_ms = 1000;
    timeouts.read_ms    = 50;

    const HttpClient client(Target::UnixSocket(root.Socket()), timeouts);
    const core::Expected<std::string, std::string> body = client.RequestJson("GET", "/", "");
    MT_EXPECT_TRUE(!body.ok());
    // Bounded, so a hung peer costs one request rather than a pool worker.
    MT_EXPECT_TRUE(body.error().find("timed out") != std::string::npos ||
                   body.error().find("without a response") != std::string::npos);
}

MT_TEST(http_client_reports_a_missing_socket) {
    TempRoot root;
    const HttpClient client(Target::UnixSocket(root.path + "/absent.sock"));
    const core::Expected<std::string, std::string> body = client.RequestJson("GET", "/", "");
    MT_EXPECT_TRUE(!body.ok());
    MT_EXPECT_TRUE(body.error().find("connect") != std::string::npos);
}

MT_TEST(http_client_runs_on_the_thread_pool) {
    TempRoot root;
    ScriptedUnixServer server(root.Socket(),
                              "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    const HttpClient client(Target::UnixSocket(root.Socket()));
    // Rust awaits a future on the runtime; here the blocking call moves to a
    // pool thread so the caller is not serialised behind it.
    std::future<core::Expected<std::string, std::string> > pending =
        client.RequestJsonAsync("GET", "/", "");
    const core::Expected<std::string, std::string> body = pending.get();
    MT_EXPECT_TRUE(body.ok());
    MT_EXPECT_EQ(body.value(), std::string("ok"));
}

// ---- firecracker connector -------------------------------------------------

MT_TEST(firecracker_connector_is_lazy) {
    TempRoot root;
    // Matches Rust, where `UnixSocketClient::new` is infallible: no connection
    // is made until a request needs one. An eager probe would cost an extra
    // connection per API call and would still be racy.
    const std::shared_ptr<sandbox::firecracker::Connector> connector =
        sandbox::firecracker::MakeUnixConnector();
    const core::Expected<std::shared_ptr<sandbox::firecracker::Socket>, std::string> socket =
        connector->Connect(root.path + "/absent.sock");
    MT_EXPECT_TRUE(socket.ok());
    // The failure surfaces on first use instead.
    MT_EXPECT_TRUE(!socket.value()->Request("GET", "/", "").ok());
}

MT_TEST(firecracker_socket_surfaces_the_status) {
    TempRoot root;
    ScriptedUnixServer server(root.Socket(),
                              "HTTP/1.1 400 Bad Request\r\nContent-Length: 20\r\n\r\n"
                              "{\"fault\":\"nope\"}xxxx");

    const std::shared_ptr<sandbox::firecracker::Connector> connector =
        sandbox::firecracker::MakeUnixConnector();
    const core::Expected<std::shared_ptr<sandbox::firecracker::Socket>, std::string> socket =
        connector->Connect(root.Socket());
    MT_EXPECT_TRUE(socket.ok());

    const core::Expected<std::string, std::string> body =
        socket.value()->Request("PUT", "/drives/1", "{}");
    MT_EXPECT_TRUE(!body.ok());
    MT_EXPECT_TRUE(body.error().find("400") != std::string::npos);
}

int main() { return microtest::RunAll(); }
