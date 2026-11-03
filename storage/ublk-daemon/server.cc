// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/server.rs
#include "agentenv/storage/ublk_daemon/server.h"

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/storage/ublk-daemon/lifecycle.h"

namespace agentenv {
namespace storage {
namespace ublk_daemon {

namespace {

std::string Errno(const char* what, int err) {
    std::ostringstream o;
    o << what << ": " << std::strerror(err);
    return o.str();
}

/// Outcome of a fixed-size read, with EOF kept apart from failure.
///
/// The distinction is not cosmetic: Rust's `recv_message` maps an EOF while
/// reading the length prefix to `Ok(None)` -- a client that connected and
/// left, which is a normal liveness probe -- and an EOF while reading the
/// body to an error, because a truncated message is corruption.
enum class ReadExactResult { kOk, kEof, kFailed };

ReadExactResult ReadExact(int fd, uint8_t* buf, std::size_t len, std::string* err) {
    std::size_t done = 0;
    while (done < len) {
        const ssize_t n = ::recv(fd, buf + done, len - done, 0);
        if (n == 0) {
            // A partial read followed by EOF is still EOF for the prefix, as
            // Rust's `read_exact` reports `UnexpectedEof` either way.
            return ReadExactResult::kEof;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (err != NULL) *err = std::strerror(errno);
            return ReadExactResult::kFailed;
        }
        done += static_cast<std::size_t>(n);
    }
    return ReadExactResult::kOk;
}

/// Rust `recv_message::<DaemonRequest>` — absent means "nothing was sent".
core::Expected<core::Optional<DaemonRequest>, std::string> RecvRequest(int fd) {
    uint8_t prefix[4];
    std::string err;
    const ReadExactResult head = ReadExact(fd, prefix, sizeof(prefix), &err);
    if (head == ReadExactResult::kEof) {
        return core::Optional<DaemonRequest>();
    }
    if (head == ReadExactResult::kFailed) {
        return core::make_unexpected("read message length prefix: " + err);
    }

    const core::Expected<uint32_t, std::string> len = DecodeMessageLength(prefix);
    if (!len.ok()) {
        // The size guard is also the byte-order guard: a little-endian prefix
        // reads as a huge length and lands here.
        return core::make_unexpected(len.error());
    }

    std::string body;
    if (len.value() > 0) {
        body.resize(static_cast<std::size_t>(len.value()));
        const ReadExactResult tail =
            ReadExact(fd, reinterpret_cast<uint8_t*>(&body[0]), body.size(), &err);
        if (tail == ReadExactResult::kEof) {
            return core::make_unexpected(
                std::string("read message body: unexpected end of stream"));
        }
        if (tail == ReadExactResult::kFailed) {
            return core::make_unexpected("read message body: " + err);
        }
    }

    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(body);
    if (!parsed.ok()) {
        return core::make_unexpected("deserialize daemon message: " + parsed.error().chain());
    }
    const core::Expected<DaemonRequest, std::string> request =
        DaemonRequest::FromJson(parsed.value());
    if (!request.ok()) {
        return core::make_unexpected("deserialize daemon message: " + request.error());
    }
    return core::Optional<DaemonRequest>(request.value());
}

/// Rust `send_message`.
core::Expected<core::Unit, std::string> SendResponse(int fd, const DaemonResponse& response) {
    const std::vector<uint8_t> frame = EncodeMessage(response.ToJson());
    std::size_t sent = 0;
    while (sent < frame.size()) {
        // MSG_NOSIGNAL: a client that closed early must surface as EPIPE on
        // this call, not as SIGPIPE killing the daemon.
        const ssize_t n = ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return core::make_unexpected(Errno("send daemon response", errno));
        }
        if (n == 0) {
            return core::make_unexpected(std::string("send daemon response: wrote nothing"));
        }
        sent += static_cast<std::size_t>(n);
    }
    return core::Unit{};
}

/// Rust `deliver_device_handoff` — `false` only when the client *certainly*
/// did not receive the response.
///
/// A client reads one response and closes. On Linux, closing a unix stream
/// socket that still has unread data makes the peer's next read fail with
/// `ECONNRESET`, so a reset proves the client gave up after the response was
/// written but before reading it. Every other outcome -- including a read
/// error this code cannot explain -- is treated as delivered, because
/// releasing a device the client is using is worse than leaking one.
bool DeliverDeviceHandoff(int fd, const DaemonResponse& response, uint32_t dev_id) {
    const core::Expected<core::Unit, std::string> sent = SendResponse(fd, response);
    if (!sent.ok()) {
        AGENTENV_WARN("dev " << dev_id << ": failed to send device handoff: " << sent.error());
        return false;
    }
    uint8_t probe = 0;
    for (;;) {
        const ssize_t n = ::recv(fd, &probe, 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == ECONNRESET) {
            AGENTENV_WARN("dev " << dev_id << ": client closed without reading device handoff");
            return false;
        }
        return true;
    }
}

/// Rust's rollback arm: the request the client would have sent had it seen the
/// response.
///
/// Routed back through the handler rather than calling a release/delete
/// function directly, because in Rust those *are* the handlers for these two
/// request kinds -- `handle_release_overlaybd` and `handle_delete`. Rebuilding
/// the request keeps the two paths from drifting apart.
DaemonRequest RollbackRequestFor(const DaemonResponse& response, uint32_t dev_id,
                                 bool has_pool) {
    DaemonRequest req;
    req.dev_id = dev_id;
    const bool pooled =
        response.status == DaemonResponse::Status::DeviceAcquired ||
        response.status == DaemonResponse::Status::OverlaybdRuntimeDeviceCreated;
    req.kind = (pooled && has_pool) ? DaemonRequest::Kind::ReleaseOverlaybd
                                    : DaemonRequest::Kind::Delete;
    return req;
}

/// Binds and listens. Split out so `Run` reads as the sequence it is.
core::Expected<int, std::string> BindAndListen(const std::string& path) {
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        return core::make_unexpected("daemon socket path too long: " + path);
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size());

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return core::make_unexpected(Errno("create daemon socket", errno));
    }
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int err = errno;
        ::close(fd);
        return core::make_unexpected(Errno(("bind daemon socket: " + path).c_str(), err));
    }
    // 1024 is tokio's `UnixListener::bind` backlog. The queue only has to
    // absorb bursts while a connection is being served, and every client call
    // is a fresh connect.
    if (::listen(fd, 1024) != 0) {
        const int err = errno;
        ::close(fd);
        ::unlink(path.c_str());
        return core::make_unexpected(Errno(("listen on daemon socket: " + path).c_str(), err));
    }
    return fd;
}

}  // namespace

