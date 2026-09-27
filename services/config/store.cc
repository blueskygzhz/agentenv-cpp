// SPDX-License-Identifier: MIT
// Rust upstream: services/config/store.go
#include "services/config/store.h"

namespace agentenv { namespace services { namespace config {

core::Expected<std::string, std::string>
Store::Get(const std::string& key) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = items_.find(key);
    if (it == items_.end()) return core::make_unexpected(std::string("not found"));
    return it->second;
}
core::Expected<core::Unit, std::string>
Store::Put(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> g(mu_);
    items_[key] = value;
    return core::Unit{};
}

}}}
