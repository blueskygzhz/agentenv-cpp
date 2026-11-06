// SPDX-License-Identifier: MIT
// Rust: src/sandbox/logs.rs
#include "agentenv/sandbox/logs.h"

#include <algorithm>
#include <cstdio>

#include "agentenv/core/json.h"

namespace agentenv {
namespace sandbox {

namespace {

const int64_t kNanosPerMilli = 1000000LL;
const int64_t kSevenDaysNanos = 7LL * 24 * 60 * 60 * 1000000000LL;

/// Rust's `DateTime::<Utc>::MAX_UTC` stand-in, and the saturating arithmetic
/// around it.
///
/// Upstream needs this because an `i64` of milliseconds covers a far wider
/// range than `chrono` can represent, so `from_timestamp_millis` returns
/// `None` and the code falls back to the maximum. Here the hazard is the same
/// shape but one level lower: milliseconds-to-nanoseconds multiplies by a
/// million, so a cursor anywhere near `i64::MAX` ms overflows `unix_nanos`.
/// Wrapping would turn a far-future cursor into a far-*past* one and hand back
/// a page of ancient logs, so both the conversion and the ±7d arithmetic
/// saturate.
const int64_t kMaxNanos = 0x7FFFFFFFFFFFFFFFLL;
const int64_t kMinNanos = -kMaxNanos - 1;

int64_t SaturatingMillisToNanos(int64_t millis) {
    if (millis > kMaxNanos / kNanosPerMilli) return kMaxNanos;
    if (millis < kMinNanos / kNanosPerMilli) return kMinNanos;
    return millis * kNanosPerMilli;
}

int64_t SaturatingAdd(int64_t value, int64_t delta) {
    if (delta > 0 && value > kMaxNanos - delta) return kMaxNanos;
    if (delta < 0 && value < kMinNanos - delta) return kMinNanos;
    return value + delta;
}

/// Rust `str::trim_end` — trailing ASCII whitespace, which is what strips the
/// `\n` the decoder kept and any `\r` from a CRLF peer.
std::string TrimEnd(const std::string& text) {
    std::string::size_type end = text.size();
    while (end > 0) {
        const char c = text[end - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v') break;
        --end;
    }
    return text.substr(0, end);
}

std::string TrimStart(const std::string& text) {
    std::string::size_type begin = 0;
    while (begin < text.size()) {
        const char c = text[begin];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v') break;
        ++begin;
    }
    return text.substr(begin);
}

/// Rust's `serde_json::from_str::<SandboxLogEntry>` — the strict shape, where
/// every field must be present and well-formed.
core::Optional<SandboxLogEntry> ParseFullEntry(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) return core::Optional<SandboxLogEntry>();
    const core::JsonObject& object = json.as_object();

    const core::JsonObject::const_iterator timestamp = object.find("timestamp");
    const core::JsonObject::const_iterator level = object.find("level");
    const core::JsonObject::const_iterator message = object.find("message");
    const core::JsonObject::const_iterator fields = object.find("fields");
    // `fields` is required here: this branch decodes what *we* serialised, and
    // a missing `fields` means the line is some other shape that the looser
    // branches below should get a chance at.
    if (timestamp == object.end() || level == object.end() || message == object.end() ||
        fields == object.end()) {
        return core::Optional<SandboxLogEntry>();
    }
    if (timestamp->second.kind() != core::Json::Kind::String || level->second.kind() != core::Json::Kind::String ||
        message->second.kind() != core::Json::Kind::String || fields->second.kind() != core::Json::Kind::Object) {
        return core::Optional<SandboxLogEntry>();
    }

    const core::Expected<core::SystemTime, std::string> when =
        core::ParseRfc3339(timestamp->second.as_string());
    if (!when.ok()) return core::Optional<SandboxLogEntry>();
    const core::Optional<core::LogRecordLevel> severity =
        core::LogRecordLevelFromString(level->second.as_string());
    if (!severity.has_value()) return core::Optional<SandboxLogEntry>();

    SandboxLogEntry entry;
    entry.timestamp = when.value();
    entry.level = *severity;
    entry.message = message->second.as_string();
    const core::JsonObject& raw_fields = fields->second.as_object();
    for (core::JsonObject::const_iterator it = raw_fields.begin(); it != raw_fields.end(); ++it) {
        if (it->second.kind() != core::Json::Kind::String) return core::Optional<SandboxLogEntry>();
        entry.fields[it->first] = it->second.as_string();
    }
    return core::Optional<SandboxLogEntry>(entry);
}

/// Rust's `Number::to_string` for a non-integral field value.
std::string DoubleToString(double value) {
    // `%.17g` then trimmed is overkill for a log field; `%g` matches what a
    // reader expects to see and what serde prints for a plain float.
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

}  // namespace

// ---------------------------------------------------------------------------
// the query window
// ---------------------------------------------------------------------------

LogQueryWindow MakeLogQueryWindow(const core::Optional<int64_t>& cursor_ms, bool backward) {
    const core::SystemTime now = core::SystemTime::Now();
    const core::SystemTime oldest(SaturatingAdd(now.unix_nanos, -kSevenDaysNanos));

    core::SystemTime start = oldest;
    core::SystemTime end = now;
    if (cursor_ms.has_value()) {
        const core::SystemTime cursor(SaturatingMillisToNanos(*cursor_ms));
        if (backward) {
            start = core::SystemTime(SaturatingAdd(cursor.unix_nanos, -kSevenDaysNanos));
            end = cursor;
        } else {
            start = cursor;
            end = core::SystemTime(SaturatingAdd(cursor.unix_nanos, kSevenDaysNanos));
        }
    }

    // Clamp order is upstream's and it is load-bearing: both ends are floored
    // at the retention horizon first, then `start` is pulled down to `end`.
    // A window may legitimately come out empty (`start == end`, which is what
    // a backward page from the horizon gives) but it must never be inverted --
    // a backend handed `start > end` would either error or scan everything.
    if (start < oldest) start = oldest;
    if (end < oldest) end = oldest;
    if (start > end) start = end;

    LogQueryWindow window;
    window.start = start;
    window.end = end;
    return window;
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

LogPage::LogPage(const core::SystemTime& start, const core::SystemTime& end, std::size_t limit,
                 bool backward, const core::Optional<core::LogRecordLevel>& level,
                 const core::Optional<std::string>& search)
    : start_(start),
      end_(end),
      limit_(limit),
      backward_(backward),
      level_(level),
      search_(search) {}

void LogPage::Push(const SandboxLogEntry& entry) {
    if (limit_ == 0) return;
    // Inclusive on both ends: a cursor names an instant that must appear in
    // the page it anchors, otherwise paging drops one entry per boundary.
    if (entry.timestamp < start_ || entry.timestamp > end_) return;
    if (level_.has_value() && entry.level < *level_) return;
    // Case-sensitive substring, as upstream: the search text comes straight
    // from the caller and folding case would widen their query silently.
    if (search_.has_value() && entry.message.find(*search_) == std::string::npos) return;
    entries_.push_back(entry);
}

std::vector<SandboxLogEntry> LogPage::Finish() {
    const bool backward = backward_;
    // `stable_sort`, not `sort`: entries sharing a timestamp keep arrival
    // order instead of being permuted arbitrarily, so a page is reproducible
    // and two identical lines stay two lines.
    std::stable_sort(entries_.begin(), entries_.end(),
                     [backward](const SandboxLogEntry& left, const SandboxLogEntry& right) {
                         return backward ? right.timestamp < left.timestamp
                                         : left.timestamp < right.timestamp;
                     });
    if (entries_.size() > limit_) entries_.resize(limit_);
    return entries_;
}

// ---------------------------------------------------------------------------
// the decoder
// ---------------------------------------------------------------------------

LogDecoder::LogDecoder()
    : has_fallback_(false), fallback_level_(core::LogRecordLevel::Info) {}

LogDecoder LogDecoder::WithFallback(const core::SystemTime& timestamp,
                                    core::LogRecordLevel level,
                                    const std::map<std::string, std::string>& fields) {
    LogDecoder decoder;
    decoder.has_fallback_ = true;
    decoder.fallback_timestamp_ = timestamp;
    decoder.fallback_level_ = level;
    decoder.fallback_fields_ = fields;
    return decoder;
}

core::Expected<std::vector<SandboxLogEntry>, std::string> LogDecoder::Feed(
    const std::string& bytes) {
    return Feed(bytes.data(), bytes.size());
}

core::Expected<std::vector<SandboxLogEntry>, std::string> LogDecoder::Feed(const char* bytes,
                                                                          std::size_t size) {
    std::vector<SandboxLogEntry> result;

    // Rust's `split_inclusive(|b| *b == b'\n')`: each part keeps its trailing
    // newline, and the final part has none if the chunk ended mid-line.
    std::size_t offset = 0;
    while (offset < size) {
        std::size_t end = offset;
        while (end < size && bytes[end] != '\n') ++end;
        const bool terminated = end < size;
        if (terminated) ++end;
        const std::size_t part_size = end - offset;

        // Checked *before* appending, as upstream does: the point is to refuse
        // a line that would exceed the bound, not to notice afterwards that
        // the buffer already holds it.
        if (pending_.size() + part_size > kMaxLineBytes) {
            return core::make_unexpected(std::string("sandbox log line exceeds size limit"));
        }
        pending_.append(bytes + offset, part_size);
        offset = end;

        if (!terminated) break;

        core::Optional<SandboxLogEntry> entry = ParseLogLine(pending_);
        if (!entry.has_value() && has_fallback_) {
            // Unparseable but not empty: attribute it to the fallback rather
            // than discard it. A crashing process's plain stderr is exactly
            // the output an operator most needs, and it has no structure.
            const std::string message = TrimEnd(pending_);
            if (!message.empty()) {
                SandboxLogEntry fallback;
                fallback.timestamp = fallback_timestamp_;
                fallback.level = fallback_level_;
                fallback.message = message;
                fallback.fields = fallback_fields_;
                entry = core::Optional<SandboxLogEntry>(fallback);
            }
        }
        if (entry.has_value()) result.push_back(*entry);
        pending_.clear();
    }

    return result;
}

// ---------------------------------------------------------------------------
// line parsing
// ---------------------------------------------------------------------------

core::Optional<SandboxLogEntry> ParseLogLine(const std::string& raw) {
    const std::string line = TrimEnd(raw);

    // 1. A fully serialised entry -- what this process itself writes.
    const core::Expected<core::Json, core::AnyError> whole = core::Json::Parse(line);
    if (whole.ok()) {
        const core::Optional<SandboxLogEntry> entry = ParseFullEntry(whole.value());
        if (entry.has_value()) return entry;
    }

    // 2. svlogd's `<stamp> <rest>`. The stamp is `2026-09-29_01:02:03.12345`:
    // `_` for the date/time separator and no zone, so it becomes RFC 3339 by
    // substituting `T` and appending `Z`.
    std::string prefix;
    std::string content = line;
    const std::string::size_type space = line.find(' ');
    if (space != std::string::npos) {
        prefix = line.substr(0, space);
        content = line.substr(space + 1);
    }

    bool have_stamp = false;
    core::SystemTime stamp;
    if (!prefix.empty()) {
        std::string candidate = prefix;
        for (std::string::size_type i = 0; i < candidate.size(); ++i) {
            if (candidate[i] == '_') candidate[i] = 'T';
        }
        const core::Expected<core::SystemTime, std::string> parsed =
            core::ParseRfc3339(candidate + "Z");
        if (parsed.ok()) {
            stamp = parsed.value();
            have_stamp = true;
        }
    }
    // Only consume the prefix if it really was a stamp. Otherwise the whole
    // line is the content -- splitting on the first space regardless would
    // silently eat the first word of an unprefixed message.
    if (have_stamp) {
        content = TrimStart(content);
    } else {
        content = line;
    }

    // 3. A JSON object after the stamp, flattened into fields.
    const core::Expected<core::Json, core::AnyError> body = core::Json::Parse(content);
    if (body.ok() && body.value().kind() == core::Json::Kind::Object) {
        const core::JsonObject& object = body.value().as_object();

        // A `timestamp` inside the body wins over the svlogd prefix: the
        // prefix is stamped by the log *shipper* on arrival, while the body's
        // is stamped by the process that emitted the line. Only the latter
        // reflects when the event happened, and it has sub-second precision
        // the prefix may have lost.
        bool resolved = false;
        core::SystemTime when;
        const core::JsonObject::const_iterator timestamp = object.find("timestamp");
        if (timestamp != object.end() && timestamp->second.kind() == core::Json::Kind::String) {
            const core::Expected<core::SystemTime, std::string> parsed =
                core::ParseRfc3339(timestamp->second.as_string());
            if (parsed.ok()) {
                when = parsed.value();
                resolved = true;
            }
        }
        if (!resolved && have_stamp) {
            when = stamp;
            resolved = true;
        }
        // No usable time from either source: the entry is dropped rather than
        // stamped with "now", which would place old output at the head of
        // every page that reads it.
        if (!resolved) return core::Optional<SandboxLogEntry>();

        const core::JsonObject::const_iterator message = object.find("message");
        if (message == object.end() || message->second.kind() != core::Json::Kind::String) {
            return core::Optional<SandboxLogEntry>();
        }

        SandboxLogEntry entry;
        entry.timestamp = when;
        entry.message = message->second.as_string();
        entry.level = core::LogRecordLevel::Info;
        const core::JsonObject::const_iterator level = object.find("level");
        if (level != object.end() && level->second.kind() == core::Json::Kind::String) {
            const core::Optional<core::LogRecordLevel> parsed =
                core::LogRecordLevelFromString(level->second.as_string());
            // An unrecognised level falls back to `Info` here -- unlike a
            // stored build-log entry, which is rejected. The difference is
            // deliberate and upstream's: this input comes from a guest process
            // we do not control, and dropping the line over one bad field
            // would lose output that is otherwise fine.
            if (parsed.has_value()) entry.level = *parsed;
        }

        // Scalars are flattened to strings; objects and arrays are dropped,
        // because `fields` is a flat string map and there is no lossless
        // spelling for a nested value in it.
        for (core::JsonObject::const_iterator it = object.begin(); it != object.end(); ++it) {
            if (it->first == "timestamp" || it->first == "message" || it->first == "level") {
                continue;
            }
            if (it->second.kind() == core::Json::Kind::String) {
                entry.fields[it->first] = it->second.as_string();
            } else if (it->second.kind() == core::Json::Kind::Int) {
                // Printed from the `int64_t` directly rather than through a
                // double: a 64-bit id past 2^53 would otherwise come out of
                // the field map rounded.
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%lld",
                              static_cast<long long>(it->second.as_int()));
                entry.fields[it->first] = buffer;
            } else if (it->second.kind() == core::Json::Kind::Double) {
                entry.fields[it->first] = DoubleToString(it->second.as_double());
            } else if (it->second.kind() == core::Json::Kind::Bool) {
                entry.fields[it->first] = it->second.as_bool() ? "true" : "false";
            }
        }
        return core::Optional<SandboxLogEntry>(entry);
    }

    // 4. A bare `<stamp> <text>` line. Without a stamp there is nothing to
    // order it by, so it is left to the caller's fallback.
    if (!have_stamp) return core::Optional<SandboxLogEntry>();

    SandboxLogEntry entry;
    entry.timestamp = stamp;
    entry.level = core::LogRecordLevel::Info;
    entry.message = content;
    entry.fields["logger"] = "envd";
    return core::Optional<SandboxLogEntry>(entry);
}

}  // namespace sandbox
}  // namespace agentenv
