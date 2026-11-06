// SPDX-License-Identifier: MIT
// Rust: src/sandbox/logs.rs — sandbox log decoding and bounded query pages.
#ifndef AGENTENV_SANDBOX_LOGS_H_
#define AGENTENV_SANDBOX_LOGS_H_

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/time.h"

namespace agentenv {
namespace sandbox {

/// Rust `MAX_LOG_BYTES` — the cap on a whole log response.
const std::size_t kMaxLogBytes = 32u * 1024u * 1024u;

/// Rust `MAX_LINE_BYTES` — the cap on a *single* line.
///
/// Separate from the total for a reason: the decoder buffers an unterminated
/// line, so without this a peer that streams megabytes with no `\n` would grow
/// `pending` without bound long before any total-size check could fire.
const std::size_t kMaxLineBytes = 256u * 1024u;

/// Rust `struct SandboxLogEntry`.
///
/// `fields` is an ordered map because upstream's is a `BTreeMap`: the field
/// order is observable in the serialised entry, and a hash map would make two
/// runs over identical input produce different bytes.
struct SandboxLogEntry {
    core::SystemTime                   timestamp;
    core::LogRecordLevel               level = core::LogRecordLevel::Info;
    std::string                        message;
    std::map<std::string, std::string> fields;

    bool operator==(const SandboxLogEntry& other) const {
        return timestamp == other.timestamp && level == other.level &&
               message == other.message && fields == other.fields;
    }
    bool operator!=(const SandboxLogEntry& other) const { return !(*this == other); }
};

/// Rust `log_query_window` — the same seven-day window as E2B's
/// `LogQueryWindow`.
///
/// `cursor_ms` is absent for "the most recent week". Both ends are clamped to
/// `now - 7d` and then `start` is clamped to `end`, so the window can come out
/// empty but never *inverted*: a backend handed `start > end` would either
/// error or, worse, scan everything.
struct LogQueryWindow {
    core::SystemTime start;
    core::SystemTime end;
};
LogQueryWindow MakeLogQueryWindow(const core::Optional<int64_t>& cursor_ms, bool backward);

/// Rust `struct LogPage` — accumulates entries, then filters, orders and
/// truncates them.
///
/// Filtering happens on `Push` and truncation only on `Finish`, which is the
/// order that matters: truncating first would let entries the caller excluded
/// consume the limit and return a short page of nothing.
class LogPage {
 public:
    LogPage(const core::SystemTime& start, const core::SystemTime& end, std::size_t limit,
            bool backward, const core::Optional<core::LogRecordLevel>& level,
            const core::Optional<std::string>& search);

    /// Rust `LogPage::push` — keeps the entry if it is inside the window, at
    /// or above the requested level, and matches the search substring.
    ///
    /// Both window bounds are **inclusive**: a cursor names an instant that
    /// must appear in the page it anchors, or paging would drop one entry per
    /// page boundary.
    void Push(const SandboxLogEntry& entry);

    /// Rust `LogPage::finish` — orders by timestamp (descending when paging
    /// backward) and truncates to the limit.
    ///
    /// The sort is **stable** and does not deduplicate: two records with the
    /// same timestamp are two distinct log lines, and collapsing them would
    /// hide genuinely repeated output.
    std::vector<SandboxLogEntry> Finish();

 private:
    core::SystemTime                      start_;
    core::SystemTime                      end_;
    std::size_t                           limit_;
    bool                                  backward_;
    core::Optional<core::LogRecordLevel>  level_;
    core::Optional<std::string>           search_;
    std::vector<SandboxLogEntry>          entries_;
};

/// Rust `struct LogDecoder` — turns a byte stream into entries.
///
/// Stateful across calls because the caller is feeding it HTTP or file chunks
/// whose boundaries have nothing to do with line boundaries; a line split
/// across two reads must still decode as one entry.
class LogDecoder {
 public:
    LogDecoder();

    /// Rust `LogDecoder::with_fallback`.
    ///
    /// The fallback is what makes a line that is neither JSON nor
    /// timestamp-prefixed usable: plain output (a crashing process's stderr,
    /// say) is attributed to the supplied timestamp/level/fields instead of
    /// being dropped. Without a fallback such a line *is* dropped, which is
    /// what the sandbox's own structured streams want.
    static LogDecoder WithFallback(const core::SystemTime& timestamp,
                                   core::LogRecordLevel level,
                                   const std::map<std::string, std::string>& fields);

    /// Rust `LogDecoder::feed`.
    ///
    /// Errors — rather than truncating — when a single line exceeds
    /// `kMaxLineBytes`. Silently cutting it would emit a *valid-looking* entry
    /// whose message is a prefix, and the caller could not tell that from real
    /// output.
    core::Expected<std::vector<SandboxLogEntry>, std::string> Feed(const char* bytes,
                                                                   std::size_t size);
    core::Expected<std::vector<SandboxLogEntry>, std::string> Feed(const std::string& bytes);

 private:
    std::string                        pending_;
    bool                               has_fallback_;
    core::SystemTime                   fallback_timestamp_;
    core::LogRecordLevel               fallback_level_;
    std::map<std::string, std::string> fallback_fields_;
};

/// Rust `parse_log_line` — exposed for testing the three shapes it accepts.
///
/// In order: a fully serialised `SandboxLogEntry`; an svlogd-style
/// `<stamp> <json>` line, where a `timestamp` inside the JSON wins over the
/// prefix; and a bare `<stamp> <text>` line, tagged `logger=envd`.
core::Optional<SandboxLogEntry> ParseLogLine(const std::string& line);

}  // namespace sandbox
}  // namespace agentenv

#endif  // AGENTENV_SANDBOX_LOGS_H_
