// SPDX-License-Identifier: MIT
// Rust: src/sandbox/metrics.rs + src/orchestrator/sandbox_metrics.rs.
//
// The sampling task (`start_metrics_task` / `collect_sandbox_metrics`) is not
// ported -- it needs `SandboxBackend::Envd()`, which the C++ trait does not
// have. What is asserted here is everything that task *decides with*: the
// validation at the envd boundary, the retention cache's two clocks, and the
// generation rule that stops a dead incarnation's numbers being served as
// current.
#include "agentenv/orchestrator/sandbox_metrics.h"

#include <string>
#include <vector>

#include "agentenv/sandbox/metrics.h"
#include "microtest.h"

using namespace agentenv;                // NOLINT
using namespace agentenv::sandbox;       // NOLINT
using namespace agentenv::orchestrator;  // NOLINT

namespace {

const char* kGood =
    "{\"ts\":1700000000,\"cpu_count\":2,\"cpu_used_pct\":50.0,"
    "\"mem_used\":4294967296,\"mem_total\":8589934592,"
    "\"mem_cache\":3221225472,\"disk_used\":17179869184,"
    "\"disk_total\":34359738368}";

core::Json Parse(const std::string& text) {
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(text);
    MT_EXPECT_TRUE(json.ok());
    return json.ok() ? json.value() : core::Json();
}

core::Expected<SandboxMetric, std::string> Decode(const std::string& text) {
    return SandboxMetric::FromEnvdJson(Parse(text));
}

/// `kGood` with one field replaced, so each rejection test differs from the
/// accepted body in exactly one place.
std::string With(const std::string& key, const std::string& value) {
    std::string body = kGood;
    const std::string needle = "\"" + key + "\":";
    const std::string::size_type at = body.find(needle);
    MT_EXPECT_TRUE(at != std::string::npos);
    std::string::size_type end = body.find_first_of(",}", at + needle.size());
    return body.substr(0, at + needle.size()) + value + body.substr(end);
}

std::string Without(const std::string& key) {
    std::string body = kGood;
    const std::string needle = "\"" + key + "\":";
    const std::string::size_type at = body.find(needle);
    MT_EXPECT_TRUE(at != std::string::npos);
    std::string::size_type end = body.find_first_of(",}", at + needle.size());
    std::string::size_type begin = at;
    if (body[end] == ',') {
        // A field with a successor: take its trailing comma with it.
        ++end;
    } else {
        // The last field: take the *preceding* comma instead, or the result is
        // `{..,}` and the parse failure would look like a decoder bug.
        const std::string::size_type comma = body.rfind(',', at);
        if (comma != std::string::npos) begin = comma;
    }
    return body.substr(0, begin) + body.substr(end);
}

SandboxMetric Sample(int64_t ts_secs) {
    SandboxMetric sample;
    sample.timestamp = core::SystemTime(ts_secs * 1000000000LL);
    sample.cpu_count = 1;
    sample.mem_used = ts_secs;
    return sample;
}

core::SandboxId Sid(const char* text) {
    const core::Expected<core::SandboxId, std::string> id = core::SandboxId::Parse(text);
    MT_EXPECT_TRUE(id.ok());
    return id.ok() ? id.value() : core::SandboxId();
}

const char* kIdA = "aaaaaaaa-0000-0000-0000-000000000001";
const char* kIdB = "bbbbbbbb-0000-0000-0000-000000000002";

core::Optional<int64_t> Bound(int64_t value) { return core::Optional<int64_t>(value); }
core::Optional<int64_t> NoBound() { return core::Optional<int64_t>(); }

const int64_t kSecond = 1000000000LL;

}  // namespace

// ---- the envd boundary ------------------------------------------------------

