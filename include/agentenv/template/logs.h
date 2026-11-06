// SPDX-License-Identifier: MIT
// Rust: src/template/logs.rs — the build log a template build streams into,
// plus its persistence.
//
// Two things live here. `BuildLogBuffer` is what a build writes to, bounded so
// a runaway build cannot exhaust memory or produce a document nothing can
// read; `WriteBuildLogs`/`ReadBuildLogs` are the typed document access.
//
// Not ported: the tracing-layer bridge (`route_build_logs` and `BuildLogLayer`)
// that lets a bare `tracing::info!` inside a build span land in the build's
// log. That needs a span-aware logging backend; `core::logging` has no span
// concept, so a build must call the logger explicitly. Stubbing the routing
// would silently drop those lines instead.
#ifndef AGENTENV_TEMPLATE_LOGS_H_
#define AGENTENV_TEMPLATE_LOGS_H_

#include <cstddef>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/time.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/repository/interfaces.h"

namespace agentenv {
namespace tpl {

// The level of an entry is `core::LogRecordLevel` — Rust `logging::LogLevel`,
// the four-level serialised severity. It lives in `core/logging.h` rather than
// here because upstream puts it in `crate::logging` and it has two consumers:
// this build log and `sandbox/logs.h`. (It was briefly a `core::LogRecordLevel` local
// to this header; the second consumer made that name wrong.)

/// Rust `struct BuildLogEntry`.
struct BuildLogEntry {
    core::SystemTime timestamp;
    core::LogRecordLevel level = core::LogRecordLevel::Info;
    std::string message;
    /// Rust `#[serde(skip_serializing_if = "Option::is_none")]` — the build
    /// step that produced the line, absent for the build's own output.
    core::Optional<std::string> step;

    core::Json ToJson() const;
    static core::Expected<BuildLogEntry, std::string> FromJson(const core::Json& json);

    bool operator==(const BuildLogEntry& o) const;
    bool operator!=(const BuildLogEntry& o) const { return !(*this == o); }
};

/// Rust `MAX_BYTES` / `MAX_ENTRIES` / `MAX_MESSAGE_BYTES`.
extern const std::size_t kBuildLogMaxBytes;
extern const std::size_t kBuildLogMaxEntries;
extern const std::size_t kBuildLogMaxMessageBytes;

/// Rust's notice emitted once when a limit is hit.
extern const char* const kBuildLogLimitNotice;

/// Rust `struct Buffer` and its `BuildLogSink` impl.
///
/// Bounded three ways because the log is persisted as one JSON document that a
/// client fetches whole: an unbounded buffer would grow with a build that
/// loops, and the document would eventually be too large to read back at all.
///
/// `BuildLogger` is not a separate type here. Upstream needs it because a
/// logger is cloned into tasks that may outlive the buffer, so it holds a
/// `Weak` and silently does nothing once the buffer is gone; with no tracing
/// bridge there is nothing to hand a detached handle to.
class BuildLogBuffer {
 public:
    BuildLogBuffer() : bytes_(0), truncated_(false), sealed_(false) {}

    /// Rust `BuildLogSink::log`.
    ///
    /// Silent once truncated or sealed, which is upstream's behaviour and the
    /// reason a late write cannot alter a closed stream: the flush has already
    /// happened and the document is final.
    void Log(core::LogRecordLevel level, const core::Optional<std::string>& step,
             const std::string& message);

    /// Rust's `output` helper — splits a child's output into lines, stderr at
    /// `Warn` and stdout at `Info`.
    ///
    /// Line-at-a-time so one entry is one line: a build step emitting a
    /// megabyte in one write would otherwise become a single entry that the
    /// message cap then truncates, losing all of it but the head.
    void Output(const core::Optional<std::string>& step, bool stderr_stream,
                const std::string& bytes);

    /// Stops accepting entries. Rust sets this on the final flush.
    void Seal() { sealed_ = true; }

    bool sealed() const { return sealed_; }
    bool truncated() const { return truncated_; }
    const std::vector<BuildLogEntry>& entries() const { return entries_; }

 private:
    std::vector<BuildLogEntry> entries_;
    std::size_t bytes_;
    bool truncated_;
    bool sealed_;
};

/// Rust `BuildLogStore::write_build_logs` — atomically replaces the whole
/// document.
///
/// Whole-document rather than append: the backends expose documents as opaque
/// blobs with no append, and a partial write would leave a log that cannot be
/// parsed as JSON.
snapshot::repository::RepositoryResult<core::Unit> WriteBuildLogs(
    snapshot::repository::SnapshotRepository& repository, const core::SnapshotId& id,
    const std::vector<BuildLogEntry>& entries);

/// Rust `BuildLogStore::read_build_logs` — empty when the document is absent.
///
/// Absent is empty, not an error: a build that has not logged yet, and one
/// whose record was deleted, both legitimately have no document.
snapshot::repository::RepositoryResult<std::vector<BuildLogEntry> > ReadBuildLogs(
    snapshot::repository::SnapshotRepository& repository, const core::SnapshotId& id);

}  // namespace tpl
}  // namespace agentenv
#endif  // AGENTENV_TEMPLATE_LOGS_H_
