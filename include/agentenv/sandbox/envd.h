// SPDX-License-Identifier: MIT
// Rust: src/sandbox/envd.rs — guest-agent (envd) client + EnvdInstance.
#ifndef AGENTENV_SANDBOX_ENVD_H_
#define AGENTENV_SANDBOX_ENVD_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/access.h"
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
/// shape (base_path + grpc_address + access token) and faithfully ports the
/// deadline-bounded readiness loop `wait_for_ready`. The probe is a callback so
/// the loop can be unit tested without a guest (mirrors the Rust
/// `readiness_deadline_bounds_a_hung_health_probe` test); `HttpHealthProbe` is
/// the real one.
class EnvdInstance {
 public:
    /// A single health probe attempt bounded by `probe_timeout_ms`.
    /// Returns true when envd is healthy. Should itself respect the timeout.
    typedef std::function<bool(int32_t probe_timeout_ms)> HealthProbe;

    explicit EnvdInstance(std::string base_path);
    /// Rust `EnvdInstance::new(base_path, access_token)`.
    EnvdInstance(std::string base_path, const core::Optional<EnvdAccessToken>& access_token);

    const std::string& BasePath() const { return base_path_; }
    const std::string& GrpcAddress() const { return grpc_address_; }
    const core::Optional<EnvdAccessToken>& AccessToken() const { return access_token_; }

    /// Rust `default_api::health_get` under `tokio::time::timeout` --
    /// `GET {base_path}/health` on a fresh connection (Rust's bootstrap client
    /// keeps no idle connections: the address may belong to the previous VM
    /// that held this slot). Healthy unless the transport fails or the status
    /// is a client or server error.
    HealthProbe HttpHealthProbe() const;

    /// Rust `wait_for_ready` with the real HTTP probe.
    core::Expected<core::Unit, std::string> WaitForReady(int64_t timeout_ms,
                                                         int64_t retry_interval_ms);

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
    core::Optional<EnvdAccessToken> access_token_;
};

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_ENVD_H_