MT_TEST(sandbox_metrics_decode_large_byte_counts_and_reject_missing_values) {
    // Upstream, verbatim: the byte counts exceed 32 bits, so a port that
    // decoded them as `int` would silently truncate an 8 GiB total.
    const core::Expected<SandboxMetric, std::string> sample = Decode(kGood);
    MT_EXPECT_TRUE(sample.ok());
    MT_EXPECT_TRUE(sample.value().mem_total == 8589934592LL);
    MT_EXPECT_TRUE(sample.value().disk_total == 34359738368LL);

    const core::Json response = sample.value().ToJson();
    // Nine fields exactly -- the schema requires all of them, and an extra
    // would be a field a client was never told to expect.
    MT_EXPECT_EQ(response.as_object().size(), static_cast<std::size_t>(9));
    MT_EXPECT_TRUE(response.as_object().find("timestampUnix")->second.as_int() == 1700000000LL);
    // Seconds precision, not nine digits of false accuracy: envd's `ts` is
    // whole seconds.
    MT_EXPECT_TRUE(response.as_object().find("timestamp")->second.as_string() ==
                   "2023-11-14T22:13:20Z");
    MT_EXPECT_TRUE(response.as_object().find("memTotal")->second.as_int() == 8589934592LL);
    MT_EXPECT_TRUE(response.as_object().find("diskTotal")->second.as_int() == 34359738368LL);

    // Upstream drops `mem_cache` specifically; every field is checked below.
    MT_EXPECT_TRUE(!Decode(Without("mem_cache")).ok());
}

MT_TEST(every_field_is_required_rather_than_defaulted) {
    // envd's generated model types them all as `Option`, so an omitted field
    // deserialises happily and would reach a client as a plausible `0` -- a
    // sandbox reported as using no memory rather than as unmeasured.
    const char* const fields[] = {"ts",        "cpu_count", "cpu_used_pct", "mem_used",
                                  "mem_total", "mem_cache", "disk_used",    "disk_total"};
    for (std::size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        MT_EXPECT_TRUE(!Decode(Without(fields[i])).ok());
        // An explicit null is the same as absent, which is what `Option`
        // deserialises it to.
        MT_EXPECT_TRUE(!Decode(With(fields[i], "null")).ok());
        // And a string where a number belongs is not coerced.
        MT_EXPECT_TRUE(!Decode(With(fields[i], "\"7\"")).ok());
    }
}

MT_TEST(a_broken_guest_clock_is_rejected_not_stored) {
    // A negative `ts` would place the sample before the epoch, where every
    // `start`/`end` history filter silently excludes it -- so the sample would
    // be retained and invisible.
    const core::Expected<SandboxMetric, std::string> negative = Decode(With("ts", "-1"));
    MT_EXPECT_TRUE(!negative.ok());
    MT_EXPECT_TRUE(negative.error() == "negative envd timestamp");

    // Zero is a legitimate instant, not an error.
    MT_EXPECT_TRUE(Decode(With("ts", "0")).ok());

    // A `ts` that cannot be scaled to nanoseconds is refused rather than
    // wrapped into a negative instant.
    MT_EXPECT_TRUE(!Decode(With("ts", "9223372036854775807")).ok());
    MT_EXPECT_TRUE(Decode(With("ts", "9223372035")).ok());
}

MT_TEST(cpu_metrics_must_be_usable_arithmetic) {
    // `cpu_count > 0`: a client computing "percent per core" would divide by
    // zero, and a guest with no CPUs is not a successful sample.
    const core::Expected<SandboxMetric, std::string> zero = Decode(With("cpu_count", "0"));
    MT_EXPECT_TRUE(!zero.ok());
    MT_EXPECT_TRUE(zero.error() == "invalid guest CPU metrics");
    MT_EXPECT_TRUE(!Decode(With("cpu_count", "-4")).ok());

    // A negative percentage is nonsense; zero is idle.
    MT_EXPECT_TRUE(!Decode(With("cpu_used_pct", "-0.1")).ok());
    MT_EXPECT_TRUE(Decode(With("cpu_used_pct", "0")).ok());
    // Over 100 is allowed: with several cores the guest's own accounting can
    // legitimately exceed it, and clamping would hide real load.
    MT_EXPECT_TRUE(Decode(With("cpu_used_pct", "180.5")).ok());

    // A `cpu_count` past `int32` is refused rather than narrowed -- a wrapped
    // value could land back in the positive range and pass the check.
    MT_EXPECT_TRUE(!Decode(With("cpu_count", "4294967296")).ok());
}

