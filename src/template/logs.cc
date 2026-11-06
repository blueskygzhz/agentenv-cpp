// SPDX-License-Identifier: MIT
// Rust: src/template/logs.rs
#include "agentenv/template/logs.h"

#include "agentenv/snapshot/repository/documents.h"

namespace agentenv {
namespace tpl {

using snapshot::repository::DocumentKey;
using snapshot::repository::RepositoryError;
using snapshot::repository::RepositoryResult;

const std::size_t kBuildLogMaxBytes = 8 * 1024 * 1024;
const std::size_t kBuildLogMaxEntries = 32768;
const std::size_t kBuildLogMaxMessageBytes = 16 * 1024;

const char* const kBuildLogLimitNotice =
    "Build log limit reached; subsequent output is omitted";

const char* BuildLogLevelToString(BuildLogLevel level) {
    switch (level) {
        case BuildLogLevel::Debug:
            return "debug";
        case BuildLogLevel::Info:
            return "info";
        case BuildLogLevel::Warn:
            return "warn";
        case BuildLogLevel::Error:
            return "error";
    }
    return "info";
}

core::Optional<BuildLogLevel> BuildLogLevelFromString(const std::string& text) {
    if (text == "debug") return core::Optional<BuildLogLevel>(BuildLogLevel::Debug);
    if (text == "info") return core::Optional<BuildLogLevel>(BuildLogLevel::Info);
    if (text == "warn") return core::Optional<BuildLogLevel>(BuildLogLevel::Warn);
    if (text == "error") return core::Optional<BuildLogLevel>(BuildLogLevel::Error);
    return core::Optional<BuildLogLevel>();
}

core::Json BuildLogEntry::ToJson() const {
    core::JsonObject out;
    out["timestamp"] = core::Json(core::FormatRfc3339Nanos(timestamp));
    out["level"] = core::Json(std::string(BuildLogLevelToString(level)));
    out["message"] = core::Json(message);
    // `skip_serializing_if = "Option::is_none"`: a build's own output has no
    // step, and writing `"step": null` would change the shape of every such
    // entry in a document a client already parses.
    if (step.has_value()) out["step"] = core::Json(*step);
    return core::Json(out);
}

core::Expected<BuildLogEntry, std::string> BuildLogEntry::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("build log entry: expected an object"));
    }
    const core::JsonObject& fields = json.as_object();

    BuildLogEntry entry;

    const core::JsonObject::const_iterator timestamp = fields.find("timestamp");
    if (timestamp == fields.end() || timestamp->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("build log entry: timestamp is required"));
    }
    const core::Expected<core::SystemTime, std::string> parsed =
        core::ParseRfc3339(timestamp->second.as_string());
    if (!parsed.ok()) {
        return core::make_unexpected("build log entry: timestamp: " + parsed.error());
    }
    entry.timestamp = parsed.value();

    const core::JsonObject::const_iterator level = fields.find("level");
    if (level == fields.end() || level->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("build log entry: level is required"));
    }
    const core::Optional<BuildLogLevel> decoded =
        BuildLogLevelFromString(level->second.as_string());
    if (!decoded.has_value()) {
        return core::make_unexpected("build log entry: unknown level '" +
                                     level->second.as_string() + "'");
    }
    entry.level = *decoded;

    const core::JsonObject::const_iterator message = fields.find("message");
    if (message == fields.end() || message->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("build log entry: message is required"));
    }
    entry.message = message->second.as_string();

    // Absent *or* null, because the field is skipped when unset: a document
    // written by an older build carries neither spelling.
    const core::JsonObject::const_iterator step = fields.find("step");
    if (step != fields.end() && step->second.kind() == core::Json::Kind::String) {
        entry.step = core::Optional<std::string>(step->second.as_string());
    }
    return entry;
}

bool BuildLogEntry::operator==(const BuildLogEntry& o) const {
    if (step.has_value() != o.step.has_value()) return false;
    if (step.has_value() && *step != *o.step) return false;
    return timestamp == o.timestamp && level == o.level && message == o.message;
}

namespace {

/// Truncates to at most `max` bytes without splitting a UTF-8 sequence.
///
/// Rust walks back to a `is_char_boundary`. Cutting mid-sequence would put
/// invalid UTF-8 into the message, and the JSON encoder would then either
/// fail or emit a replacement character for a line the user wrote.
std::string TruncateAtCharBoundary(const std::string& message, std::size_t max) {
    // Guarded rather than left to the caller: `message[end]` below reads the
    // byte *at* the cut, so `max == size()` would index one past the last
    // element. The only caller checks `size() > max` first, but that is an
    // invariant held at a distance -- cheaper to not depend on it.
    if (max >= message.size()) return message;

    std::size_t end = max;
    // A continuation byte is `10xxxxxx`; back up until a lead byte or ASCII.
    while (end > 0 && (static_cast<unsigned char>(message[end]) & 0xC0) == 0x80) {
        --end;
    }
    return message.substr(0, end);
}

}  // namespace

