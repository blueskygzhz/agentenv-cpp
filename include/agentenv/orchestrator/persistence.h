// SPDX-License-Identifier: MIT
// Rust: src/orchestrator/persistence/  — pluggable storage of Sandbox records.
#ifndef AGENTENV_ORCHESTRATOR_PERSISTENCE_H_
#define AGENTENV_ORCHESTRATOR_PERSISTENCE_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"
#include "agentenv/orchestrator/types.h"

namespace agentenv {
namespace orchestrator {

class Persister {
 public:
    virtual ~Persister() = default;

    virtual core::Expected<core::Unit, core::AnyError> Put(const Sandbox& s) = 0;
    virtual core::Expected<Sandbox, core::AnyError>    Get(core::SandboxId id) = 0;
    virtual core::Expected<std::vector<Sandbox>, core::AnyError> List() = 0;
    virtual core::Expected<core::Unit, core::AnyError> Delete(core::SandboxId id) = 0;
};

/// "memory" — in-process map (used by tests + skeleton main).
/// "rocksdb" — RocksDB-backed (compiled when AGENTENV_WITH_ROCKSDB=ON).
std::unique_ptr<Persister> MakePersister(const std::string& kind,
                                          const std::string& path);

}  // namespace orchestrator
}  // namespace agentenv
#endif  // AGENTENV_ORCHESTRATOR_PERSISTENCE_H_
