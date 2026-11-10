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
#include "snapshot_resolver_fakes.h"
#include "volume_fakes.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT
using agentenv::testing::Fixture;
using agentenv::testing::SnapshotSourceFixture;
using agentenv::testing::VolumeFixture;
using agentenv::volume::VolumeRecord;

namespace {

/// `POST /sandboxes` now resolves `templateID` into a runnable snapshot before
/// it looks at volumes at all, so every test here needs a repository and a
/// resolver that can answer for the id it sends. Without them the handler
/// reports 501 and no volume code runs -- which is the correct behaviour for a
/// node that cannot launch snapshots, and useless for testing volumes.
ApiImpl MakeImpl(Fixture& f, VolumeFixture& vf, SnapshotSourceFixture& snap) {
    return ApiImpl(f.orch, vf.manager, std::shared_ptr<image::ImageResolver>(),
                   snap.repository, snap.resolver);
}

CreateSandboxReq MountingRequest(
    const std::string& template_id,
    const std::vector<std::pair<std::string, std::string> >& mounts) {
    CreateSandboxReq req;
    req.template_id = template_id;
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
    SnapshotSourceFixture snap;
    const VolumeRecord volume = vf.PublishedVolume();
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), std::string());

    ApiImpl impl = MakeImpl(f, vf, snap);
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/mnt/data"), volume.id));

    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(MountingRequest(snap.template_id(), mounts));
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;

    // The snapshot was actually resolved, and for the id the request named.
    // Before this was wired up the handler never touched the resolver and the
    // launch took the fresh path with an empty rootfs config.
    MT_EXPECT_EQ(snap.resolver->resolve_calls, 1);
    MT_EXPECT_EQ(snap.resolver->last_resolved_id, snap.id.ToString());
    // And the launch took the snapshot branch, not the fresh one.
    MT_EXPECT_EQ(f.factory->build_from_snapshot_calls, 1);
    MT_EXPECT_EQ(f.factory->last_snapshot_id, snap.id.ToString());

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
    SnapshotSourceFixture snap;
    const VolumeRecord volume = vf.PublishedVolume();

    ApiImpl impl = MakeImpl(f, vf, snap);
    CreateSandboxReq req;
    req.template_id = snap.template_id();

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(created.ok());
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), std::string());
}

MT_TEST(an_empty_volume_mounts_list_mounts_nothing) {
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    const VolumeRecord volume = vf.PublishedVolume();

    // Upstream's `volume_mounts_from_model` treats a present-but-empty list as
    // "no mounts" rather than as an error, so this is a plain create.
    ApiImpl impl = MakeImpl(f, vf, snap);
    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(
        MountingRequest(snap.template_id(),
                        std::vector<std::pair<std::string, std::string> >()));
    MT_EXPECT_TRUE(created.ok());
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), std::string());
}

MT_TEST(create_rejects_a_duplicate_volume_mount_path) {
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    const VolumeRecord volume = vf.PublishedVolume();

    // Built by hand: `MountingRequest` takes a vector precisely so the same
    // path can appear twice, which a map could not express. The two entries
    // name the same volume because the duplicate-path check runs before any
    // volume is looked up -- a second volume would not exercise anything more.
    CreateSandboxReq req;
    req.template_id = snap.template_id();
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

    ApiImpl impl = MakeImpl(f, vf, snap);
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
    SnapshotSourceFixture snap;
    const VolumeRecord volume = vf.PublishedVolume();
    MT_EXPECT_TRUE(vf.manager->Reserve(volume.id, "other-sandbox").ok());

    ApiImpl impl = MakeImpl(f, vf, snap);
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/mnt/data"), volume.id));

    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(MountingRequest(snap.template_id(), mounts));
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
    SnapshotSourceFixture snap;
    const VolumeRecord volume = vf.PublishedVolume();

    // No manager passed, so there is nothing to resolve the reference with.
    // Refused rather than launched without the volume: a sandbox whose data
    // disk is simply absent fails later, inside the guest, as a missing mount.
    //
    // The snapshot source is still supplied, because resolution now runs
    // first: without it this would report 501 for the wrong reason and the
    // volume check would never be reached.
    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(), snap.repository, snap.resolver);
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/mnt/data"), volume.id));

    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(MountingRequest(snap.template_id(), mounts));
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 500);
}

