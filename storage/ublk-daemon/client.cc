// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/client.rs :: UblkDaemonClient
#include "agentenv/storage/ublk_daemon/client.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace agentenv {
namespace storage {
namespace ublk_daemon {

namespace {

/// Marks a transport failure so `Call` can tell "no answer" from "answered
/// with an error". The distinction decides whether a restack is terminal, so
/// it cannot be lost in a plain string.
const char* const kNoAnswerPrefix = "no-answer: ";

bool IsNoAnswer(const std::string& err) {
    return err.compare(0, std::strlen(kNoAnswerPrefix), kNoAnswerPrefix) == 0;
}

class UnixTransport : public Transport {
 public:
    explicit UnixTransport(std::string path) : path_(std::move(path)) {}

    core::Expected<core::Json, std::string> Call(const core::Json& request,
                                                 int64_t timeout_ms) override {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return core::make_unexpected(std::string(kNoAnswerPrefix) + "socket: " +
                                         std::strerror(errno));
        }
        struct FdGuard {
            int fd;
            ~FdGuard() { if (fd >= 0) ::close(fd); }
        } guard = {fd};

        // One timeout value for both directions. A daemon that accepts the
        // connection and then stalls must not pin the caller: these calls run
        // on the sandbox lifecycle path.
        struct timeval tv;
        tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
        tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(addr.sun_path)) {
            return core::make_unexpected(std::string("socket path too long: ") + path_);
        }
        std::memcpy(addr.sun_path, path_.c_str(), path_.size());

        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            // A refused connect means the daemon is not listening, which is an
            // absence of answer rather than a rejection of the request.
            return core::make_unexpected(std::string(kNoAnswerPrefix) +
                                         "connect to ublk daemon socket: " + path_ + ": " +
                                         std::strerror(errno));
        }

        const std::vector<uint8_t> frame = EncodeMessage(request);
        std::size_t sent = 0;
        while (sent < frame.size()) {
            const ssize_t n = ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) {
                return core::make_unexpected(std::string(kNoAnswerPrefix) +
                                             "send daemon request: " + std::strerror(errno));
            }
            sent += static_cast<std::size_t>(n);
        }

        // Read until a whole frame is present. `DecodeMessage` reports a short
        // buffer as Incomplete, which on a stream socket is normal and must be
        // retried rather than treated as corruption.
        std::vector<uint8_t> buf;
        uint8_t chunk[4096];
        for (;;) {
            core::Json out;
            std::size_t consumed = 0;
            std::string err;
            const DecodeStatus status =
                DecodeMessage(buf.empty() ? NULL : buf.data(), buf.size(), &out, &consumed,
                              &err);
            if (status == DecodeStatus::Ok) return out;
            if (status == DecodeStatus::Malformed) {
                // The daemon answered, just not intelligibly. Not a no-answer:
                // whatever it did, it got far enough to reply.
                return core::make_unexpected(std::string("deserialize daemon message: ") + err);
            }
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n == 0) {
                return core::make_unexpected(std::string(kNoAnswerPrefix) +
                                             "daemon closed connection without response");
            }
            if (n < 0) {
                return core::make_unexpected(std::string(kNoAnswerPrefix) +
                                             "receive daemon response: " +
                                             std::strerror(errno));
            }
            buf.insert(buf.end(), chunk, chunk + n);
        }
    }

 private:
    std::string path_;
};

std::string Unexpected(const char* what) {
    return std::string("daemon: unexpected response for ") + what;
}

/// The shared tail of every method: `TerminalError` and `Error` are separate
/// arms in Rust, and the distinction survives into `DaemonError`.
DaemonError ClassifyResponse(const char* what, const DaemonResponse& response) {
    if (response.status == DaemonResponse::Status::TerminalError) {
        return DaemonError::Terminal(std::string("daemon: ") + what +
                                     " failed terminally: " + response.message);
    }
    if (response.status == DaemonResponse::Status::Error ||
        response.status == DaemonResponse::Status::InvalidRequest) {
        return DaemonError::Recoverable(std::string("daemon: ") + what + " failed: " +
                                        response.message);
    }
    return DaemonError::Recoverable(Unexpected(what));
}

}  // namespace

std::shared_ptr<Transport> MakeUnixTransport(const std::string& socket_path) {
    return std::shared_ptr<Transport>(new UnixTransport(socket_path));
}

