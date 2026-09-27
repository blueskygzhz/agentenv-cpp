// SPDX-License-Identifier: MIT
// Rust: src/sandbox/custom_extension/{client,mod}.rs
#include "agentenv/sandbox/custom_extension.h"

#include <utility>

namespace agentenv {
namespace sandbox {
namespace custom_extension {

bool ParamsIsEmpty(const Params& p) {
    // Rust `custom_extension_params_is_empty`: empty / "{}" / "null" == empty.
    return p.json_bytes.empty() || p.json_bytes == "{}" || p.json_bytes == "null";
}

SandboxInstanceId SandboxInstanceId::New() {
    SandboxInstanceId id;
    id.value = core::Uuid::GenV7();
    return id;
}

HookGuard::HookGuard(Client* client, core::SandboxId sandbox_id,
                     SandboxInstanceId instance_id)
    : client_(client),
      sandbox_id_(sandbox_id),
      instance_id_(instance_id),
      armed_(true) {}

HookGuard::~HookGuard() {
    if (!armed_ || client_ == nullptr) return;
    // Best-effort stop hook: failures are only logged (ignored here).
    Params p;
    p.json_bytes = std::string("{\"sandbox_id\":\"") + sandbox_id_.ToString() +
                   "\",\"instance_id\":\"" + instance_id_.ToString() + "\"}";
    (void)client_->Notify(hook::kStop, p);
}

HookGuard::HookGuard(HookGuard&& o) noexcept
    : client_(o.client_),
      sandbox_id_(o.sandbox_id_),
      instance_id_(o.instance_id_),
      armed_(o.armed_) {
    o.armed_ = false;
    o.client_ = nullptr;
}

HookGuard& HookGuard::operator=(HookGuard&& o) noexcept {
    if (this != &o) {
        // Fire our own pending obligation before taking over the other's.
        if (armed_ && client_ != nullptr) {
            Params p;
            p.json_bytes = std::string("{\"sandbox_id\":\"") + sandbox_id_.ToString() +
                           "\",\"instance_id\":\"" + instance_id_.ToString() + "\"}";
            (void)client_->Notify(hook::kStop, p);
        }
        client_ = o.client_;
        sandbox_id_ = o.sandbox_id_;
        instance_id_ = o.instance_id_;
        armed_ = o.armed_;
        o.armed_ = false;
        o.client_ = nullptr;
    }
    return *this;
}

}  // namespace custom_extension
}  // namespace sandbox
}  // namespace agentenv
