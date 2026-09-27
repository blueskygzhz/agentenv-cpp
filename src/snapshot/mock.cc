// SPDX-License-Identifier: MIT
// Rust: src/snapshot/mock.rs
#include "agentenv/snapshot/mock.h"

namespace agentenv {
namespace snapshot {

core::Expected<core::Unit, core::AnyError>
MockRepository::Put(const SnapshotMeta& meta,
                    const std::string& mem_file,
                    const std::string& disk_file) {
    std::lock_guard<std::mutex> g(mu_);
    Entry e;
    e.meta = meta;
    e.mem_path = mem_file;
    e.disk_path = disk_file;
    items_[meta.id.ToString()] = e;
    return core::Unit{};
}

core::Expected<SnapshotMeta, core::AnyError>
MockRepository::Get(core::SnapshotId id, std::string* mem_out, std::string* disk_out) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = items_.find(id.ToString());
    if (it == items_.end()) {
        return core::make_unexpected(core::AnyError("snapshot not found"));
    }
    if (mem_out)  *mem_out  = it->second.mem_path;
    if (disk_out) *disk_out = it->second.disk_path;
    return it->second.meta;
}

core::Expected<std::vector<SnapshotMeta>, core::AnyError>
MockRepository::List() {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<SnapshotMeta> out;
    out.reserve(items_.size());
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        out.push_back(it->second.meta);
    }
    return out;
}

core::Expected<core::Unit, core::AnyError>
MockRepository::Delete(core::SnapshotId id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = items_.find(id.ToString());
    if (it == items_.end()) {
        return core::make_unexpected(core::AnyError("snapshot not found"));
    }
    items_.erase(it);
    return core::Unit{};
}

}  // namespace snapshot
}  // namespace agentenv
