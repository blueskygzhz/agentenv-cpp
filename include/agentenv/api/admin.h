// SPDX-License-Identifier: MIT
// Rust: src/api/impls/admin.rs — the node admin endpoints.
//
// This is a projection layer: observability's `NodeSnapshot` becomes the
// API's `Node` / `NodeDetail`. The two shapes differ in one place —
// `sandbox_starting_count` is in the collection response but not the detail
// one — and that difference is preserved rather than smoothed over.
//
// Disabled observability is not an error. The collection endpoint reports
// *no* nodes rather than a synthetic record, because a partial node would be
// indistinguishable from a real one to a scheduler reading the list. The
// detail endpoint reports 404 for the same reason.
#ifndef AGENTENV_API_ADMIN_H_
#define AGENTENV_API_ADMIN_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/observability/model.h"

namespace agentenv {
namespace api {

/// Rust `models::NodeStatus`. Rust hardcodes `Ready` at both call sites: a
/// process that can answer the request is by definition serving.
enum class NodeStatus { Ready };
const char* NodeStatusToString(NodeStatus status);

/// Rust `models::DiskMetrics`.
core::Json DiskMetricsToJson(const observability::DiskMetric& disk);

/// Rust `models::MachineInfo`.
core::Json MachineInfoToJson(const observability::MachineInfo& info);

/// Rust `models::NodeMetrics`.
core::Json NodeMetricsToJson(const observability::NodeMetricsSnapshot& metrics);

/// Rust `models::Node::from(NodeSnapshot)` — the collection shape.
core::Json NodeToJson(const observability::NodeSnapshot& node);

/// Rust `models::NodeDetail::new(..)` — the detail shape. Carries no
/// `sandbox_starting_count`.
core::Json NodeDetailToJson(const observability::NodeSnapshot& node);

/// An API-shaped failure.
struct AdminError {
    int         status = 500;
    std::string message;
};

/// What the admin endpoints need from the observability service. Injected so
/// the projection can be exercised without a live node.
class AdminObservability {
 public:
    virtual ~AdminObservability() {}
    /// Absent when observability is disabled on this process.
    virtual bool Enabled() const = 0;
    virtual std::string NodeId() const = 0;
    virtual std::string ClusterId() const = 0;
    /// Rust `observability.node_snapshot()`.
    virtual core::Expected<observability::NodeSnapshot, std::string>
        NodeSnapshot() const = 0;
};

/// Rust `nodes_get` — always 200; the list is empty when this process cannot
/// describe a node.
core::Expected<std::vector<core::Json>, AdminError>
    NodesGet(const AdminObservability* observability,
             const core::Optional<std::string>& cluster_id_filter);

/// Rust `nodes_node_id_get`.
core::Expected<core::Json, AdminError>
    NodesNodeIdGet(const AdminObservability* observability, const std::string& node_id,
                   const core::Optional<std::string>& cluster_id_filter);

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_ADMIN_H_
