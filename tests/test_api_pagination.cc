// SPDX-License-Identifier: MIT
// Rust: src/api/impls/pagination.rs, src/snapshot/types/version.rs and
// src/snapshot/repository/backends/common/acr/manifest.rs test modules, plus
// the `core::base64` / `core::time` primitives this port had to add.
#include <string>
#include <vector>

#include "agentenv/api/pagination.h"
#include "agentenv/core/base64.h"
#include "agentenv/core/time.h"
#include "agentenv/snapshot/acr_manifest.h"
#include "agentenv/snapshot/version.h"
#include "microtest.h"

namespace {

using agentenv::api::Ordering;
using agentenv::api::PaginationCursor;
using agentenv::api::PaginationError;
using agentenv::core::Json;
using agentenv::core::JsonObject;
using agentenv::core::Optional;
using agentenv::core::SystemTime;
using agentenv::core::Unit;
namespace snapshot = agentenv::snapshot;

typedef PaginationCursor<std::string> Cursor;

SystemTime At(int64_t seconds) {
    return SystemTime(seconds * 1000000000LL);
}

/// A row ordered by (time, id), matching what the listing endpoints page over.
struct Row {
    SystemTime time;
    std::string id;
};

Row MakeRow(int64_t seconds, const std::string& id) {
    Row row;
    row.time = At(seconds);
    row.id = id;
    return row;
}

Optional<uint32_t> Limit(uint32_t value) { return Optional<uint32_t>(value); }

std::vector<std::string> Ids(const std::vector<Row>& rows) {
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < rows.size(); ++i) ids.push_back(rows[i].id);
    return ids;
}

/// Compares a row against the cursor using the cursor's own direction, which
/// is how the endpoints drive `paginate_sorted`.
struct CompareAgainstCursor {
    Ordering operator()(const Row& row, const Cursor& cursor) const {
        return Cursor::Compare(cursor.is_descending(), row.time, row.id, cursor.time(),
                               cursor.value());
    }
};

struct RowToCursor {
    bool descending;
    Cursor operator()(const Row& row) const {
        return Cursor::New(row.time, row.id, descending);
    }
};

