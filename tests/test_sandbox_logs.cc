// SPDX-License-Identifier: MIT
// Rust: src/sandbox/logs.rs — `mod tests`, all three cases ported with their
// original assertions, plus the branches upstream's inline tests do not reach
// (the fallback path, the stamp-vs-body timestamp precedence, and the
// millisecond-to-nanosecond overflow that only exists in this port).
#include "agentenv/sandbox/logs.h"

#include <string>
#include <vector>

#include "microtest.h"

using namespace agentenv;           // NOLINT
using namespace agentenv::sandbox;  // NOLINT

namespace {

SandboxLogEntry Entry(int64_t ms, core::LogRecordLevel level, const std::string& message) {
    SandboxLogEntry entry;
    entry.timestamp = core::SystemTime(ms * 1000000LL);
    entry.level = level;
    entry.message = message;
    return entry;
}

LogPage TestPage(std::size_t limit, bool backward) {
    return LogPage(core::SystemTime(0), core::SystemTime(10000LL * 1000000LL), limit, backward,
                   core::Optional<core::LogRecordLevel>(), core::Optional<std::string>());
}

std::vector<int64_t> Millis(const std::vector<SandboxLogEntry>& entries) {
    std::vector<int64_t> out;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        out.push_back(entries[i].timestamp.unix_nanos / 1000000LL);
    }
    return out;
}

core::Optional<int64_t> Cursor(int64_t ms) { return core::Optional<int64_t>(ms); }

/// `fields[key]` on a const map, returning a sentinel rather than inserting.
/// `operator[]` would default-construct the entry and turn "field absent" into
/// "field empty" — which is exactly the distinction several of these tests are
/// asserting.
std::string Field(const SandboxLogEntry& entry, const std::string& key) {
    const std::map<std::string, std::string>::const_iterator at = entry.fields.find(key);
    return at == entry.fields.end() ? std::string("<absent>") : at->second;
}

bool HasField(const SandboxLogEntry& entry, const std::string& key) {
    return entry.fields.find(key) != entry.fields.end();
}

}  // namespace

// ---- the page ---------------------------------------------------------------

MT_TEST(sandbox_logs_merge_filters_before_limit_and_preserves_equal_timestamps) {
    // Upstream, verbatim.
    LogPage page(core::SystemTime(0), core::SystemTime(10000LL * 1000000LL), 3, true,
                 core::Optional<core::LogRecordLevel>(core::LogRecordLevel::Warn),
                 core::Optional<std::string>("Error"));
    page.Push(Entry(10, core::LogRecordLevel::Error, "Error old"));
    page.Push(Entry(20, core::LogRecordLevel::Warn, "Error same"));
    page.Push(Entry(20, core::LogRecordLevel::Warn, "Error same"));
    page.Push(Entry(30, core::LogRecordLevel::Debug, "Error debug"));
    page.Push(Entry(40, core::LogRecordLevel::Error, "error lowercase"));
    page.Push(Entry(50, core::LogRecordLevel::Error, "Error new"));

    const std::vector<int64_t> got = Millis(page.Finish());
    // 50, 20, 20 — newest first; `30` is below the level filter and `40` fails
    // the case-sensitive search, and both are excluded *before* the limit is
    // applied. Filtering after truncation would return a page of one.
    MT_EXPECT_EQ(got.size(), static_cast<std::size_t>(3));
    MT_EXPECT_TRUE(got[0] == 50 && got[1] == 20 && got[2] == 20);
}

