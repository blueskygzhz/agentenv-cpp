// SPDX-License-Identifier: MIT
// Rust: src/snapshot/runtime_support.rs
#include "agentenv/snapshot/runtime_support.h"
namespace agentenv { namespace snapshot {
core::Expected<RuntimeVersions, std::string> DetectRuntimeVersions() {
    RuntimeVersions v;
    v.kernel_version = "unknown";
    v.firecracker_version = "unknown";
    v.overlaybd_version = "unknown";
    v.envd_version = "unknown";
    return v;
}
}}