void BuildLogBuffer::Log(BuildLogLevel level, const core::Optional<std::string>& step,
                         const std::string& message) {
    // Silent once either is set. Sealed means the final flush has run and the
    // document is final, so accepting more would record lines that are never
    // persisted -- `temporary` would disagree with what a reader sees.
    if (truncated_ || sealed_) return;

    std::string text = message;
    if (text.size() > kBuildLogMaxMessageBytes) {
        text = TruncateAtCharBoundary(text, kBuildLogMaxMessageBytes) + " [truncated]";
    }

    BuildLogLevel entry_level = level;
    core::Optional<std::string> entry_step = step;
    if (entries_.size() >= kBuildLogMaxEntries ||
        bytes_ + text.size() > kBuildLogMaxBytes) {
        // One notice replacing the offending line, then nothing: the point is
        // that a reader can tell the log is incomplete. Dropping silently
        // would make a truncated build look like a short one.
        text = kBuildLogLimitNotice;
        entry_level = BuildLogLevel::Warn;
        entry_step = core::Optional<std::string>();
        truncated_ = true;
    }

    // Monotonic by construction: the clock can go backwards (NTP step, or a
    // suspended host) and several producers can interleave, but the entries
    // are an ordered stream. A timestamp that moved backwards would make a
    // reader sorting by time reorder the build's output.
    core::SystemTime timestamp = core::SystemTime::Now();
    if (!entries_.empty() && entries_.back().timestamp > timestamp) {
        timestamp = entries_.back().timestamp;
    }

    bytes_ += text.size();
    BuildLogEntry entry;
    entry.timestamp = timestamp;
    entry.level = entry_level;
    entry.message = text;
    entry.step = entry_step;
    entries_.push_back(entry);
}

void BuildLogBuffer::Output(const core::Optional<std::string>& step, bool stderr_stream,
                            const std::string& bytes) {
    // Rust `str::lines`: splits on `\n`, drops a single trailing `\r`, and
    // yields nothing for empty input -- so a child that writes nothing adds
    // no entries rather than one blank one.
    const BuildLogLevel level = stderr_stream ? BuildLogLevel::Warn : BuildLogLevel::Info;
    std::string::size_type begin = 0;
    while (begin < bytes.size()) {
        std::string::size_type end = bytes.find('\n', begin);
        const bool last = end == std::string::npos;
        if (last) end = bytes.size();
        std::string line = bytes.substr(begin, end - begin);
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        Log(level, step, line);
        if (last) break;
        begin = end + 1;
    }
}

RepositoryResult<core::Unit> WriteBuildLogs(snapshot::repository::SnapshotRepository& repository,
                                            const core::SnapshotId& id,
                                            const std::vector<BuildLogEntry>& entries) {
    core::JsonArray array;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        array.push_back(entries[i].ToJson());
    }
    return repository.PutDocument(DocumentKey::BuildLog(id), core::Json(array).ToString());
}

RepositoryResult<std::vector<BuildLogEntry> > ReadBuildLogs(
    snapshot::repository::SnapshotRepository& repository, const core::SnapshotId& id) {
    const RepositoryResult<core::Optional<std::string> > document =
        repository.GetDocument(DocumentKey::BuildLog(id));
    if (!document.ok()) return core::make_unexpected(document.error());

    std::vector<BuildLogEntry> entries;
    // Absent is empty: a build that has not logged yet, and one whose record
    // was deleted, both legitimately have no document.
    if (!document.value().has_value()) return entries;

    const core::Expected<core::Json, core::AnyError> json =
        core::Json::Parse(*document.value());
    if (!json.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("decode build logs", json.error().chain()));
    }
    if (json.value().kind() != core::Json::Kind::Array) {
        return core::make_unexpected(
            RepositoryError::Backend("decode build logs", "expected an array"));
    }
    const core::JsonArray& array = json.value().as_array();
    for (std::size_t i = 0; i < array.size(); ++i) {
        const core::Expected<BuildLogEntry, std::string> entry =
            BuildLogEntry::FromJson(array[i]);
        if (!entry.ok()) {
            return core::make_unexpected(
                RepositoryError::Backend("decode build logs", entry.error()));
        }
        entries.push_back(entry.value());
    }
    return entries;
}

}  // namespace tpl
}  // namespace agentenv
