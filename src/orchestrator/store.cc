// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/store/{metadata,in_memory}.rs
#include "agentenv/orchestrator/store.h"

#include <algorithm>
#include <sstream>

namespace agentenv {
namespace orchestrator {

// ---- SandboxMetadata -------------------------------------------------------

void SandboxMetadata::SetTimeout(core::Optional<int64_t> timeout_ms_val) {
    timeout_ms  = timeout_ms_val;
    expires_at_ms = timeout_ms_val
        ? core::Optional<int64_t>(created_at_ms + *timeout_ms_val)
        : core::Optional<int64_t>(core::nullopt);
}

void SandboxMetadata::UpdateTimeout(NewTimeout new_timeout) {
    core::Optional<int64_t> next;
    switch (new_timeout.kind) {
        case NewTimeout::Kind::UseExisting:
            next = timeout_ms;
            break;
        case NewTimeout::Kind::Set:
            next = core::Optional<int64_t>(new_timeout.duration_ms);
            break;
        case NewTimeout::Kind::EnsureMinimum:
            if (timeout_ms) {
                next = core::Optional<int64_t>(
                    std::max(*timeout_ms, new_timeout.duration_ms));
            } else {
                next = core::Optional<int64_t>(new_timeout.duration_ms);
            }
            break;
        case NewTimeout::Kind::None:
            next = core::nullopt;
            break;
    }
    SetTimeout(next);
}

bool SandboxMetadata::IsExpired(int64_t now_ms) const {
    if (!expires_at_ms) return false;
    return *expires_at_ms <= now_ms;
}

// ---- StoreError ------------------------------------------------------------

StoreError StoreError::Backend(std::string msg) {
    StoreError e;
    e.kind   = Kind::Backend;
    e.detail = std::move(msg);
    return e;
}

StoreError StoreError::SandboxNotFound(core::SandboxId id) {
    StoreError e;
    e.kind       = Kind::SandboxNotFound;
    e.sandbox_id = id;
    return e;
}

StoreError StoreError::SandboxAlreadyExists(core::SandboxId id) {
    StoreError e;
    e.kind       = Kind::SandboxAlreadyExists;
    e.sandbox_id = id;
    return e;
}

StoreError StoreError::StateConflict(core::SandboxId id,
                                     std::vector<SandboxState> expected,
                                     SandboxState actual) {
    StoreError e;
    e.kind            = Kind::StateConflict;
    e.sandbox_id      = id;
    e.expected_states = std::move(expected);
    e.actual_state    = actual;
    return e;
}

std::string StoreError::Message() const {
    std::ostringstream os;
    switch (kind) {
        case Kind::Backend:
            os << "store backend error: " << detail;
            return os.str();
        case Kind::SandboxNotFound:
            os << "sandbox " << sandbox_id.ToString() << " not found";
            return os.str();
        case Kind::SandboxAlreadyExists:
            os << "sandbox " << sandbox_id.ToString() << " already exists";
            return os.str();
        case Kind::StateConflict:
            os << "sandbox " << sandbox_id.ToString()
               << " state conflict: expected one of [";
            for (std::size_t i = 0; i < expected_states.size(); ++i) {
                if (i) os << ", ";
                os << SandboxStateName(expected_states[i]);
            }
            os << "], got " << SandboxStateName(actual_state);
            return os.str();
    }
    return "unknown store error";
}

// ---- InMemoryMetadataStore -------------------------------------------------

core::Expected<core::Unit, StoreError>
InMemoryMetadataStore::Add(SandboxMetadata meta) {
    std::lock_guard<std::mutex> g(mu_);
    const std::string key = meta.id.ToString();
    if (items_.find(key) != items_.end()) {
        return core::make_unexpected(StoreError::SandboxAlreadyExists(meta.id));
    }
    items_[key] = meta;
    return core::Unit{};
}

core::Expected<core::Unit, StoreError>
InMemoryMetadataStore::Update(SandboxMetadata meta) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = items_.find(meta.id.ToString());
    if (it == items_.end()) {
        return core::make_unexpected(StoreError::SandboxNotFound(meta.id));
    }
    it->second = meta;
    return core::Unit{};
}

core::Expected<SandboxState, StoreError>
InMemoryMetadataStore::UpdateStateIfState(
    const core::SandboxId& id,
    SandboxState new_state,
    const std::vector<SandboxState>& expected_states) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = items_.find(id.ToString());
    if (it == items_.end()) {
        return core::make_unexpected(StoreError::SandboxNotFound(id));
    }
    SandboxMetadata& m = it->second;
    for (std::size_t i = 0; i < expected_states.size(); ++i) {
        if (m.state == expected_states[i]) {
            SandboxState prev = m.state;
            m.state = new_state;
            return prev;
        }
    }
    return core::make_unexpected(
        StoreError::StateConflict(id, expected_states, m.state));
}

