// SPDX-License-Identifier: MIT
#include "agentenv/sandbox/mock.h"

#include <chrono>

namespace agentenv {
namespace sandbox {

MockBackend::MockBackend() = default;
MockBackend::~MockBackend() = default;

// Small helper: make a ready future from a value.
template <typename T>
static std::future<core::Expected<T, core::AnyError>> MakeReady(core::Expected<T, core::AnyError> v) {
    std::promise<core::Expected<T, core::AnyError>> p;
    p.set_value(std::move(v));
    return p.get_future();
}

std::future<core::Expected<Handle, core::AnyError>>
MockBackend::Boot(LaunchPlan plan) {
    Handle h;
    h.sandbox_id = plan.sandbox_id;
    h.backend_kind = "mock";
    h.control_endpoint = "mock://" + plan.sandbox_id.ToString();
    h.boot_ms = 1;

    {
        std::lock_guard<std::mutex> lg(mu_);
        live_[plan.sandbox_id.ToString()] = Entry{h, false};
    }
    return MakeReady<Handle>(core::Expected<Handle, core::AnyError>(h));
}

std::future<core::Expected<core::Unit, core::AnyError>>
MockBackend::Shutdown(core::SandboxId id) {
    {
        std::lock_guard<std::mutex> lg(mu_);
        live_.erase(id.ToString());
    }
    return MakeReady<core::Unit>(core::Expected<core::Unit, core::AnyError>(core::Unit{}));
}

std::future<core::Expected<core::Unit, core::AnyError>>
MockBackend::Pause(core::SandboxId id) {
    std::lock_guard<std::mutex> lg(mu_);
    auto it = live_.find(id.ToString());
    if (it == live_.end())
        return MakeReady<core::Unit>(core::make_unexpected(core::err("mock: not found")));
    it->second.paused = true;
    return MakeReady<core::Unit>(core::Expected<core::Unit, core::AnyError>(core::Unit{}));
}

std::future<core::Expected<core::Unit, core::AnyError>>
MockBackend::Resume(core::SandboxId id) {
    std::lock_guard<std::mutex> lg(mu_);
    auto it = live_.find(id.ToString());
    if (it == live_.end())
        return MakeReady<core::Unit>(core::make_unexpected(core::err("mock: not found")));
    it->second.paused = false;
    return MakeReady<core::Unit>(core::Expected<core::Unit, core::AnyError>(core::Unit{}));
}

std::future<core::Expected<std::string, core::AnyError>>
MockBackend::Snapshot(core::SandboxId id, const std::string& out_dir) {
    return MakeReady<std::string>(
        core::Expected<std::string, core::AnyError>(out_dir + "/" + id.ToString() + ".snap"));
}

std::future<core::Expected<Handle, core::AnyError>>
MockBackend::Restore(LaunchPlan plan, const std::string& /*snapshot_dir*/) {
    return Boot(std::move(plan));
}

std::future<core::Expected<ExecResult, core::AnyError>>
MockBackend::Exec(core::SandboxId /*id*/, ExecSpec spec) {
    ExecResult r;
    r.exit_code = 0;
    // Echo the command for deterministic testing.
    for (const auto& s : spec.cmd) r.stdout_output += s + " ";
    if (!r.stdout_output.empty()) r.stdout_output.pop_back();
    r.stdout_output += "\n";
    return MakeReady<ExecResult>(core::Expected<ExecResult, core::AnyError>(r));
}

}  // namespace sandbox
}  // namespace agentenv
