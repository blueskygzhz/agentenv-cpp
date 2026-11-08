// SPDX-License-Identifier: MIT
// Rust: src/api/impls/sandbox.rs :: create_sandbox_from_snapshot, and
//       src/snapshot/manager.rs :: SnapshotManager::load_runnable.
//
// The chain this covers is the one that was broken end to end:
//
//   POST /sandboxes {"template_id": X}
//     -> repository.Get(X)                     (was never called)
//     -> SnapshotRuntimeResolver::Resolve(rec)  (had no production caller)
//     -> SandboxLaunchSource::FromRunnableSnapshot  (did not exist)
//     -> Orchestrator, Kind::Snapshot branch    (was unreachable)
//     -> SandboxBackendFactory::BuildFromSnapshot
//
// Every assertion here would have passed vacuously before, because the old
// handler set only `source.snapshot_id` and left `kind` at `Kind::Image`: the
// create "succeeded" by taking the fresh path with an empty
// `image_config_path`, which no real factory can build a rootfs ublk device
// from. So these tests check *which branch ran*, not just that the call
// returned 201.
#include "microtest.h"

#include <memory>
#include <string>

#include "agentenv/api/impls.h"
#include "orchestrator_fakes.h"
#include "snapshot_resolver_fakes.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT
using agentenv::testing::Fixture;
using agentenv::testing::SnapshotSourceFixture;

namespace {

ApiImpl MakeImpl(Fixture& f, SnapshotSourceFixture& snap) {
    return ApiImpl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                   std::shared_ptr<image::ImageResolver>(), snap.repository, snap.resolver);
}

CreateSandboxReq RequestFor(const std::string& template_id) {
    CreateSandboxReq req;
    req.template_id = template_id;
    return req;
}

}  // namespace

MT_TEST(create_resolves_the_template_and_launches_from_the_snapshot) {
    Fixture f;
    SnapshotSourceFixture snap;

    ApiImpl impl = MakeImpl(f, snap);
    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(RequestFor(snap.template_id()));
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;

    // Both halves of `load_runnable` ran, in order, for the id requested.
    MT_EXPECT_EQ(snap.repository->get_calls, 1);
    MT_EXPECT_EQ(snap.resolver->resolve_calls, 1);
    MT_EXPECT_EQ(snap.resolver->last_resolved_id, snap.id.ToString());

    // The snapshot branch, not the fresh one. This is the assertion the whole
    // change exists for: `build_calls` would be 1 and this 0 before it.
    MT_EXPECT_EQ(f.factory->build_from_snapshot_calls, 1);
    MT_EXPECT_EQ(f.factory->build_calls, 0);
    MT_EXPECT_EQ(f.factory->last_snapshot_id, snap.id.ToString());
}

MT_TEST(the_launch_carries_the_resolved_manifest_paths) {
    // The materialised `image.json` paths are the reason resolution has to
    // happen before the launch: the backend reads them to build the ublk
    // devices, and a fresh-path create leaves them empty.
    Fixture f;
    SnapshotSourceFixture snap;
    snap.resolver->manifest.vm_state.path            = "/materialized/vm_state.bin";
    snap.resolver->manifest.rootfs.image_config_path = "/materialized/rootfs-image.json";
    snap.resolver->manifest.memory.image_config_path = "/materialized/mem-image.json";

    ApiImpl impl = MakeImpl(f, snap);
    MT_EXPECT_TRUE(impl.CreateSandbox(RequestFor(snap.template_id())).ok());

    const sandbox::firecracker::SnapshotManifestView& view = f.factory->last_snapshot_manifest;
    MT_EXPECT_EQ(view.vm_state_path, std::string("/materialized/vm_state.bin"));
    MT_EXPECT_EQ(view.rootfs_image_config_path,
                 std::string("/materialized/rootfs-image.json"));
    MT_EXPECT_EQ(view.memory_image_config_path, std::string("/materialized/mem-image.json"));
    MT_EXPECT_EQ(view.backend, std::string(sandbox::kFirecrackerBackend));
}

MT_TEST(the_launch_inherits_the_committed_context_and_resources) {
    // Rust's snapshot arm reads these off the committed record rather than the
    // request. A projection that dropped them would boot the guest in `/` with
    // this node's default resources instead of the snapshot's.
    Fixture f;
    SnapshotSourceFixture snap;

    ApiImpl impl = MakeImpl(f, snap);
    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(RequestFor(snap.template_id()));
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;

    const core::Expected<core::SandboxId, std::string> id =
        core::SandboxId::Parse(created.value().sandbox_id);
    MT_EXPECT_TRUE(id.ok());
    if (!id.ok()) return;
    const orchestrator::OrchestratorResult<core::Optional<orchestrator::SandboxMetadata> >
        meta = f.orch->GetSandbox(id.value());
    MT_EXPECT_TRUE(meta.ok() && meta.value().has_value());
    if (!meta.ok() || !meta.value().has_value()) return;

    MT_EXPECT_EQ(meta.value()->snapshot_id, snap.id.ToString());
    MT_EXPECT_EQ(meta.value()->resources.memory_mib, static_cast<uint32_t>(512));
    MT_EXPECT_EQ(meta.value()->resources.disk_size_mib, static_cast<uint32_t>(1024));
    // From `committed.runtime_versions`, not from this node's configured ones:
    // a resume must run the envd the capture was taken with.
    MT_EXPECT_EQ(meta.value()->runtime_versions.envd_version, std::string("0.5.15"));
    MT_EXPECT_EQ(f.factory->last_launch_config.snapshot_id, snap.id.ToString());
}

