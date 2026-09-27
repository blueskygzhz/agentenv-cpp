// SPDX-License-Identifier: MIT
#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/firecracker/sandbox.h"
#include "agentenv/sandbox/mock.h"

namespace agentenv {
namespace sandbox {

// Vtable anchor.
// Backend::~Backend() is =default in the header; no anchor needed here.

std::unique_ptr<Backend> MakeBackend(const std::string& kind) {
    if (kind == "mock" || kind.empty()) {
        return std::unique_ptr<Backend>(new MockBackend());
    }
    if (kind == "firecracker") {
        firecracker::Config cfg;
        return std::unique_ptr<Backend>(new firecracker::FirecrackerBackend(cfg));
    }
    // Unknown backend — return mock to keep tests deterministic.
    return std::unique_ptr<Backend>(new MockBackend());
}

}  // namespace sandbox
}  // namespace agentenv
