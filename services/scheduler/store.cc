// SPDX-License-Identifier: MIT
// Go: services/scheduler/internal/store.go
#include "services/scheduler/internal.h"

namespace agentenv {
namespace services {
namespace scheduler {

static const int64_t kDefaultBindingTtlMs = 30000;

InMemoryBindingStore::InMemoryBindingStore(int64_t binding_ttl_ms)
    : ttl_ms_(binding_ttl_ms > 0 ? binding_ttl_ms : kDefaultBindingTtlMs) {}

core::Expected<Node, std::string>
InMemoryBindingStore::Get(const std::string& sandbox_id, int64_t now_ms) {
    std::lock_guard<std::mutex> g(mu_);
    std::map<std::string, BindingRecord>::iterator it = bindings_.find(sandbox_id);
    if (it == bindings_.end()) {
        return core::make_unexpected(std::string("binding not found"));
    }
    if (now_ms >= it->second.expires_at_ms) {
        bindings_.erase(it);
        return core::make_unexpected(std::string("binding expired"));
    }
    return it->second.node;
}

core::Expected<core::Unit, std::string>
InMemoryBindingStore::Record(const std::string& sandbox_id, const Node& node, int64_t now_ms) {
    std::lock_guard<std::mutex> g(mu_);
    BindingRecord r;
    r.node = node;
    r.expires_at_ms = now_ms + ttl_ms_;
    bindings_[sandbox_id] = r;
    return core::Unit{};
}

core::Expected<core::Unit, std::string>
InMemoryBindingStore::ReconcileNode(const Node& node,
                                    const std::vector<std::string>& sandbox_ids,
                                    int64_t now_ms) {
    std::lock_guard<std::mutex> g(mu_);
    // Refresh/insert bindings for the reported sandboxes on this node.
    for (size_t i = 0; i < sandbox_ids.size(); ++i) {
        BindingRecord r;
        r.node = node;
        r.expires_at_ms = now_ms + ttl_ms_;
        bindings_[sandbox_ids[i]] = r;
    }
    return core::Unit{};
}

}  // namespace scheduler
}  // namespace services
}  // namespace agentenv