/// The sort must agree with the cursor comparison, or a page would hand out a
/// token pointing into a part of the list the next page no longer reaches.
struct SortDescending {
    Ordering operator()(const Row& a, const Row& b) const {
        return Cursor::Compare(true, a.time, a.id, b.time, b.id);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// core::base64
// ---------------------------------------------------------------------------

MT_TEST(base64_url_round_trips_every_length_class) {
    // The three tail cases (0, 1, 2 leftover bytes) are where a base64
    // implementation usually breaks.
    const char* const inputs[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char* const expected[] = {"",         "Zg==",     "Zm8=",     "Zm9v",
                                    "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (std::size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        MT_EXPECT_EQ(agentenv::core::Base64UrlEncode(inputs[i]), std::string(expected[i]));
        MT_EXPECT_EQ(agentenv::core::Base64UrlDecode(expected[i]).value(),
                     std::string(inputs[i]));
    }
}

MT_TEST(base64_url_uses_the_url_safe_alphabet) {
    // 0xFB 0xFF would encode as `+/` under the standard alphabet; a `+` in a
    // query string decodes as a space, which would corrupt the cursor.
    const uint8_t bytes[] = {0xFB, 0xFF, 0xBF};
    const std::string encoded = agentenv::core::Base64UrlEncode(bytes, sizeof(bytes));
    MT_EXPECT_TRUE(encoded.find('+') == std::string::npos);
    MT_EXPECT_TRUE(encoded.find('/') == std::string::npos);
    MT_EXPECT_EQ(encoded, std::string("-_-_"));
    MT_EXPECT_EQ(agentenv::core::Base64UrlDecode(encoded).value().size(),
                 static_cast<std::size_t>(3));
}

MT_TEST(base64_url_decode_rejects_malformed_input) {
    const char* const rejected[] = {
        "Zg=",      // not a multiple of four
        "Zg===",    // over-padded
        "Zm9v!",    // bad length and symbol
        "Z===",     // padding where a digit must be
        "Zg=A",     // padding followed by a digit
        "Zm+9",     // standard-alphabet symbol
        "Zm/9",
    };
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        MT_EXPECT_TRUE(!agentenv::core::Base64UrlDecode(rejected[i]).ok());
    }
}

// ---------------------------------------------------------------------------
// core::time
// ---------------------------------------------------------------------------

MT_TEST(rfc3339_formats_nine_fractional_digits) {
    // Rust uses `SecondsFormat::Nanos`, so the fraction is always nine digits
    // and cursors from the same second still sort deterministically.
    MT_EXPECT_EQ(agentenv::core::FormatRfc3339Nanos(SystemTime(0)),
                 std::string("1970-01-01T00:00:00.000000000Z"));
    MT_EXPECT_EQ(agentenv::core::FormatRfc3339Nanos(SystemTime(1700000000123456789LL)),
                 std::string("2023-11-14T22:13:20.123456789Z"));
}

MT_TEST(rfc3339_round_trips) {
    const int64_t values[] = {0, 1, 1000000000LL, 1700000000123456789LL,
                              -1000000000LL, 951782400000000000LL};
    for (std::size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        const SystemTime time(values[i]);
        const std::string text = agentenv::core::FormatRfc3339Nanos(time);
        MT_EXPECT_TRUE(agentenv::core::ParseRfc3339(text).ok());
        MT_EXPECT_EQ(agentenv::core::ParseRfc3339(text).value().unix_nanos, values[i]);
    }
}

MT_TEST(rfc3339_handles_pre_epoch_without_a_negative_fraction) {
    // Floor division: half a second before the epoch is `.500000000`, not a
    // negative nanosecond field.
    MT_EXPECT_EQ(agentenv::core::FormatRfc3339Nanos(SystemTime(-500000000LL)),
                 std::string("1969-12-31T23:59:59.500000000Z"));
    MT_EXPECT_EQ(agentenv::core::ParseRfc3339("1969-12-31T23:59:59.500000000Z")
                     .value()
                     .unix_nanos,
                 -500000000LL);
}

MT_TEST(rfc3339_accepts_offsets_and_varied_precision) {
    // Same instant, three spellings.
    const int64_t expected = agentenv::core::ParseRfc3339("2023-11-14T22:13:20Z")
                                 .value()
                                 .unix_nanos;
    MT_EXPECT_EQ(agentenv::core::ParseRfc3339("2023-11-14T23:13:20+01:00").value().unix_nanos,
                 expected);
    MT_EXPECT_EQ(agentenv::core::ParseRfc3339("2023-11-14T21:13:20-01:00").value().unix_nanos,
                 expected);
    MT_EXPECT_EQ(agentenv::core::ParseRfc3339("2023-11-14T22:13:20.5Z").value().unix_nanos,
                 expected + 500000000LL);
    // Beyond nanosecond precision the extra digits are dropped, as chrono does.
    MT_EXPECT_EQ(
        agentenv::core::ParseRfc3339("2023-11-14T22:13:20.1234567891Z").value().unix_nanos,
        expected + 123456789LL);
}

MT_TEST(rfc3339_rejects_malformed_or_out_of_range_input) {
    const char* const rejected[] = {
        "",
        "2023-11-14",
        "2023-11-14T22:13:20",          // no offset: the instant is ambiguous
        "2023-13-01T00:00:00Z",         // month 13
        "2023-02-29T00:00:00Z",         // not a leap year
        "2023-11-14T24:00:00Z",         // hour 24
        "2023-11-14T22:13:20.Z",        // empty fraction
        "2023-11-14T22:13:20Zjunk",     // trailing input
        "2023-11-14T22:13:20+0100",     // offset needs a colon
        "not-a-timestamp-at-all-xxxx",
    };
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        MT_EXPECT_TRUE(!agentenv::core::ParseRfc3339(rejected[i]).ok());
    }
    // A leap year is accepted.
    MT_EXPECT_TRUE(agentenv::core::ParseRfc3339("2024-02-29T00:00:00Z").ok());
}

// ---------------------------------------------------------------------------
// PaginationCursor — encoding
// ---------------------------------------------------------------------------

MT_TEST(cursor_round_trips_through_its_token) {
    const Cursor descending = Cursor::NewDescending(At(1700000000), "abc");
    const Cursor parsed = Cursor::Parse(descending.Encode()).value();
    MT_EXPECT_TRUE(parsed == descending);
    MT_EXPECT_TRUE(parsed.is_descending());

    const Cursor ascending = Cursor::NewAscending(At(1700000000), "abc");
    MT_EXPECT_TRUE(Cursor::Parse(ascending.Encode()).value() == ascending);
    MT_EXPECT_TRUE(!Cursor::Parse(ascending.Encode()).value().is_descending());
}

MT_TEST(cursor_marks_only_ascending_tokens) {
    // The direction travels with the cursor: replaying a descending token
    // against an ascending listing would select the wrong side of the keyset.
    const std::string descending =
        agentenv::core::Base64UrlDecode(Cursor::NewDescending(At(0), "x").Encode()).value();
    const std::string ascending =
        agentenv::core::Base64UrlDecode(Cursor::NewAscending(At(0), "x").Encode()).value();

    MT_EXPECT_EQ(descending, std::string("1970-01-01T00:00:00.000000000Z__x"));
    MT_EXPECT_EQ(ascending, std::string("1970-01-01T00:00:00.000000000Z__x__asc"));
}

MT_TEST(cursor_parse_rejects_a_value_containing_the_separator) {
    // A value containing `__` does not round-trip: `rsplit_once("__")` reads
    // its tail as a direction marker, and anything other than `asc` is
    // refused. Rust behaves the same way, so this is a real constraint on
    // cursor value types rather than a porting gap — ids used as cursor
    // values must not contain `__`.
    const Cursor cursor = Cursor::NewDescending(At(5), "a__b");
    const agentenv::core::Expected<Cursor, PaginationError> parsed =
        Cursor::Parse(cursor.Encode());
    MT_EXPECT_TRUE(!parsed.ok());
    MT_EXPECT_TRUE(parsed.error().kind == PaginationError::Kind::InvalidCursorFormat);

    // A value with no `__` is unaffected, including one containing a single
    // underscore.
    MT_EXPECT_EQ(Cursor::Parse(Cursor::NewDescending(At(5), "a_b").Encode()).value().value(),
                 std::string("a_b"));
}

MT_TEST(cursor_parse_reports_each_failure_distinctly) {
    // Not base64 at all.
    const agentenv::core::Expected<Cursor, PaginationError> bad_base64 =
        Cursor::Parse("!!!!");
    MT_EXPECT_TRUE(!bad_base64.ok());
    MT_EXPECT_TRUE(bad_base64.error().kind == PaginationError::Kind::DecodeCursor);

    // Valid base64, but no `__` separator.
    const agentenv::core::Expected<Cursor, PaginationError> no_separator =
        Cursor::Parse(agentenv::core::Base64UrlEncode("no-separator"));
    MT_EXPECT_TRUE(!no_separator.ok());
    MT_EXPECT_TRUE(no_separator.error().kind == PaginationError::Kind::InvalidCursorFormat);
    MT_EXPECT_EQ(no_separator.error().Message(), std::string("invalid cursor format"));

    // Unparsable timestamp.
    const agentenv::core::Expected<Cursor, PaginationError> bad_time =
        Cursor::Parse(agentenv::core::Base64UrlEncode("not-a-time__abc"));
    MT_EXPECT_TRUE(!bad_time.ok());
    MT_EXPECT_TRUE(bad_time.error().kind == PaginationError::Kind::InvalidCursorTimestamp);

    // A trailing marker that is not `asc` must be rejected, not ignored: a
    // hand-edited token should not be able to flip the direction silently.
    const agentenv::core::Expected<Cursor, PaginationError> bad_marker = Cursor::Parse(
        agentenv::core::Base64UrlEncode("1970-01-01T00:00:00.000000000Z__abc__desc"));
    MT_EXPECT_TRUE(!bad_marker.ok());
    MT_EXPECT_TRUE(bad_marker.error().kind == PaginationError::Kind::InvalidCursorFormat);
}

MT_TEST(pagination_error_messages_match_the_rust_renderings) {
    MT_EXPECT_EQ(
        agentenv::api::MakePaginationError(PaginationError::Kind::DecodeCursor, "boom")
            .Message(),
        std::string("error decoding cursor: boom"));
    MT_EXPECT_EQ(
        agentenv::api::MakePaginationError(PaginationError::Kind::InvalidCursorUtf8, "boom")
            .Message(),
        std::string("invalid cursor format (not utf-8): boom"));
    MT_EXPECT_EQ(agentenv::api::MakePaginationError(
                     PaginationError::Kind::InvalidCursorTimestamp, "boom")
                     .Message(),
                 std::string("invalid timestamp format in cursor: boom"));
    MT_EXPECT_EQ(
        agentenv::api::MakePaginationError(PaginationError::Kind::InvalidCursorValue, "boom")
            .Message(),
        std::string("invalid cursor value: boom"));
}

// ---------------------------------------------------------------------------
// PaginationCursor — ordering
// ---------------------------------------------------------------------------

MT_TEST(compare_orders_by_time_then_reverses_the_value_tiebreak) {
    // Descending compares the *second* time against the first
    // (`b_time.cmp(&a_time)`), so an earlier `a` is Greater. Read through
    // `paginate_sorted`, which keeps items comparing Greater against the
    // cursor, this is what makes a descending page walk towards older rows.
    MT_EXPECT_TRUE(Cursor::CompareDesc(At(1), "a", At(2), "b") == Ordering::Greater);
    MT_EXPECT_TRUE(Cursor::CompareDesc(At(2), "a", At(1), "b") == Ordering::Less);
    MT_EXPECT_TRUE(Cursor::CompareDesc(At(1), "a", At(1), "a") == Ordering::Equal);
    // Within one timestamp the tiebreak is *not* reversed for descending, so
    // the walk continues towards larger ids.
    MT_EXPECT_TRUE(Cursor::CompareDesc(At(1), "b", At(1), "a") == Ordering::Greater);

    // Ascending mirrors both halves, which is what keeps the keyset walk from
    // skipping records that share a timestamp.
    MT_EXPECT_TRUE(Cursor::Compare(false, At(2), "a", At(1), "b") == Ordering::Greater);
    MT_EXPECT_TRUE(Cursor::Compare(false, At(1), "a", At(2), "b") == Ordering::Less);
    MT_EXPECT_TRUE(Cursor::Compare(false, At(1), "a", At(1), "b") == Ordering::Greater);
}

// ---------------------------------------------------------------------------
// PaginationCursor — paging
// ---------------------------------------------------------------------------

MT_TEST(paginate_sorted_drops_everything_up_to_and_including_the_cursor) {
    std::vector<Row> rows;
    rows.push_back(MakeRow(5, "e"));
    rows.push_back(MakeRow(4, "d"));
    rows.push_back(MakeRow(3, "c"));

    // A cursor at (4, "d") must yield only what sorts strictly after it, so
    // the named row is not repeated on the next page.
    const Cursor cursor = Cursor::NewDescending(At(4), "d");
    const agentenv::api::Paginated<Row> page = cursor.PaginateSorted(
        rows, Optional<uint32_t>(), CompareAgainstCursor(), RowToCursor{true});

    MT_EXPECT_EQ(page.items.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(page.items[0].id, std::string("c"));
    MT_EXPECT_TRUE(!page.next_token.has_value());
}

MT_TEST(paginate_sorted_emits_a_token_only_when_more_remains) {
    std::vector<Row> rows;
    rows.push_back(MakeRow(5, "e"));
    rows.push_back(MakeRow(4, "d"));
    rows.push_back(MakeRow(3, "c"));

    // A cursor before everything: `MAX` time under descending order.
    const Cursor start = Cursor::NewDescending(At(100), "");

    const agentenv::api::Paginated<Row> first =
        start.PaginateSorted(rows, Limit(2), CompareAgainstCursor(), RowToCursor{true});
    MT_EXPECT_EQ(Ids(first.items).size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(first.items[0].id, std::string("e"));
    MT_EXPECT_EQ(first.items[1].id, std::string("d"));
    MT_EXPECT_TRUE(first.next_token.has_value());

    // Resuming from that token yields the remainder and no further token.
    const Cursor resumed = Cursor::Parse(*first.next_token).value();
    const agentenv::api::Paginated<Row> second =
        resumed.PaginateSorted(rows, Limit(2), CompareAgainstCursor(), RowToCursor{true});
    MT_EXPECT_EQ(second.items.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(second.items[0].id, std::string("c"));
    MT_EXPECT_TRUE(!second.next_token.has_value());
}

MT_TEST(paginate_sorted_does_not_emit_a_token_for_an_exactly_full_page) {
    std::vector<Row> rows;
    rows.push_back(MakeRow(5, "e"));
    rows.push_back(MakeRow(4, "d"));

    // Strictly-greater, not greater-or-equal: a page that exactly fills the
    // limit is the last one, and a token here would hand out an empty page.
    const agentenv::api::Paginated<Row> page =
        Cursor::NewDescending(At(100), "")
            .PaginateSorted(rows, Limit(2), CompareAgainstCursor(), RowToCursor{true});
    MT_EXPECT_EQ(page.items.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(!page.next_token.has_value());
}

MT_TEST(paginate_with_limit_zero_returns_nothing) {
    std::vector<Row> rows;
    rows.push_back(MakeRow(5, "e"));

    const Cursor start = Cursor::NewDescending(At(100), "");
    const agentenv::api::Paginated<Row> sorted =
        start.PaginateSorted(rows, Limit(0), CompareAgainstCursor(), RowToCursor{true});
    MT_EXPECT_EQ(sorted.items.size(), static_cast<std::size_t>(0));
    MT_EXPECT_TRUE(!sorted.next_token.has_value());

    const agentenv::api::Paginated<Row> unsorted = start.Paginate(
        rows, Limit(0), SortDescending(), CompareAgainstCursor(), RowToCursor{true});
    MT_EXPECT_EQ(unsorted.items.size(), static_cast<std::size_t>(0));
}

MT_TEST(paginate_sorts_before_paging) {
    std::vector<Row> rows;
    rows.push_back(MakeRow(3, "c"));
    rows.push_back(MakeRow(5, "e"));
    rows.push_back(MakeRow(4, "d"));

    const agentenv::api::Paginated<Row> page =
        Cursor::NewDescending(At(100), "")
            .Paginate(rows, Limit(2), SortDescending(), CompareAgainstCursor(),
                      RowToCursor{true});
    // Newest first, regardless of the input order.
    MT_EXPECT_EQ(page.items[0].id, std::string("e"));
    MT_EXPECT_EQ(page.items[1].id, std::string("d"));
}

MT_TEST(paginate_walks_records_sharing_one_timestamp) {
    // The value tiebreak is the reason this terminates instead of looping on
    // the same second forever. Under a descending cursor the tiebreak is
    // *not* inverted, so within one timestamp the walk moves towards larger
    // ids and the rows must be sorted that way.
    std::vector<Row> rows;
    rows.push_back(MakeRow(7, "a"));
    rows.push_back(MakeRow(7, "b"));
    rows.push_back(MakeRow(7, "c"));

    // An empty value sorts before every id, so this starts before the first
    // row rather than after the last.
    Cursor cursor = Cursor::NewDescending(At(7), "");
    std::vector<std::string> seen;
    for (int page = 0; page < 5; ++page) {
        const agentenv::api::Paginated<Row> result =
            cursor.PaginateSorted(rows, Limit(1), CompareAgainstCursor(), RowToCursor{true});
        if (result.items.empty()) break;
        seen.push_back(result.items[0].id);
        if (!result.next_token.has_value()) break;
        cursor = Cursor::Parse(*result.next_token).value();
    }

    MT_EXPECT_EQ(seen.size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(seen[0], std::string("a"));
    MT_EXPECT_EQ(seen[1], std::string("b"));
    MT_EXPECT_EQ(seen[2], std::string("c"));
}

// ---------------------------------------------------------------------------
// snapshot version parsing
// ---------------------------------------------------------------------------

MT_TEST(parse_plain_semver) {
    // Rust: `parse_plain_semver`.
    MT_EXPECT_EQ(snapshot::ParseEnvdVersion("0.5.15\n").value(), std::string("0.5.15"));
}

MT_TEST(parse_version_with_prefix_words) {
    // Rust: `parse_version_with_prefix_words`. The banner word is skipped
    // because it does not look like a version.
    MT_EXPECT_EQ(snapshot::ParseEnvdVersion("envd version 0.5.15\n").value(),
                 std::string("0.5.15"));
}

MT_TEST(parse_version_with_v_prefix) {
    // Rust: `parse_version_with_v_prefix`.
    MT_EXPECT_EQ(snapshot::ParseEnvdVersion("envd v0.5.15").value(), std::string("0.5.15"));
}

MT_TEST(reject_unparseable_output) {
    // Rust: `reject_unparseable_output`.
    const agentenv::core::Expected<std::string, std::string> parsed =
        snapshot::ParseEnvdVersion("envd release");
    MT_EXPECT_TRUE(!parsed.ok());
    MT_EXPECT_TRUE(parsed.error().find("failed to parse envd version") != std::string::npos);
}

MT_TEST(parse_firecracker_version_with_prefix) {
    // Rust: `parse_firecracker_version_with_prefix`.
    MT_EXPECT_EQ(snapshot::ParseFirecrackerVersion("Firecracker v1.8.0").value(),
                 std::string("1.8.0"));
}

MT_TEST(parse_plain_kernel_version) {
    // Rust: `parse_plain_kernel_version`. A kernel release keeps its suffix,
    // so it is taken whole rather than tokenised.
    MT_EXPECT_EQ(snapshot::ParseKernelVersion("6.1.12-agentenv\n").value(),
                 std::string("6.1.12-agentenv"));
}

MT_TEST(reject_empty_kernel_version) {
    // Rust: `reject_empty_kernel_version`.
    MT_EXPECT_TRUE(!snapshot::ParseKernelVersion("\n").ok());
    MT_EXPECT_TRUE(!snapshot::ParseKernelVersion("   ").ok());
}

MT_TEST(normalize_version_token_requires_a_digit_and_a_dot) {
    MT_EXPECT_EQ(*snapshot::NormalizeVersionToken("1.2.3"), std::string("1.2.3"));
    MT_EXPECT_EQ(*snapshot::NormalizeVersionToken("v1.2"), std::string("1.2"));
    // A bare integer would otherwise make a build number look like a version.
    MT_EXPECT_TRUE(!snapshot::NormalizeVersionToken("2024").has_value());
    MT_EXPECT_TRUE(!snapshot::NormalizeVersionToken("release").has_value());
    MT_EXPECT_TRUE(!snapshot::NormalizeVersionToken("v").has_value());
    MT_EXPECT_TRUE(!snapshot::NormalizeVersionToken("").has_value());
    MT_EXPECT_TRUE(!snapshot::NormalizeVersionToken("version1.0").has_value());
}

MT_TEST(fallback_to_unknown_when_optional_probe_fails) {
    // Rust: `fallback_to_unknown_when_optional_probe_fails`. An optional
    // probe failing must not fail the whole snapshot.
    MT_EXPECT_EQ(snapshot::VersionOrUnknown(
                     "kernel", agentenv::core::make_unexpected(std::string("boom"))),
                 std::string("unknown"));
    MT_EXPECT_EQ(snapshot::VersionOrUnknown("kernel",
                                            agentenv::core::Expected<std::string, std::string>(
                                                std::string("6.1.0"))),
                 std::string("6.1.0"));
}

MT_TEST(runtime_versions_round_trip_and_default_the_tools_drive) {
    const snapshot::SnapshotRuntimeVersions versions =
        snapshot::SnapshotRuntimeVersions::New("6.1.0", "1.8.0", "0.5.15", "1.2.3");
    MT_EXPECT_TRUE(snapshot::SnapshotRuntimeVersions::FromJson(versions.ToJson()).value() ==
                   versions);

    // `#[serde(default)]`: a record written before the tools drive existed.
    const agentenv::core::Expected<Json, agentenv::core::AnyError> older = Json::Parse(
        "{\"kernel_version\":\"6.1.0\",\"firecracker_version\":\"1.8.0\","
        "\"envd_version\":\"0.5.15\"}");
    MT_EXPECT_TRUE(older.ok());
    const agentenv::core::Expected<snapshot::SnapshotRuntimeVersions, std::string> parsed =
        snapshot::SnapshotRuntimeVersions::FromJson(older.value());
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_EQ(parsed.value().tools_drive_version, std::string(""));

    MT_EXPECT_TRUE(!snapshot::SnapshotRuntimeVersions::FromJson(Json(JsonObject())).ok());
}

// ---------------------------------------------------------------------------
// OCI manifest
// ---------------------------------------------------------------------------

MT_TEST(overlaybd_layer_descriptor_carries_the_blob_annotations) {
    // The annotations are what let the overlaybd snapshotter find the blob
    // behind an ordinary tar layer media type.
    const snapshot::OciDescriptor descriptor =
        snapshot::OciDescriptor::OverlaybdLayer("sha256:abc", 4096);
    MT_EXPECT_EQ(descriptor.media_type, std::string(snapshot::kOciTarLayerMediaType));
    MT_EXPECT_EQ(descriptor.annotations.find(snapshot::kOverlaybdBlobDigestAnnotation)->second,
                 std::string("sha256:abc"));
    MT_EXPECT_EQ(descriptor.annotations.find(snapshot::kOverlaybdBlobSizeAnnotation)->second,
                 std::string("4096"));

    const snapshot::OciDescriptor config =
        snapshot::OciDescriptor::Config("sha256:def", 12);
    MT_EXPECT_EQ(config.media_type, std::string(snapshot::kOciImageConfigMediaType));
    MT_EXPECT_TRUE(config.annotations.empty());
}

MT_TEST(oci_descriptor_round_trips_and_omits_empty_annotations) {
    const snapshot::OciDescriptor config =
        snapshot::OciDescriptor::Config("sha256:def", 12);
    // The Json must outlive the reference: `as_object()` returns a reference
    // into it, and binding that to a temporary would dangle.
    const Json json = config.ToJson();
    const JsonObject& fields = json.as_object();
    // `skip_serializing_if = "BTreeMap::is_empty"`.
    MT_EXPECT_TRUE(fields.find("annotations") == fields.end());
    MT_EXPECT_TRUE(snapshot::OciDescriptor::FromJson(config.ToJson()).value() == config);

    const snapshot::OciDescriptor layer =
        snapshot::OciDescriptor::OverlaybdLayer("sha256:abc", 4096);
    MT_EXPECT_TRUE(snapshot::OciDescriptor::FromJson(layer.ToJson()).value() == layer);

    MT_EXPECT_TRUE(!snapshot::OciDescriptor::FromJson(Json(JsonObject())).ok());
}

MT_TEST(oci_config_blob_serializes_fields_in_rust_declaration_order) {
    // `serde` writes struct fields in declaration order, while a JSON object
    // would come out key-sorted. The blob is hashed, so this ordering is part
    // of the image's identity and must match Rust byte for byte.
    const snapshot::OciConfigBlob blob = snapshot::MinimalOciConfigBlob("amd64");

    const std::size_t created = blob.bytes.find("\"created\"");
    const std::size_t architecture = blob.bytes.find("\"architecture\"");
    const std::size_t os = blob.bytes.find("\"os\"");
    const std::size_t config = blob.bytes.find("\"config\"");
    const std::size_t rootfs = blob.bytes.find("\"rootfs\"");
    const std::size_t history = blob.bytes.find("\"history\"");

    MT_EXPECT_TRUE(created < architecture);
    MT_EXPECT_TRUE(architecture < os);
    MT_EXPECT_TRUE(os < config);
    MT_EXPECT_TRUE(config < rootfs);
    MT_EXPECT_TRUE(rootfs < history);

    // The exact minimal blob, so a change to any field is caught here.
    MT_EXPECT_EQ(blob.bytes,
                 std::string("{\"created\":\"1970-01-01T00:00:00Z\",\"architecture\":\"amd64\""
                             ",\"os\":\"linux\",\"config\":{\"Env\":[],\"WorkingDir\":\"\"}"
                             ",\"rootfs\":{\"type\":\"layers\",\"diff_ids\":[]}"
                             ",\"history\":[]}"));
    MT_EXPECT_EQ(blob.size, static_cast<uint64_t>(blob.bytes.size()));
    MT_EXPECT_EQ(blob.digest, agentenv::core::Sha256Digest(blob.bytes));
}

MT_TEST(config_blob_is_reproducible_for_equal_configuration) {
    // Two contexts built in a different order must hash identically, or every
    // publish would create a new config layer.
    snapshot::CommandContext first;
    first.env_vars["B"] = "2";
    first.env_vars["A"] = "1";
    first.workdir = "/work";

    snapshot::CommandContext second;
    second.env_vars["A"] = "1";
    second.env_vars["B"] = "2";
    second.workdir = "/work";

    MT_EXPECT_EQ(
        snapshot::SnapshotOciConfigBlob("amd64", first, Optional<Json>()).digest,
        snapshot::SnapshotOciConfigBlob("amd64", second, Optional<Json>()).digest);
}

MT_TEST(merged_runtime_config_sorts_env_and_sets_working_dir) {
    snapshot::CommandContext context;
    context.env_vars["PATH"] = "/usr/bin";
    context.env_vars["HOME"] = "/root";
    context.workdir = "/srv";

    const Json config = snapshot::MergedRuntimeConfig(context, Optional<Json>());
    const agentenv::core::JsonArray& env =
        config.as_object().find("Env")->second.as_array();
    MT_EXPECT_EQ(env.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(env[0].as_string(), std::string("HOME=/root"));
    MT_EXPECT_EQ(env[1].as_string(), std::string("PATH=/usr/bin"));
    MT_EXPECT_EQ(config.as_object().find("WorkingDir")->second.as_string(),
                 std::string("/srv"));
}

MT_TEST(merged_runtime_config_clears_inherited_fields_that_are_now_absent) {
    // `set_optional` removes rather than leaves: a base image's Entrypoint
    // must not survive into a snapshot that has none.
    JsonObject raw;
    raw["Entrypoint"] = Json(agentenv::core::JsonArray(1, Json("/old")));
    raw["User"] = Json("olduser");
    raw["Labels"] = Json(JsonObject());

    snapshot::CommandContext context;  // no user, entrypoint, or labels
    const Json config = snapshot::MergedRuntimeConfig(context, Optional<Json>(Json(raw)));

    MT_EXPECT_TRUE(config.as_object().find("Entrypoint") == config.as_object().end());
    MT_EXPECT_TRUE(config.as_object().find("User") == config.as_object().end());
    MT_EXPECT_TRUE(config.as_object().find("Labels") == config.as_object().end());
}

MT_TEST(merged_runtime_config_keeps_an_explicitly_empty_entrypoint) {
    // Absent and empty are distinct for OCI: an explicitly empty entrypoint
    // overrides the base image's.
    snapshot::CommandContext context;
    context.entrypoint = Optional<std::vector<std::string> >(std::vector<std::string>());

    const Json config = snapshot::MergedRuntimeConfig(context, Optional<Json>());
    const JsonObject::const_iterator entrypoint = config.as_object().find("Entrypoint");
    MT_EXPECT_TRUE(entrypoint != config.as_object().end());
    MT_EXPECT_EQ(entrypoint->second.as_array().size(), static_cast<std::size_t>(0));
}

MT_TEST(merged_runtime_config_renders_ports_and_volumes_as_object_maps) {
    snapshot::CommandContext context;
    context.exposed_ports.push_back("8080/tcp");
    context.volumes.push_back("/data");
    context.labels["a"] = "b";

    const Json config = snapshot::MergedRuntimeConfig(context, Optional<Json>());
    const JsonObject& ports = config.as_object().find("ExposedPorts")->second.as_object();
    MT_EXPECT_TRUE(ports.find("8080/tcp") != ports.end());
    // OCI wants an empty object as the value, not `true` or `null`.
    MT_EXPECT_TRUE(ports.find("8080/tcp")->second.kind() == Json::Kind::Object);
    MT_EXPECT_TRUE(ports.find("8080/tcp")->second.as_object().empty());

    MT_EXPECT_TRUE(config.as_object().find("Volumes")->second.as_object().find("/data") !=
                   config.as_object().find("Volumes")->second.as_object().end());
    MT_EXPECT_EQ(config.as_object().find("Labels")->second.as_object().find("a")->second.
                     as_string(),
                 std::string("b"));
}

MT_TEST(canonicalize_json_recurses_into_nested_values) {
    JsonObject inner;
    inner["z"] = Json("1");
    inner["a"] = Json("2");
    JsonObject outer;
    outer["nested"] = Json(inner);
    outer["list"] = Json(agentenv::core::JsonArray(1, Json(inner)));

    const Json canonical = snapshot::CanonicalizeJson(Json(outer));
    // `core::JsonObject` is already key-sorted; what matters is that the
    // recursion reaches nested objects and objects inside arrays.
    MT_EXPECT_TRUE(canonical.as_object().find("nested") != canonical.as_object().end());
    MT_EXPECT_EQ(canonical.ToString(), snapshot::CanonicalizeJson(canonical).ToString());
    MT_EXPECT_EQ(canonical.as_object().find("list")->second.as_array().size(),
                 static_cast<std::size_t>(1));
}

MT_TEST(build_oci_image_manifest_follows_the_rust_field_order) {
    const snapshot::OciDescriptor config =
        snapshot::OciDescriptor::Config("sha256:cfg", 10);
    std::vector<snapshot::OciDescriptor> layers;
    layers.push_back(snapshot::OciDescriptor::OverlaybdLayer("sha256:layer", 20));

    const std::string manifest =
        snapshot::BuildOciImageManifest(config, layers, "snapshot-1");

    const std::size_t schema = manifest.find("\"schemaVersion\"");
    const std::size_t media = manifest.find("\"mediaType\"");
    const std::size_t config_pos = manifest.find("\"config\"");
    const std::size_t layers_pos = manifest.find("\"layers\"");
    const std::size_t annotations = manifest.find("\"annotations\"");
    MT_EXPECT_TRUE(schema < media);
    MT_EXPECT_TRUE(media < config_pos);
    MT_EXPECT_TRUE(config_pos < layers_pos);
    MT_EXPECT_TRUE(layers_pos < annotations);

    MT_EXPECT_TRUE(manifest.find("\"schemaVersion\":2") != std::string::npos);
    MT_EXPECT_TRUE(manifest.find(snapshot::kOciImageManifestMediaType) != std::string::npos);
    MT_EXPECT_TRUE(manifest.find(snapshot::kSnapshotTagAnnotation) != std::string::npos);
    MT_EXPECT_TRUE(manifest.find("snapshot-1") != std::string::npos);

    // It must still be valid JSON despite being written by hand.
    MT_EXPECT_TRUE(Json::Parse(manifest).ok());
}

MT_TEST(host_architecture_maps_to_an_oci_name) {
    const std::string architecture = snapshot::HostArchitectureForOci();
    MT_EXPECT_TRUE(architecture == "amd64" || architecture == "arm64" ||
                   architecture == "arm" || architecture == "riscv64" ||
                   architecture == "unknown");
}

int main() { return microtest::RunAll(); }
