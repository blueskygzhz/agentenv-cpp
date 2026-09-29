// SPDX-License-Identifier: MIT
// Rust: src/sandbox/mock.rs — in-memory backend used by tests.
#ifndef AGENTENV_SANDBOX_MOCK_H_
#define AGENTENV_SANDBOX_MOCK_H_

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "agentenv/sandbox/backend.h"

namespace agentenv {
namespace sandbox {

class MockBackend final : public Backend {
 public:
    MockBackend();
    ~MockBackend() override;

    core::Expected<Handle, core::AnyError>
        Boot(LaunchPlan plan) override;
    core::Expected<core::Unit, core::AnyError>
        Shutdown(core::SandboxId id) override;
    core::Expected<core::Unit, core::AnyError>
        Pause(core::SandboxId id) override;
    core::Expected<core::Unit, core::AnyError>
        Resume(core::SandboxId id) override;
    core::Expected<std::string, core::AnyError>
        Snapshot(core::SandboxId id, const std::string& out_dir) override;
    core::Expected<Handle, core::AnyError>
        Restore(LaunchPlan plan, const std::string& snapshot_dir) override;
    core::Expected<ExecResult, core::AnyError>
        Exec(core::SandboxId id, ExecSpec spec) override;

 private:
    struct Entry {
        Handle handle;
        bool paused;
        Entry() : paused(false) {}
        Entry(Handle h, bool p) : handle(std::move(h)), paused(p) {}
    };
    std::mutex mu_;
    std::unordered_map<std::string, Entry> live_;   // key: id.ToString()
};

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_MOCK_H_
