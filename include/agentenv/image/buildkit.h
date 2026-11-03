// SPDX-License-Identifier: MIT
// Rust: src/image/buildkit.rs, src/image/buildkit/history.rs
//
// Read-only access to BuildKit's content store and build history. Image data
// stays on the node; build completion and the image digest are read directly
// from BuildKit.
//
// Everything that decides something -- digest validation, chunk verification,
// the replay/dedup rules, which history event counts as "our build finished"
// -- is here and is transport-independent. The transport sits behind the two
// `*Wire` interfaces, the same split `p2p/discovery.h` uses for the scheduler.
#ifndef AGENTENV_IMAGE_BUILDKIT_H_
#define AGENTENV_IMAGE_BUILDKIT_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/cancel_token.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace image {

/// Rust `BUILDKIT_PORT`.
extern const uint16_t kBuildkitPort;
/// Rust `MAX_METADATA_BYTES` — 4 MiB.
extern const uint64_t kMaxMetadataBytes;
/// Rust `MAX_IMAGE_BYTES` — 64 GiB.
extern const uint64_t kMaxImageBytes;
/// Rust's per-message `tokio::time::timeout` in `BuildkitContent::copy`. A
/// wire's content stream has to fail a `Next` that waits longer than this,
/// with `kDeadlineExceeded`, since this layer cannot interrupt a blocked call.
extern const int kBuildkitContentMessageTimeoutSecs;

/// The gRPC status of a failed call or stream.
struct BuildkitRpcStatus {
    enum Code { kCancelled = 1, kDeadlineExceeded = 4, kUnavailable = 14, kOther = 2 };
    int code;
    std::string message;

    BuildkitRpcStatus() : code(kOther) {}
    BuildkitRpcStatus(int c, const std::string& m) : code(c), message(m) {}
};

/// One server stream. `Next` yields `nullopt` at clean end of stream.
template <typename T>
class BuildkitStream {
 public:
    virtual ~BuildkitStream() {}
    virtual core::Expected<core::Optional<T>, BuildkitRpcStatus> Next() = 0;
};

// ---- containerd.services.content.v1.Content ----

struct ReadContentRequest {
    std::string digest;
    int64_t offset;
    int64_t size;
    ReadContentRequest() : offset(0), size(0) {}
};

struct ReadContentResponse {
    int64_t offset;
    std::string data;
    ReadContentResponse() : offset(0) {}
};

class BuildkitContentWire {
 public:
    virtual ~BuildkitContentWire() {}
    virtual core::Expected<std::shared_ptr<BuildkitStream<ReadContentResponse> >,
                           BuildkitRpcStatus>
    Read(const ReadContentRequest& request) = 0;
};

/// Rust `validate_digest` — a lowercase sha256 digest and nothing else. The
/// value is used to read content and to name files, so this is also what keeps
/// a hostile digest from carrying a path.
core::Expected<core::Unit, std::string> ValidateDigest(const std::string& digest);

/// Rust `BuildkitContent`. Copyable, like the cloneable tonic client.
class BuildkitContent {
 public:
    explicit BuildkitContent(std::shared_ptr<BuildkitContentWire> wire) : wire_(wire) {}

    /// Rust `BuildkitContent::connect`.
    static core::Expected<BuildkitContent, std::string> Connect(const std::string& host,
                                                                uint16_t port);

    /// Rust `metadata` — a verified document of at most `kMaxMetadataBytes`.
    core::Expected<std::string, std::string> Metadata(const std::string& digest) const;

    /// Rust `download` — a verified blob of exactly `size` bytes at `path`.
    core::Expected<core::Unit, std::string> Download(const std::string& digest, uint64_t size,
                                                     const std::string& path) const;

    /// Rust's `impl AsyncWrite` parameter of `copy`.
    class Output;

 private:
    core::Expected<uint64_t, std::string> Copy(const std::string& digest, uint64_t limit,
                                               Output* output) const;

    std::shared_ptr<BuildkitContentWire> wire_;
};

// ---- moby.buildkit.v1.Control ----

/// Rust `build_image_name` — `aenv-build:<id>`.
std::string BuildImageName(const std::string& build_id);

struct BuildkitTimestamp {
    int64_t seconds;
    int32_t nanos;
    BuildkitTimestamp() : seconds(0), nanos(0) {}
    BuildkitTimestamp(int64_t s, int32_t n) : seconds(s), nanos(n) {}
    bool operator<(const BuildkitTimestamp& o) const {
        return seconds != o.seconds ? seconds < o.seconds : nanos < o.nanos;
    }
    bool operator==(const BuildkitTimestamp& o) const {
        return seconds == o.seconds && nanos == o.nanos;
    }
};