MT_TEST(a_failed_launch_releases_the_volume_lease) {
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    const VolumeRecord volume = vf.PublishedVolume();
    f.factory->fail_build = true;

    ApiImpl impl = MakeImpl(f, vf, snap);
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/mnt/data"), volume.id));

    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(MountingRequest(snap.template_id(), mounts));
    MT_EXPECT_TRUE(!created.ok());

    // Released, not deleted: this volume pre-existed the request and belongs
    // to the caller. Left leased to the `pending-` placeholder it would be
    // unusable forever, since no sandbox exists to free it.
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), std::string());
}

// ---- restoring a capture's volumes ----------------------------------------
//
// Rust `restore_snapshot_volume_mounts`. A captured snapshot records a
// `SnapshotVolume` per mounted volume; resuming it has to turn each one back
// into a live volume and mount it at the path it was captured on.
//
// Until this was wired up the resume *succeeded* with no volumes at all --
// a 201, a running sandbox, and every path the guest had mounted now empty.
// That is why these assert the volume catalog and the recorded mounts rather
// than the response: the response is identical either way.

namespace {

/// Adds a captured volume to the fixture's committed snapshot, the way a
/// `pause`/`snapshot` of a sandbox holding that volume would have.
///
/// The layers must be non-empty and the size non-zero, because
/// `CreateFromSnapshot` refuses either -- a captured volume with no backing is
/// not a blank disk, it is a record nothing can be rebuilt from.
void CaptureVolume(SnapshotSourceFixture& snap, const std::string& mount_path,
                   volume::VolumeMode mode, uint64_t size_mb,
                   const std::string& digest) {
    snapshot::SnapshotVolume captured;
    captured.mount_path = mount_path;
    captured.mode       = mode;
    captured.size_mb    = size_mb;
    snapshot::ExternalLayer layer;
    layer.repo_blob_url = "https://registry.example.test/v2/volumes/blobs";
    layer.digest        = digest;
    layer.size          = 64;
    captured.layers.push_back(snapshot::OverlaybdLayerRef::External(layer));
    snap.repository->record->committed->volume_snapshots.push_back(captured);
}

/// Every volume in the catalog, by id.
std::vector<VolumeRecord> AllVolumes(VolumeFixture& vf) {
    const volume::VolumeResult<volume::VolumePage> page =
        vf.manager->ListPage(core::Optional<std::string>(), 100);
    MT_EXPECT_TRUE(page.ok());
    return page.ok() ? page.value().records : std::vector<VolumeRecord>();
}

/// The volumes the restore created, which are the ones named `volume-restore-`.
std::vector<VolumeRecord> RestoredVolumes(VolumeFixture& vf) {
    const std::vector<VolumeRecord> all = AllVolumes(vf);
    std::vector<VolumeRecord> restored;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].name.find("volume-restore-") == 0) restored.push_back(all[i]);
    }
    return restored;
}

}  // namespace