DaemonClient::DaemonClient(std::shared_ptr<Transport> transport, std::string socket_path)
    : transport_(std::move(transport)), socket_path_(std::move(socket_path)),
      daemon_dead_(false), shutting_down_(false) {}

void DaemonClient::MarkDaemonDead(const std::string& reason) {
    daemon_dead_ = true;
    death_reason_ = reason;
}

core::Expected<DaemonResponse, DaemonError> DaemonClient::Call(const DaemonRequest& request,
                                                               int64_t timeout_ms) {
    // Fail fast. The socket file can outlive the process, so without this a
    // call against a dead daemon would wait out the whole timeout before
    // reporting a connect error that names the wrong cause.
    if (daemon_dead_) {
        return core::make_unexpected(DaemonError::Recoverable(
            std::string("ublk daemon is not running: ") +
            (death_reason_.empty() ? std::string("unknown reason") : death_reason_)));
    }
    if (!transport_) {
        return core::make_unexpected(DaemonError::Recoverable("no daemon transport"));
    }

    const core::Expected<core::Json, std::string> reply =
        transport_->Call(request.ToJson(), timeout_ms);
    if (!reply.ok()) {
        return core::make_unexpected(DaemonError::Recoverable(reply.error()));
    }
    const core::Expected<DaemonResponse, std::string> decoded =
        DaemonResponse::FromJson(reply.value());
    if (!decoded.ok()) {
        return core::make_unexpected(DaemonError::Recoverable(decoded.error()));
    }
    return decoded.value();
}

DaemonResult<core::Unit> DaemonClient::Shutdown() {
    // Set before sending, as Rust does: the watchdog reads this to decide
    // whether the child's exit is expected, and the daemon may die before the
    // reply arrives.
    shutting_down_ = true;
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::Shutdown;
    // Best-effort: the daemon closing the connection before replying is the
    // expected outcome, not a failure.
    (void)Call(req, kDefaultTimeoutMs);
    return core::Unit{};
}

DaemonResult<CreatedDevice> DaemonClient::CreateOverlaybd(const std::string& image_config,
                                                          const std::string& global_config) {
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::CreateOverlaybd;
    req.image_config = image_config;
    req.global_config = global_config;
    const core::Expected<DaemonResponse, DaemonError> r = Call(req, kDefaultTimeoutMs);
    if (!r.ok()) return core::make_unexpected(r.error());
    if (r.value().status == DaemonResponse::Status::DeviceCreated) {
        CreatedDevice out;
        out.dev_id = r.value().dev_id;
        out.device_path = r.value().device_path;
        return out;
    }
    return core::make_unexpected(ClassifyResponse("create overlaybd", r.value()));
}

DaemonResult<CreatedRuntimeDevice> DaemonClient::CreateOverlaybdRuntimeDevice(
    const std::string& source_image_config, const std::string& global_config,
    const std::string& runtime_dir, bool read_only, overlaybd::UpperMode runtime_upper_mode,
    const core::Optional<uint64_t>& requested_virtual_size,
    const core::Optional<uint64_t>& known_source_virtual_size, bool allow_shrink) {
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::CreateOverlaybdRuntimeDevice;
    req.source_image_config = source_image_config;
    req.global_config = global_config;
    req.runtime_dir = runtime_dir;
    req.read_only = read_only;
    req.runtime_upper_mode = runtime_upper_mode;
    req.requested_virtual_size = requested_virtual_size;
    req.known_source_virtual_size = known_source_virtual_size;
    req.allow_shrink = allow_shrink;

    const core::Expected<DaemonResponse, DaemonError> r = Call(req, kDefaultTimeoutMs);
    if (!r.ok()) return core::make_unexpected(r.error());
    if (r.value().status == DaemonResponse::Status::OverlaybdRuntimeDeviceCreated) {
        CreatedRuntimeDevice out;
        out.dev_id = r.value().dev_id;
        out.device_path = r.value().device_path;
        out.actual_virtual_size = r.value().actual_virtual_size;
        out.runtime_image_config_path = r.value().runtime_image_config_path;
        return out;
    }
    return core::make_unexpected(
        ClassifyResponse("create overlaybd runtime device", r.value()));
}