MT_TEST(an_unknown_template_is_a_400_and_never_reaches_the_resolver) {
    // Upstream's `Ok(None)` arm is a 400, not a 404: `/sandboxes` exists, and
    // the bad value is in the body.
    Fixture f;
    SnapshotSourceFixture snap;

    ApiImpl impl = MakeImpl(f, snap);
    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(RequestFor("no-such-template"));
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 400);
    MT_EXPECT_TRUE(created.error().message.find("no-such-template") != std::string::npos);
    MT_EXPECT_TRUE(created.error().message.find("not found") != std::string::npos);
    // Nothing was materialised for a record that does not exist.
    MT_EXPECT_EQ(snap.resolver->resolve_calls, 0);
    MT_EXPECT_EQ(f.factory->build_from_snapshot_calls, 0);
}

MT_TEST(a_node_without_a_resolver_reports_501_and_does_not_launch) {
    // An OSS node: `MakeSnapshotBackendFromConfig` hands back a repository and
    // no resolver, because this port has no resolver for that backend. The
    // handler must say so rather than fall through to a fresh boot it cannot
    // build -- which is exactly what it used to do.
    Fixture f;
    SnapshotSourceFixture snap;

    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(), snap.repository,
                 std::shared_ptr<snapshot::SnapshotRuntimeResolver>());
    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(RequestFor(snap.template_id()));
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 501);
    MT_EXPECT_EQ(f.factory->build_calls, 0);
    MT_EXPECT_EQ(f.factory->build_from_snapshot_calls, 0);
}

MT_TEST(a_node_without_a_repository_reports_500) {
    // The resolver alone is not enough: the record has to be read before it
    // can be resolved, and a bundle with one and not the other is a wiring
    // mistake, not a caller error.
    Fixture f;
    SnapshotSourceFixture snap;

    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(),
                 std::shared_ptr<snapshot::repository::SnapshotRepository>(), snap.resolver);
    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(RequestFor(snap.template_id()));
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 500);
    MT_EXPECT_EQ(snap.resolver->resolve_calls, 0);
}

MT_TEST(a_resolve_failure_is_a_500_and_not_a_fresh_boot) {
    // A missing `vm_state.bin` for a record that claims to be committed is a
    // node-side fault. Reported as one; the dangerous answer is to launch
    // something else instead.
    Fixture f;
    SnapshotSourceFixture snap;
    snap.resolver->fail = core::Optional<snapshot::repository::RepositoryError>(
        snapshot::repository::RepositoryError::ArtifactNotFound("vm_state.bin"));

    ApiImpl impl = MakeImpl(f, snap);
    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateSandbox(RequestFor(snap.template_id()));
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 500);
    MT_EXPECT_TRUE(created.error().message.find("vm_state.bin") != std::string::npos);
    MT_EXPECT_EQ(f.factory->build_calls, 0);
    MT_EXPECT_EQ(f.factory->build_from_snapshot_calls, 0);
}

MT_TEST(an_alias_resolves_the_same_way_an_id_does) {
    // `repository.Get` accepts either, as Rust's does, so the handler must not
    // pre-parse the value as a UUID. One that did would reject every alias.
    Fixture f;
    SnapshotSourceFixture snap("py311-warmed");

    ApiImpl impl = MakeImpl(f, snap);
    MT_EXPECT_TRUE(impl.CreateSandbox(RequestFor("py311-warmed")).ok());
    MT_EXPECT_EQ(snap.resolver->resolve_calls, 1);
    // The *record's* id reaches the launch, not the alias the caller typed:
    // the sandbox is pinned to a snapshot, and an alias can be re-pointed.
    MT_EXPECT_EQ(f.factory->last_snapshot_id, snap.id.ToString());
}

