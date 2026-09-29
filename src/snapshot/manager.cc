// SPDX-License-Identifier: MIT
#include "agentenv/snapshot/manager.h"

namespace agentenv {
namespace snapshot {

Manager::Manager(std::shared_ptr<Repository> repo,
                 std::shared_ptr<sandbox::Backend> backend)
    : repo_(std::move(repo)), backend_(std::move(backend)) {}

core::Expected<SnapshotMeta, core::AnyError>
Manager::Create(core::SandboxId sandbox_id, const std::string& template_id) {
    if (!backend_) return core::make_unexpected(core::err("no backend"));
    // Pause the sandbox.
    auto pause_res = backend_->Pause(sandbox_id);
    if (!pause_res.ok()) return core::make_unexpected(pause_res.take_error());

    // Ask backend to dump snapshot files.
    const std::string out_dir = "/tmp/agentenv_snap_" + sandbox_id.ToString();
    auto snap_res = backend_->Snapshot(sandbox_id, out_dir);
    if (!snap_res.ok()) return core::make_unexpected(snap_res.take_error());

    // Resume the sandbox (best-effort).
    backend_->Resume(sandbox_id);

    SnapshotMeta meta;
    meta.id = core::SnapshotId::Fresh();
    meta.template_id = template_id;
    meta.created_at_ms = 0;   // TODO: use system clock

    if (repo_) {
        auto put_res = repo_->Put(meta, snap_res.value() + "/mem", snap_res.value() + "/disk");
        if (!put_res.ok()) return core::make_unexpected(put_res.take_error());
    }
    return meta;
}

core::Expected<sandbox::Handle, core::AnyError>
Manager::Restore(core::SnapshotId snapshot_id, const std::string& template_id) {
    if (!repo_)     return core::make_unexpected(core::err("no repo"));
    if (!backend_)  return core::make_unexpected(core::err("no backend"));

    std::string mem_file, disk_file;
    auto meta_res = repo_->Get(snapshot_id, &mem_file, &disk_file);
    if (!meta_res.ok()) return core::make_unexpected(meta_res.take_error());

    sandbox::LaunchPlan plan;
    plan.sandbox_id = core::SandboxId::Fresh();
    plan.template_id = template_id;
    auto res = backend_->Restore(std::move(plan), mem_file);
    if (!res.ok()) return core::make_unexpected(res.take_error());
    return res.take_value();
}

}  // namespace snapshot
}  // namespace agentenv
