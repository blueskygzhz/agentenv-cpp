// SPDX-License-Identifier: MIT
// Rust: src/sandbox/custom_extension/{client,mod}.rs
#ifndef AGENTENV_SANDBOX_CUSTOM_EXTENSION_H_
#define AGENTENV_SANDBOX_CUSTOM_EXTENSION_H_

#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"

namespace agentenv {
namespace sandbox {
namespace custom_extension {

/// Rust: opaque JSON blob passed to the extension hook.
struct Params {
    std::string json_bytes;
};

bool ParamsIsEmpty(const Params& p);

/// Rust struct `SandboxInstanceId` — a fresh UUID per sandbox start (fresh boot
/// or resume). Together with the sandbox id it disambiguates lifecycle hooks
/// across pause/resume where the sandbox id is reused.
struct SandboxInstanceId {
    core::Uuid value;
    static SandboxInstanceId New();// Rust: Uuid::now_v7()
    std::string ToString() const { return value.ToString(); }
};

/// Rust hook endpoints under `POST {url}/sandbox-hook/...`.
namespace hook {
static const char* const kStartFresh  = "start-fresh";
static const char* const kStartResume = "start-resume";
static const char* const kPatchParams = "patch-params";
static const char* const kStop        = "stop";
}  // namespace hook

/// Rust trait `CustomExtensionClient`.
class Client {
 public:
    virtual ~Client() {}
    virtual core::Expected<core::Unit, std::string>
        Notify(const std::string& event, const Params& params) = 0;
};

/// Rust struct `CustomExtensionHookGuard` — RAII: fires the stop hook (best
/// effort) for `(sandbox_id, instance_id)` when the running instance is torn
/// down. Constructed after a successful start hook.
class HookGuard {
 public:
    HookGuard(Client* client, core::SandboxId sandbox_id, SandboxInstanceId instance_id);
    ~HookGuard();

    // Move-only (ownership of the "must fire stop" obligation).
    HookGuard(HookGuard&& o) noexcept;
    HookGuard& operator=(HookGuard&& o) noexcept;
    HookGuard(const HookGuard&) = delete;
    HookGuard& operator=(const HookGuard&) = delete;

    /// Rust `disarm` — release the obligation without firing (e.g. handed off).
    void Disarm() { armed_ = false; }

    const SandboxInstanceId& InstanceId() const { return instance_id_; }

 private:
    Client*client_;
    core::SandboxId   sandbox_id_;
    SandboxInstanceId instance_id_;
    bool              armed_;
};

}  // namespace custom_extension
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_CUSTOM_EXTENSION_H_