MT_TEST(more_requested_drives_than_free_slots_is_a_400) {
    // Rust's free-slot check, which this port could not perform until the
    // manifest was available here. A capture with no reserved slots has
    // nowhere to bind a launch-time volume, so booting anyway would hand the
    // guest a sandbox missing the disk the caller asked for.
    Fixture f;
    SnapshotSourceFixture snap;
    snap.resolver->manifest.volume_drive_slots = 0;

    CreateSandboxReq req = RequestFor(snap.template_id());
    std::vector<SandboxVolumeMountModel> models;
    SandboxVolumeMountModel mount;
    mount.path = "/mnt/data";
    mount.name = "some-volume";
    models.push_back(mount);
    req.volume_mounts = core::Optional<std::vector<SandboxVolumeMountModel> >(models);

    ApiImpl impl = MakeImpl(f, snap);
    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 400);
    MT_EXPECT_TRUE(created.error().message.find("free drive slots") != std::string::npos);
    MT_EXPECT_EQ(f.factory->build_from_snapshot_calls, 0);
}

MT_TEST(explicit_mounts_are_refused_for_a_snapshot_that_captured_volumes) {
    // Rust `check_snapshot_volume_replacement`. The restore brings the
    // captured filesystems back with it, so honouring `volumeMounts` too would
    // put two sets of disks on the same slots.
    Fixture f;
    SnapshotSourceFixture snap;
    snapshot::SnapshotVolume captured;
    captured.mount_path = "/mnt/captured";
    captured.size_mb    = 1024;
    snap.repository->record->committed->volume_snapshots.push_back(captured);

    CreateSandboxReq req = RequestFor(snap.template_id());
    std::vector<SandboxVolumeMountModel> models;
    SandboxVolumeMountModel mount;
    mount.path = "/mnt/data";
    mount.name = "some-volume";
    models.push_back(mount);
    req.volume_mounts = core::Optional<std::vector<SandboxVolumeMountModel> >(models);

    ApiImpl impl = MakeImpl(f, snap);
    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateSandbox(req);
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 400);
    MT_EXPECT_TRUE(created.error().message.find("omit volumeMounts") != std::string::npos);

    // A present-but-empty list is still an explicit replacement -- it says
    // "no volumes" -- so it is refused too. This is the distinction the
    // Optional in `CreateSandboxReq::volume_mounts` exists for, and matching
    // upstream's `is_some()` here is what makes the field's documented
    // absent-vs-empty contract real rather than decorative.
    CreateSandboxReq empty = RequestFor(snap.template_id());
    empty.volume_mounts = core::Optional<std::vector<SandboxVolumeMountModel> >(
        std::vector<SandboxVolumeMountModel>());
    const core::Expected<CreateSandboxResp, ApiError> emptied = impl.CreateSandbox(empty);
    MT_EXPECT_TRUE(!emptied.ok());
    if (emptied.ok()) return;
    MT_EXPECT_EQ(emptied.error().code, 400);

    // Omitting the field entirely is the restore path, and that one launches.
    MT_EXPECT_TRUE(impl.CreateSandbox(RequestFor(snap.template_id())).ok());
}

MT_TEST(free_drive_slots_saturates_rather_than_wrapping) {
    // Directly, because the API path above cannot express a manifest whose
    // counts disagree.
    sandbox::SandboxSnapshotManifest manifest = agentenv::testing::ResolvedManifest();
    manifest.volume_drive_slots         = 2;
    manifest.physical_extra_drive_count = 0;
    MT_EXPECT_EQ(manifest.FreeDriveSlots(), static_cast<std::size_t>(2));

    // A pre-slots capture: every attached drive is physical, whatever the
    // stored count says. Reading `physical_extra_drive_count` here would
    // report 7 physical drives for a manifest that has none.
    manifest.volume_drive_slots         = 0;
    manifest.physical_extra_drive_count = 7;
    MT_EXPECT_EQ(manifest.EffectivePhysicalExtraDriveCount(),
                 static_cast<std::size_t>(0));
    MT_EXPECT_EQ(manifest.FreeDriveSlots(), static_cast<std::size_t>(0));

    // The underflow itself: three attached drives occupying reserved slots
    // against one declared slot. Unsaturated this is `1 - 3`, which as a
    // `size_t` is SIZE_MAX -- a manifest that admits every request. The
    // earlier cases above cannot reach it, because both of their subtractions
    // come out at zero.
    manifest.volume_drive_slots         = 1;
    manifest.physical_extra_drive_count = 0;
    for (int i = 0; i < 3; ++i) {
        sandbox::SnapshotAttachedDriveArtifacts drive;
        drive.drive_id          = std::string("slot-") + static_cast<char>('0' + i);
        drive.virtual_size      = 1024;
        drive.image_config_path = "/snap/drive.json";
        manifest.attached_drives.push_back(drive);
    }
    MT_EXPECT_EQ(manifest.EffectivePhysicalExtraDriveCount(),
                 static_cast<std::size_t>(0));
    MT_EXPECT_EQ(manifest.FreeDriveSlots(), static_cast<std::size_t>(0));
}

MT_MAIN
