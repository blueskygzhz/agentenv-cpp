// SPDX-License-Identifier: MIT
// Rust: src/snapshot/manager.rs — orchestrates snapshot create/restore.
#ifndef AGENTENV_SNAPSHOT_MANAGER_H_
#define AGENTENV_SNAPSHOT_MANAGER_H_

#include <memory>
#include <string>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/snapshot/repository.h"

namespace agentenv {
namespace snapshot {

class Manager {
 public:
    Manager(std::shared_ptr<Repository> repo,
            std::shared_ptr<sandbox::Backend> backend);

    /// Pause the sandbox, dump memory + disk delta, upload to the repository.
    core::Expected<SnapshotMeta, core::AnyError>
        Create(core::SandboxId sandbox_id, const std::string& template_id);

    /// Restore a sandbox from a snapshot id.
    core::Expected<sandbox::Handle, core::AnyError>
        Restore(core::SnapshotId snapshot_id,
                const std::string& template_id);

 private:
    std::shared_ptr<Repository>       repo_;
    std::shared_ptr<sandbox::Backend> backend_;
};

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_MANAGER_H_
