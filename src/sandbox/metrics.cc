// SPDX-License-Identifier: MIT
// Rust: src/sandbox/metrics.rs
#include "agentenv/sandbox/metrics.h"

#include <cmath>

namespace agentenv {
namespace sandbox {

namespace {

/// envd's generated model types every field as `Option`, so "absent" and
/// "present" are the distinction that matters here; the *kind* check is what
/// stops a `null` or a string from being read as a number.
core::Expected<int64_t, std::string> RequireInt(const core::Json& object,
                                                const std::string& key) {
    const core::JsonObject& fields = object.as_object();
    const core::JsonObject::const_iterator at = fields.find(key);
    if (at == fields.end() || at->second.kind() == core::Json::Kind::Null) {
        return core::make_unexpected("missing " + key);
    }
    if (at->second.kind() == core::Json::Kind::Int) return at->second.as_int();
    // A whole-valued double is accepted because JSON has one number type and
    // a serialiser may well emit `4294967296.0`; a fractional one is not, as
    // silently flooring it would under-report a byte count.
    if (at->second.kind() == core::Json::Kind::Double) {
        const double value = at->second.as_double();
        if (value == std::floor(value) && std::isfinite(value)) {
            return static_cast<int64_t>(value);
        }
    }
    return core::make_unexpected("missing " + key);
}

core::Expected<double, std::string> RequireNumber(const core::Json& object,
                                                  const std::string& key) {
    const core::JsonObject& fields = object.as_object();
    const core::JsonObject::const_iterator at = fields.find(key);
    if (at == fields.end() || at->second.kind() == core::Json::Kind::Null) {
        return core::make_unexpected("missing " + key);
    }
    if (at->second.kind() == core::Json::Kind::Int) {
        return static_cast<double>(at->second.as_int());
    }
    if (at->second.kind() == core::Json::Kind::Double) return at->second.as_double();
    return core::make_unexpected("missing " + key);
}

}  // namespace

core::Expected<SandboxMetric, std::string> SandboxMetric::FromEnvdJson(const core::Json& raw) {
    if (raw.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("missing envd timestamp"));
    }

    const core::Expected<int64_t, std::string> ts = RequireInt(raw, "ts");
    if (!ts.ok()) return core::make_unexpected(std::string("missing envd timestamp"));
    // Checked before the conversion, not after: a negative `ts` is a broken
    // guest clock, and admitting it would place the sample before the epoch
    // where every `start`/`end` history filter silently excludes it.
    if (ts.value() < 0) return core::make_unexpected(std::string("negative envd timestamp"));

    const int64_t kNanosPerSecond = 1000000000LL;
    // `ts` is in seconds; reject what cannot be scaled to nanoseconds rather
    // than wrapping into a negative instant.
    if (ts.value() > 0x7FFFFFFFFFFFFFFFLL / kNanosPerSecond) {
        return core::make_unexpected(std::string("invalid envd timestamp"));
    }

    const core::Expected<int64_t, std::string> cpu_count = RequireInt(raw, "cpu_count");
    if (!cpu_count.ok()) return core::make_unexpected(cpu_count.error());
    const core::Expected<double, std::string> cpu_used_pct = RequireNumber(raw, "cpu_used_pct");
    if (!cpu_used_pct.ok()) return core::make_unexpected(cpu_used_pct.error());
    const core::Expected<int64_t, std::string> mem_used = RequireInt(raw, "mem_used");
    if (!mem_used.ok()) return core::make_unexpected(mem_used.error());
    const core::Expected<int64_t, std::string> mem_total = RequireInt(raw, "mem_total");
    if (!mem_total.ok()) return core::make_unexpected(mem_total.error());
    const core::Expected<int64_t, std::string> mem_cache = RequireInt(raw, "mem_cache");
    if (!mem_cache.ok()) return core::make_unexpected(mem_cache.error());
    const core::Expected<int64_t, std::string> disk_used = RequireInt(raw, "disk_used");
    if (!disk_used.ok()) return core::make_unexpected(disk_used.error());
    const core::Expected<int64_t, std::string> disk_total = RequireInt(raw, "disk_total");
    if (!disk_total.ok()) return core::make_unexpected(disk_total.error());

    SandboxMetric sample;
    sample.timestamp = core::SystemTime(ts.value() * kNanosPerSecond);
    sample.cpu_count = static_cast<int32_t>(cpu_count.value());
    sample.cpu_used_pct = cpu_used_pct.value();
    sample.mem_used = mem_used.value();
    sample.mem_total = mem_total.value();
    sample.mem_cache = mem_cache.value();
    sample.disk_used = disk_used.value();
    sample.disk_total = disk_total.value();

    // `cpu_count > 0`: a zero would make any "percent per core" the client
    // computes a division by zero, and a guest with no CPUs is not a state
    // worth reporting as a successful sample.
    //
    // `is_finite`: a NaN or infinity survives JSON as a number in some
    // encoders and then propagates through every average a client takes, so it
    // is rejected at the boundary rather than stored. NaN also compares false
    // against every bound, so a stored NaN would be invisible to range checks
    // forever after.
    if (!(sample.cpu_count > 0) ||
        static_cast<int64_t>(sample.cpu_count) != cpu_count.value() ||
        !std::isfinite(sample.cpu_used_pct) || sample.cpu_used_pct < 0.0) {
        return core::make_unexpected(std::string("invalid guest CPU metrics"));
    }

    // One message for all five, as upstream: they are the same class of
    // nonsense and a negative byte count is never partially meaningful.
    if (sample.mem_used < 0 || sample.mem_total < 0 || sample.mem_cache < 0 ||
        sample.disk_used < 0 || sample.disk_total < 0) {
        return core::make_unexpected(std::string("negative guest byte count"));
    }

    return sample;
}

core::Json SandboxMetric::ToJson() const {
    core::JsonObject out;
    // Nine fields, all required by the schema. The deprecated string form is
    // kept alongside the Unix one because clients that have not migrated still
    // read it.
    out["timestamp"] = core::Json(core::FormatRfc3339Secs(timestamp));
    out["timestampUnix"] = core::Json(static_cast<int64_t>(timestamp.unix_nanos / 1000000000LL));
    out["cpuCount"] = core::Json(static_cast<int64_t>(cpu_count));
    out["cpuUsedPct"] = core::Json(cpu_used_pct);
    out["memUsed"] = core::Json(mem_used);
    out["memTotal"] = core::Json(mem_total);
    out["memCache"] = core::Json(mem_cache);
    out["diskUsed"] = core::Json(disk_used);
    out["diskTotal"] = core::Json(disk_total);
    return core::Json(out);
}

}  // namespace sandbox
}  // namespace agentenv