MT_TEST(resuming_a_capture_restores_the_volumes_it_was_holding) {
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    CaptureVolume(snap, "/mnt/data", volume::VolumeMode::Exclusive, 1024, "sha256:aaa");

    ApiImpl impl = MakeImpl(f, vf, snap);
    CreateSandboxReq req;
    req.template_id = snap.template_id();

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;

    // One new volume, built over the captured layers rather than looked up.
    const std::vector<VolumeRecord> restored = RestoredVolumes(vf);
    MT_EXPECT_EQ(restored.size(), static_cast<std::size_t>(1));
    if (restored.size() != 1) return;
    MT_EXPECT_EQ(restored[0].size_mb, static_cast<uint64_t>(1024));
    MT_EXPECT_TRUE(restored[0].mode == volume::VolumeMode::Exclusive);
    MT_EXPECT_EQ(restored[0].backing_layers.size(), static_cast<std::size_t>(1));
    if (!restored[0].backing_layers.empty()) {
        MT_EXPECT_EQ(restored[0].backing_layers[0].external.digest, std::string("sha256:aaa"));
    }

    // And it is leased to the sandbox that is now running on it, not to the
    // `pending-` placeholder the restore passed through.
    MT_EXPECT_EQ(LeaseHolder(vf, restored[0].id), created.value().sandbox_id);

    // Mounted back at the path it was captured on. This is the assertion the
    // missing restore used to pass by returning an empty map.
    const core::Expected<core::SandboxId, std::string> id =
        core::SandboxId::Parse(created.value().sandbox_id);
    MT_EXPECT_TRUE(id.ok());
    if (!id.ok()) return;
    const orchestrator::OrchestratorResult<core::Optional<orchestrator::SandboxMetadata> >
        meta = f.orch->GetSandbox(id.value());
    MT_EXPECT_TRUE(meta.ok() && meta.value().has_value());
    if (!meta.ok() || !meta.value().has_value()) return;
    MT_EXPECT_EQ(meta.value()->volume_mounts.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(meta.value()->volume_mounts.count("/mnt/data") == 1);
    if (meta.value()->volume_mounts.count("/mnt/data") == 1) {
        MT_EXPECT_EQ(meta.value()->volume_mounts.find("/mnt/data")->second, restored[0].id);
    }
}

MT_TEST(a_restore_creates_a_fresh_volume_rather_than_reusing_the_captured_one) {
    // The original volume is not what comes back, and it must not be: the
    // capture froze a point in time while the volume kept running, so it may
    // have been written to, deleted, or leased to another sandbox since. This
    // is also what lets one snapshot be resumed twice at once.
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    const VolumeRecord original = vf.PublishedVolume();
    CaptureVolume(snap, "/mnt/data", volume::VolumeMode::Exclusive, 1024, "sha256:bbb");

    ApiImpl impl = MakeImpl(f, vf, snap);
    CreateSandboxReq req;
    req.template_id = snap.template_id();

    const core::Expected<CreateSandboxResp, ApiError> first = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(first.ok());
    const core::Expected<CreateSandboxResp, ApiError> second = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(second.ok());
    if (!first.ok() || !second.ok()) return;

    // Two restores, two distinct volumes, neither of them the original.
    const std::vector<VolumeRecord> restored = RestoredVolumes(vf);
    MT_EXPECT_EQ(restored.size(), static_cast<std::size_t>(2));
    if (restored.size() != 2) return;
    MT_EXPECT_TRUE(restored[0].id != restored[1].id);
    MT_EXPECT_TRUE(restored[0].id != original.id);
    MT_EXPECT_TRUE(restored[1].id != original.id);

    // The original is untouched -- same lease state as before the resumes.
    // Reusing it would have failed the second resume on the lease instead.
    MT_EXPECT_EQ(LeaseHolder(vf, original.id), std::string());

    // One lease each, to different sandboxes.
    const std::string holder_a = LeaseHolder(vf, restored[0].id);
    const std::string holder_b = LeaseHolder(vf, restored[1].id);
    MT_EXPECT_TRUE(!holder_a.empty() && !holder_b.empty());
    MT_EXPECT_TRUE(holder_a != holder_b);
}

MT_TEST(every_captured_volume_comes_back_on_its_own_path) {
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    CaptureVolume(snap, "/mnt/data", volume::VolumeMode::Exclusive, 1024, "sha256:ccc");
    CaptureVolume(snap, "/mnt/shared", volume::VolumeMode::ReadOnly, 2048, "sha256:ddd");

    ApiImpl impl = MakeImpl(f, vf, snap);
    CreateSandboxReq req;
    req.template_id = snap.template_id();

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;

    const core::Expected<core::SandboxId, std::string> id =
        core::SandboxId::Parse(created.value().sandbox_id);
    if (!id.ok()) return;
    const orchestrator::OrchestratorResult<core::Optional<orchestrator::SandboxMetadata> >
        meta = f.orch->GetSandbox(id.value());
    MT_EXPECT_TRUE(meta.ok() && meta.value().has_value());
    if (!meta.ok() || !meta.value().has_value()) return;
    MT_EXPECT_EQ(meta.value()->volume_mounts.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(meta.value()->volume_mounts.count("/mnt/data") == 1);
    MT_EXPECT_TRUE(meta.value()->volume_mounts.count("/mnt/shared") == 1);

    // The mode travels with each one. A read-only volume restored as exclusive
    // would take a write lease nothing releases and refuse the next resume.
    const std::vector<VolumeRecord> restored = RestoredVolumes(vf);
    MT_EXPECT_EQ(restored.size(), static_cast<std::size_t>(2));
    int read_only = 0, exclusive = 0;
    for (std::size_t i = 0; i < restored.size(); ++i) {
        if (restored[i].mode == volume::VolumeMode::ReadOnly) {
            ++read_only;
            MT_EXPECT_EQ(restored[i].size_mb, static_cast<uint64_t>(2048));
        } else {
            ++exclusive;
            MT_EXPECT_EQ(restored[i].size_mb, static_cast<uint64_t>(1024));
        }
    }
    MT_EXPECT_EQ(read_only, 1);
    MT_EXPECT_EQ(exclusive, 1);
}

MT_TEST(a_restore_marks_the_drives_as_already_bound_in_the_snapshot) {
    // Rust `extra_drives_in_snapshot`. The captured config already names a
    // drive per captured volume and the guest resumed with those mount points
    // live, so there is nothing to patch after the load. Left false, the
    // launch takes the patch path, counts this launch's drives back from the
    // end of the config's drive list and rebinds that many -- writing the
    // restored volumes over the slots the capture was already using.
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    CaptureVolume(snap, "/mnt/data", volume::VolumeMode::Exclusive, 1024, "sha256:eee");

    ApiImpl impl = MakeImpl(f, vf, snap);
    CreateSandboxReq req;
    req.template_id = snap.template_id();
    MT_EXPECT_TRUE(impl.CreateSandbox(req).ok());
    MT_EXPECT_TRUE(f.factory->last_launch_config.extra_drives_in_snapshot);

    // And *not* set when the request supplies the mounts itself: those drives
    // are new to this launch and do need binding after the load.
    Fixture f2;
    VolumeFixture vf2;
    SnapshotSourceFixture snap2;
    const VolumeRecord volume = vf2.PublishedVolume();
    ApiImpl impl2 = MakeImpl(f2, vf2, snap2);
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/mnt/data"), volume.id));
    MT_EXPECT_TRUE(impl2.CreateSandbox(MountingRequest(snap2.template_id(), mounts)).ok());
    MT_EXPECT_TRUE(!f2.factory->last_launch_config.extra_drives_in_snapshot);

    // Nor when the capture had no volumes at all, which is the case that keeps
    // the flag from being set for every resume.
    Fixture f3;
    VolumeFixture vf3;
    SnapshotSourceFixture snap3;
    ApiImpl impl3 = MakeImpl(f3, vf3, snap3);
    CreateSandboxReq plain;
    plain.template_id = snap3.template_id();
    MT_EXPECT_TRUE(impl3.CreateSandbox(plain).ok());
    MT_EXPECT_TRUE(!f3.factory->last_launch_config.extra_drives_in_snapshot);
}

MT_TEST(a_failed_launch_deletes_the_volumes_the_restore_created) {
    // The other half of `a_failed_launch_releases_the_volume_lease`, and the
    // opposite action: a restored volume was created by *this* request and
    // nothing else refers to it, so a failed launch has to remove it. Merely
    // released, every retry would leave another full copy of the capture's
    // volumes behind.
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    CaptureVolume(snap, "/mnt/data", volume::VolumeMode::Exclusive, 1024, "sha256:fff");
    f.factory->fail_build = true;

    ApiImpl impl = MakeImpl(f, vf, snap);
    CreateSandboxReq req;
    req.template_id = snap.template_id();

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(!created.ok());
    MT_EXPECT_TRUE(RestoredVolumes(vf).empty());
}

MT_TEST(a_partly_failed_restore_leaves_nothing_behind) {
    // The unwind inside the restore itself. A capture whose second volume
    // cannot be rebuilt must not leave the first one created: the request
    // fails, so nothing will ever lease or delete it.
    //
    // The second volume is given a zero size, which `CreateFromSnapshot`
    // refuses -- a captured record a `size_mb` of 0 cannot be rebuilt from.
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    CaptureVolume(snap, "/mnt/data", volume::VolumeMode::Exclusive, 1024, "sha256:111");
    CaptureVolume(snap, "/mnt/broken", volume::VolumeMode::Exclusive, 0, "sha256:222");

    ApiImpl impl = MakeImpl(f, vf, snap);
    CreateSandboxReq req;
    req.template_id = snap.template_id();

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    // A caller cannot fix this, so it is not their fault.
    MT_EXPECT_TRUE(created.error().code >= 400);
    MT_EXPECT_TRUE(RestoredVolumes(vf).empty());
    // And no sandbox was launched on a half-restored set of volumes.
    MT_EXPECT_EQ(f.factory->build_from_snapshot_calls, 0);
}

MT_TEST(a_captured_mount_path_is_renormalised_rather_than_trusted) {
    // The record is durable and may have been written by another node or an
    // older build, and this path becomes a guest mount point. A traversal in
    // it is refused, and refused *before* any volume is created.
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    CaptureVolume(snap, "/mnt/../../etc", volume::VolumeMode::Exclusive, 1024, "sha256:333");

    ApiImpl impl = MakeImpl(f, vf, snap);
    CreateSandboxReq req;
    req.template_id = snap.template_id();

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 400);
    MT_EXPECT_TRUE(RestoredVolumes(vf).empty());
    // *Before* any volume is created, which is the part an end-state
    // assertion cannot see: `ResolveVolumeMounts` normalises the keys again
    // and would refuse this path too, so trusting the record here answers
    // with the same 400 -- after having built, leased and deleted a volume
    // for a path that was never going to be accepted.
    MT_EXPECT_EQ(vf.repository->create_volume_calls, 0);
}

MT_TEST(a_bad_mount_path_deletes_the_volumes_already_restored) {
    // The unwind inside the restore's own path check. The first captured
    // volume is fine and gets built; the second one's path is refused, and the
    // first has to go with it. Nothing downstream can do it instead -- the
    // failure returns the error without the ids, so an uncleaned record is
    // leaked for good.
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    CaptureVolume(snap, "/mnt/data", volume::VolumeMode::Exclusive, 1024, "sha256:777");
    CaptureVolume(snap, "/mnt/../../etc", volume::VolumeMode::Exclusive, 1024, "sha256:888");

    ApiImpl impl = MakeImpl(f, vf, snap);
    CreateSandboxReq req;
    req.template_id = snap.template_id();

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 400);
    // The first one was built -- so the walk really did get past it -- and
    // then removed again.
    MT_EXPECT_EQ(vf.repository->create_volume_calls, 1);
    MT_EXPECT_TRUE(RestoredVolumes(vf).empty());
}

