// SPDX-License-Identifier: MIT
// Rust: src/image/buildkit/history.rs
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

#include "agentenv/core/digest.h"
#include "agentenv/image/buildkit.h"

namespace agentenv {
namespace image {

namespace {

const int kHistoryEventComplete = 1;
const std::size_t kMaxTrackedEntries = 32768;
const int kStatusAttempts = 4;
const int kRetryDelayMs = 250;

/// The bits of `ours` shared by the event loop and `CompletedImage`.
int FindExporter(const BuildkitHistoryRecord& record, const std::string& image_name) {
    for (std::size_t i = 0; i < record.exporters.size(); ++i) {
        const BuildkitExporter& exporter = record.exporters[i];
        if (exporter.type != "image") continue;
        const std::map<std::string, std::string>::const_iterator name =
            exporter.attrs.find("name");
        if (name != exporter.attrs.end() && name->second == image_name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool IsRetryable(const BuildkitRpcStatus& status) {
    return status.code == BuildkitRpcStatus::kUnavailable ||
           status.code == BuildkitRpcStatus::kCancelled ||
           status.code == BuildkitRpcStatus::kDeadlineExceeded;
}

/// Rust's `(vertex, timestamp, stream, sha256(msg))` key. Message bytes are
/// hashed rather than retained, so dedup does not hold a second copy of the
/// build's output.
typedef std::tuple<std::string, bool, BuildkitTimestamp, int64_t, std::string> LogKey;

LogKey MakeLogKey(const BuildkitVertexLog& entry) {
    const bool has_timestamp = entry.timestamp.has_value();
    return LogKey(entry.vertex, has_timestamp,
                  has_timestamp ? *entry.timestamp : BuildkitTimestamp(), entry.stream,
                  core::Sha256Hex(entry.msg));
}

/// The state of one `collect_status` task.
///
/// Shared with a detached thread rather than joined by the caller: Rust's
/// `JoinSet` aborts a task when it is dropped, even one blocked on the network.
/// Joining here would turn "the build finished but the status stream hung" into
/// a hang of the build, so an abandoned collector is only told to stop, and
/// the shared state keeps what it touches alive until it does.
struct Collector {
    std::shared_ptr<BuildkitControlWire> wire;
    std::string reference;
    core::BuildLogger logger;
    std::atomic<bool> aborted;
    std::mutex mu;
    std::condition_variable cv;
    bool finished;
    core::Optional<std::string> error;

    Collector() : aborted(false), finished(false) {}
};

/// Rust `collect_status` — one attempt's body. Occurrence counts are per
/// attempt and `seen` is across attempts, so a replay after a reconnect
/// re-emits nothing, while identical repeated output within a stream still
/// prints every time.
core::Expected<core::Unit, std::string> CollectStatusAttempt(
    Collector* task, int attempt, std::map<LogKey, std::size_t>* seen,
    std::map<std::string, BuildkitVertexProgress>* vertices) {
    std::map<LogKey, std::size_t> occurrences;
    const core::Expected<std::shared_ptr<BuildkitStream<BuildkitStatusResponse> >,
                         BuildkitRpcStatus>
        stream = task->wire->Status(task->reference);
    if (!stream.ok()) return core::make_unexpected(stream.error().message);

    for (;;) {
        if (task->aborted.load()) return core::Unit();
        const core::Expected<core::Optional<BuildkitStatusResponse>, BuildkitRpcStatus> next =
            stream.value()->Next();
        if (!next.ok()) return core::make_unexpected(next.error().message);
        if (!next.value().has_value()) return core::Unit();
        const BuildkitStatusResponse& status = *next.value();

        for (std::size_t i = 0; i < status.vertexes.size(); ++i) {
            const BuildkitVertex& vertex = status.vertexes[i];
            if (!vertex.started.has_value() && !vertex.completed.has_value()) continue;
            if (vertices->find(vertex.digest) == vertices->end() &&
                vertices->size() >= kMaxTrackedEntries) {
                continue;
            }
            const std::vector<std::pair<core::LogLevel, std::string> > updates =
                VertexUpdates(vertex, &(*vertices)[vertex.digest]);
            for (std::size_t u = 0; u < updates.size(); ++u) {
                task->logger.Log(updates[u].first, 0, vertex.name + ": " + updates[u].second);
            }
        }

        for (std::size_t i = 0; i < status.logs.size(); ++i) {
            if (seen->size() >= kMaxTrackedEntries) continue;
            const BuildkitVertexLog& entry = status.logs[i];
            const LogKey key = MakeLogKey(entry);
            const std::size_t count = ++occurrences[key];
            std::size_t& previous = (*seen)[key];
            if (count > previous) {
                task->logger.Output(0, entry.stream == 2, entry.msg);
                previous = count;
            }
        }

        // Warnings are emitted only on the initial subscription; replayed
        // process output and vertex errors above retain their severity.
        if (attempt == 0) {
            for (std::size_t i = 0; i < status.warnings.size(); ++i) {
                task->logger.Log(core::LogLevel::Warn, 0,
                                 core::FromUtf8Lossy(status.warnings[i].short_message));
            }
        }
    }
}

void CollectStatus(const std::shared_ptr<Collector>& task) {
    std::map<LogKey, std::size_t> seen;
    std::map<std::string, BuildkitVertexProgress> vertices;
    core::Optional<std::string> error;
    for (int attempt = 0; attempt < kStatusAttempts; ++attempt) {
        const core::Expected<core::Unit, std::string> result =
            CollectStatusAttempt(task.get(), attempt, &seen, &vertices);
        if (result.ok()) {
            error = core::Optional<std::string>();
            break;
        }
        error = core::Optional<std::string>(result.error());
        if (attempt == kStatusAttempts - 1 || task->aborted.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
    }
    std::lock_guard<std::mutex> lock(task->mu);
    task->error = error;
    task->finished = true;
    task->cv.notify_all();
}

/// Aborts the collector when `WaitForImage` returns, as dropping the `JoinSet`
/// does.
struct CollectorGuard {
    std::shared_ptr<Collector> task;
    ~CollectorGuard() {
        if (task) task->aborted.store(true);
    }
};

/// Why one history subscription ended.
struct SubscriptionResult {
    core::Optional<std::string> digest;
    /// Set for a gRPC status; the only failure that reconnects.
    core::Optional<BuildkitRpcStatus> status;
    /// Set for everything else, including a failed build.
    core::Optional<std::string> error;
};

SubscriptionResult Subscribe(const std::shared_ptr<BuildkitControlWire>& wire,
                             const std::string& image_name, const core::BuildLogger& logger,
                             const core::CancelToken& cancel, CollectorGuard* collector) {
    SubscriptionResult out;
    const core::Expected<std::shared_ptr<BuildkitStream<BuildkitHistoryEvent> >,
                         BuildkitRpcStatus>
        stream = wire->ListenBuildHistory(BuildkitHistoryRequest());
    if (!stream.ok()) {
        out.status = core::Optional<BuildkitRpcStatus>(stream.error());
        return out;
    }
    for (;;) {
        if (cancel.IsCancelled()) {
            out.error = core::Optional<std::string>(std::string("build history wait cancelled"));
            return out;
        }
        const core::Expected<core::Optional<BuildkitHistoryEvent>, BuildkitRpcStatus> next =
            stream.value()->Next();
        if (!next.ok()) {
            out.status = core::Optional<BuildkitRpcStatus>(next.error());
            return out;
        }
        if (!next.value().has_value()) {
            out.status = core::Optional<BuildkitRpcStatus>(BuildkitRpcStatus(
                BuildkitRpcStatus::kUnavailable, "build history stream closed"));
            return out;
        }
        const BuildkitHistoryEvent& event = *next.value();
        const bool ours =
            event.record.has_value() && FindExporter(*event.record, image_name) >= 0;

        // `collecting` in Rust: once per wait, not once per subscription, so a
        // reconnect does not start a second collector replaying the same logs.
        if (ours && !collector->task) {
            std::shared_ptr<Collector> task(new Collector());
            task->wire = wire;
            task->reference = event.record->ref;
            task->logger = logger;
            std::thread([task]() { CollectStatus(task); }).detach();
            collector->task = task;
        }
        if (ours && event.type == kHistoryEventComplete && collector->task) {
            Collector& task = *collector->task;
            std::unique_lock<std::mutex> lock(task.mu);
            task.cv.wait(lock, [&task]() { return task.finished; });
            if (task.error.has_value()) {
                logger.Log(core::LogLevel::Warn, 0,
                           "BuildKit log collection did not finish: Some(Ok(Err(" +
                               *task.error + ")))");
            }
        }

        const core::Expected<core::Optional<std::string>, std::string> completed =
            CompletedImage(event, image_name);
        if (!completed.ok()) {
            out.error = core::Optional<std::string>(completed.error());
            return out;
        }
        if (completed.value().has_value()) {
            out.digest = completed.value();
            return out;
        }
    }
}

}  // namespace

std::string BuildImageName(const std::string& build_id) { return "aenv-build:" + build_id; }

std::vector<std::pair<core::LogLevel, std::string> > VertexUpdates(
    const BuildkitVertex& vertex, BuildkitVertexProgress* progress) {
    std::vector<std::pair<core::LogLevel, std::string> > updates;
    if (vertex.started.has_value() && !progress->started && !progress->finished) {
        progress->started = true;
        updates.push_back(std::make_pair(core::LogLevel::Info, std::string("started")));
    }
    if (!vertex.error.empty()) {
        if (!progress->error.has_value() || *progress->error != vertex.error) {
            progress->error = core::Optional<std::string>(vertex.error);
            updates.push_back(std::make_pair(core::LogLevel::Error, vertex.error));
        }
        progress->finished = true;
    } else if (vertex.cached) {
        if (!progress->cached) {
            progress->cached = true;
            updates.push_back(std::make_pair(core::LogLevel::Info, std::string("cached")));
        }
        progress->finished = true;
    } else if (vertex.completed.has_value() && !progress->finished) {
        progress->finished = true;
        updates.push_back(std::make_pair(core::LogLevel::Info, std::string("completed")));
    }
    return updates;
}

core::Expected<core::Optional<std::string>, std::string> CompletedImage(
    const BuildkitHistoryEvent& event, const std::string& image_name) {
    typedef core::Optional<std::string> Result;
    if (event.type != kHistoryEventComplete) return Result();
    if (!event.record.has_value()) return Result();
    const BuildkitHistoryRecord& record = *event.record;

    const int exporter_index = FindExporter(record, image_name);
    if (exporter_index < 0) return Result();

    if (record.error.has_value() && record.error->code != 0) {
        return core::make_unexpected("BuildKit build " + record.ref + " failed: " +
                                     record.error->message);
    }

    // BuildKit records exported descriptors by exporter index. ExporterResponse
    // contains frontend metadata and need not contain containerimage.digest.
    const std::string missing = "completed BuildKit build has no image descriptor";
    if (!record.result.has_value()) return core::make_unexpected(missing);
    const std::map<int64_t, std::string>::const_iterator descriptor =
        record.result->results.find(exporter_index);
    if (descriptor == record.result->results.end()) return core::make_unexpected(missing);

    const core::Expected<core::Unit, std::string> valid = ValidateDigest(descriptor->second);
    if (!valid.ok()) return core::make_unexpected(valid.error());
    return Result(descriptor->second);
}

core::Expected<BuildkitHistory, std::string> BuildkitHistory::Connect(const std::string& host,
                                                                      uint16_t port) {
    const core::Expected<std::shared_ptr<BuildkitControlWire>, std::string> wire =
        MakeBuildkitControlWire(host, port);
    if (!wire.ok()) return core::make_unexpected("connect to builder history: " + wire.error());
    return BuildkitHistory(wire.value());
}

core::Expected<std::string, std::string> BuildkitHistory::WaitForImage(
    const std::string& build_id, const core::BuildLogger& logger,
    const core::CancelToken& cancel) const {
    const std::string image_name = BuildImageName(build_id);
    CollectorGuard collector;
    for (;;) {
        const SubscriptionResult result = Subscribe(wire_, image_name, logger, cancel, &collector);
        if (result.digest.has_value()) return *result.digest;
        if (result.error.has_value()) return core::make_unexpected(*result.error);
        if (!IsRetryable(*result.status)) return core::make_unexpected(result.status->message);
        if (cancel.WaitCancelled(kRetryDelayMs)) {
            return core::make_unexpected(std::string("build history wait cancelled"));
        }
    }
}

}  // namespace image
}  // namespace agentenv