DaemonResult<CreatedDevice> DaemonClient::AcquireOverlaybd(
    const std::string& image_config, const std::string& global_config,
    const core::Optional<uint64_t>& virtual_size, AccessMode access_mode) {
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::AcquireOverlaybd;
    req.image_config = image_config;
    req.global_config = global_config;
    req.virtual_size = virtual_size;
    req.access_mode = access_mode;
    const core::Expected<DaemonResponse, DaemonError> r = Call(req, kDefaultTimeoutMs);
    if (!r.ok()) return core::make_unexpected(r.error());
    if (r.value().status == DaemonResponse::Status::DeviceAcquired) {
        CreatedDevice out;
        out.dev_id = r.value().dev_id;
        out.device_path = r.value().device_path;
        return out;
    }
    return core::make_unexpected(ClassifyResponse("acquire overlaybd", r.value()));
}

DaemonResult<core::Unit> DaemonClient::ReleaseOverlaybd(uint32_t dev_id) {
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::ReleaseOverlaybd;
    req.dev_id = dev_id;
    const core::Expected<DaemonResponse, DaemonError> r = Call(req, kDefaultTimeoutMs);
    if (!r.ok()) return core::make_unexpected(r.error());
    if (r.value().status == DaemonResponse::Status::Released) return core::Unit{};
    return core::make_unexpected(ClassifyResponse("release overlaybd", r.value()));
}

DaemonResult<core::Unit> DaemonClient::Delete(uint32_t dev_id) {
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::Delete;
    req.dev_id = dev_id;
    const core::Expected<DaemonResponse, DaemonError> r = Call(req, kDefaultTimeoutMs);
    if (!r.ok()) return core::make_unexpected(r.error());
    if (r.value().status == DaemonResponse::Status::Deleted) return core::Unit{};
    return core::make_unexpected(ClassifyResponse("delete device", r.value()));
}

DaemonResult<core::Unit> DaemonClient::UpdateSize(uint32_t dev_id, uint64_t new_sectors) {
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::UpdateSize;
    req.dev_id = dev_id;
    req.new_sectors = new_sectors;
    const core::Expected<DaemonResponse, DaemonError> r = Call(req, kDefaultTimeoutMs);
    if (!r.ok()) return core::make_unexpected(r.error());
    if (r.value().status == DaemonResponse::Status::SizeUpdated) return core::Unit{};
    return core::make_unexpected(ClassifyResponse("update size", r.value()));
}

DaemonResult<uint64_t> DaemonClient::GetFeatures() {
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::GetFeatures;
    const core::Expected<DaemonResponse, DaemonError> r = Call(req, kDefaultTimeoutMs);
    if (!r.ok()) return core::make_unexpected(r.error());
    if (r.value().status == DaemonResponse::Status::Features) return r.value().flags;
    return core::make_unexpected(ClassifyResponse("get features", r.value()));
}

DaemonResult<core::Unit> DaemonClient::NotifySandboxReady(const std::string& device_key) {
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::NotifySandboxReady;
    req.device_key = device_key;
    const core::Expected<DaemonResponse, DaemonError> r = Call(req, kDefaultTimeoutMs);
    if (!r.ok()) return core::make_unexpected(r.error());
    if (r.value().status == DaemonResponse::Status::Ok) return core::Unit{};
    return core::make_unexpected(ClassifyResponse("notify sandbox ready", r.value()));
}

