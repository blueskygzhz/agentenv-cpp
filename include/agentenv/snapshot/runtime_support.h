// SPDX-License-Identifier: MIT
// Rust: src/snapshot/runtime_support.rs
#ifndef AGENTENV_SNAPSHOT_RUNTIME_SUPPORT_H_
#define AGENTENV_SNAPSHOT_RUNTIME_SUPPORT_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace snapshot {

/// Rust struct `RuntimeVersions` (subset).
struct RuntimeVersions {
    std::string kernel_version;
    std::string firecracker_version;
    std::string overlaybd_version;
    std::string envd_version;
};

/// Rust fn: detect the runtime versions supported on this host.
core::Expected<RuntimeVersions, std::string> DetectRuntimeVersions();

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_RUNTIME_SUPPORT_H_