HandlerError HandlerError::Internal(const std::string& message) {
    HandlerError e;
    e.kind = kInternal;
    e.message = message;
    return e;
}

HandlerError HandlerError::InvalidRequest(const std::string& message) {
    HandlerError e;
    e.kind = kInvalidRequest;
    e.message = message;
    return e;
}

HandlerError HandlerError::Terminal(const std::string& message) {
    HandlerError e;
    e.kind = kTerminal;
    e.message = message;
    return e;
}

core::Expected<core::Unit, std::string> ClearStaleSocket(const std::string& path) {
    const core::Expected<bool, std::string> exists = core::fs::TryExists(path);
    if (!exists.ok()) {
        return core::make_unexpected("check daemon socket: " + path + ": " + exists.error());
    }
    if (!exists.value()) return core::Unit{};

    // A successful connect means a live daemon is already serving here.
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (path.size() < sizeof(addr.sun_path)) {
            std::memcpy(addr.sun_path, path.c_str(), path.size());
            const int rc =
                ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
            ::close(fd);
            if (rc == 0) {
                return core::make_unexpected("daemon socket " + path +
                                             " is in use by another process");
            }
        } else {
            ::close(fd);
        }
    }

    const core::Expected<core::Unit, std::string> removed = core::fs::RemoveFile(path);
    if (!removed.ok()) {
        return core::make_unexpected("remove stale daemon socket: " + path + ": " +
                                     removed.error());
    }
    return core::Unit{};
}

