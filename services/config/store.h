// SPDX-License-Identifier: MIT
// Rust upstream: services/config/*.go — cluster-wide config service.
#ifndef AGENTENV_SERVICES_CONFIG_H_
#define AGENTENV_SERVICES_CONFIG_H_

#include <map>
#include <mutex>
#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace services {
namespace config {

/// Rust upstream: services/config/store.go — versioned KV.
class Store {
 public:
    core::Expected<std::string, std::string> Get(const std::string& key) const;
    core::Expected<core::Unit, std::string>  Put(const std::string& key,
                                                 const std::string& value);
 private:
    mutable std::mutex mu_;
    std::map<std::string, std::string> items_;
};

}}}
#endif
