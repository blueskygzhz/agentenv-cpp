// SPDX-License-Identifier: MIT
// Rust: `confique + toml`.
// C++11: minimal TOML subset loader; produces strongly typed config objects.
#ifndef AGENTENV_CORE_CONFIG_H_
#define AGENTENV_CORE_CONFIG_H_

#include <cstdint>
#include <string>
#include <unordered_map>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"

namespace agentenv {
namespace core {

/// Rust file: src/cfg.rs
struct Config {
    std::string server_bind_addr = "0.0.0.0:8080";
    std::string data_dir         = "/var/lib/agentenv";
    int         log_level        = 2;  // 0..4 = trace..error
    int         num_workers      = 0;  // 0 = hw threads

    // Backend selection — the equivalent of Rust `SandboxBackendKind`.
    std::string sandbox_backend  = "mock";  // "mock" | "firecracker"

    // Snapshot repo:
    std::string snapshot_repo_kind = "local"; // "local" | "s3" | "mock"
    std::string snapshot_repo_path = "/var/lib/agentenv/snapshots";

    // Storage:
    bool  overlaybd_enable = false;
    bool  ublk_enable      = false;

    static Expected<Config, AnyError> Load(const std::string& toml_path);
    static Expected<Config, AnyError> ParseString(const std::string& toml_text);
};

}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_CONFIG_H_
