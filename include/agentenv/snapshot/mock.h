// SPDX-License-Identifier: MIT
// Rust: src/snapshot/mock.rs — in-memory test double for Repository.
#ifndef AGENTENV_SNAPSHOT_MOCK_H_
#define AGENTENV_SNAPSHOT_MOCK_H_

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "agentenv/snapshot/repository.h"

namespace agentenv {
namespace snapshot {

class MockRepository : public Repository {
 public:
    core::Expected<core::Unit, core::AnyError>
        Put(const SnapshotMeta& meta,
            const std::string& mem_file,
            const std::string& disk_file) override;

    core::Expected<SnapshotMeta, core::AnyError>
        Get(core::SnapshotId id,
            std::string* mem_out,
            std::string* disk_out) override;

    core::Expected<std::vector<SnapshotMeta>, core::AnyError>
        List() override;

    core::Expected<core::Unit, core::AnyError>
        Delete(core::SnapshotId id) override;

 private:
    struct Entry {
        SnapshotMeta meta;
        std::string  mem_path;
        std::string  disk_path;
    };
    mutable std::mutex mu_;
    std::map<std::string, Entry> items_;
};

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_MOCK_H_