MT_TEST(negative_byte_counts_are_rejected_with_one_message) {
    const char* const counts[] = {"mem_used", "mem_total", "mem_cache", "disk_used",
                                  "disk_total"};
    for (std::size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        const core::Expected<SandboxMetric, std::string> sample = Decode(With(counts[i], "-1"));
        MT_EXPECT_TRUE(!sample.ok());
        MT_EXPECT_TRUE(sample.error() == "negative guest byte count");
        // Zero is a real reading.
        MT_EXPECT_TRUE(Decode(With(counts[i], "0")).ok());
    }
}

MT_TEST(a_whole_valued_double_is_accepted_but_a_fractional_byte_count_is_not) {
    // JSON has one number type, so a serialiser may well emit `4294967296.0`.
    const core::Expected<SandboxMetric, std::string> whole =
        Decode(With("mem_used", "4294967296.0"));
    MT_EXPECT_TRUE(whole.ok());
    MT_EXPECT_TRUE(whole.value().mem_used == 4294967296LL);
    // Flooring a fractional byte count would under-report it silently.
    MT_EXPECT_TRUE(!Decode(With("mem_used", "1.5")).ok());
}

// ---- the retention cache ----------------------------------------------------

MT_TEST(metrics_concurrency_rounds_up_and_never_returns_zero_for_work) {
    // Rounding up matters because upstream's `for_each_concurrent(0, ..)`
    // means *unbounded*, not none -- a down-rounded 4 sandboxes would open
    // every guest connection at once.
    MT_EXPECT_EQ(MetricsConcurrency(0), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(MetricsConcurrency(1), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(MetricsConcurrency(4), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(MetricsConcurrency(5), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(MetricsConcurrency(6), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(MetricsConcurrency(100), static_cast<std::size_t>(20));
}

MT_TEST(retention_is_measured_on_the_host_clock_not_the_guests) {
    // The two clocks are the whole reason `collected_at` is stored separately
    // from `metric.timestamp`. Here the guest's clock is absurd (year 1970)
    // while the host has only just collected the samples: they must survive.
    SandboxMetricsCache cache;
    const core::SandboxId id = Sid(kIdA);
    cache.Insert(id, 1, 1000 * kSecond, Sample(0));
    cache.Insert(id, 1, 1001 * kSecond, Sample(1));

    cache.Prune(1002 * kSecond, 60 * kSecond);
    MT_EXPECT_EQ(cache.History(id, NoBound(), NoBound()).size(), static_cast<std::size_t>(2));

    // And conversely: a guest claiming "now" does not keep a sample the host
    // has held past retention.
    SandboxMetricsCache future;
    SandboxMetric ahead = Sample(0);
    ahead.timestamp = core::SystemTime(2000000000LL * kSecond);
    future.Insert(id, 1, 0, ahead);
    future.Prune(120 * kSecond, 60 * kSecond);
    MT_EXPECT_TRUE(future.History(id, NoBound(), NoBound()).empty());
}

MT_TEST(pruning_forgets_the_sandbox_entirely_rather_than_leaving_an_empty_entry) {
    // Without this the map grows one key per sandbox ever seen -- a leak that
    // scales with process lifetime rather than with load.
    SandboxMetricsCache cache;
    cache.Insert(Sid(kIdA), 1, 0, Sample(1));
    cache.Insert(Sid(kIdB), 1, 50 * kSecond, Sample(2));
    MT_EXPECT_EQ(cache.TrackedSandboxesForTest(), static_cast<std::size_t>(2));

    cache.Prune(60 * kSecond, 60 * kSecond);
    MT_EXPECT_EQ(cache.TrackedSandboxesForTest(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(cache.History(Sid(kIdA), NoBound(), NoBound()).empty());
    MT_EXPECT_EQ(cache.History(Sid(kIdB), NoBound(), NoBound()).size(),
                 static_cast<std::size_t>(1));

    cache.Prune(200 * kSecond, 60 * kSecond);
    MT_EXPECT_EQ(cache.TrackedSandboxesForTest(), static_cast<std::size_t>(0));
}

MT_TEST(the_retention_edge_is_half_open) {
    // `>=`: a sample exactly at the edge is already expired, so the window a
    // client sees does not flicker by one sample with the sweep's timing.
    SandboxMetricsCache cache;
    const core::SandboxId id = Sid(kIdA);
    cache.Insert(id, 1, 0, Sample(1));
    cache.Prune(60 * kSecond, 60 * kSecond);
    MT_EXPECT_TRUE(cache.History(id, NoBound(), NoBound()).empty());

    SandboxMetricsCache inside;
    inside.Insert(id, 1, 1, Sample(1));
    inside.Prune(60 * kSecond, 60 * kSecond);
    MT_EXPECT_EQ(inside.History(id, NoBound(), NoBound()).size(), static_cast<std::size_t>(1));
}

MT_TEST(history_filters_on_the_guests_seconds_and_is_inclusive) {
    SandboxMetricsCache cache;
    const core::SandboxId id = Sid(kIdA);
    for (int64_t ts = 100; ts <= 104; ++ts) {
        cache.Insert(id, 1, ts * kSecond, Sample(ts));
    }

    MT_EXPECT_EQ(cache.History(id, NoBound(), NoBound()).size(), static_cast<std::size_t>(5));
    // Both bounds inclusive -- a client asking for [101, 103] means those
    // seconds, and excluding them would drop an entry per page boundary.
    const std::vector<SandboxMetric> window = cache.History(id, Bound(101), Bound(103));
    MT_EXPECT_EQ(window.size(), static_cast<std::size_t>(3));
    MT_EXPECT_TRUE(window[0].timestamp.unix_nanos == 101 * kSecond);
    MT_EXPECT_TRUE(window[2].timestamp.unix_nanos == 103 * kSecond);

    MT_EXPECT_EQ(cache.History(id, Bound(104), NoBound()).size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(cache.History(id, NoBound(), Bound(100)).size(), static_cast<std::size_t>(1));
    // An inverted range is empty rather than an error: it is a client's
    // mistake, not a server fault.
    MT_EXPECT_TRUE(cache.History(id, Bound(103), Bound(101)).empty());
    // An unknown sandbox is an empty history -- "no samples yet" is normal
    // for one that has just started.
    MT_EXPECT_TRUE(cache.History(Sid(kIdB), NoBound(), NoBound()).empty());
}

MT_TEST(history_is_ordered_by_the_guests_timestamp_even_if_it_stepped_back) {
    // Insertion order is collection order; a guest whose clock jumped
    // backwards would otherwise hand a client a non-monotonic series.
    SandboxMetricsCache cache;
    const core::SandboxId id = Sid(kIdA);
    cache.Insert(id, 1, 1 * kSecond, Sample(300));
    cache.Insert(id, 1, 2 * kSecond, Sample(100));
    cache.Insert(id, 1, 3 * kSecond, Sample(200));

    const std::vector<SandboxMetric> history = cache.History(id, NoBound(), NoBound());
    MT_EXPECT_EQ(history.size(), static_cast<std::size_t>(3));
    MT_EXPECT_TRUE(history[0].timestamp.unix_nanos == 100 * kSecond);
    MT_EXPECT_TRUE(history[1].timestamp.unix_nanos == 200 * kSecond);
    MT_EXPECT_TRUE(history[2].timestamp.unix_nanos == 300 * kSecond);

    // Samples sharing a second keep collection order (stable sort), so a
    // repeated second is not reshuffled between two identical requests.
    SandboxMetricsCache tied;
    SandboxMetric first = Sample(5);
    first.mem_used = 111;
    SandboxMetric second = Sample(5);
    second.mem_used = 222;
    tied.Insert(id, 1, 1, first);
    tied.Insert(id, 1, 2, second);
    const std::vector<SandboxMetric> both = tied.History(id, NoBound(), NoBound());
    MT_EXPECT_EQ(both.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(both[0].mem_used == 111);
    MT_EXPECT_TRUE(both[1].mem_used == 222);
}

// ---- the generation rule ----------------------------------------------------

MT_TEST(latest_refuses_to_serve_a_dead_incarnations_sample_as_current) {
    // A fork or resume bumps the proxy route's version. Reporting the previous
    // incarnation's final sample as current would show an operator stale
    // numbers with nothing marking them stale, so the honest answer is none
    // until the new incarnation has been sampled.
    SandboxMetricsCache cache;
    const core::SandboxId id = Sid(kIdA);
    cache.Insert(id, 7, 1 * kSecond, Sample(100));

    MT_EXPECT_TRUE(cache.Latest(id, 7).has_value());
    MT_EXPECT_TRUE(cache.Latest(id, 7)->mem_used == 100);
    MT_EXPECT_TRUE(!cache.Latest(id, 8).has_value());
    MT_EXPECT_TRUE(!cache.Latest(id, 6).has_value());
    MT_EXPECT_TRUE(!cache.Latest(Sid(kIdB), 7).has_value());

    // Once the new incarnation is sampled, it is served -- and the old sample
    // stays in `History`, which is about what happened rather than what is
    // current.
    cache.Insert(id, 8, 2 * kSecond, Sample(200));
    MT_EXPECT_TRUE(cache.Latest(id, 8).has_value());
    MT_EXPECT_TRUE(cache.Latest(id, 8)->mem_used == 200);
    MT_EXPECT_TRUE(!cache.Latest(id, 7).has_value());
    MT_EXPECT_EQ(cache.History(id, NoBound(), NoBound()).size(), static_cast<std::size_t>(2));
}

MT_TEST(latest_is_newest_by_insertion_not_by_timestamp) {
    // Deliberately unlike `History`: "current" means the most recently
    // collected sample. Picking the largest guest timestamp instead would let
    // a guest with a fast clock pin `Latest` to an old reading forever.
    SandboxMetricsCache cache;
    const core::SandboxId id = Sid(kIdA);
    cache.Insert(id, 1, 1 * kSecond, Sample(9999));
    cache.Insert(id, 1, 2 * kSecond, Sample(5));
    MT_EXPECT_TRUE(cache.Latest(id, 1)->mem_used == 5);
}

MT_TEST(has_generation_tracks_only_the_newest_sample) {
    // It exists to skip an event-triggered sample when the periodic sweep
    // already took one for this incarnation, so a burst of create/resume
    // events does not multiply guest calls.
    SandboxMetricsCache cache;
    const core::SandboxId id = Sid(kIdA);
    MT_EXPECT_TRUE(!cache.HasGeneration(id, 1));

    cache.Insert(id, 1, 1 * kSecond, Sample(1));
    MT_EXPECT_TRUE(cache.HasGeneration(id, 1));
    MT_EXPECT_TRUE(!cache.HasGeneration(id, 2));

    // After the generation moves on, the older one is no longer "sampled" --
    // the new incarnation still needs its own sample.
    cache.Insert(id, 2, 2 * kSecond, Sample(2));
    MT_EXPECT_TRUE(cache.HasGeneration(id, 2));
    MT_EXPECT_TRUE(!cache.HasGeneration(id, 1));

    // And pruning the history away leaves nothing claiming to be sampled.
    cache.Prune(1000 * kSecond, 1 * kSecond);
    MT_EXPECT_TRUE(!cache.HasGeneration(id, 2));
}

MT_MAIN
