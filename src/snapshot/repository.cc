// SPDX-License-Identifier: MIT
// Local / mock repository implementations. S3 goes behind AGENTENV_WITH_LIBCURL.
#include "agentenv/snapshot/repository.h"

#include <mutex>
#include <unordered_map>

namespace agentenv {
namespace snapshot {
namespace {

// -------------------- LocalMemRepository (in-memory) --------------------
// NOTE: the public MockRepository lives in mock.h/mock.cc; this file-local
// class is only the default fallback for MakeRepository("mock").
class LocalMemRepository final : public Repository {
 public:
    core::Expected<core::Unit, core::AnyError>
    Put(const SnapshotMeta& meta,
        const std::string& /*mem_file*/,
        const std::string& /*disk_file*/) override {
        std::lock_guard<std::mutex> lg(mu_);
        store_[meta.id.ToString()] = meta;
        return core::Unit{};
    }
    core::Expected<SnapshotMeta, core::AnyError>
    Get(core::SnapshotId id, std::string* /*mem_out*/, std::string* /*disk_out*/) override {
        std::lock_guard<std::mutex> lg(mu_);
        auto it = store_.find(id.ToString());
        if (it == store_.end()) return core::make_unexpected(core::err("not found"));
        return it->second;
    }
    core::Expected<std::vector<SnapshotMeta>, core::AnyError>
    List() override {
        std::lock_guard<std::mutex> lg(mu_);
        std::vector<SnapshotMeta> out;
        out.reserve(store_.size());
        for (auto& kv : store_) out.push_back(kv.second);
        return out;
    }
    core::Expected<core::Unit, core::AnyError>
    Delete(core::SnapshotId id) override {
        std::lock_guard<std::mutex> lg(mu_);
        store_.erase(id.ToString());
        return core::Unit{};
    }

 private:
    std::mutex mu_;
    std::unordered_map<std::string, SnapshotMeta> store_;
};

// -------------------- LocalRepository (filesystem) ------------------
// Skeleton — real impl would use `stat`/`opendir` (no std::filesystem in C++11)
// to write meta.json alongside the mem/disk blobs.
class LocalRepository final : public Repository {
 public:
    explicit LocalRepository(std::string root) : root_(std::move(root)) {}

    core::Expected<core::Unit, core::AnyError>
    Put(const SnapshotMeta& /*meta*/,
        const std::string& /*mem_file*/,
        const std::string& /*disk_file*/) override {
        return core::make_unexpected(core::err("LocalRepository::Put not implemented"));
    }
    core::Expected<SnapshotMeta, core::AnyError>
    Get(core::SnapshotId /*id*/, std::string* /*mem_out*/, std::string* /*disk_out*/) override {
        return core::make_unexpected(core::err("LocalRepository::Get not implemented"));
    }
    core::Expected<std::vector<SnapshotMeta>, core::AnyError>
    List() override {
        return core::make_unexpected(core::err("LocalRepository::List not implemented"));
    }
    core::Expected<core::Unit, core::AnyError>
    Delete(core::SnapshotId /*id*/) override {
        return core::make_unexpected(core::err("LocalRepository::Delete not implemented"));
    }

 private:
    std::string root_;
};

}  // namespace (anonymous)

std::unique_ptr<Repository> MakeRepository(const std::string& kind, const std::string& path) {
    if (kind == "local") return std::unique_ptr<Repository>(new LocalRepository(path));
    if (kind == "mock")  return std::unique_ptr<Repository>(new LocalMemRepository());
    // Fallback.
    return std::unique_ptr<Repository>(new LocalMemRepository());
}

}  // namespace snapshot
}  // namespace agentenv
