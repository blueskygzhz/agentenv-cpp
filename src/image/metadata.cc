// SPDX-License-Identifier: MIT
// Rust: src/image/metadata.rs
#include "agentenv/image/metadata.h"

namespace agentenv {
namespace image {

std::map<std::string, std::string> EnvVarsFromEntries(
    const std::vector<std::string>& entries) {
    std::map<std::string, std::string> out;
    for (const std::string& kv : entries) {
        auto eq = kv.find('=');
        if (eq == std::string::npos) continue;
        out[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    return out;
}

}  // namespace image
}  // namespace agentenv
