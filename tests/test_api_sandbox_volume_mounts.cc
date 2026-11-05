// SPDX-License-Identifier: MIT
// Rust: src/api/impls/sandbox.rs :: sandboxes_post (volume section),
//       prepare_volume_mounts, finish_volume_reservation
//
// The point of these is the hand-off, not the resolution: `resolve_volume_
// mounts` is covered by `test_api_volumes.cc`, but nothing until now checked
// that `POST /sandboxes` actually *runs* it and then re-points the placeholder
// lease at the sandbox that ends up holding the volumes. Both halves are
// invisible to a response-shape test -- a create that silently dropped
// `volumeMounts` returns exactly the same 201.
#include "microtest.h"

#include <memory>
#include <string>
#include <vector>

#include "agentenv/api/impls.h"
#include "agentenv/core/fs.h"
// `orchestrator_fakes.h` first so its `Fixture` wins the short name and the
// volume one stays `VolumeFixture`; see the note at the bottom of
// `volume_fakes.h`.
#include "orchestrator_fakes.h"
#include "volume_fakes.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT
using agentenv::testing::Fixture;
using agentenv::testing::VolumeFixture;
using agentenv::volume::VolumeRecord;

namespace {

CreateSandboxReq MountingRequest(
    const std::vector<std::pair<std::string, std::string> >& mounts) {
    CreateSandboxReq req;
    req.template_id = "tpl-volume";
    std::vector<SandboxVolumeMountModel> models;
    for (std::size_t i = 0; i < mounts.size(); ++i) {
        SandboxVolumeMountModel model;
        model.path = mounts[i].first;
        model.name = mounts[i].second;
        models.push_back(model);
    }
    req.volume_mounts = core::Optional<std::vector<SandboxVolumeMountModel> >(models);
    return req;
}

/// The lease holder recorded on the volume, or "" when it is free.
std::string LeaseHolder(VolumeFixture& vf, const std::string& id) {
    const volume::VolumeResult<VolumeRecord> record = vf.manager->Get(id);
    MT_EXPECT_TRUE(record.ok());
    if (!record.ok()) return std::string("<missing>");
    return record.value().reserved_by_sandbox_id.has_value()
               ? *record.value().reserved_by_sandbox_id
               : std::string();
}

}  // namespace

MT_TEST(create_with_a_volume_mount_hands_the_lease_to_the_sandbox) {
    Fixture f;
    VolumeFixture vf;
    const VolumeRecord volume = vf.PublishedVolume();
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), std::string());

    ApiImpl impl(f.orch, vf.manager);
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/mnt/data"), volume.id));

    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(MountingRequest(mounts));
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;

    // The placeholder `pending-<uuid>` owner is gone and the real sandbox
    // holds the lease. Left on the placeholder, nothing would ever release it.
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), created.value().sandbox_id);

    // And the mount is recorded on the sandbox, which is what a resume reads
    // to rebind the same volume to the same path.
    const core::Expected<core::SandboxId, std::string> id =
        core::SandboxId::Parse(created.value().sandbox_id);
    MT_EXPECT_TRUE(id.ok());
    if (!id.ok()) return;
    const orchestrator::OrchestratorResult<core::Optional<orchestrator::SandboxMetadata> >
        meta = f.orch->GetSandbox(id.value());
    MT_EXPECT_TRUE(meta.ok() && meta.value().has_value());
    if (!meta.ok() || !meta.value().has_value()) return;
    MT_EXPECT_EQ(static_cast<int>(meta.value()->volume_mounts.size()), 1);
    MT_EXPECT_TRUE(meta.value()->volume_mounts.find("/mnt/data") !=
                   meta.value()->volume_mounts.end());
    MT_EXPECT_EQ(meta.value()->volume_mounts.find("/mnt/data")->second, volume.id);
}

MT_TEST(create_without_volume_mounts_leaves_volumes_alone) {
    Fixture f;
    VolumeFixture vf;
    const VolumeRecord volume = vf.PublishedVolume();

    ApiImpl impl(f.orch, vf.manager);
    CreateSandboxReq req;
    req.template_id = "tpl-plain";

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(created.ok());
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), std::string());
}

MT_TEST(an_empty_volume_mounts_list_mounts_nothing) {
    Fixture f;
    VolumeFixture vf;
    const VolumeRecord volume = vf.PublishedVolume();

    // Upstream's `volume_mounts_from_model` treats a present-but-empty list as
    // "no mounts" rather than as an error, so this is a plain create.
    ApiImpl impl(f.orch, vf.manager);
    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(
        MountingRequest(std::vector<std::pair<std::string, std::string> >()));
    MT_EXPECT_TRUE(created.ok());
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), std::string());
}

MT_TEST(create_rejects_a_duplicate_volume_mount_path) {
    Fixture f;
    VolumeFixture vf;
    const VolumeRecord volume = vf.PublishedVolume();

    // Built by hand: `MountingRequest` takes a vector precisely so the same
    // path can appear twice, which a map could not express. The two entries
    // name the same volume because the duplicate-path check runs before any
    // volume is looked up -- a second volume would not exercise anything more.
    CreateSandboxReq req;
    req.template_id = "tpl-dup";
    std::vector<SandboxVolumeMountModel> models;
    SandboxVolumeMountModel a;
    a.path = "/mnt/data";
    a.name = volume.id;
    models.push_back(a);
    SandboxVolumeMountModel b;
    b.path = "/mnt/data";
    b.name = volume.id;
    models.push_back(b);
    req.volume_mounts = core::Optional<std::vector<SandboxVolumeMountModel> >(models);

    ApiImpl impl(f.orch, vf.manager);
    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    // 400, not "last one wins": the sandbox would otherwise come up with a
    // volume the caller did not ask for at that path.
    MT_EXPECT_EQ(created.error().code, 400);
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), std::string());
}

MT_TEST(create_reports_a_conflict_for_a_volume_already_leased) {
    Fixture f;
    VolumeFixture vf;
    const VolumeRecord volume = vf.PublishedVolume();
    MT_EXPECT_TRUE(vf.manager->Reserve(volume.id, "other-sandbox").ok());

    ApiImpl impl(f.orch, vf.manager);
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/mnt/data"), volume.id));

    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(MountingRequest(mounts));
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    // 409: the volume exists but is in use, so the caller should retry rather
    // than give up on a malformed request.
    MT_EXPECT_EQ(created.error().code, 409);
    // The existing holder keeps it.
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), std::string("other-sandbox"));
}

MT_TEST(create_refuses_volume_mounts_without_a_volume_manager) {
    Fixture f;
    VolumeFixture vf;
    const VolumeRecord volume = vf.PublishedVolume();

    // No manager passed, so there is nothing to resolve the reference with.
    // Refused rather than launched without the volume: a sandbox whose data
    // disk is simply absent fails later, inside the guest, as a missing mount.
    ApiImpl impl(f.orch);
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/mnt/data"), volume.id));

    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(MountingRequest(mounts));
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 500);
}

MT_TEST(a_failed_launch_releases_the_volume_lease) {
    Fixture f;
    VolumeFixture vf;
    const VolumeRecord volume = vf.PublishedVolume();
    f.factory->fail_build = true;

    ApiImpl impl(f.orch, vf.manager);
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/mnt/data"), volume.id));

    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(MountingRequest(mounts));
    MT_EXPECT_TRUE(!created.ok());

    // Released, not deleted: this volume pre-existed the request and belongs
    // to the caller. Left leased to the `pending-` placeholder it would be
    // unusable forever, since no sandbox exists to free it.
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), std::string());
}

MT_MAIN
