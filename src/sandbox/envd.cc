// SPDX-License-Identifier: MIT
// Rust: src/sandbox/envd.rs
//
// EnvdClient stays an interface (its concrete impl speaks gRPC to the in-guest
// daemon). EnvdInstance::wait_for_ready is pure timing logic and is ported
// faithfully here, with the health probe injected so it is testable.
#include "agentenv/sandbox/envd.h"

#include <algorithm>
#include <ctime>

namespace agentenv {
namespace sandbox {

namespace {
int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
void sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, nullptr);
}
}  // namespace

EnvdInstance::EnvdInstance(std::string base_path)
    : base_path_(base_path), grpc_address_(base_path) {}

core::Expected<core::Unit, std::string>
EnvdInstance::WaitForReady(int64_t timeout_ms, int64_t retry_interval_ms,
                           const HealthProbe& probe) {
    const int64_t start = now_ms();

    for (;;) {
        const int64_t elapsed = now_ms() - start;
        if (elapsed >= timeout_ms) {
            return core::make_unexpected(std::string("timed out waiting for envd"));
        }

        const int64_t remaining = timeout_ms - elapsed;
        const int64_t probe_timeout =
            std::min<int64_t>(kHealthProbeTimeoutMs, remaining);

        bool healthy = false;
        if (probe) {
            healthy = probe(static_cast<int32_t>(probe_timeout));
        }
        if (healthy) {
            return core::Unit{};
        }

        // Rust: remaining = timeout.saturating_sub(elapsed); if zero -> timeout.
        const int64_t remaining_after = timeout_ms - (now_ms() - start);
        if (remaining_after <= 0) {
            return core::make_unexpected(std::string("timed out waiting for envd"));
        }
        sleep_ms(std::min<int64_t>(retry_interval_ms, remaining_after));
    }
}

}  // namespace sandbox
}  // namespace agentenv
