// SPDX-License-Identifier: MIT
#include "agentenv/orchestrator/persistence.h"

#include <mutex>
#include <unordered_map>

namespace agentenv {
namespace orchestrator {

class MemoryPersister final : public Persister {
 public:
    core::Expected<core::Unit, core::AnyError> Put(const Sandbox& s) override {
        std::lock_guard<std::mutex> lg(mu_);
        store_[s.id.ToString()] = s;
        return core::Unit{};
    }
    core::Expected<Sandbox, core::AnyError> Get(core::SandboxId id) override {
        std::lock_guard<std::mutex> lg(mu_);
        auto it = store_.find(id.ToString());
        if (it == store_.end()) return core::make_unexpected(core::err("not found"));
        return it->second;
    }
    core::Expected<std::vector<Sandbox>, core::AnyError> List() override {
        std::lock_guard<std::mutex> lg(mu_);
        std::vector<Sandbox> v;
        v.reserve(store_.size());
        for (auto& kv : store_) v.push_back(kv.second);
        return v;
    }
    core::Expected<core::Unit, core::AnyError> Delete(core::SandboxId id) override {
        std::lock_guard<std::mutex> lg(mu_);
        store_.erase(id.ToString());
        return core::Unit{};
    }
 private:
    std::mutex mu_;
    std::unordered_map<std::string, Sandbox> store_;
};

std::unique_ptr<Persister> MakePersister(const std::string& kind,
                                          const std::string& /*path*/) {
    if (kind == "memory") return std::unique_ptr<Persister>(new MemoryPersister());
#ifdef AGENTENV_WITH_ROCKSDB
    // TODO: RocksdbPersister
#endif
    return std::unique_ptr<Persister>(new MemoryPersister());
}

}  // namespace orchestrator
}  // namespace agentenv
