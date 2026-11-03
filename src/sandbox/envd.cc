// SPDX-License-Identifier: MIT
// Rust: src/sandbox/envd.rs
//
// EnvdClient stays an interface (its concrete impl speaks gRPC to the in-guest
// daemon). EnvdInstance::wait_for_ready is pure timing logic and is ported
// faithfully here, with the health probe injected so it is testable.
#include "agentenv/sandbox/envd.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>

#include "agentenv/core/http.h"

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

// `std::min` takes its arguments by const reference, which odr-uses the
// constant below. An in-class initialiser alone is only a compile-time value;
// without this definition the reference has nothing to bind to and the link
// fails.
const int64_t EnvdInstance::kHealthProbeTimeoutMs;

EnvdInstance::EnvdInstance(std::string base_path)
    : base_path_(base_path), grpc_address_(base_path) {}

EnvdInstance::EnvdInstance(std::string base_path,
                           const core::Optional<EnvdAccessToken>& access_token)
    : base_path_(base_path), grpc_address_(base_path), access_token_(access_token) {}

namespace {

/// `http://<host>:<port>[/prefix]` -> target + path prefix. Only the form
/// `init_envd_instance` builds is accepted.
bool ParseHttpBase(const std::string& base, core::http::Target* target, std::string* prefix) {
    const std::string scheme = "http://";
    if (base.compare(0, scheme.size(), scheme) != 0) return false;
    const std::string rest = base.substr(scheme.size());
    const std::string::size_type slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    *prefix = slash == std::string::npos ? std::string() : rest.substr(slash);
    while (!prefix->empty() && (*prefix)[prefix->size() - 1] == '/') {
        prefix->erase(prefix->size() - 1);
    }
    const std::string::size_type colon = authority.rfind(':');
    std::string host = authority;
    unsigned long port = 80;
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        const std::string digits = authority.substr(colon + 1);
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        port = std::strtoul(digits.c_str(), NULL, 10);
        if (port == 0 || port > 65535) return false;
    }
    if (host.empty()) return false;
    *target = core::http::Target::Tcp(host, static_cast<uint16_t>(port));
    return true;
}

}  // namespace

EnvdInstance::HealthProbe EnvdInstance::HttpHealthProbe() const {
    const std::string base = base_path_;
    return [base](int32_t probe_timeout_ms) -> bool {
        core::http::Target target;
        std::string prefix;
        if (!ParseHttpBase(base, &target, &prefix)) return false;
        core::http::Timeouts timeouts;
        timeouts.connect_ms = probe_timeout_ms;
        timeouts.read_ms = probe_timeout_ms;
        core::http::Request request;
        request.method = "GET";
        request.path = prefix + "/health";
        const core::Expected<core::http::Response, std::string> response =
            core::http::HttpClient(target, timeouts).Request(request);
        // reqwest `!is_client_error() && !is_server_error()`.
        return response.ok() && response.value().status < 400;
    };
}

core::Expected<core::Unit, std::string>
EnvdInstance::WaitForReady(int64_t timeout_ms, int64_t retry_interval_ms) {
    return WaitForReady(timeout_ms, retry_interval_ms, HttpHealthProbe());
}

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
