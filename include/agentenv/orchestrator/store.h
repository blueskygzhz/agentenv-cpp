// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/store/{mod,metadata,in_memory}.rs
#ifndef AGENTENV_ORCHESTRATOR_STORE_H_
#define AGENTENV_ORCHESTRATOR_STORE_H_

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/time.h"
#include "agentenv/orchestrator/types.h"
#include "agentenv/sandbox/types.h"

namespace agentenv {
namespace sandbox {
/// Declared in agentenv/sandbox/backend.h. Only held behind a shared_ptr here,
/// which does not require the complete type.
class PausedSandboxState;
}  // namespace sandbox

namespace orchestrator {

/// Rust enum `SandboxTimeoutAction` (store/metadata.rs).
enum class SandboxTimeoutAction {
    Pause,
    Delete,
};

/// Rust enum `NewTimeout` (store/metadata.rs).
///
/// Rust: `UseExisting | Set(Duration) | EnsureMinimum(Duration) | None`
/// C++11: tagged union expressed as a struct — only the tag relevant field is live.
struct NewTimeout {
    enum class Kind { UseExisting, Set, EnsureMinimum, None };

    Kind    kind        = Kind::None;
    int64_t duration_ms = 0;  // live when kind == Set or EnsureMinimum

    static NewTimeout UseExisting()                   { NewTimeout t; t.kind = Kind::UseExisting; return t; }
    static NewTimeout Set(int64_t ms)                 { NewTimeout t; t.kind = Kind::Set;          t.duration_ms = ms; return t; }
    static NewTimeout EnsureMinimum(int64_t ms)       { NewTimeout t; t.kind = Kind::EnsureMinimum; t.duration_ms = ms; return t; }
    static NewTimeout None()                          { NewTimeout t; t.kind = Kind::None;          return t; }
};

/// Rust struct `SandboxMetadata` (store/metadata.rs).
///
/// Fields align with the Rust struct field-for-field; fields not yet used by
/// the C++ service implementations carry their zero/default values.
struct SandboxMetadata {
    core::SandboxId  id;
    bool             template_builder = false;
    std::string      snapshot_id;
    core::Optional<std::string> snapshot_alias;
    SandboxState     state          = SandboxState::Creating;
    int64_t          created_at_ms  = 0;
    core::Optional<int64_t> timeout_ms;        // Rust `timeout: Option<Duration>`
    SandboxTimeoutAction    timeout_action = SandboxTimeoutAction::Pause;
    core::Optional<int64_t> expires_at_ms;     // Rust `expires_at: Option<SystemTime>`
    bool             auto_resume    = false;
    sandbox::SandboxResources resources;       // replaces cpu_count/memory_mib/disk_size_mib
    std::string      template_id;              // carried over from port's scaffold
    std::unordered_map<std::string, std::string> user_metadata;
    bool             secure = false;

    /// Rust `volume_mounts: HashMap<String, String>` — requested volume mounts
    /// keyed by guest path, valued by volume id.
    std::unordered_map<std::string, std::string> volume_mounts;

    /// Rust `paused_state: Option<Arc<dyn PausedSandboxState>>`.
    ///
    /// Set while the sandbox is Paused; the orchestrator treats it as opaque
    /// and hands it back to the backend factory on resume.
    std::shared_ptr<sandbox::PausedSandboxState> paused_state;

    // ---- Rust `SandboxMetadata::set_timeout` / `update_timeout` / `is_expired` ----

    /// Rust `fn set_timeout(&mut self, timeout: Option<Duration>)`.
    void SetTimeout(core::Optional<int64_t> timeout_ms_val);

    /// Rust `fn update_timeout(&mut self, new_timeout: NewTimeout)`.
    void UpdateTimeout(NewTimeout new_timeout);

    /// Rust `fn is_expired(&self, now: SystemTime) -> bool`.
    bool IsExpired(int64_t now_ms) const;
};

/// Rust struct `SandboxListFilter` (store/mod.rs).
struct SandboxListFilter {
    core::Optional<std::vector<SandboxState>> states;
    core::Optional<std::vector<SandboxState>> excluded_states;
    core::Optional<std::unordered_map<std::string, std::string>> user_metadata;
    core::Optional<int64_t> started_after_ms;
    /// Rust field is named `template`, which is a C++ keyword.
    core::Optional<std::string> template_filter;

    /// Rust `SandboxListFilter::matches_all()`.
    static SandboxListFilter MatchesAll() { return SandboxListFilter{}; }
};

/// Rust struct `MetadataUpdateResult` (store/mod.rs).
struct MetadataUpdateResult {
    SandboxMetadata previous;
    SandboxMetadata current;
};

/// Rust enum `StoreError` (store/mod.rs).
struct StoreError {
    enum class Kind { Backend, SandboxNotFound, SandboxAlreadyExists, StateConflict };
    Kind        kind = Kind::Backend;
    core::SandboxId sandbox_id;
    std::string detail;
    std::vector<SandboxState> expected_states;  // for StateConflict
    SandboxState actual_state = SandboxState::Creating;

