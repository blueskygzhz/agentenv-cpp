// SPDX-License-Identifier: MIT
// Rust: src/observability/service.rs
#include "agentenv/observability/service.h"

#include <utility>

#include "agentenv/observability/machine.h"

namespace agentenv {
namespace observability {

NodeSnapshot ProjectNodeSnapshot(const core::NodeIdentity& identity,
                                 const MachineInfo& machine_info,
                                 const orchestrator::OrchestratorMetrics& runtime,
                                 const HostMetrics& host,
                                 const std::vector<core::SandboxId>& sandbox_ids) {
    NodeSnapshot snap;
    snap.version             = identity.version;
    snap.commit              = identity.commit;
    snap.node_id             = identity.id;
    snap.service_instance_id = identity.service_instance_id;
    snap.cluster_id          = identity.cluster_id;
    snap.machine_info        = machine_info;
    // Rust: sandbox_count comes from the orchestrator's running count, NOT
    // from sandbox_ids.len() — the two are sampled separately and may differ.
    snap.sandbox_count       = runtime.running_sandbox_count;
    snap.sandbox_ids         = sandbox_ids;

    // Allocation figures come from the orchestrator; utilization from the host.
    snap.metrics.allocated_cpu          = runtime.allocated_cpu;
    snap.metrics.allocated_memory_bytes = runtime.allocated_memory_bytes;
    snap.metrics.cpu_percent            = host.cpu_percent;
    snap.metrics.cpu_count              = host.cpu_count;
    snap.metrics.memory_used_bytes      = host.memory_used_bytes;
    snap.metrics.memory_total_bytes     = host.memory_total_bytes;
    snap.metrics.disks                  = host.disks;
    snap.metrics.paused_allocated_cpu   = runtime.paused_allocated_cpu;
    snap.metrics.paused_allocated_memory_bytes = runtime.paused_allocated_memory_bytes;

    snap.create_successes       = runtime.create_successes;
    snap.create_fails           = runtime.create_fails;
    snap.sandbox_starting_count = runtime.starting_sandbox_count;
    snap.paused_sandbox_count   = runtime.paused_sandbox_count;
    return snap;
}

ObservabilityService::ObservabilityService(
    core::NodeIdentity identity,
    MetricsSampler metrics_sampler,
    SandboxIdSampler sandbox_id_sampler,
    const core::Optional<std::string>& cpu_template_helper)
    : identity_(std::move(identity)),
      machine_info_(DetectMachineInfo()),
      host_metrics_(),
      metrics_sampler_(std::move(metrics_sampler)),
      sandbox_id_sampler_(std::move(sandbox_id_sampler)) {
    // Rust: `if let Some(p) = cpu_template_helper { dump_cpu_config(p).await }`
    if (cpu_template_helper.has_value()) {
        pending_cpu_config_ = DumpCpuConfig(*cpu_template_helper);
    }
}

core::Optional<std::string> ObservabilityService::TakeCpuConfigJson() {
    std::lock_guard<std::mutex> g(mu_);
    // Rust `Option::take` — leaves None behind.
    core::Optional<std::string> taken = pending_cpu_config_;
    pending_cpu_config_.reset();
    return taken;
}

void ObservabilityService::StoreClusterCpuConfig(std::string config) {
    std::lock_guard<std::mutex> g(mu_);
    cluster_cpu_config_ = std::move(config);
}

core::Optional<std::string> ObservabilityService::ClusterCpuConfig() const {
    std::lock_guard<std::mutex> g(mu_);
    return cluster_cpu_config_;
}

core::Expected<NodeSnapshot, std::string> ObservabilityService::NodeSnapshotNow() {
    if (!metrics_sampler_ || !sandbox_id_sampler_) {
        return core::make_unexpected(std::string("observability service not wired"));
    }
    // Rust ordering: metrics first (propagates `?`), then host, then IDs.
    core::Expected<orchestrator::OrchestratorMetrics, std::string> runtime = metrics_sampler_();
    if (!runtime.ok()) return core::make_unexpected(runtime.error());

    const HostMetrics host = host_metrics_.Collect();

    core::Expected<std::vector<core::SandboxId>, std::string> ids = sandbox_id_sampler_();
    if (!ids.ok()) return core::make_unexpected(ids.error());

    return ProjectNodeSnapshot(identity_, machine_info_, runtime.value(), host, ids.value());
}

}  // namespace observability
}  // namespace agentenv