core::Optional<uint32_t> HandedOffDevId(const DaemonResponse& response) {
    switch (response.status) {
        case DaemonResponse::Status::DeviceCreated:
        case DaemonResponse::Status::OverlaybdRuntimeDeviceCreated:
        case DaemonResponse::Status::DeviceAcquired:
            return core::Optional<uint32_t>(response.dev_id);
        default:
            return core::Optional<uint32_t>();
    }
}

DaemonResponse ResponseFromHandlerError(const HandlerError& error) {
    DaemonResponse response;
    response.message = error.message;
    switch (error.kind) {
        case HandlerError::kTerminal:
            response.status = DaemonResponse::Status::TerminalError;
            break;
        case HandlerError::kInvalidRequest:
            response.status = DaemonResponse::Status::InvalidRequest;
            break;
        case HandlerError::kInternal:
        default:
            response.status = DaemonResponse::Status::Error;
            break;
    }
    return response;
}

core::Expected<core::Unit, std::string> ServeConnection(int fd, DeviceHandler* handler,
                                                        ShutdownFlag* shutdown) {
    const core::Expected<core::Optional<DaemonRequest>, std::string> received = RecvRequest(fd);
    if (!received.ok()) {
        return core::make_unexpected(received.error());
    }
    if (!received.value().has_value()) {
        // Connected and left without sending. Rust returns `Ok(())`.
        return core::Unit{};
    }
    const DaemonRequest request = *received.value();

    core::Expected<DaemonResponse, HandlerError> outcome =
        core::make_unexpected(HandlerError::Internal("no handler"));
    if (request.kind == DaemonRequest::Kind::Shutdown) {
        // Answered here, not by the handler: stopping the accept loop is not a
        // device operation, and Rust fires the notify from this same place.
        if (shutdown != NULL) shutdown->Request();
        DaemonResponse ok;
        ok.status = DaemonResponse::Status::Ok;
        outcome = ok;
    } else if (handler == NULL) {
        outcome = core::make_unexpected(HandlerError::Internal("no device handler configured"));
    } else {
        outcome = handler->Handle(request);
    }

    const DaemonResponse response =
        outcome.ok() ? outcome.value() : ResponseFromHandlerError(outcome.error());

    const core::Optional<uint32_t> handed_off = HandedOffDevId(response);
    if (!handed_off.has_value()) {
        return SendResponse(fd, response);
    }
    const uint32_t dev_id = *handed_off;
    if (DeliverDeviceHandoff(fd, response, dev_id)) {
        return core::Unit{};
    }

    // The response was the client's only record of the device, so nobody else
    // will ever release it.
    if (handler == NULL) {
        std::ostringstream o;
        o << "roll back undelivered handoff of device " << dev_id
          << ": no device handler configured";
        return core::make_unexpected(o.str());
    }
    const DaemonRequest rollback = RollbackRequestFor(response, dev_id, handler->HasPool());
    const core::Expected<DaemonResponse, HandlerError> undone = handler->Handle(rollback);
    if (!undone.ok()) {
        std::ostringstream o;
        o << "roll back undelivered handoff of device " << dev_id << ": "
          << undone.error().message;
        return core::make_unexpected(o.str());
    }
    AGENTENV_INFO("dev " << dev_id << ": rolled back undelivered device handoff");
    return core::Unit{};
}

UblkDaemonServer::UblkDaemonServer(const std::string& socket_path, DeviceHandler* handler)
    : socket_path_(socket_path), handler_(handler) {}