MT_TEST(a_failed_lease_deletes_the_restored_volumes) {
    // Between the restore and the launch sits `PrepareVolumeMounts`, which
    // refuses more mounts than a sandbox may hold -- `max_volume_count`, four
    // by default. A capture with five is the one case where the restore
    // succeeds in full and the request still fails before anything is
    // launched, so it is the only way in to that cleanup.
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    MT_EXPECT_EQ(vf.manager->limits().max_mounts, static_cast<std::size_t>(4));
    CaptureVolume(snap, "/mnt/a", volume::VolumeMode::Exclusive, 1024, "sha256:a01");
    CaptureVolume(snap, "/mnt/b", volume::VolumeMode::Exclusive, 1024, "sha256:a02");
    CaptureVolume(snap, "/mnt/c", volume::VolumeMode::Exclusive, 1024, "sha256:a03");
    CaptureVolume(snap, "/mnt/d", volume::VolumeMode::Exclusive, 1024, "sha256:a04");
    CaptureVolume(snap, "/mnt/e", volume::VolumeMode::Exclusive, 1024, "sha256:a05");

    ApiImpl impl = MakeImpl(f, vf, snap);
    CreateSandboxReq req;
    req.template_id = snap.template_id();

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 400);
    // All five were built, and all five are gone. Left behind, a client
    // retrying this hopeless resume would add five volumes per attempt.
    MT_EXPECT_EQ(vf.repository->create_volume_calls, 5);
    MT_EXPECT_TRUE(RestoredVolumes(vf).empty());
    MT_EXPECT_EQ(f.factory->build_from_snapshot_calls, 0);
}

