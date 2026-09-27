// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/store.rs
#include "agentenv/orchestrator/store.h"

namespace agentenv {
namespace orchestrator {

core::Expected<core::Unit, StoreError>
InMemoryMetadataStore::Insert(SandboxMetadata meta) {
    std::lock_guard<std::mutex> g(mu_);
    const std::string key = meta.id.ToString();
    if (items_.find(key) != items_.end()) {
        return core::Unexpected<StoreError>(StoreError::Conflict);
    }
    items_[key] = meta;
    return core::Unit{};
}

core::Expected<SandboxMetadata, StoreError>
InMemoryMetadataStore::Get(const core::SandboxId& id) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = items_.find(id.ToString());
    if (it == items_.end()) {
        return core::Unexpected<StoreError>(StoreError::SandboxNotFound);
    }
    return it->second;
}

core::Expected<core::Unit, StoreError>
InMemoryMetadataStore::Update(SandboxMetadata meta) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = items_.find(meta.id.ToString());
    if (it == items_.end()) {
        return core::Unexpected<StoreError>(StoreError::SandboxNotFound);
    }
    it->second = meta;
    return core::Unit{};
}

core::Expected<core::Unit, StoreError>
InMemoryMetadataStore::Remove(const core::SandboxId& id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = items_.find(id.ToString());
    if (it == items_.end()) {
        return core::Unexpected<StoreError>(StoreError::SandboxNotFound);
    }
    items_.erase(it);
    return core::Unit{};
}

std::vector<SandboxMetadata>
InMemoryMetadataStore::List(const SandboxListFilter& filter) const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<SandboxMetadata> out;
    out.reserve(items_.size());
    for (std::map<std::string, SandboxMetadata>::const_iterator it = items_.begin();
         it != items_.end(); ++it) {
        const SandboxMetadata& m = it->second;
        if (!filter.include_paused &&
            m.state == LifecyclePhase::Snapshotting) {
            continue;
        }
        if (!filter.include_stopped &&
            m.state == LifecyclePhase::Stopped) {
            continue;
        }
        out.push_back(m);
    }
    return out;
}

}  // namespace orchestrator
}  // namespace agentenv
