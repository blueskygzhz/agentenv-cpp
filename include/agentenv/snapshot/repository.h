// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/  — Object storage or local FS for snapshots.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"
#include "agentenv/snapshot/types.h"

namespace agentenv {
namespace snapshot {

/// Storage abstraction. Implementations:
///  - LocalRepository  (files under a directory)
///  - S3Repository     (via libcurl — TODO)
///  - MockRepository   (in-memory, for tests)
class Repository {
 public:
    virtual ~Repository() = default;

    virtual core::Expected<core::Unit, core::AnyError>
        Put(const SnapshotMeta& meta,
            const std::string& mem_file,
            const std::string& disk_file) = 0;

    virtual core::Expected<SnapshotMeta, core::AnyError>
        Get(core::SnapshotId id, std::string* mem_out, std::string* disk_out) = 0;

    virtual core::Expected<std::vector<SnapshotMeta>, core::AnyError>
        List() = 0;

    virtual core::Expected<core::Unit, core::AnyError>
        Delete(core::SnapshotId id) = 0;
};

/// Factory dispatched by config.
std::unique_ptr<Repository> MakeRepository(const std::string& kind,
                                            const std::string& path);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_H_