MT_TEST(a_failed_handover_deletes_the_restored_volumes) {
    // The last cleanup, and the only one that runs after a sandbox has
    // actually started: the placeholder lease cannot be handed to the new
    // sandbox, so the sandbox is torn down and the volumes it was mounting
    // have to be deleted -- after the teardown, never before.
    //
    // Reaching it needs one specific repository write to fail, and which one
    // that is depends on how many times `CreateFromSnapshot` rewrites a record
    // on the way to `Ready`. So the count is measured rather than guessed: one
    // successful resume reports how many rewrites a resume performs, and the
    // hand-off is the last of them -- so the second resume's is the last index
    // of two runs.
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    CaptureVolume(snap, "/mnt/data", volume::VolumeMode::Exclusive, 1024, "sha256:999");

    ApiImpl impl = MakeImpl(f, vf, snap);
    CreateSandboxReq req;
    req.template_id = snap.template_id();

    MT_EXPECT_TRUE(impl.CreateSandbox(req).ok());
    const int per_resume = vf.repository->volume_record_updates;
    MT_EXPECT_TRUE(per_resume > 0);
    vf.repository->fail_volume_record_update_at = 2 * per_resume - 1;

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 500);
    // The sandbox did come up -- this is not the failed-launch path.
    MT_EXPECT_EQ(f.factory->build_from_snapshot_calls, 2);
    // Two volumes were built; only the first resume's survives, still held by
    // the sandbox that is running on it.
    MT_EXPECT_EQ(vf.repository->create_volume_calls, 2);
    const std::vector<VolumeRecord> restored = RestoredVolumes(vf);
    MT_EXPECT_EQ(restored.size(), static_cast<std::size_t>(1));
    if (restored.size() != 1) return;
    MT_EXPECT_TRUE(!LeaseHolder(vf, restored[0].id).empty());
}

