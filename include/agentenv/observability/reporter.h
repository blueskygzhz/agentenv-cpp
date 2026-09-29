// SPDX-License-Identifier: MIT
// Rust: src/observability/reporter.rs — scheduler heartbeat + sandbox lifecycle
// event reporting.
//
// The gRPC transport itself is gated (AGENTENV_WITH_GRPC). What is ported here
// is everything that decides *what* gets sent and *when*:
//   - ReporterConfig::resolve  (enable/endpoint/interval validation + clamping)
//   - build_heartbeat_request  (snapshot -> wire projection, incl. unit math)
//   - build_sandbox_event_request + map_sandbox_event_type
//   - the retry/backoff schedule and the "never succeeded => skip unregister"
//     shutdown rule
// so the wire payload and the failure behaviour are testable without a server.
#ifndef AGENTENV_OBSERVABILITY_REPORTER_H_
#define AGENTENV_OBSERVABILITY_REPORTER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/cfg.h"
#include "agentenv/core/optional.h"
#include "agentenv/observability/model.h"
#include "agentenv/orchestrator/types.h"
#include "agentenv/p2p/types.h"

namespace agentenv {
namespace observability {

/// Rust `MAX_REPORT_BACKOFF` = 60s.
extern const uint64_t kMaxReportBackoffMs;
/// Rust `GRPC_CALL_TIMEOUT` = 10s.
extern const uint64_t kGrpcCallTimeoutMs;

/// Rust `struct HeartbeatNodeNotConfigured` — a distinct error type so the
/// reporter loop can log an actionable remediation hint instead of a generic
/// transient-failure warning.
extern const char* const kHeartbeatNodeNotConfiguredMessage;

/// Rust: the scheduler `InvalidArgument` message that is classified as
/// `HeartbeatNodeNotConfigured`.
bool IsHeartbeatNodeNotConfigured(int grpc_code, const std::string& message);

/// Rust private `struct ReporterConfig`.
struct ReporterConfig {
    std::string scheduler_endpoint;
    uint64_t    interval_ms = 0;

    /// Rust `ReporterConfig::resolve` — returns None when reporting is
    /// disabled or the cluster scheduler endpoint is missing/blank. The
    /// interval is clamped to a minimum of one second.
    static core::Optional<ReporterConfig>
        Resolve(const cfg::ObservabilitySchedulerReportConfig& config,
                const cfg::ClusterConfig& cluster_config);
};

/// Rust `scheduler::NodeStatus::Ready`.
enum class WireNodeStatus { Unknown = 0, Ready = 1 };

/// Rust `scheduler::SandboxEventType`.
enum class WireSandboxEventType { Create, Delete, Pause, Resume, Fork };

/// Rust `Self::map_sandbox_event_type`.
WireSandboxEventType MapSandboxEventType(orchestrator::SandboxLifecycleEventType event_type);

/// Rust `scheduler::DiskMetric`.
struct WireDiskMetric {
    std::string mount_point;
    std::string device;
    std::string filesystem_type;
    uint64_t    used_bytes  = 0;
    uint64_t    total_bytes = 0;
};

/// Rust `scheduler::MachineInfo` — note `cpu_config_json` is a plain string on
/// the wire, so `None` becomes "" (`unwrap_or_default`).
struct WireMachineInfo {
    std::string cpu_family;
    std::string cpu_model;
    std::string cpu_model_name;
    std::string cpu_architecture;
    std::string cpu_config_json;
};

/// Rust `scheduler::NodeSnapshot`.
struct WireNodeSnapshot {
    WireNodeStatus status = WireNodeStatus::Ready;
    uint32_t allocated_cpu          = 0;
    uint64_t allocated_memory_bytes = 0;
    uint32_t cpu_percent            = 0;
    uint32_t cpu_count              = 0;
    uint64_t memory_used_bytes      = 0;
    uint64_t memory_total_bytes     = 0;
    std::vector<WireDiskMetric> disks;
    uint32_t sandbox_count          = 0;
    uint32_t sandbox_starting_count = 0;
    uint64_t create_successes       = 0;
    uint64_t create_fails           = 0;
    int64_t  reported_at_unix_ms    = 0;
    uint32_t paused_sandbox_count   = 0;
    uint32_t paused_allocated_cpu   = 0;
    uint64_t paused_allocated_memory_bytes = 0;
};

/// Rust `scheduler::HeartbeatRequest`.
struct WireHeartbeatRequest {
    std::string node_id;
    std::string cluster_id;
    std::string service_instance_id;
    std::string version;
    std::string commit;
    WireMachineInfo  machine_info;
    WireNodeSnapshot snapshot;
    std::vector<std::string> sandbox_ids;
    /// Rust `Option<scheduler::P2pEndpoint>`.
    bool             has_p2p_endpoint = false;
    p2p::P2pEndpoint p2p_endpoint;
};

/// Rust `scheduler::SandboxEvent`.
struct WireSandboxEvent {
    std::string          sandbox_id;
    WireSandboxEventType event_type = WireSandboxEventType::Create;
    uint32_t             requested_cpu          = 0;
    uint64_t             requested_memory_bytes = 0;
    uint64_t             requested_disk_bytes   = 0;
};

/// Rust `scheduler::ReportSandboxEventRequest`.
struct WireReportSandboxEventRequest {
    std::string node_id;
    std::string cluster_id;
    std::string service_instance_id;
    std::vector<WireSandboxEvent> events;
};

/// Rust `ObservabilityReporter::build_heartbeat_request`.
WireHeartbeatRequest
    BuildHeartbeatRequest(const NodeSnapshot& snapshot, int64_t now_ms,
                          const core::Optional<p2p::P2pEndpoint>& p2p_endpoint);

/// Rust `ObservabilityReporter::build_sandbox_event_request`. MiB fields are
/// converted to bytes (`* 1024 * 1024`) on the wire.
WireReportSandboxEventRequest
    BuildSandboxEventRequest(const std::string& node_id, const std::string& cluster_id,
                             const std::string& service_instance_id,
                             const std::vector<orchestrator::SandboxLifecycleEvent>& events);

/// Rust heartbeat loop backoff: `wait = backoff; backoff = min(backoff * 2,
/// MAX_REPORT_BACKOFF)` on failure, and both reset to `interval` on success.
/// Modelled as an explicit state machine so the schedule is testable.
class HeartbeatBackoff {
 public:
    explicit HeartbeatBackoff(uint64_t interval_ms);

    /// Rust `Ok(())` arm.
    void OnSuccess();
    /// Rust `Err(..)` arms — returns the wait to apply before the next attempt.
    uint64_t OnFailure();

    uint64_t wait_ms() const { return wait_ms_; }
    uint64_t backoff_ms() const { return backoff_ms_; }
    /// Rust `ever_heartbeat_succeeded`.
    bool ever_succeeded() const { return ever_succeeded_; }
    /// Rust shutdown rule: skip `UnregisterNode` when no heartbeat ever
    /// reached the scheduler.
    bool ShouldUnregisterOnShutdown() const { return ever_succeeded_; }

 private:
    uint64_t interval_ms_;
    uint64_t wait_ms_;
    uint64_t backoff_ms_;
    bool     ever_succeeded_;
};

/// Rust `unregister_node` retry loop: `for attempt in 1..=3` with a
/// `200 * attempt` ms sleep after each failure.
extern const int kUnregisterMaxAttempts;
uint64_t UnregisterRetryDelayMs(int attempt);

}  // namespace observability
}  // namespace agentenv
#endif  // AGENTENV_OBSERVABILITY_REPORTER_H_
