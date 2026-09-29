// SPDX-License-Identifier: MIT
#include "agentenv/sandbox/mock.h"

#include <utility>

namespace agentenv {
namespace sandbox {

MockBackend::MockBackend() = default;
MockBackend::~MockBackend() = default;

core::Expected<Handle, core::AnyError>
MockBackend::Boot(LaunchPlan plan) {
    Handle h;
    h.sandbox_id = plan.sandbox_id;
    h.backend_kind = "mock";
    h.control_endpoint = "mock://" + plan.sandbox_id.ToString();
    h.boot_ms = 1;

    {
        std::lock_guard<std::mutex> lg(mu_);
        live_[plan.sandbox_id.ToString()] = Entry(h, false);
    }
    return core::Expected<Handle, core::AnyError>(h);
}

core::Expected<core::Unit, core::AnyError>
MockBackend::Shutdown(core::SandboxId id) {
    {
        std::lock_guard<std::mutex> lg(mu_);
        live_.erase(id.ToString());
    }
    return core::Expected<core::Unit, core::AnyError>(core::Unit());
}

core::Expected<core::Unit, core::AnyError>
MockBackend::Pause(core::SandboxId id) {
    std::lock_guard<std::mutex> lg(mu_);
    std::unordered_map<std::string, Entry>::iterator it = live_.find(id.ToString());
    if (it == live_.end())
        return core::make_unexpected(core::err("mock: not found"));
    it->second.paused = true;
    return core::Expected<core::Unit, core::AnyError>(core::Unit());
}

core::Expected<core::Unit, core::AnyError>
MockBackend::Resume(core::SandboxId id) {
    std::lock_guard<std::mutex> lg(mu_);
    std::unordered_map<std::string, Entry>::iterator it = live_.find(id.ToString());
    if (it == live_.end())
        return core::make_unexpected(core::err("mock: not found"));
    it->second.paused = false;
    return core::Expected<core::Unit, core::AnyError>(core::Unit());
}

core::Expected<std::string, core::AnyError>
MockBackend::Snapshot(core::SandboxId id, const std::string& out_dir) {
    return core::Expected<std::string, core::AnyError>(
        out_dir + "/" + id.ToString() + ".snap");
}

core::Expected<Handle, core::AnyError>
MockBackend::Restore(LaunchPlan plan, const std::string& /*snapshot_dir*/) {
    return Boot(std::move(plan));
}

core::Expected<ExecResult, core::AnyError>
MockBackend::Exec(core::SandboxId /*id*/, ExecSpec spec) {
    ExecResult r;
    r.exit_code = 0;
    // Echo the command for deterministic testing.
    for (std::size_t i = 0; i < spec.cmd.size(); ++i) r.stdout_output += spec.cmd[i] + " ";
    if (!r.stdout_output.empty()) r.stdout_output.erase(r.stdout_output.size() - 1);
    r.stdout_output += "\n";
    return core::Expected<ExecResult, core::AnyError>(r);
}

}  // namespace sandbox
}  // namespace agentenv