    static StoreError Backend(std::string msg);
    static StoreError SandboxNotFound(core::SandboxId id);
    static StoreError SandboxAlreadyExists(core::SandboxId id);
    static StoreError StateConflict(core::SandboxId id,
                                    std::vector<SandboxState> expected,
                                    SandboxState actual);

    std::string Message() const;
};

/// Rust trait `MetadataStore` (store/mod.rs).
class MetadataStore {
 public:
    virtual ~MetadataStore() {}

    virtual core::Expected<core::Unit, StoreError>
        Add(SandboxMetadata meta) = 0;
    virtual core::Expected<core::Unit, StoreError>
        Update(SandboxMetadata meta) = 0;
    /// Rust `update_state_if_state` — returns the PREVIOUS state on success.
    virtual core::Expected<SandboxState, StoreError>
        UpdateStateIfState(const core::SandboxId& id,
                           SandboxState new_state,
                           const std::vector<SandboxState>& expected_states) = 0;
    virtual core::Expected<core::Optional<SandboxMetadata>, StoreError>
        Get(const core::SandboxId& id) const = 0;
    virtual core::Expected<core::Optional<SandboxMetadata>, StoreError>
        Remove(const core::SandboxId& id) = 0;
    virtual core::Expected<std::vector<SandboxMetadata>, StoreError>
        List() const = 0;
    virtual core::Expected<std::vector<SandboxMetadata>, StoreError>
        ListFiltered(const SandboxListFilter& filter) const = 0;
    virtual core::Expected<std::vector<SandboxMetadata>, StoreError>
        ListExpired(int64_t now_ms) const = 0;
    virtual core::Expected<std::vector<core::SandboxId>, StoreError>
        ListIds() const = 0;

    /// Rust `update_if_state` — atomically mutates the stored record via the
    /// callback when the current state is one of `expected_states`.
    ///
    /// Rust documents the callback as synchronous by design because
    /// implementations may run it while holding the metadata lock; the same
    /// constraint applies here.
    virtual core::Expected<MetadataUpdateResult, StoreError>
        UpdateIfState(const core::SandboxId& id,
                      const std::vector<SandboxState>& expected_states,
                      const std::function<void(SandboxMetadata*)>& update) = 0;

    /// Rust `wait_while_in_states` — blocks until the sandbox leaves every one
    /// of `transitional_states`, then returns its metadata. Yields an unset
    /// optional when the sandbox is removed while waiting or never existed.
    ///
    /// `timeout_ms <= 0` waits indefinitely. A timeout yields
    /// `StoreError::Backend("wait timed out")`, which the orchestrator maps
    /// onto `InvalidSandboxState` (Rust does this via `tokio::time::timeout`).
    virtual core::Expected<core::Optional<SandboxMetadata>, StoreError>
        WaitWhileInStates(const core::SandboxId& id,
                          const std::vector<SandboxState>& transitional_states,
                          int64_t timeout_ms) = 0;

    // --- kept for backward compat with scaffold code using old interface ---
    /// Legacy: maps onto Add().
    virtual core::Expected<core::Unit, StoreError>
        Insert(SandboxMetadata meta) { return Add(std::move(meta)); }
};

/// Rust struct `InMemoryMetadataStore` (store/in_memory.rs).
class InMemoryMetadataStore : public MetadataStore {
 public:
    core::Expected<core::Unit, StoreError>       Add(SandboxMetadata meta) override;
    core::Expected<core::Unit, StoreError>       Update(SandboxMetadata meta) override;
    core::Expected<SandboxState, StoreError>     UpdateStateIfState(
        const core::SandboxId& id,
        SandboxState new_state,
        const std::vector<SandboxState>& expected_states) override;
    core::Expected<core::Optional<SandboxMetadata>, StoreError>
        Get(const core::SandboxId& id) const override;
    core::Expected<core::Optional<SandboxMetadata>, StoreError>
        Remove(const core::SandboxId& id) override;
    core::Expected<std::vector<SandboxMetadata>, StoreError> List() const override;
    core::Expected<std::vector<SandboxMetadata>, StoreError>
        ListFiltered(const SandboxListFilter& filter) const override;
    core::Expected<std::vector<SandboxMetadata>, StoreError>
        ListExpired(int64_t now_ms) const override;
    core::Expected<std::vector<core::SandboxId>, StoreError> ListIds() const override;
    core::Expected<MetadataUpdateResult, StoreError>
        UpdateIfState(const core::SandboxId& id,
                      const std::vector<SandboxState>& expected_states,
                      const std::function<void(SandboxMetadata*)>& update) override;
    core::Expected<core::Optional<SandboxMetadata>, StoreError>
        WaitWhileInStates(const core::SandboxId& id,
                          const std::vector<SandboxState>& transitional_states,
                          int64_t timeout_ms) override;

 private:
    bool MatchesFilter_(const SandboxMetadata& m, const SandboxListFilter& f) const;

    mutable std::mutex mu_;
    /// Rust uses one `watch` channel per sandbox; a single condition variable
    /// broadcast on every state change is the synchronous equivalent. Waiters
    /// re-check their own predicate on each wake, so sharing it is sound.
    mutable std::condition_variable state_cv_;
    std::map<std::string, SandboxMetadata> items_;
};

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_STORE_H_
