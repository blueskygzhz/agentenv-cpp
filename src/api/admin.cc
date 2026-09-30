// SPDX-License-Identifier: MIT
// Rust: src/api/impls/admin.rs
#include "agentenv/api/admin.h"

#include <sstream>

namespace agentenv {
namespace api {

const char* NodeStatusToString(NodeStatus status) {
    switch (status) {
        case NodeStatus::Ready: return "ready";
    }
    return "ready";
}

core::Json DiskMetricsToJson(const observability::DiskMetric& disk) {
    core::JsonObject object;
    object["mountPoint"]     = core::Json(disk.mount_point);
    object["device"]         = core::Json(disk.device);
    object["filesystemType"] = core::Json(disk.filesystem_type);
    object["usedBytes"]      = core::Json(static_cast<int64_t>(disk.used_bytes));
    object["totalBytes"]     = core::Json(static_cast<int64_t>(disk.total_bytes));
    return core::Json(object);
}

core::Json MachineInfoToJson(const observability::MachineInfo& info) {
    // Rust's `models::MachineInfo::new` takes only the four CPU identity
    // fields; `cpu_config_json` is deliberately not part of the API shape.
    core::JsonObject object;
    object["cpuFamily"]       = core::Json(info.cpu_family);
    object["cpuModel"]        = core::Json(info.cpu_model);
    object["cpuModelName"]    = core::Json(info.cpu_model_name);
    object["cpuArchitecture"] = core::Json(info.cpu_architecture);
    return core::Json(object);
}

core::Json NodeMetricsToJson(const observability::NodeMetricsSnapshot& metrics) {
    core::JsonObject object;
    object["allocatedCpu"] = core::Json(static_cast<int64_t>(metrics.allocated_cpu));
    object["cpuPercent"]   = core::Json(static_cast<int64_t>(metrics.cpu_percent));
    object["cpuCount"]     = core::Json(static_cast<int64_t>(metrics.cpu_count));
    object["allocatedMemoryBytes"] =
        core::Json(static_cast<int64_t>(metrics.allocated_memory_bytes));
    object["memoryUsedBytes"] =
        core::Json(static_cast<int64_t>(metrics.memory_used_bytes));
    object["memoryTotalBytes"] =
        core::Json(static_cast<int64_t>(metrics.memory_total_bytes));

    core::JsonArray disks;
    for (std::size_t i = 0; i < metrics.disks.size(); ++i) {
        disks.push_back(DiskMetricsToJson(metrics.disks[i]));
    }
    object["disks"] = core::Json(disks);

    object["pausedAllocatedCpu"] =
        core::Json(static_cast<int64_t>(metrics.paused_allocated_cpu));
    object["pausedAllocatedMemoryBytes"] =
        core::Json(static_cast<int64_t>(metrics.paused_allocated_memory_bytes));
    return core::Json(object);
}

namespace {

/// The fields both response shapes share.
void AppendCommonNodeFields(const observability::NodeSnapshot& node,
                            core::JsonObject* object) {
    (*object)["version"]           = core::Json(node.version);
    (*object)["commit"]            = core::Json(node.commit);
    (*object)["nodeID"]            = core::Json(node.node_id);
    (*object)["serviceInstanceID"] = core::Json(node.service_instance_id);
    (*object)["clusterID"]         = core::Json(node.cluster_id.ToString());
    (*object)["machineInfo"]       = MachineInfoToJson(node.machine_info);
    (*object)["status"]            = core::Json(std::string(NodeStatusToString(
        NodeStatus::Ready)));
    (*object)["sandboxCount"]      = core::Json(static_cast<int64_t>(node.sandbox_count));
    (*object)["metrics"]           = NodeMetricsToJson(node.metrics);
    (*object)["createSuccesses"]   = core::Json(static_cast<int64_t>(node.create_successes));
    (*object)["createFails"]       = core::Json(static_cast<int64_t>(node.create_fails));
    (*object)["pausedSandboxCount"] =
        core::Json(static_cast<int64_t>(node.paused_sandbox_count));
}

}  // namespace

core::Json NodeToJson(const observability::NodeSnapshot& node) {
    core::JsonObject object;
    AppendCommonNodeFields(node, &object);
    // Only the collection shape carries this: a scheduler reading the list
    // uses it to avoid piling work onto a node that is already starting VMs.
    object["sandboxStartingCount"] =
        core::Json(static_cast<int64_t>(node.sandbox_starting_count));
    return core::Json(object);
}

core::Json NodeDetailToJson(const observability::NodeSnapshot& node) {
    core::JsonObject object;
    AppendCommonNodeFields(node, &object);
    return core::Json(object);
}

core::Expected<std::vector<core::Json>, AdminError>
NodesGet(const AdminObservability* observability,
         const core::Optional<std::string>& cluster_id_filter) {
    // A disabled service exposes no nodes rather than a partial record: to a
    // scheduler reading the list, a synthetic node is indistinguishable from
    // a real one.
    if (observability == NULL || !observability->Enabled()) {
        return std::vector<core::Json>();
    }
    if (cluster_id_filter.has_value() &&
        *cluster_id_filter != observability->ClusterId()) {
        return std::vector<core::Json>();
    }

    core::Expected<observability::NodeSnapshot, std::string> node =
        observability->NodeSnapshot();
    if (!node.ok()) {
        AdminError error;
        error.status  = 500;
        error.message = node.error();
        return core::make_unexpected(error);
    }

    std::vector<core::Json> nodes;
    nodes.push_back(NodeToJson(node.value()));
    return nodes;
}

core::Expected<core::Json, AdminError>
NodesNodeIdGet(const AdminObservability* observability, const std::string& node_id,
               const core::Optional<std::string>& cluster_id_filter) {
    if (observability == NULL || !observability->Enabled()) {
        AdminError error;
        error.status  = 404;
        error.message = "observability is disabled on this node";
        return core::make_unexpected(error);
    }

    const bool cluster_mismatch = cluster_id_filter.has_value() &&
                                  *cluster_id_filter != observability->ClusterId();
    if (node_id != observability->NodeId() || cluster_mismatch) {
        AdminError error;
        error.status = 404;
        std::ostringstream os;
        os << "node " << node_id << " not found";
        error.message = os.str();
        return core::make_unexpected(error);
    }

    core::Expected<observability::NodeSnapshot, std::string> node =
        observability->NodeSnapshot();
    if (!node.ok()) {
        AdminError error;
        error.status  = 500;
        error.message = node.error();
        return core::make_unexpected(error);
    }
    return NodeDetailToJson(node.value());
}

}  // namespace api
}  // namespace agentenv
