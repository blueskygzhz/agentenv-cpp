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
    bool state_changed = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = items_.find(meta.id.ToString());
        if (it == items_.end()) {
            return core::make_unexpected(StoreError::SandboxNotFound(meta.id));
        }
        state_changed = (it->second.state != meta.state);
        it->second = meta;
    }
    if (state_changed) state_cv_.notify_all();
    return core::Unit{};
}

core::Expected<SandboxState, StoreError>
InMemoryMetadataStore::UpdateStateIfState(
    const core::SandboxId& id,
    SandboxState new_state,
    const std::vector<SandboxState>& expected_states) {
    SandboxState prev;
    bool changed = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = items_.find(id.ToString());
        if (it == items_.end()) {
            return core::make_unexpected(StoreError::SandboxNotFound(id));
        }
        SandboxMetadata& m = it->second;
        bool matched = false;
        for (std::size_t i = 0; i < expected_states.size(); ++i) {
            if (m.state == expected_states[i]) { matched = true; break; }
        }
        if (!matched) {
            return core::make_unexpected(
                StoreError::StateConflict(id, expected_states, m.state));
        }
        prev    = m.state;
        m.state = new_state;
        changed = (prev != new_state);
    }
    // Rust notifies *after* releasing the write lock, so a woken waiter always
    // observes the state that triggered the notification (or a newer one).
    if (changed) state_cv_.notify_all();
    return prev;
}

// Rust `update_if_state`.
core::Expected<MetadataUpdateResult, StoreError>
InMemoryMetadataStore::UpdateIfState(
    const core::SandboxId& id,
    const std::vector<SandboxState>& expected_states,
    const std::function<void(SandboxMetadata*)>& update) {
    MetadataUpdateResult result;
    bool state_changed = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = items_.find(id.ToString());
        if (it == items_.end()) {
            return core::make_unexpected(StoreError::SandboxNotFound(id));
        }
        SandboxMetadata& record = it->second;
        bool matched = false;
        for (std::size_t i = 0; i < expected_states.size(); ++i) {
            if (record.state == expected_states[i]) { matched = true; break; }
        }
        if (!matched) {
            return core::make_unexpected(
                StoreError::StateConflict(id, expected_states, record.state));
        }

        result.previous = record;
        if (update) update(&record);
        result.current = record;

        state_changed = (result.previous.state != result.current.state);
    }
    if (state_changed) state_cv_.notify_all();
    return result;
}

// Rust `wait_while_in_states`.
core::Expected<core::Optional<SandboxMetadata>, StoreError>
InMemoryMetadataStore::WaitWhileInStates(
    const core::SandboxId& id,
    const std::vector<SandboxState>& transitional_states,
    int64_t timeout_ms) {
    const std::string key = id.ToString();
    std::unique_lock<std::mutex> lk(mu_);

    // Rust subscribes to the record's watch channel and returns Ok(None) when
    // there is no channel — i.e. the sandbox was removed or never existed.
    if (items_.find(key) == items_.end()) {
        return core::Optional<SandboxMetadata>(core::nullopt);
    }

    // Predicate mirrors the Rust closure: satisfied when the sandbox is gone
    // (deleted) OR its state is no longer one of `transitional_states`.
    auto stable_or_gone = [this, &key, &transitional_states]() {
        auto it = items_.find(key);
        if (it == items_.end()) return true;  // deleted
        for (std::size_t i = 0; i < transitional_states.size(); ++i) {
            if (it->second.state == transitional_states[i]) return false;
        }
        return true;
    };

    if (timeout_ms > 0) {
        const bool ok = state_cv_.wait_for(
            lk, std::chrono::milliseconds(timeout_ms), stable_or_gone);
        if (!ok) {
            // Rust maps the elapsed timeout onto InvalidSandboxState at the
            // orchestrator layer; the store reports it as a backend error.
            return core::make_unexpected(StoreError::Backend("wait timed out"));
        }
    } else {
        state_cv_.wait(lk, stable_or_gone);
    }

    auto it = items_.find(key);
    if (it == items_.end()) {
        return core::Optional<SandboxMetadata>(core::nullopt);
    }
    return core::Optional<SandboxMetadata>(it->second);
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
    SandboxMetadata meta;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = items_.find(id.ToString());
        if (it == items_.end()) {
            return core::Optional<SandboxMetadata>(core::nullopt);
        }
        meta = it->second;
        items_.erase(it);
    }
    // Rust sends `None` through the watch channel so waiters see the deletion.
    state_cv_.notify_all();
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
