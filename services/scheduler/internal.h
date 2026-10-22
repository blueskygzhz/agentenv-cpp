// SPDX-License-Identifier: MIT
// Rust upstream (Go): services/scheduler/internal/{types,strategy,filter,
//node_registry,store,metrics,service}.go — multi-node placement.
#ifndef AGENTENV_SERVICES_SCHEDULER_INTERNAL_H_
#define AGENTENV_SERVICES_SCHEDULER_INTERNAL_H_

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace services {
namespace scheduler {

// ---- Go: internal/types.go ----
struct Node {
    std::string id;        // json "node_id"
    std::string endpoint;  // json "endpoint"

    /// Go `observedRecord.node.GetClusterId()`.
    ///
    /// Part of every P2P query rather than ambient state: one scheduler can
    /// serve several clusters, and a peer list that leaked across them would
    /// hand out addresses the requester has no business dialling.
    std::string cluster_id;

    /// Go `observedRecord.p2pEndpoint` — the node's P2P backend and address.
    ///
    /// Distinct from `endpoint`, which is the control-plane address the
    /// scheduler itself uses. A node may serve P2P on a different port, a
    /// different interface, or not at all; collapsing the two would advertise
    /// the control plane as a blob source.
    std::string p2p_backend;
    std::string p2p_address;

    bool operator==(const Node& o) const {
        return id == o.id && endpoint == o.endpoint && cluster_id == o.cluster_id &&
               p2p_backend == o.p2p_backend && p2p_address == o.p2p_address;
    }
};

/// Go: NodeSnapshot — observed runtime state from a node heartbeat.
struct NodeSnapshot {
    uint32_t sandbox_count = 0;
    uint32_t sandbox_starting_count = 0;
    uint32_t cpu_percent = 0;
    uint32_t cpu_count = 0;
    uint32_t allocated_cpu = 0;
    uint64_t memory_total_bytes = 0;
    uint64_t memory_used_bytes = 0;
    uint64_t allocated_memory_bytes = 0;
    uint32_t paused_sandbox_count = 0;
    uint32_t paused_allocated_cpu = 0;
    uint64_t paused_allocated_memory_bytes = 0;
};

/// Go: RichNode — discovery identity + optional heartbeat snapshot.
struct RichNode {
    Node node;
    bool has_snapshot = false;
    NodeSnapshot snapshot;
};

// ---- Go: internal/filter.go :: NodeResourceLimit ----
// Optional fields (Go used *T pointers): a has_* flag replaces nil.
struct NodeResourceLimit {
    bool has_max_sandbox_count = false;              uint32_t max_sandbox_count = 0;
    bool has_max_sandbox_starting_count = false;     uint32_t max_sandbox_starting_count = 0;
    bool has_max_cpu_used_percent = false;           uint32_t max_cpu_used_percent = 0;
    bool has_max_cpu_allocated_percent = false;      uint32_t max_cpu_allocated_percent = 0;
    bool has_max_memory_used_percent = false;        uint32_t max_memory_used_percent = 0;
    bool has_max_memory_allocated_percent = false;   uint32_t max_memory_allocated_percent = 0;
    bool has_max_sandbox_count_incl_paused = false;  uint32_t max_sandbox_count_incl_paused = 0;
    bool has_max_alloc_cpu_incl_paused = false;      uint32_t max_alloc_cpu_incl_paused = 0;
    bool has_max_alloc_mem_incl_paused = false;uint64_t max_alloc_mem_incl_paused = 0;
};

/// Go: FilterByResourceLimit — drop nodes exceeding any threshold.
/// Nodes without a snapshot are always kept.
std::vector<RichNode> FilterByResourceLimit(const std::vector<RichNode>& nodes,
                                            const NodeResourceLimit* limit);

// ---- Go: internal/strategy.go ----
/// Go: ScheduleRequestHint (opaque placement hint).
struct ScheduleRequestHint {
    std::string kind;   // "new_sandbox" | "new_cold_sandbox" | ""
    std::string image_ref;
};

/// Go: interface Strategy.
class Strategy {
 public:
    virtual ~Strategy() {}
    virtual core::Expected<RichNode, std::string>
        Select(const std::vector<RichNode>& nodes, const ScheduleRequestHint* hint) = 0;
    virtual std::string Name() const = 0;
};

/// Go: RoundRobinStrategy.
class RoundRobinStrategy : public Strategy {
 public:
    core::Expected<RichNode, std::string>
        Select(const std::vector<RichNode>& nodes, const ScheduleRequestHint* hint) override;
    std::string Name() const override { return "round_robin"; }
 private:
    uint64_t next_ = 0;
    std::mutex mu_;
};

/// Go: RandomStrategy.
class RandomStrategy : public Strategy {
 public:
    core::Expected<RichNode, std::string>
        Select(const std::vector<RichNode>& nodes, const ScheduleRequestHint* hint) override;
    std::string Name() const override { return "random"; }
};

/// Go: NewStrategy(name).
std::unique_ptr<Strategy> NewStrategy(const std::string& name);

// ---- Go: internal/store.go ----
/// Go: interface BindingStore — sandbox -> node placement bindings with TTL.
class BindingStore {
 public:
    virtual ~BindingStore() {}
    virtual core::Expected<Node, std::string> Get(const std::string& sandbox_id,
                                                  int64_t now_ms) = 0;
    virtual core::Expected<core::Unit, std::string> Record(const std::string& sandbox_id,
                                                           const Node& node,
                                                           int64_t now_ms) = 0;
    virtual core::Expected<core::Unit, std::string>
        ReconcileNode(const Node& node,
                      const std::vector<std::string>& sandbox_ids,
                      int64_t now_ms) = 0;
};

/// Go: InMemoryBindingStore.
class InMemoryBindingStore : public BindingStore {
 public:
    explicit InMemoryBindingStore(int64_t binding_ttl_ms);
    core::Expected<Node, std::string> Get(const std::string& sandbox_id, int64_t now_ms) override;
    core::Expected<core::Unit, std::string> Record(const std::string& sandbox_id,
                                                   const Node& node, int64_t now_ms) override;
    core::Expected<core::Unit, std::string>
        ReconcileNode(const Node& node, const std::vector<std::string>& sandbox_ids,
                      int64_t now_ms) override;
 private:
    struct BindingRecord { Node node; int64_t expires_at_ms; };
    std::mutex mu_;
    int64_t ttl_ms_;
    std::map<std::string, BindingRecord> bindings_;
};

// ---- Go: internal/node_registry.go ----
/// Go: NodeRegistry — tracks known nodes + last heartbeat.
class NodeRegistry {
 public:
    void Upsert(const Node& node);
    void RecordHeartbeat(const std::string& node_id, const NodeSnapshot& snap);
    void Remove(const std::string& node_id);
    std::vector<RichNode> RichNodes() const;

    /// Go `Resolve` — is this node in the scheduler's list?
    ///
    /// `RecordP2PArtifact` gates on it: an unknown node claiming to hold an
    /// artifact would be advertised to peers who then cannot reach it, so the
    /// claim is refused rather than indexed.
    bool Resolve(const std::string& node_id, Node* out) const;

    /// Go `ListP2pPeers` — every ready peer in `cluster_id` on `backend`.
    ///
    /// `node_ids` empty means "no restriction" for the list form. The lookup
    /// form passes the artifact index's answer, and an *empty* allow-list
    /// there must yield nothing rather than everything — see
    /// `FilterP2pPeers`, which is why the two are separate entry points.
    std::vector<Node> ListP2pPeers(const std::string& cluster_id,
                                   const std::string& backend,
                                   const std::string& exclude_node_id) const;

    /// Go `FilterP2pPeers` — the peers among `node_ids`, same filtering.
    ///
    /// An empty `node_ids` returns empty, matching Go's early return. The
    /// distinction is load-bearing: a lookup that found no holders must not
    /// degrade into "here is the whole cluster".
    std::vector<Node> FilterP2pPeers(const std::string& cluster_id,
                                     const std::string& backend,
                                     const std::vector<std::string>& node_ids,
                                     const std::string& exclude_node_id) const;

 private:
    /// Shared by both public forms. `allowed == nullptr` is no restriction.
    std::vector<Node> FilterP2pPeersLocked(const std::string& cluster_id,
                                           const std::string& backend,
                                           const std::set<std::string>* allowed,
                                           const std::string& exclude_node_id) const;

    mutable std::mutex mu_;
    std::map<std::string, RichNode> nodes_;
};

}  // namespace scheduler
}  // namespace services
}  // namespace agentenv
#endif  // AGENTENV_SERVICES_SCHEDULER_INTERNAL_H_
