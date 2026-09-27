// SPDX-License-Identifier: MIT
// Rust: src/sandbox/envd.rs — guest-agent (envd) client + EnvdInstance.
#ifndef AGENTENV_SANDBOX_ENVD_H_
#define AGENTENV_SANDBOX_ENVD_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/sandbox/types.h"

namespace agentenv {
namespace sandbox {

/// Rust re-export `envd::process::Signal`.
enum class Signal : int32_t {
    TERM = 15,
    KILL = 9,
    INT  = 2,
    HUP  = 1,
};

/// Rust client of the in-guest envd agent (streams process I/O over gRPC).
class EnvdClient {
 public:
    virtual ~EnvdClient() {}
    virtual core::Expected<ExecResult, std::string>
        Exec(const ExecSpec& spec) = 0;
    virtual core::Expected<core::Unit, std::string>
        Signal(int64_t pid, sandbox::Signal sig) = 0;
    virtual core::Expected<core::Unit, std::string>
        WaitReady(int32_t timeout_ms) = 0;
};

/// Rust struct `EnvdInstance` — bootstrap handle to a guest envd daemon.
///
/// The Rust type wraps a reqwest/gRPC configuration. This port keeps the same
/// shape (base_path + grpc_address) and faithfully ports the deadline-bounded
/// readiness loop `wait_for_ready`, which is pure timing logic. The actual HTTP
/// health probe is injected as a callback so the loop can be unit tested without
/// a real network stack (mirrors the Rust `readiness_deadline_bounds_a_hung_
/// health_probe` test).
class EnvdInstance {
 public:
    /// A single health probe attempt bounded by `probe_timeout_ms`.
    /// Returns true when envd is healthy. Should itself respect the timeout.
    typedef std::function<bool(int32_t probe_timeout_ms)> HealthProbe;

    explicit EnvdInstance(std::string base_path);

    const std::string& BasePath() const { return base_path_; }
    const std::string& GrpcAddress() const { return grpc_address_; }

    /// Rust `wait_for_ready(timeout, retry_interval)`.
    /// Loops probing until healthy or the deadline elapses. HEALTH_PROBE_TIMEOUT
    /// (1s) caps each probe; retry_interval spaces attempts. `probe` is invoked
    /// for each attempt; when null a default always-fail probe is used.
    core::Expected<core::Unit, std::string>
        WaitForReady(int64_t timeout_ms, int64_t retry_interval_ms,
                     const HealthProbe& probe);

    /// Rust `HEALTH_PROBE_TIMEOUT` (1s).
    static const int64_t kHealthProbeTimeoutMs = 1000;

 private:
    std::string base_path_;
    std::string grpc_address_;
};

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_ENVD_H_