DaemonResult<RestackSnapshotStats> DaemonClient::RestackSnapshot(
    uint32_t dev_id, const std::string& output_layer_path) {
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::RestackSnapshot;
    req.dev_id = dev_id;
    req.output_layer_path = output_layer_path;

    char what[96];
    std::snprintf(what, sizeof(what), "restack snapshot dev_id=%u", dev_id);

    const core::Expected<DaemonResponse, DaemonError> r = Call(req, kSnapshotTimeoutMs);
    if (!r.ok()) {
        // A lost reply can follow a successful seal: the daemon may have
        // already rewritten the live upper layer. Resuming that runtime would
        // read a layer that is half published, so an unknown outcome is
        // terminal even though nothing here observed a failure.
        return core::make_unexpected(DaemonError::Terminal(
            std::string("daemon: ") + what + " outcome unknown: " + r.error().message));
    }
    if (r.value().status == DaemonResponse::Status::RestackSnapshotCreated) {
        RestackSnapshotStats out;
        out.has_descriptor = r.value().has_descriptor;
        out.descriptor = r.value().descriptor;
        out.has_data_stat = r.value().has_data_stat;
        out.data_stat = r.value().data_stat;
        out.ext4_used_bytes = r.value().ext4_used_bytes;
        return out;
    }
    if (r.value().status == DaemonResponse::Status::Error) {
        // The one case that is *not* terminal: the daemon answered explicitly,
        // which establishes that it left the live runtime intact.
        return core::make_unexpected(DaemonError::Recoverable(
            std::string("daemon: ") + what + " failed: " + r.value().message));
    }
    if (r.value().status == DaemonResponse::Status::TerminalError) {
        return core::make_unexpected(DaemonError::Terminal(
            std::string("daemon: ") + what +
            " failed after mutating live state: " + r.value().message));
    }
    // Any other reply is terminal too: it tells us nothing about what was
    // done, which is the same position as no reply at all.
    return core::make_unexpected(DaemonError::Terminal(
        std::string("daemon: unexpected response for ") + what));
}

DaemonResult<core::Unit> DaemonClient::StartPackRecording(uint32_t dev_id,
                                                          const std::string& output,
                                                          uint32_t max_pages,
                                                          uint64_t min_window_ms,
                                                          uint64_t quiet_ms,
                                                          uint64_t max_window_ms) {
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::StartPackRecording;
    req.dev_id = dev_id;
    req.output = output;
    req.max_pages = max_pages;
    req.min_window_ms = min_window_ms;
    req.quiet_ms = quiet_ms;
    req.max_window_ms = max_window_ms;
    const core::Expected<DaemonResponse, DaemonError> r = Call(req, kDefaultTimeoutMs);
    if (!r.ok()) return core::make_unexpected(r.error());
    if (r.value().status == DaemonResponse::Status::Ok) return core::Unit{};
    return core::make_unexpected(ClassifyResponse("start pack recording", r.value()));
}

DaemonResult<PackRecordingState> DaemonClient::PackRecordingStatus(uint32_t dev_id) {
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::PackRecordingStatus;
    req.dev_id = dev_id;
    const core::Expected<DaemonResponse, DaemonError> r = Call(req, kDefaultTimeoutMs);
    if (!r.ok()) return core::make_unexpected(r.error());
    if (r.value().status == DaemonResponse::Status::PackRecording) return r.value().state;
    return core::make_unexpected(ClassifyResponse("pack recording status", r.value()));
}

DaemonResult<core::Unit> DaemonClient::AbortPackRecording(uint32_t dev_id) {
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::AbortPackRecording;
    req.dev_id = dev_id;
    const core::Expected<DaemonResponse, DaemonError> r = Call(req, kDefaultTimeoutMs);
    if (!r.ok()) return core::make_unexpected(r.error());
    if (r.value().status == DaemonResponse::Status::Ok) return core::Unit{};
    return core::make_unexpected(ClassifyResponse("abort pack recording", r.value()));
}

DaemonResult<core::Unit> DaemonClient::PrefetchStartupPack(
    const std::string& image_config, const std::string& global_config, const std::string& url,
    uint64_t pack_size, const std::string& index_sha256, uint64_t mem_virtual_size,
    uint64_t timeout_secs) {
    DaemonRequest req;
    req.kind = DaemonRequest::Kind::PrefetchStartupPack;
    req.image_config = image_config;
    req.global_config = global_config;
    req.url = url;
    req.pack_size = pack_size;
    req.index_sha256 = index_sha256;
    req.mem_virtual_size = mem_virtual_size;
    req.timeout_secs = timeout_secs;
    const core::Expected<DaemonResponse, DaemonError> r = Call(req, kDefaultTimeoutMs);
    if (!r.ok()) return core::make_unexpected(r.error());
    if (r.value().status == DaemonResponse::Status::Ok) return core::Unit{};
    // No terminal arm here, matching Rust: the daemon answers `Ok` even when
    // registration failed, because prefetching is an optimisation whose
    // failure must never fail a boot. Anything else is a plain error.
    return core::make_unexpected(DaemonError::Recoverable(
        std::string("daemon: prefetch startup pack failed: ") + r.value().message));
}

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
