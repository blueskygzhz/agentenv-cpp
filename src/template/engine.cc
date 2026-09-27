// SPDX-License-Identifier: MIT
#include "agentenv/template/engine.h"

#include <mutex>
#include <unordered_map>

namespace agentenv {
namespace tpl {

class MemoryEngine final : public Engine {
 public:
    core::Expected<Template, core::AnyError> Get(const std::string& id) override {
        std::lock_guard<std::mutex> lg(mu_);
        auto it = m_.find(id);
        if (it == m_.end()) return core::make_unexpected(core::err("not found: " + id));
        return it->second;
    }
    core::Expected<std::vector<Template>, core::AnyError> List() override {
        std::lock_guard<std::mutex> lg(mu_);
        std::vector<Template> v;
        v.reserve(m_.size());
        for (auto& kv : m_) v.push_back(kv.second);
        return v;
    }
    core::Expected<core::Unit, core::AnyError> Put(Template t) override {
        std::lock_guard<std::mutex> lg(mu_);
        m_[t.id] = std::move(t);
        return core::Unit{};
    }
 private:
    std::mutex mu_;
    std::unordered_map<std::string, Template> m_;
};

std::unique_ptr<Engine> MakeMemoryEngine() {
    return std::unique_ptr<Engine>(new MemoryEngine());
}

}  // namespace tpl
}  // namespace agentenv
