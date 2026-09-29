// SPDX-License-Identifier: MIT
// Rust: src/observability/prometheus.rs
#include "agentenv/observability/prometheus.h"

#include <time.h>

#include <vector>

#include "agentenv/observability/metrics.h"

namespace agentenv {
namespace observability {

const char* const kHttpRequestDurationSeconds = "agentenv_http_request_duration_seconds";
const char* const kSandboxStageDurationSeconds = "agentenv_sandbox_stage_duration_seconds";
const char* const kSandboxStageInflight = "agentenv_sandbox_stage_inflight";

namespace {

int64_t NowNanos() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

/// Rust `start.elapsed().as_secs_f64()`.
double ElapsedSecs(int64_t start_ns) {
    return static_cast<double>(NowNanos() - start_ns) / 1e9;
}

/// The `metrics!` macro takes name + label pairs. The C++ facade is
/// label-free, so labels are folded into the series name in a stable
/// `name{k="v",..}` form — the standard Prometheus text rendering.
std::string WithLabels(const char* metric,
                       const std::vector<std::pair<const char*, const char*> >& labels) {
    std::string out(metric);
    if (labels.empty()) return out;
    out += "{";
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i) out += ",";
        out += labels[i].first;
        out += "=\"";
        out += labels[i].second;
        out += "\"";
    }
    out += "}";
    return out;
}

/// Rust `dynamic_route_label` — matches on the first five path segments.
const char* DynamicRouteLabel(const std::string& path) {
    // Rust: path.trim_matches('/').split('/')
    size_t b = 0;
    size_t e = path.size();
    while (b < e && path[b] == '/') ++b;
    while (e > b && path[e - 1] == '/') --e;
    const std::string inner = path.substr(b, e - b);

    // `"".split('/')` yields exactly one empty element, so `parts` is never
    // empty and `first` is always Some — matching Rust's iterator semantics.
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t slash = inner.find('/', start);
        if (slash == std::string::npos) {
            parts.push_back(inner.substr(start));
            break;
        }
        parts.push_back(inner.substr(start, slash - start));
        start = slash + 1;
    }

    const size_t n = parts.size();
    const std::string& first = parts[0];

    // (Some("sandboxes"), Some(_), None, None, None)
    if (n == 2 && first == "sandboxes") return "/sandboxes/{sandbox_id}";
    // (Some("sandboxes"), Some(_), Some(<verb>), None, None)
    if (n == 3 && first == "sandboxes") {
        const std::string& third = parts[2];
        if (third == "snapshots") return "/sandboxes/{sandbox_id}/snapshots";
        if (third == "network")   return "/sandboxes/{sandbox_id}/network";
        if (third == "pause")     return "/sandboxes/{sandbox_id}/pause";
        if (third == "resume")    return "/sandboxes/{sandbox_id}/resume";
        if (third == "fork")      return "/sandboxes/{sandbox_id}/fork";
        return "unmatched";
    }
    // (Some("nodes"), Some(_), None, None, None)
    if (n == 2 && first == "nodes") return "/nodes/{node_id}";
    // (Some("proxy"), _, _, _, _) — any depth.
    if (first == "proxy") return "/proxy/*";
    return "unmatched";
}

}  // namespace

const char* HttpRouteSourceAsStr(HttpRouteSource source) {
    switch (source) {
        case HttpRouteSource::ControlPlane: return "control_plane";
        case HttpRouteSource::ProxyHost:    return "proxy_host";
        case HttpRouteSource::ProxyHeader:  return "proxy_header";
        case HttpRouteSource::ProxyPrefix:  return "proxy_prefix";
    }
    return "control_plane";
}

const char* ResultStatus(bool ok) { return ok ? "ok" : "error"; }

const char* HttpMethodLabel(const std::string& method) {
    if (method == "GET")     return "GET";
    if (method == "POST")    return "POST";
    if (method == "PUT")     return "PUT";
    if (method == "PATCH")   return "PATCH";
    if (method == "DELETE")  return "DELETE";
    if (method == "HEAD")    return "HEAD";
    if (method == "OPTIONS") return "OPTIONS";
    return "OTHER";
}

