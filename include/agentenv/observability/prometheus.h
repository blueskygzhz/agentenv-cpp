// SPDX-License-Identifier: MIT
// Rust: src/observability/prometheus.rs — metric label normalization + RAII
// latency guards.
//
// The label helpers are the part that must not drift: they bound metric
// cardinality, so an ID leaking into a label name is a production problem.
// They are pure functions and are ported verbatim.
#ifndef AGENTENV_OBSERVABILITY_PROMETHEUS_H_
#define AGENTENV_OBSERVABILITY_PROMETHEUS_H_

#include <cstdint>
#include <string>

namespace agentenv {
namespace observability {

// ---- metric names (Rust string literals, kept in one place) ----

/// Rust `"agentenv_http_request_duration_seconds"`.
extern const char* const kHttpRequestDurationSeconds;
/// Rust `"agentenv_sandbox_stage_duration_seconds"`.
extern const char* const kSandboxStageDurationSeconds;
/// Rust `"agentenv_sandbox_stage_inflight"`.
extern const char* const kSandboxStageInflight;

/// Rust `enum HttpRouteSource` — how the gateway matched the request, recorded
/// as the `route_source` label. Default is `ControlPlane` (Rust
/// `unwrap_or(HttpRouteSource::ControlPlane)`).
enum class HttpRouteSource {
    ControlPlane,
    ProxyHost,
    ProxyHeader,
    ProxyPrefix,
};

/// Rust `HttpRouteSource::as_str`.
const char* HttpRouteSourceAsStr(HttpRouteSource source);

/// Rust `result_status` — "ok" / "error".
const char* ResultStatus(bool ok);

/// Rust `http_method_label` — a closed set; anything unknown becomes "OTHER"
/// so a hostile client cannot inflate label cardinality.
const char* HttpMethodLabel(const std::string& method);

/// Rust `http_status_label` — bucketed to "1xx".."5xx", else "other".
const char* HttpStatusLabel(uint16_t status);

/// Rust `http_route_label` — maps a concrete request path onto a bounded set of
/// route templates. Trailing slashes are trimmed (an all-slash path becomes
/// "/"), the static table is consulted first, then `dynamic_route_label`.
/// Unrecognized paths collapse to "unmatched".
const char* HttpRouteLabel(const std::string& path);

/// Rust `MetricGuardLabel` enum — the label shape a guard will emit.
class MetricGuard {
 public:
    /// Rust `MetricGuard::operation`.
    static MetricGuard Operation(const char* metric, const char* operation);
    /// Rust `MetricGuard::operation_artifact` — adds an `artifact` dimension.
    static MetricGuard OperationArtifact(const char* metric, const char* operation,
                                         const char* artifact);
    /// Rust `MetricGuard::stage`.
    static MetricGuard Stage(const char* metric, const char* stage);

    /// Rust `finish(&Result<T, E>)` — records with status ok/error.
    void Finish(bool ok);

    /// Rust `impl Drop` — a guard dropped before `finish()` records
    /// status="canceled". This distinguishes a caller-side timeout from an
    /// operation that ran to completion and returned an error.
    ~MetricGuard();

    /// Test hook: the status this guard did record (empty until recorded).
    const char* recorded_status() const { return recorded_ ? status_ : ""; }

 private:
    enum class LabelKind { Operation, OperationArtifact, Stage };

    MetricGuard(const char* metric, LabelKind kind, const char* operation,
                const char* artifact, const char* stage);
    void Record();

    const char* metric_;
    LabelKind   kind_;
    const char* operation_;
    const char* artifact_;
    const char* stage_;
    int64_t     start_ns_;
    const char* status_;
    bool        recorded_;
};

/// Rust `SandboxStageInFlight` — increments the inflight gauge on construction
/// and decrements it on drop, so the gauge is balanced even on early return.
class SandboxStageInFlight {
 public:
    SandboxStageInFlight(const char* operation, const char* stage);
    ~SandboxStageInFlight();

 private:
    const char* operation_;
    const char* stage_;
};

/// Rust `SandboxStageTimer` — wraps one stage of a multi-stage sandbox
/// operation: holds the inflight gauge for the duration and records the
/// stage latency histogram with an ok/error status.
class SandboxStageTimer {
 public:
    explicit SandboxStageTimer(const char* operation) : operation_(operation) {}

    /// Rust `time(stage, future)`. C++11 has no `async fn`, so the caller's
    /// work is passed as a callable returning "did it succeed".
    template <typename Fn>
    bool Time(const char* stage, Fn fn) {
        SandboxStageInFlight inflight(operation_, stage);
        MetricGuard guard = MetricGuard::Stage(kSandboxStageDurationSeconds, stage);
        bool ok = fn();
        guard.Finish(ok);
        return ok;
    }

    const char* operation() const { return operation_; }

 private:
    const char* operation_;
};

}  // namespace observability
}  // namespace agentenv
#endif  // AGENTENV_OBSERVABILITY_PROMETHEUS_H_
