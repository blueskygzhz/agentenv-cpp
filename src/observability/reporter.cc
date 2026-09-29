// SPDX-License-Identifier: MIT
// Rust: src/observability/reporter.rs
#include "agentenv/observability/reporter.h"

#include <algorithm>

namespace agentenv {
namespace observability {

// Rust: const MAX_REPORT_BACKOFF: Duration = Duration::from_secs(60);
const uint64_t kMaxReportBackoffMs = 60 * 1000;
// Rust: const GRPC_CALL_TIMEOUT: Duration = Duration::from_secs(10);
const uint64_t kGrpcCallTimeoutMs = 10 * 1000;

// Rust: #[error("node is not in the scheduler's configured node list")]
const char* const kHeartbeatNodeNotConfiguredMessage =
    "node is not in the scheduler's configured node list";

// Rust: the status is classified only when the code is InvalidArgument AND the
// message contains this needle. tonic::Code::InvalidArgument == 3.
bool IsHeartbeatNodeNotConfigured(int grpc_code, const std::string& message) {
    const int kInvalidArgument = 3;
    if (grpc_code != kInvalidArgument) return false;
    return message.find("node is not in scheduler node list") != std::string::npos;
}

core::Optional<ReporterConfig>
ReporterConfig::Resolve(const cfg::ObservabilitySchedulerReportConfig& config,
                        const cfg::ClusterConfig& cluster_config) {
    // Rust: if !config.enabled { return None; }
    if (!config.enabled) return core::Optional<ReporterConfig>();

    // Rust: scheduler_endpoint.as_deref().map(str::trim).filter(|e| !e.is_empty())
    if (!cluster_config.scheduler_endpoint.has_value()) {
        return core::Optional<ReporterConfig>();
    }
    const std::string& raw = *cluster_config.scheduler_endpoint;
    size_t b = 0;
    size_t e = raw.size();
    while (b < e && (raw[b] == ' ' || raw[b] == '\t' || raw[b] == '\n' || raw[b] == '\r')) ++b;
    while (e > b && (raw[e - 1] == ' ' || raw[e - 1] == '\t' || raw[e - 1] == '\n' ||
                     raw[e - 1] == '\r')) --e;
    const std::string trimmed = raw.substr(b, e - b);
    if (trimmed.empty()) {
        // Rust logs a warning here and disables reporting.
        return core::Optional<ReporterConfig>();
    }

    ReporterConfig out;
    out.scheduler_endpoint = trimmed;
    // Rust: Duration::from_secs(config.interval_secs.max(1))
    const uint64_t secs = config.interval_secs < 1 ? 1 : config.interval_secs;
    out.interval_ms = secs * 1000;
    return out;
}

WireSandboxEventType MapSandboxEventType(orchestrator::SandboxLifecycleEventType event_type) {
    switch (event_type) {
        case orchestrator::SandboxLifecycleEventType::Create: return WireSandboxEventType::Create;
        case orchestrator::SandboxLifecycleEventType::Delete: return WireSandboxEventType::Delete;
        case orchestrator::SandboxLifecycleEventType::Pause:  return WireSandboxEventType::Pause;
        case orchestrator::SandboxLifecycleEventType::Resume: return WireSandboxEventType::Resume;
        case orchestrator::SandboxLifecycleEventType::Fork:   return WireSandboxEventType::Fork;
    }
    return WireSandboxEventType::Create;
}

WireHeartbeatRequest
BuildHeartbeatRequest(const NodeSnapshot& snapshot, int64_t now_ms,
                      const core::Optional<p2p::P2pEndpoint>& p2p_endpoint) {
    WireHeartbeatRequest req;
    req.node_id             = snapshot.node_id;
    req.cluster_id          = snapshot.cluster_id.ToString();
    req.service_instance_id = snapshot.service_instance_id;
    req.version             = snapshot.version;
    req.commit              = snapshot.commit;

    req.machine_info.cpu_family       = snapshot.machine_info.cpu_family;
    req.machine_info.cpu_model        = snapshot.machine_info.cpu_model;
    req.machine_info.cpu_model_name   = snapshot.machine_info.cpu_model_name;
    req.machine_info.cpu_architecture = snapshot.machine_info.cpu_architecture;
    // Rust: cpu_config_json.unwrap_or_default() — None becomes "".
    req.machine_info.cpu_config_json =
        snapshot.machine_info.cpu_config_json.value_or(std::string());

    // Rust always reports Ready; liveness is inferred by the scheduler from the
    // heartbeat arriving at all.
    req.snapshot.status                 = WireNodeStatus::Ready;
    req.snapshot.allocated_cpu          = snapshot.metrics.allocated_cpu;
    req.snapshot.allocated_memory_bytes = snapshot.metrics.allocated_memory_bytes;
    req.snapshot.cpu_percent            = snapshot.metrics.cpu_percent;
    req.snapshot.cpu_count              = snapshot.metrics.cpu_count;
    req.snapshot.memory_used_bytes      = snapshot.metrics.memory_used_bytes;
    req.snapshot.memory_total_bytes     = snapshot.metrics.memory_total_bytes;
    for (size_t i = 0; i < snapshot.metrics.disks.size(); ++i) {
        const DiskMetric& d = snapshot.metrics.disks[i];
        WireDiskMetric w;
        w.mount_point     = d.mount_point;
        w.device          = d.device;
        w.filesystem_type = d.filesystem_type;
        w.used_bytes      = d.used_bytes;
        w.total_bytes     = d.total_bytes;
        req.snapshot.disks.push_back(w);
    }
    req.snapshot.sandbox_count          = snapshot.sandbox_count;
    req.snapshot.sandbox_starting_count = snapshot.sandbox_starting_count;
    req.snapshot.create_successes       = snapshot.create_successes;
    req.snapshot.create_fails           = snapshot.create_fails;
    req.snapshot.reported_at_unix_ms    = now_ms;
    req.snapshot.paused_sandbox_count   = snapshot.paused_sandbox_count;
    req.snapshot.paused_allocated_cpu   = snapshot.metrics.paused_allocated_cpu;
    req.snapshot.paused_allocated_memory_bytes =
        snapshot.metrics.paused_allocated_memory_bytes;

    for (size_t i = 0; i < snapshot.sandbox_ids.size(); ++i) {
        req.sandbox_ids.push_back(snapshot.sandbox_ids[i].ToString());
    }

    if (p2p_endpoint.has_value()) {
        req.has_p2p_endpoint = true;
        req.p2p_endpoint = *p2p_endpoint;
    }
    return req;
}

WireReportSandboxEventRequest
BuildSandboxEventRequest(const std::string& node_id, const std::string& cluster_id,
                         const std::string& service_instance_id,
                         const std::vector<orchestrator::SandboxLifecycleEvent>& events) {
    WireReportSandboxEventRequest req;
    req.node_id             = node_id;
    req.cluster_id          = cluster_id;
    req.service_instance_id = service_instance_id;
    for (size_t i = 0; i < events.size(); ++i) {
        const orchestrator::SandboxLifecycleEvent& ev = events[i];
        WireSandboxEvent w;
        w.sandbox_id = ev.sandbox_id.ToString();
        w.event_type = MapSandboxEventType(ev.event_type);
        w.requested_cpu = ev.resources.cpu_count;
        // Rust: u64::from(memory_mib) * 1024 * 1024 — widen before multiplying.
        w.requested_memory_bytes =
            static_cast<uint64_t>(ev.resources.memory_mib) * 1024ULL * 1024ULL;
        w.requested_disk_bytes =
            static_cast<uint64_t>(ev.resources.disk_size_mib) * 1024ULL * 1024ULL;
        req.events.push_back(w);
    }
    return req;
}

HeartbeatBackoff::HeartbeatBackoff(uint64_t interval_ms)
    // Rust: `let mut backoff = config.interval; let mut wait = 100ms;`
    : interval_ms_(interval_ms), wait_ms_(100), backoff_ms_(interval_ms),
      ever_succeeded_(false) {}

void HeartbeatBackoff::OnSuccess() {
    ever_succeeded_ = true;
    backoff_ms_ = interval_ms_;
    wait_ms_    = interval_ms_;
}

uint64_t HeartbeatBackoff::OnFailure() {
    // Rust: `wait = backoff; backoff = min(backoff.saturating_mul(2), MAX);`
    wait_ms_ = backoff_ms_;
    uint64_t doubled;
    if (backoff_ms_ > (static_cast<uint64_t>(-1) / 2)) {
        doubled = static_cast<uint64_t>(-1);  // saturating_mul
    } else {
        doubled = backoff_ms_ * 2;
    }
    backoff_ms_ = std::min(doubled, kMaxReportBackoffMs);
    return wait_ms_;
}

// Rust: `for attempt in 1..=3`.
const int kUnregisterMaxAttempts = 3;

uint64_t UnregisterRetryDelayMs(int attempt) {
    // Rust: sleep(Duration::from_millis(200 * attempt))
    if (attempt < 1) return 0;
    return 200ULL * static_cast<uint64_t>(attempt);
}

}  // namespace observability
}  // namespace agentenv
