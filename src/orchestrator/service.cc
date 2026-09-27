// SPDX-License-Identifier: MIT
// The state-machine driver. Semantics mirror Rust src/orchestrator/service.rs.
#include "agentenv/orchestrator/service.h"

#include <chrono>

#include "agentenv/core/logging.h"

namespace agentenv {
namespace orchestrator {

namespace {
int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}  // namespace

Service::Service(std::shared_ptr<sandbox::Backend> backend,
                 std::shared_ptr<Persister> persister,
                 core::Executor* executor)
    : backend_(std::move(backend)),
      persister_(std::move(persister)),
      executor_(executor) {}

void Service::Transition_(Sandbox* s, LifecyclePhase to) {
    AGENTENV_DEBUG("sandbox " << s->id.ToString()
                  << " " << PhaseName(s->phase) << " -> " << PhaseName(to));
    s->phase = to;
}

core::Expected<Sandbox, core::AnyError>
Service::DoCreate_(CreateOptions opts) {
    Sandbox s;
    s.id = core::SandboxId::Fresh();
    s.template_id = opts.template_id;
    s.created_at_ms = NowMs();
    Transition_(&s, LifecyclePhase::Created);

    {
        std::lock_guard<std::mutex> lg(mu_);
        auto r = persister_->Put(s);
        if (!r.ok()) return core::make_unexpected(r.take_error());
    }
    Transition_(&s, LifecyclePhase::Reserved);

    // Boot the backend.
    sandbox::LaunchPlan plan;
    plan.sandbox_id = s.id;
    plan.template_id = s.template_id;
    plan.env_vars = std::move(opts.env_vars);

    Transition_(&s, LifecyclePhase::Booting);
    {
        std::lock_guard<std::mutex> lg(mu_);
        persister_->Put(s);
    }

    auto boot_fut = backend_->Boot(std::move(plan));
    auto boot_res = boot_fut.get();
    if (!boot_res.ok()) {
        s.last_error = boot_res.error().chain();
        Transition_(&s, LifecyclePhase::Failed);
        std::lock_guard<std::mutex> lg(mu_);
        persister_->Put(s);
        return core::make_unexpected(boot_res.take_error());
    }
    s.handle = boot_res.take_value();
    s.started_at_ms = NowMs();
    Transition_(&s, LifecyclePhase::Ready);
    Transition_(&s, LifecyclePhase::Running);

    {
        std::lock_guard<std::mutex> lg(mu_);
        auto r = persister_->Put(s);
        if (!r.ok()) return core::make_unexpected(r.take_error());
    }
    return s;
}

std::future<core::Expected<Sandbox, core::AnyError>>
Service::Create(CreateOptions opts) {
    return executor_->Submit([this, opts]() { return DoCreate_(std::move(const_cast<CreateOptions&>(opts))); });
}

core::Expected<core::Unit, core::AnyError>
Service::DoStop_(core::SandboxId id) {
    Sandbox s;
    {
        std::lock_guard<std::mutex> lg(mu_);
        auto r = persister_->Get(id);
        if (!r.ok()) return core::make_unexpected(r.take_error());
        s = r.take_value();
    }
    Transition_(&s, LifecyclePhase::Stopping);
    {
        std::lock_guard<std::mutex> lg(mu_);
        persister_->Put(s);
    }

    auto fut = backend_->Shutdown(id);
    auto res = fut.get();
    if (!res.ok()) {
        s.last_error = res.error().chain();
        Transition_(&s, LifecyclePhase::Failed);
        std::lock_guard<std::mutex> lg(mu_);
        persister_->Put(s);
        return core::make_unexpected(res.take_error());
    }
    s.stopped_at_ms = NowMs();
    Transition_(&s, LifecyclePhase::Stopped);
    {
        std::lock_guard<std::mutex> lg(mu_);
        persister_->Put(s);
    }
    return core::Unit{};
}

std::future<core::Expected<core::Unit, core::AnyError>>
Service::Stop(core::SandboxId id) {
    return executor_->Submit([this, id]() { return DoStop_(id); });
}

core::Expected<Sandbox, core::AnyError>
Service::Get(core::SandboxId id) const {
    return persister_->Get(id);
}

core::Expected<std::vector<Sandbox>, core::AnyError>
Service::List() const { return persister_->List(); }

std::future<core::Expected<sandbox::ExecResult, core::AnyError>>
Service::Exec(core::SandboxId id, sandbox::ExecSpec spec) {
    return executor_->Submit([this, id, spec]() {
        auto s = persister_->Get(id);
        if (!s.ok()) return core::Expected<sandbox::ExecResult, core::AnyError>(
                       core::make_unexpected(s.take_error()));
        auto fut = backend_->Exec(id, spec);
        return fut.get();
    });
}

}  // namespace orchestrator
}  // namespace agentenv