core::Expected<core::Optional<SandboxMetadata>, StoreError>
InMemoryMetadataStore::Get(const core::SandboxId& id) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = items_.find(id.ToString());
    if (it == items_.end()) {
        return core::Optional<SandboxMetadata>(core::nullopt);
    }
    return core::Optional<SandboxMetadata>(it->second);
}

core::Expected<core::Optional<SandboxMetadata>, StoreError>
InMemoryMetadataStore::Remove(const core::SandboxId& id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = items_.find(id.ToString());
    if (it == items_.end()) {
        return core::Optional<SandboxMetadata>(core::nullopt);
    }
    SandboxMetadata meta = it->second;
    items_.erase(it);
    return core::Optional<SandboxMetadata>(meta);
}

bool InMemoryMetadataStore::MatchesFilter_(
    const SandboxMetadata& m, const SandboxListFilter& f) const {
    // states whitelist
    if (f.states) {
        bool found = false;
        for (std::size_t i = 0; i < f.states->size(); ++i) {
            if (m.state == (*f.states)[i]) { found = true; break; }
        }
        if (!found) return false;
    }
    // excluded_states blacklist
    if (f.excluded_states) {
        for (std::size_t i = 0; i < f.excluded_states->size(); ++i) {
            if (m.state == (*f.excluded_states)[i]) return false;
        }
    }
    // user_metadata subset match
    if (f.user_metadata) {
        const auto& required = *f.user_metadata;
        for (auto it = required.begin(); it != required.end(); ++it) {
            auto found = m.user_metadata.find(it->first);
            if (found == m.user_metadata.end()) return false;
            if (found->second != it->second) return false;
        }
    }
    // started_after
    if (f.started_after_ms && m.created_at_ms < *f.started_after_ms) {
        return false;
    }
    // template filter (snapshot_id or snapshot_alias)
    if (f.template_filter) {
        const std::string& t = *f.template_filter;
        bool match = (m.snapshot_id == t) ||
                     (m.snapshot_alias && *m.snapshot_alias == t) ||
                     (m.template_id == t);
        if (!match) return false;
    }
    return true;
}

core::Expected<std::vector<SandboxMetadata>, StoreError>
InMemoryMetadataStore::List() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<SandboxMetadata> out;
    out.reserve(items_.size());
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        out.push_back(it->second);
    }
    return out;
}

core::Expected<std::vector<SandboxMetadata>, StoreError>
InMemoryMetadataStore::ListFiltered(const SandboxListFilter& filter) const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<SandboxMetadata> out;
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (MatchesFilter_(it->second, filter)) {
            out.push_back(it->second);
        }
    }
    return out;
}

core::Expected<std::vector<SandboxMetadata>, StoreError>
InMemoryMetadataStore::ListExpired(int64_t now_ms) const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<SandboxMetadata> out;
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->second.IsExpired(now_ms)) out.push_back(it->second);
    }
    return out;
}

core::Expected<std::vector<core::SandboxId>, StoreError>
InMemoryMetadataStore::ListIds() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<core::SandboxId> out;
    out.reserve(items_.size());
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        out.push_back(it->second.id);
    }
    return out;
}

}  // namespace orchestrator
}  // namespace agentenv
