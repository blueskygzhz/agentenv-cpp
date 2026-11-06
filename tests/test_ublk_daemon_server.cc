// SPDX-License-Identifier: MIT
// Tests for storage/ublk-daemon's server, ported against
// `storage/ublk-daemon/src/server.rs`.
//
// These cover the two things in that file that are silent when wrong.
//
// The first is the device handoff. A reply carrying a `dev_id` is the client's
// only record of the device, so a reply that was written but never read leaks
// a device nobody will release -- and "roll it back anyway" is worse, because
// releasing a device a live sandbox is using corrupts it. Neither mistake
// shows up on the happy path, and neither is reachable through a real socket
// without a daemon that can be made to misbehave, which is why the device
// plane is behind an interface here.
//
// The second is error classification. `TerminalError`, `InvalidRequest` and
// `Error` all read as "it failed" at a glance, but the client's retry decision
// reads the difference: a terminal error means the live runtime may already
// have been rewritten.
#include "agentenv/storage/ublk-daemon/server.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <vector>

#include "agentenv/storage/ublk-daemon/client.h"
#include "microtest.h"

namespace ud = agentenv::storage::ublk_daemon;
namespace core = agentenv::core;

namespace {

/// A `DeviceHandler` whose answers the test writes. Records every request it
/// saw, including the ones the server synthesizes for a rollback -- which is
/// the only way to observe that a rollback happened at all.
class ScriptedHandler : public ud::DeviceHandler {
 public:
    ScriptedHandler() : has_pool(false), stop_calls(0), fail_from_call(-1) {}

    core::Expected<ud::DaemonResponse, ud::HandlerError> Handle(
        const ud::DaemonRequest& request) {
        seen.push_back(request);
        const int call = static_cast<int>(seen.size()) - 1;
        if (fail_from_call >= 0 && call >= fail_from_call) {
            return core::make_unexpected(ud::HandlerError::Internal("scripted failure"));
        }
        if (!error.message.empty()) {
            return core::make_unexpected(error);
        }
        return reply;
    }
    bool HasPool() const { return has_pool; }
    void StopAllDevices() { ++stop_calls; }

    ud::DaemonResponse reply;
    ud::HandlerError error;
    bool has_pool;
    int stop_calls;
    /// Makes `Handle` fail from the n-th call onward. Used to fail only the
    /// rollback, which is the call the server makes on its own.
    int fail_from_call;
    std::vector<ud::DaemonRequest> seen;
};

/// A connected pair of unix stream sockets: `server` is what `ServeConnection`
/// is given, `client` is what a `DaemonClient` would hold.
struct Pair {
    Pair() {
        int sv[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            server = client = -1;
            return;
        }
        server = sv[0];
        client = sv[1];
    }
    ~Pair() {
        CloseServer();
        CloseClient();
    }
    void CloseServer() {
        if (server >= 0) ::close(server);
        server = -1;
    }
    void CloseClient() {
        if (client >= 0) ::close(client);
        client = -1;
    }

    int server;
    int client;

 private:
    Pair(const Pair&);
    Pair& operator=(const Pair&);
};

void WriteAll(int fd, const std::vector<uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += static_cast<std::size_t>(n);
    }
}

void SendRequest(int fd, const ud::DaemonRequest& request) {
    WriteAll(fd, ud::EncodeMessage(request.ToJson()));
}

/// Reads one framed response, or nothing if the peer closed first.
core::Optional<ud::DaemonResponse> ReadResponse(int fd) {
    std::vector<uint8_t> buf;
    uint8_t chunk[4096];
    for (;;) {
        core::Json out;
        std::size_t consumed = 0;
        std::string err;
        const ud::DecodeStatus status = ud::DecodeMessage(
            buf.empty() ? NULL : buf.data(), buf.size(), &out, &consumed, &err);
        if (status == ud::DecodeStatus::Ok) {
            const core::Expected<ud::DaemonResponse, std::string> decoded =
                ud::DaemonResponse::FromJson(out);
            if (!decoded.ok()) return core::Optional<ud::DaemonResponse>();
            return core::Optional<ud::DaemonResponse>(decoded.value());
        }
        if (status == ud::DecodeStatus::Malformed) return core::Optional<ud::DaemonResponse>();
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return core::Optional<ud::DaemonResponse>();
        buf.insert(buf.end(), chunk, chunk + n);
    }
}