MT_TEST(a_capture_with_volumes_still_refuses_an_explicit_mount_list) {
    // `CheckSnapshotVolumeReplacement`, which is what makes the restore branch
    // unambiguous: the two cannot both apply, so the restore needs no guard of
    // its own. Asserted here because the restore landing beside it is exactly
    // the change that could make this check look redundant.
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    const VolumeRecord volume = vf.PublishedVolume();
    CaptureVolume(snap, "/mnt/data", volume::VolumeMode::Exclusive, 1024, "sha256:444");

    ApiImpl impl = MakeImpl(f, vf, snap);
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/mnt/other"), volume.id));

    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(MountingRequest(snap.template_id(), mounts));
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 400);
    // Refused before anything was built or leased.
    MT_EXPECT_TRUE(RestoredVolumes(vf).empty());
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), std::string());
}

MT_TEST(an_empty_mount_list_opts_out_of_the_restore) {
    // Present-but-empty is an explicit "this sandbox has no volumes", which is
    // why the branch keys on `has_value()` rather than on emptiness. It also
    // means this request contradicts a capture that recorded volumes, so it is
    // refused by the check above rather than silently restoring them.
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    CaptureVolume(snap, "/mnt/data", volume::VolumeMode::Exclusive, 1024, "sha256:555");

    ApiImpl impl = MakeImpl(f, vf, snap);
    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(
        MountingRequest(snap.template_id(),
                        std::vector<std::pair<std::string, std::string> >()));
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 400);
    MT_EXPECT_TRUE(RestoredVolumes(vf).empty());
}

MT_TEST(a_restore_without_a_volume_manager_is_refused_rather_than_dropped) {
    // The shape of the bug this whole section replaces: without a manager
    // there is nothing to rebuild the captured volumes with, and launching
    // anyway would hand back a 201 for a sandbox whose data disks are absent.
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;
    CaptureVolume(snap, "/mnt/data", volume::VolumeMode::Exclusive, 1024, "sha256:666");

    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(), snap.repository, snap.resolver);
    CreateSandboxReq req;
    req.template_id = snap.template_id();

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 501);
    MT_EXPECT_EQ(f.factory->build_from_snapshot_calls, 0);
}

MT_TEST(a_capture_with_no_volumes_resumes_without_a_volume_manager) {
    // The flip side, and the reason the restore returns early on an empty
    // list instead of checking for a manager first: a node built without
    // volume support must still be able to resume the snapshots that never
    // had any. Upstream gets this free -- its `volume_manager` is not
    // optional -- so the early return is what keeps the port's null manager
    // from turning every ordinary resume into a 501.
    Fixture f;
    VolumeFixture vf;
    SnapshotSourceFixture snap;

    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(), snap.repository, snap.resolver);
    CreateSandboxReq req;
    req.template_id = snap.template_id();

    MT_EXPECT_TRUE(impl.CreateSandbox(req).ok());
    MT_EXPECT_EQ(f.factory->build_from_snapshot_calls, 1);
}

MT_MAIN
