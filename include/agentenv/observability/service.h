// SPDX-License-Identifier: MIT
// Rust: src/observability/service.rs — projects node-level observability
// responses from precomputed inputs.
#ifndef AGENTENV_OBSERVABILITY_SERVICE_H_
#define AGENTENV_OBSERVABILITY_SERVICE_H_

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/observability/host.h"
#include "agentenv/observability/model.h"
#include "agentenv/orchestrator/metrics.h"
#include "agentenv/orchestrator/types.h"

namespace agentenv {
namespace observability {

/// Rust `ObservabilityService::node_snapshot` body, as a pure projection.
///
/// Rust samples three sources on every request (orchestrator metrics, host
/// metrics, the running sandbox ID list) and merges them with the static node
/// identity + machine info. Factoring the merge out keeps the field mapping
/// verifiable without a live orchestrator, which is where drift would hide.
NodeSnapshot ProjectNodeSnapshot(const core::NodeIdentity& identity,
                                 const MachineInfo& machine_info,
                                 const orchestrator::OrchestratorMetrics& runtime,
                                 const HostMetrics& host,
                                 const std::vector<core::SandboxId>& sandbox_ids);

/// Rust `struct ObservabilityService`.
///
/// `Arc<Orchestrator>` is replaced by the two sampling callbacks the Rust code
/// actually invokes (`metrics_snapshot()` and `list_sandbox_ids()`), so this
/// module does not have to link the orchestrator.
class ObservabilityService {
 public:
    /// Rust `self.orchestrator.metrics_snapshot().await?`.
    using MetricsSampler =
        std::function<core::Expected<orchestrator::OrchestratorMetrics, std::string>()>;
    /// Rust `self.orchestrator.list_sandbox_ids().await?`.
    using SandboxIdSampler =
        std::function<core::Expected<std::vector<core::SandboxId>, std::string>()>;
    /// Rust `self.orchestrator.subscribe_sandbox_events()`.
    using SandboxEventSubscriber =
        std::function<void(const orchestrator::SandboxLifecycleEvent&)>;

    /// Rust `ObservabilityService::new` — detects machine info and constructs
    /// the host collector; `cpu_template_helper` is dumped once here and then
    /// handed to the reporter exactly once via `TakeCpuConfigJson`.
    ObservabilityService(core::NodeIdentity identity,
                         MetricsSampler metrics_sampler,
                         SandboxIdSampler sandbox_id_sampler,
                         const core::Optional<std::string>& cpu_template_helper);

    /// Rust `take_cpu_config_json` — `Option::take`, so the second call yields
    /// nothing. The CPU config is large and only needs to reach the scheduler
    /// once per process.
    core::Optional<std::string> TakeCpuConfigJson();

    /// Rust `store_cluster_cpu_config`.
    void StoreClusterCpuConfig(std::string config);
    /// Reads back what the scheduler sent (Rust keeps this in a shared RwLock
    /// that the sandbox launch path consults).
    core::Optional<std::string> ClusterCpuConfig() const;

    /// Rust `node_snapshot`.
    core::Expected<NodeSnapshot, std::string> NodeSnapshotNow();

    /// Rust `node_id` / `cluster_id` / `service_instance_id`.
    const std::string& node_id() const { return identity_.id; }
    core::Uuid cluster_id() const { return identity_.cluster_id; }
    const std::string& service_instance_id() const { return identity_.service_instance_id; }

    const MachineInfo& machine_info() const { return machine_info_; }

 private:
    core::NodeIdentity   identity_;
    MachineInfo          machine_info_;
    HostMetricsCollector host_metrics_;
    MetricsSampler       metrics_sampler_;
    SandboxIdSampler     sandbox_id_sampler_;

    mutable std::mutex           mu_;
    core::Optional<std::string>  pending_cpu_config_;
    core::Optional<std::string>  cluster_cpu_config_;
};

}  // namespace observability
}  // namespace agentenv
#endif  // AGENTENV_OBSERVABILITY_SERVICE_H_