core::Expected<core::Unit, std::string> UblkDaemonServer::Run(
    int parent_pidfd,
    const std::function<core::Expected<core::Unit, std::string>()>& ready) {
    struct PidfdGuard {
        int fd;
        ~PidfdGuard() {
            if (fd >= 0) ::close(fd);
        }
    } pidfd_guard = {parent_pidfd};

    const core::Expected<core::Unit, std::string> cleared = ClearStaleSocket(socket_path_);
    if (!cleared.ok()) return core::make_unexpected(cleared.error());

    const core::Expected<int, std::string> listener = BindAndListen(socket_path_);
    if (!listener.ok()) return core::make_unexpected(listener.error());
    const int listen_fd = listener.value();
    struct ListenGuard {
        int fd;
        ~ListenGuard() {
            if (fd >= 0) ::close(fd);
        }
    } listen_guard = {listen_fd};

    AGENTENV_INFO("ublk daemon listening: " << socket_path_);

    if (ready) {
        const core::Expected<core::Unit, std::string> signalled = ready();
        if (!signalled.ok()) {
            // The socket is already bound, so leaving it behind would make the
            // next start report "stale socket" for a daemon that never served.
            ::unlink(socket_path_.c_str());
            return core::make_unexpected(signalled.error());
        }
    }

    // Connection threads are joined before returning rather than detached.
    // Rust lets tokio drop in-flight handlers when the accept loop breaks; a
    // detached thread here would keep calling into `handler_`, which the
    // caller is free to destroy once `Run` returns. The cost is that shutdown
    // waits for the slowest in-flight request.
    std::vector<std::thread> connections;

    for (;;) {
        if (shutdown_.requested()) {
            AGENTENV_INFO("ublk daemon shutdown requested");
            break;
        }
        if (ShutdownRequested()) {
            // SIGTERM/SIGINT. `lifecycle.cc` records only that one arrived,
            // not which, so this does not claim to know.
            AGENTENV_INFO("received shutdown signal, stopping ublk daemon");
            break;
        }

        struct pollfd pfds[2];
        pfds[0].fd = listen_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = parent_pidfd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        const int nfds = parent_pidfd >= 0 ? 2 : 1;

        const int ready_count = ::poll(pfds, nfds, kAcceptPollTimeoutMs);
        if (ready_count < 0) {
            if (errno == EINTR) continue;  // a signal arrived; re-check above
            return core::make_unexpected(Errno("poll daemon socket", errno));
        }
        if (ready_count == 0) continue;

        if (nfds == 2 && pfds[1].revents != 0) {
            // Either way the parent is gone, and a daemon nobody supervises is
            // the one outcome worse than stopping.
            const char* const why = (pfds[1].revents & (POLLERR | POLLNVAL)) != 0
                                        ? "parent pidfd error, shutting down"
                                        : "parent process exited, shutting down";
            AGENTENV_INFO(why);
            break;
        }
        if ((pfds[0].revents & POLLIN) == 0) continue;

        const int conn = ::accept(listen_fd, NULL, NULL);
        if (conn < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED) continue;
            return core::make_unexpected(Errno("accept daemon connection", errno));
        }

        DeviceHandler* const handler = handler_;
        ShutdownFlag* const flag = &shutdown_;
        connections.push_back(std::thread([conn, handler, flag]() {
            const core::Expected<core::Unit, std::string> served =
                ServeConnection(conn, handler, flag);
            if (!served.ok()) {
                AGENTENV_ERROR("daemon connection handler failed: " << served.error());
            }
            ::close(conn);
        }));
    }

    for (std::size_t i = 0; i < connections.size(); ++i) {
        connections[i].join();
    }

    if (handler_ != NULL) handler_->StopAllDevices();

    ::unlink(socket_path_.c_str());
    AGENTENV_INFO("ublk daemon stopped");
    return core::Unit{};
}

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