ud::DaemonRequest Req(ud::DaemonRequest::Kind kind) {
    ud::DaemonRequest r;
    r.kind = kind;
    return r;
}

std::string UniqueSocketPath() {
    char buf[] = "/tmp/agentenv-ud-srv-XXXXXX";
    const char* const dir = ::mkdtemp(buf);
    if (dir == NULL) return std::string();
    return std::string(dir) + "/daemon.sock";
}

}  // namespace

// ── framing ────────────────────────────────────────────────────────────────

MT_TEST(a_connection_that_sends_nothing_is_not_an_error) {
    // A client that connects and leaves is how a liveness probe looks, and
    // Rust's `recv_message` answers `Ok(None)` for it. Reporting it as a
    // failed connection would fill the log with noise that means nothing.
    Pair p;
    ScriptedHandler handler;
    ud::ShutdownFlag flag;
    p.CloseClient();

    const core::Expected<core::Unit, std::string> served =
        ud::ServeConnection(p.server, &handler, &flag);
    MT_EXPECT_TRUE(served.ok());
    MT_EXPECT_EQ(handler.seen.size(), static_cast<std::size_t>(0));
}

MT_TEST(a_request_is_dispatched_and_its_reply_framed_back) {
    Pair p;
    ScriptedHandler handler;
    ud::ShutdownFlag flag;
    handler.reply.status = ud::DaemonResponse::Status::Features;
    handler.reply.flags = 0x2a;

    SendRequest(p.client, Req(ud::DaemonRequest::Kind::GetFeatures));
    const core::Expected<core::Unit, std::string> served =
        ud::ServeConnection(p.server, &handler, &flag);
    MT_EXPECT_TRUE(served.ok());

    MT_EXPECT_EQ(handler.seen.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(handler.seen[0].kind == ud::DaemonRequest::Kind::GetFeatures);

    const core::Optional<ud::DaemonResponse> reply = ReadResponse(p.client);
    MT_EXPECT_TRUE(reply.has_value());
    MT_EXPECT_TRUE(reply->status == ud::DaemonResponse::Status::Features);
    MT_EXPECT_EQ(reply->flags, static_cast<uint64_t>(0x2a));
}

MT_TEST(the_length_prefix_is_read_big_endian) {
    // Rust writes `len.to_be_bytes()`. The same body with a little-endian
    // prefix must not be accepted: a byte-order mistake that happened to work
    // for short messages would corrupt every long one.
    const std::string body = "{\"kind\":\"get_features\"}";
    const uint32_t len = static_cast<uint32_t>(body.size());

    {
        Pair p;
        ScriptedHandler handler;
        ud::ShutdownFlag flag;
        handler.reply.status = ud::DaemonResponse::Status::Ok;
        std::vector<uint8_t> frame;
        frame.push_back(static_cast<uint8_t>((len >> 24) & 0xff));
        frame.push_back(static_cast<uint8_t>((len >> 16) & 0xff));
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
        frame.push_back(static_cast<uint8_t>(len & 0xff));
        frame.insert(frame.end(), body.begin(), body.end());
        WriteAll(p.client, frame);
        MT_EXPECT_TRUE(ud::ServeConnection(p.server, &handler, &flag).ok());
        MT_EXPECT_EQ(handler.seen.size(), static_cast<std::size_t>(1));
    }
    {
        Pair p;
        ScriptedHandler handler;
        ud::ShutdownFlag flag;
        std::vector<uint8_t> frame;
        frame.push_back(static_cast<uint8_t>(len & 0xff));
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
        frame.push_back(static_cast<uint8_t>((len >> 16) & 0xff));
        frame.push_back(static_cast<uint8_t>((len >> 24) & 0xff));
        frame.insert(frame.end(), body.begin(), body.end());
        WriteAll(p.client, frame);
        const core::Expected<core::Unit, std::string> served =
            ud::ServeConnection(p.server, &handler, &flag);
        // The size guard is what catches it, which is why the limit exists.
        MT_EXPECT_TRUE(!served.ok());
        MT_EXPECT_TRUE(served.error().find("too large") != std::string::npos);
        MT_EXPECT_EQ(handler.seen.size(), static_cast<std::size_t>(0));
    }
}

MT_TEST(a_truncated_body_is_an_error_not_an_empty_request) {
    // EOF before the prefix is complete means "nothing was sent"; EOF inside
    // the body means the message was cut in half. Folding the second into the
    // first would make a corrupt stream look like an idle client.
    Pair p;
    ScriptedHandler handler;
    ud::ShutdownFlag flag;
    std::vector<uint8_t> frame;
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(100);
    for (int i = 0; i < 10; ++i) frame.push_back('x');
    WriteAll(p.client, frame);
    p.CloseClient();

    const core::Expected<core::Unit, std::string> served =
        ud::ServeConnection(p.server, &handler, &flag);
    MT_EXPECT_TRUE(!served.ok());
    MT_EXPECT_TRUE(served.error().find("read message body") != std::string::npos);
}

MT_TEST(an_unintelligible_body_is_reported_without_reaching_the_handler) {
    Pair p;
    ScriptedHandler handler;
    ud::ShutdownFlag flag;
    core::Json garbage("not a request object");
    WriteAll(p.client, ud::EncodeMessage(garbage));

    const core::Expected<core::Unit, std::string> served =
        ud::ServeConnection(p.server, &handler, &flag);
    MT_EXPECT_TRUE(!served.ok());
    MT_EXPECT_EQ(handler.seen.size(), static_cast<std::size_t>(0));
}

// ── shutdown ───────────────────────────────────────────────────────────────

MT_TEST(a_shutdown_request_sets_the_flag_without_reaching_the_handler) {
    // Rust answers `Shutdown` in `handle_connection`, not in a handler:
    // stopping the accept loop is not a device operation, and routing it
    // through the device plane would make it fail on a daemon whose devices
    // are wedged -- exactly when shutdown is needed most.
    Pair p;
    ScriptedHandler handler;
    ud::ShutdownFlag flag;
    MT_EXPECT_TRUE(!flag.requested());

    SendRequest(p.client, Req(ud::DaemonRequest::Kind::Shutdown));
    MT_EXPECT_TRUE(ud::ServeConnection(p.server, &handler, &flag).ok());

    MT_EXPECT_TRUE(flag.requested());
    MT_EXPECT_EQ(handler.seen.size(), static_cast<std::size_t>(0));
    const core::Optional<ud::DaemonResponse> reply = ReadResponse(p.client);
    MT_EXPECT_TRUE(reply.has_value());
    MT_EXPECT_TRUE(reply->status == ud::DaemonResponse::Status::Ok);
}

// ── error classification ───────────────────────────────────────────────────

MT_TEST(the_three_error_flavours_stay_distinct_on_the_wire) {
    const ud::HandlerError::Kind kinds[3] = {ud::HandlerError::kInternal,
                                             ud::HandlerError::kInvalidRequest,
                                             ud::HandlerError::kTerminal};
    const ud::DaemonResponse::Status expected[3] = {
        ud::DaemonResponse::Status::Error, ud::DaemonResponse::Status::InvalidRequest,
        ud::DaemonResponse::Status::TerminalError};

    for (int i = 0; i < 3; ++i) {
        Pair p;
        ScriptedHandler handler;
        ud::ShutdownFlag flag;
        handler.error.kind = kinds[i];
        handler.error.message = "why it failed";

        SendRequest(p.client, Req(ud::DaemonRequest::Kind::GetFeatures));
        MT_EXPECT_TRUE(ud::ServeConnection(p.server, &handler, &flag).ok());

        const core::Optional<ud::DaemonResponse> reply = ReadResponse(p.client);
        MT_EXPECT_TRUE(reply.has_value());
        MT_EXPECT_TRUE(reply->status == expected[i]);
        MT_EXPECT_EQ(reply->message, std::string("why it failed"));
        // Only the terminal flavour forbids a retry.
        MT_EXPECT_EQ(reply->IsTerminal(), kinds[i] == ud::HandlerError::kTerminal);
        MT_EXPECT_TRUE(reply->IsError());
    }
}

// ── the device handoff ─────────────────────────────────────────────────────

MT_TEST(handed_off_dev_id_names_only_the_three_device_responses) {
    // Getting this set wrong in either direction is silent: too wide rolls
    // back devices that were never handed out, too narrow leaks the ones that
    // were.
    const ud::DaemonResponse::Status handed[3] = {
        ud::DaemonResponse::Status::DeviceCreated,
        ud::DaemonResponse::Status::OverlaybdRuntimeDeviceCreated,
        ud::DaemonResponse::Status::DeviceAcquired};
    for (int i = 0; i < 3; ++i) {
        ud::DaemonResponse r;
        r.status = handed[i];
        r.dev_id = 7;
        const core::Optional<uint32_t> id = ud::HandedOffDevId(r);
        MT_EXPECT_TRUE(id.has_value());
        MT_EXPECT_EQ(*id, static_cast<uint32_t>(7));
    }

    const ud::DaemonResponse::Status plain[5] = {
        ud::DaemonResponse::Status::Ok, ud::DaemonResponse::Status::Released,
        ud::DaemonResponse::Status::Deleted, ud::DaemonResponse::Status::SizeUpdated,
        ud::DaemonResponse::Status::Error};
    for (int i = 0; i < 5; ++i) {
        ud::DaemonResponse r;
        r.status = plain[i];
        r.dev_id = 7;
        MT_EXPECT_TRUE(!ud::HandedOffDevId(r).has_value());
    }
}

MT_TEST(a_device_reply_the_client_reads_is_not_rolled_back) {
    Pair p;
    ScriptedHandler handler;
    ud::ShutdownFlag flag;
    handler.reply.status = ud::DaemonResponse::Status::DeviceCreated;
    handler.reply.dev_id = 3;
    handler.reply.device_path = "/dev/ublkb3";

    SendRequest(p.client, Req(ud::DaemonRequest::Kind::CreateOverlaybd));

    // The server blocks reading the delivery probe until the client closes,
    // so the client must read its reply *and* close from another thread.
    std::thread client([&p]() {
        const core::Optional<ud::DaemonResponse> reply = ReadResponse(p.client);
        (void)reply;
        p.CloseClient();
    });
    const core::Expected<core::Unit, std::string> served =
        ud::ServeConnection(p.server, &handler, &flag);
    client.join();

    MT_EXPECT_TRUE(served.ok());
    // One call only: the create. A second would be a rollback.
    MT_EXPECT_EQ(handler.seen.size(), static_cast<std::size_t>(1));
}

MT_TEST(a_device_reply_that_could_not_be_sent_is_rolled_back_by_delete) {
    // The client is gone before the reply is written, so it certainly never
    // saw the device id and nobody else can release it.
    Pair p;
    ScriptedHandler handler;
    ud::ShutdownFlag flag;
    handler.has_pool = false;
    handler.reply.status = ud::DaemonResponse::Status::DeviceCreated;
    handler.reply.dev_id = 11;

    SendRequest(p.client, Req(ud::DaemonRequest::Kind::CreateOverlaybd));
    // Closing now is a clean close: the request is already queued on the
    // server side and this end has nothing unread, so the server reads the
    // request and then fails to send.
    p.CloseClient();

    const core::Expected<core::Unit, std::string> served =
        ud::ServeConnection(p.server, &handler, &flag);
    MT_EXPECT_TRUE(served.ok());
    MT_EXPECT_EQ(handler.seen.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(handler.seen[1].kind == ud::DaemonRequest::Kind::Delete);
    MT_EXPECT_EQ(handler.seen[1].dev_id, static_cast<uint32_t>(11));
}

MT_TEST(a_client_that_closes_without_reading_its_device_reply_is_rolled_back) {
    // The `ECONNRESET` path specifically: the reply *was* written, and the
    // client discarded it. Waiting for readability without reading is what
    // makes the close carry unread data, which is what makes Linux send the
    // reset -- read the bytes first and the close would be clean and the
    // device would be leaked.
    Pair p;
    ScriptedHandler handler;
    ud::ShutdownFlag flag;
    handler.has_pool = false;
    handler.reply.status = ud::DaemonResponse::Status::DeviceCreated;
    handler.reply.dev_id = 5;

    SendRequest(p.client, Req(ud::DaemonRequest::Kind::CreateOverlaybd));
    std::thread giver_upper([&p]() {
        struct pollfd pfd;
        pfd.fd = p.client;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ::poll(&pfd, 1, 5000);
        p.CloseClient();
    });
    const core::Expected<core::Unit, std::string> served =
        ud::ServeConnection(p.server, &handler, &flag);
    giver_upper.join();

    MT_EXPECT_TRUE(served.ok());
    MT_EXPECT_EQ(handler.seen.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(handler.seen[1].kind == ud::DaemonRequest::Kind::Delete);
    MT_EXPECT_EQ(handler.seen[1].dev_id, static_cast<uint32_t>(5));
}

MT_TEST(a_pooled_device_is_rolled_back_by_release_not_delete) {
    // Deleting a pooled device destroys a device the pool still counts, and
    // releasing a raw one returns it to a pool that never owned it. The two
    // are picked apart by the response variant plus whether a pool exists.
    struct Case {
        ud::DaemonResponse::Status status;
        bool has_pool;
        ud::DaemonRequest::Kind expected;
    };
    const Case cases[4] = {
        {ud::DaemonResponse::Status::DeviceAcquired, true,
         ud::DaemonRequest::Kind::ReleaseOverlaybd},
        {ud::DaemonResponse::Status::OverlaybdRuntimeDeviceCreated, true,
         ud::DaemonRequest::Kind::ReleaseOverlaybd},
        // No pool: there is nothing to release to.
        {ud::DaemonResponse::Status::DeviceAcquired, false, ud::DaemonRequest::Kind::Delete},
        // A raw create is never pooled, even when a pool exists.
        {ud::DaemonResponse::Status::DeviceCreated, true, ud::DaemonRequest::Kind::Delete},
    };

    for (int i = 0; i < 4; ++i) {
        Pair p;
        ScriptedHandler handler;
        ud::ShutdownFlag flag;
        handler.has_pool = cases[i].has_pool;
        handler.reply.status = cases[i].status;
        handler.reply.dev_id = 9;

        SendRequest(p.client, Req(ud::DaemonRequest::Kind::AcquireOverlaybd));
        p.CloseClient();
        MT_EXPECT_TRUE(ud::ServeConnection(p.server, &handler, &flag).ok());

        MT_EXPECT_EQ(handler.seen.size(), static_cast<std::size_t>(2));
        MT_EXPECT_TRUE(handler.seen[1].kind == cases[i].expected);
        MT_EXPECT_EQ(handler.seen[1].dev_id, static_cast<uint32_t>(9));
    }
}

MT_TEST(a_rollback_that_fails_is_reported_with_the_device_it_leaked) {
    // The device is now leaked and only the log will say which one, so the
    // error has to name it.
    Pair p;
    ScriptedHandler handler;
    ud::ShutdownFlag flag;
    handler.reply.status = ud::DaemonResponse::Status::DeviceCreated;
    handler.reply.dev_id = 17;
    handler.fail_from_call = 1;  // the create succeeds, the rollback does not

    SendRequest(p.client, Req(ud::DaemonRequest::Kind::CreateOverlaybd));
    p.CloseClient();

    const core::Expected<core::Unit, std::string> served =
        ud::ServeConnection(p.server, &handler, &flag);
    MT_EXPECT_TRUE(!served.ok());
    MT_EXPECT_TRUE(served.error().find("17") != std::string::npos);
    MT_EXPECT_TRUE(served.error().find("roll back") != std::string::npos);
}

// ── the socket lifecycle ───────────────────────────────────────────────────

MT_TEST(a_socket_file_nobody_answers_on_is_removed) {
    const std::string path = UniqueSocketPath();
    MT_EXPECT_TRUE(!path.empty());
    // A plain file stands in for a socket inode left behind by a crash: the
    // thing that matters is that connecting to it fails.
    FILE* const f = ::fopen(path.c_str(), "w");
    MT_EXPECT_TRUE(f != NULL);
    ::fclose(f);

    MT_EXPECT_TRUE(ud::ClearStaleSocket(path).ok());
    MT_EXPECT_TRUE(::access(path.c_str(), F_OK) != 0);
}

MT_TEST(a_socket_a_live_daemon_answers_on_is_refused) {
    // Removing it would let two daemons serve one socket, each managing
    // devices the other does not know about.
    const std::string path = UniqueSocketPath();
    MT_EXPECT_TRUE(!path.empty());

    ScriptedHandler handler;
    ud::UblkDaemonServer first(path, &handler);
    bool bound = false;
    std::thread serving([&first, &bound]() {
        const core::Expected<core::Unit, std::string> ran =
            first.Run(-1, [&bound]() -> core::Expected<core::Unit, std::string> {
                bound = true;
                return core::Unit{};
            });
        (void)ran;
    });
    while (!bound) {
        struct pollfd none;
        none.fd = -1;
        none.events = 0;
        none.revents = 0;
        ::poll(&none, 0, 5);
    }

    const core::Expected<core::Unit, std::string> cleared = ud::ClearStaleSocket(path);
    MT_EXPECT_TRUE(!cleared.ok());
    MT_EXPECT_TRUE(cleared.error().find("in use") != std::string::npos);

    first.shutdown()->Request();
    serving.join();
    // The loop removes its own socket on the way out, which is what makes the
    // next start a clean bind rather than a stale-socket report.
    MT_EXPECT_TRUE(::access(path.c_str(), F_OK) != 0);
    MT_EXPECT_EQ(handler.stop_calls, 1);
}

MT_TEST(the_ready_signal_runs_only_once_the_socket_accepts_connections) {
    // A supervisor starts sending requests when it sees this. Firing it before
    // `listen` makes every early client fail to connect; firing it after the
    // first accept makes a correct startup look like a hang.
    const std::string path = UniqueSocketPath();
    MT_EXPECT_TRUE(!path.empty());

    ScriptedHandler handler;
    ud::UblkDaemonServer server(path, &handler);
    bool connectable = false;
    std::thread serving([&server, &path, &connectable]() {
        const core::Expected<core::Unit, std::string> ran =
            server.Run(-1, [&path, &connectable]() -> core::Expected<core::Unit, std::string> {
                // Still inside the ready callback: no accept has run yet, and
                // the connect must already succeed off the backlog.
                std::shared_ptr<ud::Transport> transport = ud::MakeUnixTransport(path);
                ud::DaemonClient probe(transport, path);
                connectable = ::access(path.c_str(), F_OK) == 0;
                return core::Unit{};
            });
        (void)ran;
    });
    while (!connectable) {
        struct pollfd none;
        none.fd = -1;
        none.events = 0;
        none.revents = 0;
        ::poll(&none, 0, 5);
    }
    MT_EXPECT_TRUE(connectable);
    server.shutdown()->Request();
    serving.join();
}

MT_TEST(a_real_client_round_trip_over_the_accept_loop) {
    // The one test that exercises the whole socket half as a client sees it:
    // a real `DaemonClient`, a real unix transport, a real accept loop, and a
    // real `Shutdown` that stops it. Everything above this point drives
    // `ServeConnection` directly, so nothing else proves the accept loop hands
    // connections to it at all.
    const std::string path = UniqueSocketPath();
    MT_EXPECT_TRUE(!path.empty());

    ScriptedHandler handler;
    handler.reply.status = ud::DaemonResponse::Status::Features;
    handler.reply.flags = 0x40;

    ud::UblkDaemonServer server(path, &handler);
    bool bound = false;
    std::thread serving([&server, &bound]() {
        const core::Expected<core::Unit, std::string> ran =
            server.Run(-1, [&bound]() -> core::Expected<core::Unit, std::string> {
                bound = true;
                return core::Unit{};
            });
        MT_EXPECT_TRUE(ran.ok());
    });
    while (!bound) {
        struct pollfd none;
        none.fd = -1;
        none.events = 0;
        none.revents = 0;
        ::poll(&none, 0, 5);
    }

    ud::DaemonClient client(ud::MakeUnixTransport(path), path);
    const ud::DaemonResult<uint64_t> features = client.GetFeatures();
    MT_EXPECT_TRUE(features.ok());
    MT_EXPECT_EQ(features.value(), static_cast<uint64_t>(0x40));

    // Each call is its own connection, so a second one must work too.
    const ud::DaemonResult<uint64_t> again = client.GetFeatures();
    MT_EXPECT_TRUE(again.ok());

    client.Shutdown();
    serving.join();
    MT_EXPECT_EQ(handler.stop_calls, 1);
    MT_EXPECT_TRUE(::access(path.c_str(), F_OK) != 0);
}

MT_MAIN
