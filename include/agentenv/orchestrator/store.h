// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/store.rs — the in-memory metadata store.
#ifndef AGENTENV_ORCHESTRATOR_STORE_H_
#define AGENTENV_ORCHESTRATOR_STORE_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/orchestrator/types.h"

namespace agentenv {
namespace orchestrator {

/// Rust struct `SandboxTimeoutAction`.
enum class SandboxTimeoutAction {
    Delete,
    Pause,
};

/// Rust struct `NewTimeout`.
struct NewTimeout {
    int64_t              timeout_ms = 0;
    SandboxTimeoutAction action     = SandboxTimeoutAction::Delete;
};

/// Rust struct `SandboxMetadata` — the durable per-sandbox record.
struct SandboxMetadata {
    core::SandboxId id;
    LifecyclePhase  state          = LifecyclePhase::Created;
    uint32_t        cpu_count      = 1;
    uint32_t        memory_mib     = 128;
    uint32_t        disk_size_mib  = 0;
    int64_t         created_at_ms  = 0;
    int64_t         updated_at_ms  = 0;
    NewTimeout      timeout;
    std::string     template_id;
};

/// Rust struct `SandboxListFilter`.
struct SandboxListFilter {
    bool include_paused  = true;
    bool include_stopped = false;
};

/// Rust enum `StoreError`.
enum class StoreError {
    SandboxNotFound,
    Conflict,
    Internal,
};

/// Rust trait `MetadataStore`.
class MetadataStore {
 public:
    virtual ~MetadataStore() {}
    virtual core::Expected<core::Unit, StoreError>
        Insert(SandboxMetadata meta) = 0;
    virtual core::Expected<SandboxMetadata, StoreError>
        Get(const core::SandboxId& id) const = 0;
    virtual core::Expected<core::Unit, StoreError>
        Update(SandboxMetadata meta) = 0;
    virtual core::Expected<core::Unit, StoreError>
        Remove(const core::SandboxId& id) = 0;
    virtual std::vector<SandboxMetadata>
        List(const SandboxListFilter& filter) const = 0;
};

/// Rust struct `InMemoryMetadataStore`.
class InMemoryMetadataStore : public MetadataStore {
 public:
    core::Expected<core::Unit, StoreError>       Insert(SandboxMetadata meta) override;
    core::Expected<SandboxMetadata, StoreError>  Get(const core::SandboxId& id) const override;
    core::Expected<core::Unit, StoreError>       Update(SandboxMetadata meta) override;
    core::Expected<core::Unit, StoreError>       Remove(const core::SandboxId& id) override;
    std::vector<SandboxMetadata>                 List(const SandboxListFilter& filter) const override;

 private:
    mutable std::mutex mu_;
    std::map<std::string, SandboxMetadata> items_;
};

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_STORE_H_
