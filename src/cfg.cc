// SPDX-License-Identifier: MIT
// Rust: src/cfg.rs
#include "agentenv/cfg.h"

#include "agentenv/core/config.h"

namespace agentenv {
namespace cfg {

AppConfig Default() {
    return AppConfig();
}

core::Expected<AppConfig, std::string> LoadFromFile(const std::string& path) {
    auto r = core::Config::Load(path);
    if (!r.ok()) {
        return core::make_unexpected(r.error().chain());
    }
    // A real impl would extract keys; skeleton returns default.
    AppConfig cfg;
    (void)r.value();
    return cfg;
}

}  // namespace cfg
}  // namespace agentenv