struct BuildkitVertex {
    std::string digest;
    std::string name;
    bool cached;
    core::Optional<BuildkitTimestamp> started;
    core::Optional<BuildkitTimestamp> completed;
    std::string error;
    BuildkitVertex() : cached(false) {}
};

struct BuildkitVertexLog {
    std::string vertex;
    core::Optional<BuildkitTimestamp> timestamp;
    int64_t stream;
    std::string msg;
    BuildkitVertexLog() : stream(0) {}
};

struct BuildkitVertexWarning {
    std::string vertex;
    int64_t level;
    std::string short_message;
    std::vector<std::string> detail;
    BuildkitVertexWarning() : level(0) {}
};

struct BuildkitStatusResponse {
    std::vector<BuildkitVertex> vertexes;
    std::vector<BuildkitVertexLog> logs;
    std::vector<BuildkitVertexWarning> warnings;
};

struct BuildkitExporter {
    std::string type;
    std::map<std::string, std::string> attrs;
};

/// Wire-compatible subset of `google.rpc.Status`.
struct BuildkitBuildError {
    int32_t code;
    std::string message;
    BuildkitBuildError() : code(0) {}
};

struct BuildkitResultInfo {
    /// `map<int64, Descriptor>` — exporter index to descriptor digest.
    std::map<int64_t, std::string> results;
};

struct BuildkitHistoryRecord {
    std::string ref;
    std::vector<BuildkitExporter> exporters;
    core::Optional<BuildkitBuildError> error;
    core::Optional<BuildkitResultInfo> result;
};

struct BuildkitHistoryEvent {
    /// STARTED = 0, COMPLETE = 1, DELETED = 2.
    int32_t type;
    core::Optional<BuildkitHistoryRecord> record;
    BuildkitHistoryEvent() : type(0) {}
};

struct BuildkitHistoryRequest {
    bool active_only;
    std::string ref;
    bool early_exit;
    BuildkitHistoryRequest() : active_only(false), early_exit(false) {}
};

class BuildkitControlWire {
 public:
    virtual ~BuildkitControlWire() {}
    virtual core::Expected<std::shared_ptr<BuildkitStream<BuildkitStatusResponse> >,
                           BuildkitRpcStatus>
    Status(const std::string& ref) = 0;
    virtual core::Expected<std::shared_ptr<BuildkitStream<BuildkitHistoryEvent> >,
                           BuildkitRpcStatus>
    ListenBuildHistory(const BuildkitHistoryRequest& request) = 0;
};

/// Rust `VertexProgress`.
struct BuildkitVertexProgress {
    bool started;
    bool finished;
    bool cached;
    core::Optional<std::string> error;
    BuildkitVertexProgress() : started(false), finished(false), cached(false) {}
};

/// Rust `vertex_updates` — the log lines this sighting of `vertex` adds. Status
/// replays history, so a state already reported must not be reported again and
/// a finished vertex must not regress.
std::vector<std::pair<core::LogLevel, std::string> > VertexUpdates(
    const BuildkitVertex& vertex, BuildkitVertexProgress* progress);

/// Rust `completed_image` — the image digest iff `event` is the completion of
/// `image_name`'s build. A failed build is an error. Cache seeds carry old
/// history, so a completion under any other exporter name does not count.
core::Expected<core::Optional<std::string>, std::string> CompletedImage(
    const BuildkitHistoryEvent& event, const std::string& image_name);

/// Rust `BuildkitHistory`.
class BuildkitHistory {
 public:
    explicit BuildkitHistory(std::shared_ptr<BuildkitControlWire> wire) : wire_(wire) {}

    /// Rust `BuildkitHistory::connect`.
    static core::Expected<BuildkitHistory, std::string> Connect(const std::string& host,
                                                                uint16_t port);

    /// Rust `wait_for_image` — replays completed records as well as live
    /// events, including after a reconnect.
    ///
    /// In Rust, the owning build's deadline and cancellation bound this by
    /// dropping the future. There is no such thing as dropping a blocked call
    /// here, so the bound is explicit: `cancel` is checked between stream
    /// messages and during the reconnect backoff, and the wire is expected to
    /// have its stream `Next` fail with `kCancelled` when it observes it.
    core::Expected<std::string, std::string> WaitForImage(const std::string& build_id,
                                                          const core::BuildLogger& logger,
                                                          const core::CancelToken& cancel) const;

 private:
    std::shared_ptr<BuildkitControlWire> wire_;
};

// ---- the transport ----

/// The real transport for `Connect`. Returns an error when this build has none.
core::Expected<std::shared_ptr<BuildkitContentWire>, std::string> MakeBuildkitContentWire(
    const std::string& host, uint16_t port);
core::Expected<std::shared_ptr<BuildkitControlWire>, std::string> MakeBuildkitControlWire(
    const std::string& host, uint16_t port);

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_BUILDKIT_H_
