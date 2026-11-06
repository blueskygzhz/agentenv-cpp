// SPDX-License-Identifier: MIT
// Rust: src/sandbox/metrics.rs
//
// Guest resource samples. These are *not* host process RSS or allocation
// counters, which is the first line of the upstream module and worth keeping:
// the numbers describe what the workload inside the VM sees, so an operator
// comparing them against the host's own metrics should expect them to differ.
#ifndef AGENTENV_SANDBOX_METRICS_H_
#define AGENTENV_SANDBOX_METRICS_H_

#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/time.h"

namespace agentenv {
namespace sandbox {

/// Rust `struct SandboxMetric`.
struct SandboxMetric {
    /// The guest's wall clock at sampling time, to second precision — envd
    /// reports `ts` in whole seconds, so there is nothing finer to keep.
    core::SystemTime timestamp;
    int32_t          cpu_count = 0;
    /// `f32` upstream. Kept as `double` here because C++ has no reason to
    /// narrow it: the value is parsed from JSON as a double and re-serialised
    /// as one, and a round trip through `float` would perturb the digits a
    /// client sees for no gain.
    double  cpu_used_pct = 0.0;
    int64_t mem_used = 0;
    int64_t mem_total = 0;
    int64_t mem_cache = 0;
    int64_t disk_used = 0;
    int64_t disk_total = 0;

    /// Rust `impl TryFrom<Metrics> for SandboxMetric` — decodes envd's
    /// `/metrics` body and validates it.
    ///
    /// Every field is required. envd's generated model has them all as
    /// `Option`, so a response that omits one deserialises happily and would
    /// otherwise reach a client as a plausible `0` — a sandbox reported as
    /// using no memory rather than as unmeasured. Hence `missing ...` is an
    /// error, not a default.
    static core::Expected<SandboxMetric, std::string> FromEnvdJson(const core::Json& raw);

    /// Rust `impl From<SandboxMetric> for models::SandboxMetric` — the API
    /// response, nine fields.
    ///
    /// `timestamp` and `timestampUnix` carry the same instant twice: the
    /// string form is `deprecated: true` in the OpenAPI schema but still
    /// required, so dropping it would break clients that have not migrated.
    core::Json ToJson() const;

    bool operator==(const SandboxMetric& o) const {
        return timestamp == o.timestamp && cpu_count == o.cpu_count &&
               cpu_used_pct == o.cpu_used_pct && mem_used == o.mem_used &&
               mem_total == o.mem_total && mem_cache == o.mem_cache &&
               disk_used == o.disk_used && disk_total == o.disk_total;
    }
    bool operator!=(const SandboxMetric& o) const { return !(*this == o); }
};

}  // namespace sandbox
}  // namespace agentenv

#endif  // AGENTENV_SANDBOX_METRICS_H_