MT_TEST(sandbox_logs_forward_order_and_a_zero_limit) {
    LogPage forward = TestPage(2, false);
    forward.Push(Entry(30, core::LogRecordLevel::Info, "message"));
    forward.Push(Entry(10, core::LogRecordLevel::Info, "message"));
    forward.Push(Entry(20, core::LogRecordLevel::Info, "message"));
    const std::vector<int64_t> got = Millis(forward.Finish());
    MT_EXPECT_EQ(got.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(got[0] == 10 && got[1] == 20);

    // A zero limit keeps nothing — and keeps nothing *cheaply*, since `Push`
    // returns before the entry is copied.
    LogPage none = TestPage(0, true);
    none.Push(Entry(1, core::LogRecordLevel::Error, "message"));
    MT_EXPECT_TRUE(none.Finish().empty());
}

MT_TEST(sandbox_logs_cursor_is_inclusive_and_handles_int64_max) {
    // Upstream: a degenerate window `[cursor, cursor]` must still return the
    // entries exactly at the cursor, or paging loses one entry per boundary.
    const core::SystemTime cursor(20LL * 1000000LL);
    LogPage page(cursor, cursor, 10, true, core::Optional<core::LogRecordLevel>(),
                 core::Optional<std::string>());
    page.Push(Entry(19, core::LogRecordLevel::Info, "same"));
    page.Push(Entry(20, core::LogRecordLevel::Info, "same"));
    page.Push(Entry(20, core::LogRecordLevel::Info, "same"));
    page.Push(Entry(21, core::LogRecordLevel::Info, "same"));
    MT_EXPECT_EQ(page.Finish().size(), static_cast<std::size_t>(2));

    const LogQueryWindow future = MakeLogQueryWindow(Cursor(0x7FFFFFFFFFFFFFFFLL), false);
    MT_EXPECT_TRUE(future.start <= future.end);
    const LogQueryWindow old = MakeLogQueryWindow(Cursor(0), true);
    MT_EXPECT_TRUE(old.start == old.end);
}

MT_TEST(the_level_filter_is_a_floor_not_an_equality) {
    // `entry.level < requested` — asking for Warn must include Error, or an
    // operator filtering for problems would not see the worst of them.
    LogPage page(core::SystemTime(0), core::SystemTime(10000LL * 1000000LL), 10, false,
                 core::Optional<core::LogRecordLevel>(core::LogRecordLevel::Warn),
                 core::Optional<std::string>());
    page.Push(Entry(1, core::LogRecordLevel::Debug, "d"));
    page.Push(Entry(2, core::LogRecordLevel::Info, "i"));
    page.Push(Entry(3, core::LogRecordLevel::Warn, "w"));
    page.Push(Entry(4, core::LogRecordLevel::Error, "e"));
    const std::vector<SandboxLogEntry> got = page.Finish();
    MT_EXPECT_EQ(got.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(got[0].message == "w" && got[1].message == "e");
}

MT_TEST(equal_timestamps_keep_arrival_order_rather_than_being_permuted) {
    // A stable sort, so a page is reproducible and two identical lines stay
    // two distinguishable lines in the order they were produced.
    LogPage page = TestPage(10, false);
    page.Push(Entry(5, core::LogRecordLevel::Info, "first"));
    page.Push(Entry(5, core::LogRecordLevel::Info, "second"));
    page.Push(Entry(5, core::LogRecordLevel::Info, "third"));
    const std::vector<SandboxLogEntry> got = page.Finish();
    MT_EXPECT_EQ(got.size(), static_cast<std::size_t>(3));
    MT_EXPECT_TRUE(got[0].message == "first");
    MT_EXPECT_TRUE(got[1].message == "second");
    MT_EXPECT_TRUE(got[2].message == "third");
}

// ---- the window -------------------------------------------------------------

MT_TEST(the_window_is_never_inverted_and_is_floored_at_the_horizon) {
    // No cursor: the most recent week.
    const LogQueryWindow recent =
        MakeLogQueryWindow(core::Optional<int64_t>(), false);
    MT_EXPECT_TRUE(recent.start < recent.end);
    const int64_t seven_days = 7LL * 24 * 60 * 60 * 1000000000LL;
    MT_EXPECT_TRUE(recent.end.unix_nanos - recent.start.unix_nanos == seven_days);

    // A cursor older than the horizon collapses to an empty window rather
    // than reaching back past retention.
    const LogQueryWindow ancient = MakeLogQueryWindow(Cursor(1000), true);
    MT_EXPECT_TRUE(ancient.start == ancient.end);

    // A far-future cursor paging forward: both ends saturate and the window
    // stays ordered. The hazard here is this port's own -- milliseconds are
    // multiplied by a million to reach nanoseconds, so a cursor near
    // `i64::MAX` ms overflows. Wrapping would turn a future cursor into a past
    // one and silently serve ancient logs as the newest page.
    const int64_t extremes[] = {0x7FFFFFFFFFFFFFFFLL, 0x7FFFFFFFFFFFFFFELL,
                                -0x7FFFFFFFFFFFFFFFLL - 1, 0x0020000000000000LL};
    for (std::size_t i = 0; i < sizeof(extremes) / sizeof(extremes[0]); ++i) {
        const LogQueryWindow forward = MakeLogQueryWindow(Cursor(extremes[i]), false);
        MT_EXPECT_TRUE(forward.start <= forward.end);
        const LogQueryWindow backward = MakeLogQueryWindow(Cursor(extremes[i]), true);
        MT_EXPECT_TRUE(backward.start <= backward.end);
    }
}

// ---- the decoder ------------------------------------------------------------

MT_TEST(sandbox_logs_decoder_handles_svlogd_chunking_and_flat_fields) {
    // Upstream, including the seven-byte chunking: the point is that chunk
    // boundaries have nothing to do with line boundaries.
    const std::string line =
        "2026-09-29_01:02:03.12345 {\"timestamp\":\"2026-09-29T01:02:03.123456Z\","
        "\"message\":\"started\",\"level\":\"debug\",\"logger\":\"process\",\"pid\":42,"
        "\"nested\":{}}\n";

    LogDecoder decoder;
    std::vector<SandboxLogEntry> logs;
    for (std::size_t at = 0; at < line.size(); at += 7) {
        const std::string chunk = line.substr(at, 7);
        const core::Expected<std::vector<SandboxLogEntry>, std::string> fed = decoder.Feed(chunk);
        MT_EXPECT_TRUE(fed.ok());
        for (std::size_t i = 0; i < fed.value().size(); ++i) logs.push_back(fed.value()[i]);
    }

    MT_EXPECT_EQ(logs.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(logs[0].level == core::LogRecordLevel::Debug);
    MT_EXPECT_TRUE(logs[0].message == "started");
    MT_EXPECT_TRUE(Field(logs[0], "pid") == "42");
    MT_EXPECT_TRUE(Field(logs[0], "logger") == "process");
    // A nested object has no lossless spelling in a flat string map, so it is
    // dropped rather than rendered as `{}` or as its JSON text.
    MT_EXPECT_TRUE(!HasField(logs[0], "nested"));
    // The body's timestamp wins over the svlogd prefix: `.123456` not
    // `.12345`. The prefix is stamped by the shipper on arrival, the body by
    // the process that emitted the line — only the latter says when the event
    // happened.
    MT_EXPECT_EQ(logs[0].timestamp.unix_nanos % 1000000000LL, 123456000LL);

    // An unterminated tail is buffered, not emitted.
    const core::Expected<std::vector<SandboxLogEntry>, std::string> partial =
        decoder.Feed("partial");
    MT_EXPECT_TRUE(partial.ok());
    MT_EXPECT_TRUE(partial.value().empty());

    // One oversized line is an error, not a truncated entry: a silently cut
    // message is indistinguishable from real output.
    LogDecoder bounded;
    const core::Expected<std::vector<SandboxLogEntry>, std::string> huge =
        bounded.Feed(std::string(kMaxLineBytes + 1, 'x'));
    MT_EXPECT_TRUE(!huge.ok());
}

MT_TEST(the_line_bound_is_on_a_line_not_on_a_chunk) {
    // Many large chunks are fine as long as each *line* fits; the bound exists
    // because the decoder buffers an unterminated line, not to limit reads.
    LogDecoder decoder;
    const std::string big_line(kMaxLineBytes - 1, 'y');
    const core::Expected<std::vector<SandboxLogEntry>, std::string> first =
        decoder.Feed(big_line + "\n");
    MT_EXPECT_TRUE(first.ok());
    // Not parseable and no fallback, so nothing is emitted -- but the buffer
    // was released, which is what lets the next long line through.
    MT_EXPECT_TRUE(first.value().empty());
    const core::Expected<std::vector<SandboxLogEntry>, std::string> second =
        decoder.Feed(big_line + "\n");
    MT_EXPECT_TRUE(second.ok());

    // Accumulating across chunks still trips it: the check is on
    // pending + part, not on part alone.
    LogDecoder split;
    const std::string half(kMaxLineBytes / 2 + 1, 'z');
    MT_EXPECT_TRUE(split.Feed(half).ok());
    MT_EXPECT_TRUE(!split.Feed(half).ok());
}

MT_TEST(unparseable_lines_need_a_fallback_to_survive) {
    // Without one they are dropped: a structured stream's garbage has no
    // timestamp, and stamping it with "now" would put it at the head of every
    // page that reads it.
    LogDecoder strict;
    const core::Expected<std::vector<SandboxLogEntry>, std::string> dropped =
        strict.Feed("just some words\n");
    MT_EXPECT_TRUE(dropped.ok());
    MT_EXPECT_TRUE(dropped.value().empty());

    // With one, plain output is attributed instead of lost — which is the
    // output an operator most needs when a process dies badly.
    std::map<std::string, std::string> fields;
    fields["logger"] = "startup";
    LogDecoder lenient = LogDecoder::WithFallback(core::SystemTime(1234567890LL),
                                                  core::LogRecordLevel::Error, fields);
    const core::Expected<std::vector<SandboxLogEntry>, std::string> kept =
        lenient.Feed("segfault at 0x0\nand more\n");
    MT_EXPECT_TRUE(kept.ok());
    MT_EXPECT_EQ(kept.value().size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(kept.value()[0].message == "segfault at 0x0");
    MT_EXPECT_TRUE(kept.value()[0].level == core::LogRecordLevel::Error);
    MT_EXPECT_TRUE(kept.value()[0].timestamp.unix_nanos == 1234567890LL);
    MT_EXPECT_TRUE(Field(kept.value()[0], "logger") == "startup");

    // A blank line is still nothing, fallback or not.
    const core::Expected<std::vector<SandboxLogEntry>, std::string> blank =
        lenient.Feed("   \n\n");
    MT_EXPECT_TRUE(blank.ok());
    MT_EXPECT_TRUE(blank.value().empty());
}

// ---- line shapes ------------------------------------------------------------

MT_TEST(a_fully_serialised_entry_round_trips) {
    const core::Optional<SandboxLogEntry> entry = ParseLogLine(
        "{\"timestamp\":\"2026-09-29T01:02:03.000000004Z\",\"level\":\"warn\","
        "\"message\":\"m\",\"fields\":{\"a\":\"b\"}}");
    MT_EXPECT_TRUE(entry.has_value());
    MT_EXPECT_TRUE(entry->level == core::LogRecordLevel::Warn);
    MT_EXPECT_TRUE(entry->message == "m");
    MT_EXPECT_TRUE(Field(*entry, "a") == "b");
    // Nanosecond precision survives, which is what keeps two records made in
    // the same millisecond orderable.
    MT_EXPECT_EQ(entry->timestamp.unix_nanos % 1000000000LL, 4LL);
}

MT_TEST(a_bare_stamped_line_is_tagged_as_envd) {
    const core::Optional<SandboxLogEntry> entry =
        ParseLogLine("2026-09-29_01:02:03.5 process exited with status 1\n");
    MT_EXPECT_TRUE(entry.has_value());
    MT_EXPECT_TRUE(entry->message == "process exited with status 1");
    MT_EXPECT_TRUE(entry->level == core::LogRecordLevel::Info);
    MT_EXPECT_TRUE(Field(*entry, "logger") == "envd");
    MT_EXPECT_EQ(entry->timestamp.unix_nanos % 1000000000LL, 500000000LL);
}

MT_TEST(an_unstamped_line_keeps_its_first_word) {
    // Splitting on the first space unconditionally would eat it. The prefix is
    // only consumed once it has parsed as a timestamp.
    MT_EXPECT_TRUE(!ParseLogLine("hello world").has_value());

    std::map<std::string, std::string> fields;
    LogDecoder decoder =
        LogDecoder::WithFallback(core::SystemTime(7), core::LogRecordLevel::Info, fields);
    const core::Expected<std::vector<SandboxLogEntry>, std::string> fed =
        decoder.Feed("hello world\n");
    MT_EXPECT_TRUE(fed.ok());
    MT_EXPECT_EQ(fed.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(fed.value()[0].message == "hello world");
}

MT_TEST(a_body_with_no_usable_timestamp_is_dropped) {
    // JSON with a message but no time from either source: there is nothing to
    // order it by.
    MT_EXPECT_TRUE(!ParseLogLine("{\"message\":\"m\",\"level\":\"info\"}").has_value());
    // A bad `timestamp` in the body falls back to the svlogd prefix rather
    // than dropping the line.
    const core::Optional<SandboxLogEntry> recovered =
        ParseLogLine("2026-09-29_01:02:03.0 {\"timestamp\":\"nonsense\",\"message\":\"m\"}");
    MT_EXPECT_TRUE(recovered.has_value());
    MT_EXPECT_TRUE(recovered->message == "m");
    // And a body with no `message` at all is not an entry.
    MT_EXPECT_TRUE(
        !ParseLogLine("2026-09-29_01:02:03.0 {\"level\":\"warn\",\"a\":1}").has_value());
}

MT_TEST(an_unknown_level_from_a_guest_degrades_to_info) {
    // Deliberately unlike a stored build-log entry, which is rejected: this
    // input comes from a process we do not control, and dropping an otherwise
    // fine line over one bad field would lose real output.
    const core::Optional<SandboxLogEntry> entry = ParseLogLine(
        "2026-09-29_01:02:03.0 {\"message\":\"m\",\"level\":\"trace\"}");
    MT_EXPECT_TRUE(entry.has_value());
    MT_EXPECT_TRUE(entry->level == core::LogRecordLevel::Info);
    // `level` is consumed either way and does not leak into the field map.
    MT_EXPECT_TRUE(!HasField(*entry, "level"));
}

MT_TEST(scalar_fields_are_flattened_and_a_large_int_is_not_rounded) {
    const core::Optional<SandboxLogEntry> entry = ParseLogLine(
        "2026-09-29_01:02:03.0 {\"message\":\"m\",\"id\":9007199254740993,"
        "\"ratio\":0.5,\"ok\":true,\"off\":false,\"list\":[1,2]}");
    MT_EXPECT_TRUE(entry.has_value());
    // 2^53 + 1 — printed from the int64 rather than through a double, which
    // would have rounded it to 9007199254740992.
    MT_EXPECT_TRUE(Field(*entry, "id") == "9007199254740993");
    MT_EXPECT_TRUE(Field(*entry, "ratio") == "0.5");
    MT_EXPECT_TRUE(Field(*entry, "ok") == "true");
    MT_EXPECT_TRUE(Field(*entry, "off") == "false");
    MT_EXPECT_TRUE(!HasField(*entry, "list"));
}

MT_MAIN