const char* HttpStatusLabel(uint16_t status) {
    if (status >= 100 && status <= 199) return "1xx";
    if (status >= 200 && status <= 299) return "2xx";
    if (status >= 300 && status <= 399) return "3xx";
    if (status >= 400 && status <= 499) return "4xx";
    if (status >= 500 && status <= 599) return "5xx";
    return "other";
}

const char* HttpRouteLabel(const std::string& raw_path) {
    // Rust: let path = path.trim_end_matches('/');
    size_t e = raw_path.size();
    while (e > 0 && raw_path[e - 1] == '/') --e;
    std::string path = raw_path.substr(0, e);
    // Rust: if path.is_empty() { "/" }
    if (path.empty()) path = "/";

    if (path == "/sandboxes")      return "/sandboxes";
    if (path == "/sandboxes-cold") return "/sandboxes-cold";
    if (path == "/v2/sandboxes")   return "/v2/sandboxes";
    if (path == "/snapshots")      return "/snapshots";
    if (path == "/templates")      return "/templates";
    if (path == "/v3/templates")   return "/v3/templates";
    if (path == "/nodes")          return "/nodes";
    if (path == "/health")         return "/health";
    return DynamicRouteLabel(path);
}

// ---- MetricGuard ----

MetricGuard::MetricGuard(const char* metric, LabelKind kind, const char* operation,
                         const char* artifact, const char* stage)
    : metric_(metric), kind_(kind), operation_(operation), artifact_(artifact),
      stage_(stage), start_ns_(NowNanos()),
      // Rust seeds the status with "canceled": a guard dropped before finish()
      // is a cancellation, not a failure.
      status_("canceled"), recorded_(false) {}

MetricGuard MetricGuard::Operation(const char* metric, const char* operation) {
    return MetricGuard(metric, LabelKind::Operation, operation, "", "");
}

MetricGuard MetricGuard::OperationArtifact(const char* metric, const char* operation,
                                           const char* artifact) {
    return MetricGuard(metric, LabelKind::OperationArtifact, operation, artifact, "");
}

MetricGuard MetricGuard::Stage(const char* metric, const char* stage) {
    return MetricGuard(metric, LabelKind::Stage, "", "", stage);
}

void MetricGuard::Finish(bool ok) {
    status_ = ResultStatus(ok);
    Record();
}

void MetricGuard::Record() {
    if (recorded_) return;  // Rust: early return when already recorded
    recorded_ = true;
    const double elapsed = ElapsedSecs(start_ns_);
    std::vector<std::pair<const char*, const char*> > labels;
    switch (kind_) {
        case LabelKind::Operation:
            labels.push_back(std::make_pair("operation", operation_));
            labels.push_back(std::make_pair("status", status_));
            break;
        case LabelKind::OperationArtifact:
            labels.push_back(std::make_pair("operation", operation_));
            labels.push_back(std::make_pair("artifact", artifact_));
            labels.push_back(std::make_pair("status", status_));
            break;
        case LabelKind::Stage:
            labels.push_back(std::make_pair("stage", stage_));
            labels.push_back(std::make_pair("status", status_));
            break;
    }
    HistogramObserve(WithLabels(metric_, labels), elapsed);
}

MetricGuard::~MetricGuard() { Record(); }

// ---- SandboxStageInFlight ----

SandboxStageInFlight::SandboxStageInFlight(const char* operation, const char* stage)
    : operation_(operation), stage_(stage) {
    std::vector<std::pair<const char*, const char*> > labels;
    labels.push_back(std::make_pair("operation", operation_));
    labels.push_back(std::make_pair("stage", stage_));
    GaugeSet(WithLabels(kSandboxStageInflight, labels), 1.0);  // increment(1.0)
}

SandboxStageInFlight::~SandboxStageInFlight() {
    std::vector<std::pair<const char*, const char*> > labels;
    labels.push_back(std::make_pair("operation", operation_));
    labels.push_back(std::make_pair("stage", stage_));
    GaugeSet(WithLabels(kSandboxStageInflight, labels), -1.0);  // decrement(1.0)
}

}  // namespace observability
}  // namespace agentenv
