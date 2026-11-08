// SPDX-License-Identifier: MIT
// Rust: src/api/impls/sandbox.rs :: sandboxes_sandbox_id_snapshots_post
//       (+ `snapshot_sandbox_volumes` and `bad_request_for_repository_build_error`)
//
// This handler was a 501 until the capture and publish halves were joined, so
// everything here is first coverage. Three things it does are invisible to a
// response-shape test and are what these cases are for:
//
//   * the *order*: the repository and the alias are checked before the guest
//     is paused, so a request that cannot possibly publish does not cost a
//     capture;
//   * the record it commits really describes the captured sandbox -- its
//     resources, its alias and its mounted volumes -- rather than defaults
//     that happen to serialise;
//   * a failure anywhere leaves no committed record, because a caller told
//     its snapshot was taken will launch from that id later.
#include "microtest.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/api/impls.h"
#include "agentenv/api/server.h"
#include "agentenv/core/fs.h"
#include "agentenv/observability/prometheus.h"
#include "agentenv/snapshot/repository/posixfs/artifacts.h"
#include "agentenv/snapshot/repository/posixfs/backend.h"
#include "agentenv/snapshot/repository/posixfs/catalog.h"
// `orchestrator_fakes.h` first so its `Fixture` wins the short name; see the
// note at the bottom of `volume_fakes.h`.
#include "orchestrator_fakes.h"
#include "volume_fakes.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT
using agentenv::testing::CreatingMeta;
using agentenv::testing::Fixture;
using agentenv::testing::FreshPlan;
using agentenv::testing::VolumeFixture;
using agentenv::volume::VolumeRecord;

namespace posixfs = agentenv::snapshot::repository::posixfs;

namespace {

/// A throwaway tree for the repository root and the capture's artifacts.
struct TempTree {
    std::string path;
    TempTree() {
        const core::Expected<std::string, std::string> made =
            core::fs::CreateTempDir("agentenv-api-snapshot-");
        MT_EXPECT_TRUE(made.ok());
        if (made.ok()) path = made.value();
    }
    ~TempTree() {
        if (!path.empty()) core::fs::RemoveDirAll(path);
    }
    std::string At(const std::string& name) const { return core::fs::Join(path, name); }
};

/// An overlaybd image config with one local lower whose descriptor matches the
/// file on disk.
///
/// The size has to be right: `StoreDiskLayers` trusts a capture-side
/// descriptor only after checking it against the file, and hashes otherwise.
/// A deliberately wrong size would be testing the hash fallback instead.
void WriteOneLayerConfig(const std::string& config_path, const std::string& layer_path,
                         const std::string& bytes) {
    MT_EXPECT_TRUE(core::fs::Write(layer_path, bytes).ok());
    storage::overlaybd::ImageConfig config;
    storage::overlaybd::LayerConfig lower;
    lower.file = layer_path;
    lower.digest = core::Sha256Digest(bytes);
    lower.size = static_cast<uint64_t>(bytes.size());
    config.lowers.push_back(lower);
    MT_EXPECT_TRUE(storage::overlaybd::SaveImageConfig(config_path, config).ok());
}

/// Lays out, under `dir`, exactly the files the manifest `FakeSandbox::
/// Snapshot` hands back names: `vm_state.bin`, `mem_image.json` and
/// `rootfs/image.json`.
///
/// Written by the test rather than by the fake because a publish reads them:
/// a fake that produced a manifest pointing at nothing would exercise the
/// error path on every call and never the one that commits.
void LayOutCapture(const std::string& dir) {
    MT_EXPECT_TRUE(core::fs::CreateDirAll(core::fs::Join(dir, "rootfs")).ok());
    MT_EXPECT_TRUE(core::fs::Write(core::fs::Join(dir, "vm_state.bin"), "vm state").ok());
    WriteOneLayerConfig(core::fs::Join(dir, "mem_image.json"),
                        core::fs::Join(dir, "mem.commit"), "memory layer bytes");
    WriteOneLayerConfig(core::fs::Join(dir, "rootfs/image.json"),
                        core::fs::Join(dir, "rootfs/base.commit"), "rootfs layer bytes");
}

std::shared_ptr<snapshot::repository::SnapshotRepository> Repository(const std::string& root) {
    return std::make_shared<posixfs::PosixFsSnapshotRepository>(
        std::make_shared<posixfs::PosixFsCatalogStore>(root),
        std::make_shared<posixfs::PosixFsArtifactStore>(root));
}

/// A launched, Running sandbox whose capture will succeed and whose artifacts
/// are already on disk.
struct Capturable {
    TempTree tree;
    std::shared_ptr<snapshot::repository::SnapshotRepository> repository;
    core::SandboxId id;

    explicit Capturable(Fixture* f) {
        repository = Repository(tree.At("repository"));
        f->script.succeed_snapshot = true;
        f->script.snapshot_dir = tree.At("capture");
        LayOutCapture(f->script.snapshot_dir);

        orchestrator::SandboxMetadata meta = CreatingMeta();
        id = meta.id;
        const orchestrator::OrchestratorResult<orchestrator::SandboxMetadata> launched =
            f->orch->LaunchSandbox(FreshPlan(meta));
        MT_EXPECT_TRUE(launched.ok());
    }
};

CreateSnapshotReq Named(const std::string& name) {
    CreateSnapshotReq req;
    req.name = core::Optional<std::string>(name);
    return req;
}

/// How many records the repository holds, which is the only thing that makes a
/// snapshot real to another node.
std::size_t CommittedCount(
    const std::shared_ptr<snapshot::repository::SnapshotRepository>& repository) {
    const snapshot::repository::RepositoryResult<std::vector<snapshot::SnapshotRecord> > listed =
        repository->List(snapshot::repository::SnapshotListFilter());
    MT_EXPECT_TRUE(listed.ok());
    return listed.ok() ? listed.value().size() : 0;
}

}  // namespace

// ---- the order: nothing is captured for a request that cannot publish -----

MT_TEST(snapshot_without_a_repository_is_refused_before_the_guest_is_paused) {
    Fixture f;
    f.script.succeed_snapshot = true;

    orchestrator::SandboxMetadata meta = CreatingMeta();
    MT_EXPECT_TRUE(f.orch->LaunchSandbox(FreshPlan(meta)).ok());

    // No repository: the three-argument constructor leaves it null.
    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>());
    const core::Expected<SnapshotInfoResp, ApiError> r =
        impl.CreateSnapshot(meta.id.ToString(), CreateSnapshotReq());
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_EQ(r.error().code, 500);
    MT_EXPECT_EQ(r.error().message, std::string("no snapshot repository configured"));

    // And the guest was never touched. A capture pauses the VM and writes its
    // memory out in full; paying that only to discover there is nowhere to
    // publish is the mistake this ordering exists to prevent.
    MT_EXPECT_EQ(f.script.snapshot_calls, 0);
}

MT_TEST(an_invalid_alias_is_refused_before_the_guest_is_paused) {
    Fixture f;
    Capturable sandbox(&f);
    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(), sandbox.repository);

    // `/` is not in the alias alphabet (ASCII alphanumerics, `-`, `_`).
    const core::Expected<SnapshotInfoResp, ApiError> r =
        impl.CreateSnapshot(sandbox.id.ToString(), Named("not/an/alias"));
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_EQ(r.error().code, 400);
    MT_EXPECT_TRUE(r.error().message.find("invalid snapshot alias") == 0);

    MT_EXPECT_EQ(f.script.snapshot_calls, 0);
    MT_EXPECT_EQ(CommittedCount(sandbox.repository), static_cast<std::size_t>(0));
}

MT_TEST(an_empty_name_is_an_invalid_alias_rather_than_no_alias) {
    // The distinction the DTO goes out of its way to keep: an absent `name`
    // publishes unaliased, while `""` is a malformed alias. Collapsing them
    // would silently publish a snapshot the caller meant to name.
    Fixture f;
    Capturable sandbox(&f);
    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(), sandbox.repository);

    const core::Expected<SnapshotInfoResp, ApiError> r =
        impl.CreateSnapshot(sandbox.id.ToString(), Named(std::string()));
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_EQ(r.error().code, 400);
    MT_EXPECT_EQ(f.script.snapshot_calls, 0);
}

// ---- the lookups ----------------------------------------------------------

MT_TEST(a_malformed_sandbox_id_is_a_404) {
    Fixture f;
    Capturable sandbox(&f);
    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(), sandbox.repository);

    const core::Expected<SnapshotInfoResp, ApiError> r =
        impl.CreateSnapshot("not-a-uuid", CreateSnapshotReq());
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    // Upstream takes the id through a typed path parameter, so a malformed one
    // is indistinguishable from a missing sandbox to the caller.
    MT_EXPECT_EQ(r.error().code, 404);
    MT_EXPECT_EQ(f.script.snapshot_calls, 0);
}

MT_TEST(an_unknown_sandbox_is_a_404) {
    Fixture f;
    Capturable sandbox(&f);
    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(), sandbox.repository);

    const core::Expected<SnapshotInfoResp, ApiError> r =
        impl.CreateSnapshot(core::SandboxId::Fresh().ToString(), CreateSnapshotReq());
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_EQ(r.error().code, 404);
}

// ---- a refused capture publishes nothing ---------------------------------

MT_TEST(a_refused_capture_commits_no_record) {
    Fixture f;
    Capturable sandbox(&f);
    // The backend declines after the sandbox is already Running.
    f.script.succeed_snapshot = false;

    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(), sandbox.repository);
    const core::Expected<SnapshotInfoResp, ApiError> r =
        impl.CreateSnapshot(sandbox.id.ToString(), CreateSnapshotReq());
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;

    MT_EXPECT_EQ(f.script.snapshot_calls, 1);
    // Nothing was committed. A record here would name artifacts no capture
    // ever wrote, and the failure would resurface on whichever node tried to
    // launch from it.
    MT_EXPECT_EQ(CommittedCount(sandbox.repository), static_cast<std::size_t>(0));
}

// ---- the happy path ------------------------------------------------------

MT_TEST(snapshot_publishes_a_record_describing_the_captured_sandbox) {
    Fixture f;
    Capturable sandbox(&f);
    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(), sandbox.repository);

    const core::Expected<SnapshotInfoResp, ApiError> r =
        impl.CreateSnapshot(sandbox.id.ToString(), Named("golden-image"));
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) {
        MT_EXPECT_EQ(r.error().message, std::string(""));
        return;
    }

    // The alias comes back in `names`, as upstream's one-element list.
    MT_EXPECT_EQ(r.value().names.size(), static_cast<std::size_t>(1));
    if (!r.value().names.empty()) {
        MT_EXPECT_EQ(r.value().names[0], std::string("golden-image"));
    }
    // The resources are the *sandbox's*, not defaults. `CreatingMeta` uses
    // 2 vCPU / 512 MiB / 1024 MiB, none of which is a struct default.
    MT_EXPECT_EQ(r.value().cpu_count, static_cast<uint32_t>(2));
    MT_EXPECT_EQ(r.value().memory_mb, static_cast<uint64_t>(512));
    MT_EXPECT_EQ(r.value().disk_size_mb, static_cast<uint64_t>(1024));
    MT_EXPECT_TRUE(r.value().created_at_unix_ms > 0);

    // The record is really in the repository under the id that was reported,
    // and it is reachable by its alias too -- which is what a launch resolves.
    const snapshot::repository::RepositoryResult<core::Optional<snapshot::SnapshotRecord> >
        by_id = sandbox.repository->Get(r.value().snapshot_id);
    MT_EXPECT_TRUE(by_id.ok());
    MT_EXPECT_TRUE(by_id.ok() && by_id.value().has_value());
    const snapshot::repository::RepositoryResult<core::Optional<snapshot::SnapshotRecord> >
        by_alias = sandbox.repository->Get("golden-image");
    MT_EXPECT_TRUE(by_alias.ok() && by_alias.value().has_value());
    if (!by_id.ok() || !by_id.value().has_value()) return;

    const snapshot::SnapshotRecord& record = *by_id.value();
    // Committed, not a waiting template build: an uncommitted record has no
    // artifacts and would fail at launch rather than here.
    MT_EXPECT_TRUE(record.committed.has_value());
    if (!record.committed.has_value()) return;
    // Provenance: the snapshot knows which sandbox it came from.
    MT_EXPECT_TRUE(record.source.kind == snapshot::SnapshotSourceKind::Sandbox);
    MT_EXPECT_EQ(record.source.source_sandbox_id, sandbox.id.ToString());
    // And it carries the rootfs the capture sealed. An empty list would
    // restore a VM with no root filesystem.
    MT_EXPECT_TRUE(!record.committed->rootfs_layers.empty());
    MT_EXPECT_TRUE(!record.committed->memory_layers.empty());

    MT_EXPECT_EQ(f.script.snapshot_calls, 1);
}

MT_TEST(a_snapshot_without_a_name_publishes_unaliased) {
    Fixture f;
    Capturable sandbox(&f);
    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(), sandbox.repository);

    const core::Expected<SnapshotInfoResp, ApiError> r =
        impl.CreateSnapshot(sandbox.id.ToString(), CreateSnapshotReq());
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    // Empty, not a one-element list holding "": a client reading `names[0]`
    // would otherwise treat the empty string as the snapshot's name.
    MT_EXPECT_TRUE(r.value().names.empty());
}

MT_TEST(a_second_snapshot_under_the_same_alias_is_a_400) {
    // Rust `bad_request_for_repository_build_error`: an alias someone else
    // holds is the caller's mistake, so 400 rather than 500 -- and 400 rather
    // than 409, because upstream's response for this route has only the two.
    Fixture f;
    Capturable sandbox(&f);
    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(), sandbox.repository);

    MT_EXPECT_TRUE(impl.CreateSnapshot(sandbox.id.ToString(), Named("taken")).ok());
    const core::Expected<SnapshotInfoResp, ApiError> again =
        impl.CreateSnapshot(sandbox.id.ToString(), Named("taken"));
    MT_EXPECT_TRUE(!again.ok());
    if (again.ok()) return;
    MT_EXPECT_EQ(again.error().code, 400);

    // The first snapshot is still the one the alias names, and the failed
    // publish left no second record behind.
    MT_EXPECT_EQ(CommittedCount(sandbox.repository), static_cast<std::size_t>(1));
}

// ---- the mounted volumes ------------------------------------------------

MT_TEST(snapshot_records_the_mounted_volumes_as_of_the_capture) {
    Fixture f;
    Capturable sandbox_tree(&f);
    VolumeFixture vf;
    const VolumeRecord mounted = vf.PublishedVolume();

    // Mounted on the sandbox the capture will report, which is what
    // `snapshot_sandbox_volumes` walks.
    orchestrator::SandboxMetadata meta = CreatingMeta();
    meta.volume_mounts["/mnt/data"] = mounted.id;
    MT_EXPECT_TRUE(f.orch->LaunchSandbox(FreshPlan(meta)).ok());

    ApiImpl impl(f.orch, vf.manager, std::shared_ptr<image::ImageResolver>(),
                 sandbox_tree.repository);
    const core::Expected<SnapshotInfoResp, ApiError> r =
        impl.CreateSnapshot(meta.id.ToString(), CreateSnapshotReq());
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) {
        MT_EXPECT_EQ(r.error().message, std::string(""));
        return;
    }

    const snapshot::repository::RepositoryResult<core::Optional<snapshot::SnapshotRecord> >
        got = sandbox_tree.repository->Get(r.value().snapshot_id);
    MT_EXPECT_TRUE(got.ok() && got.value().has_value());
    if (!got.ok() || !got.value().has_value() || !got.value()->committed.has_value()) return;

    const std::vector<snapshot::SnapshotVolume>& volumes =
        got.value()->committed->volume_snapshots;
    MT_EXPECT_EQ(volumes.size(), static_cast<std::size_t>(1));
    if (volumes.empty()) return;
    // The mount path and the volume's shape, so a restore recreates the same
    // volume at the same place. Dropping the layers would restore an empty
    // volume over the data the guest was using.
    MT_EXPECT_EQ(volumes[0].mount_path, std::string("/mnt/data"));
    MT_EXPECT_EQ(volumes[0].size_mb, mounted.size_mb);
    MT_EXPECT_TRUE(volumes[0].mode == mounted.mode);
    MT_EXPECT_TRUE(!volumes[0].layers.empty());
}

MT_TEST(a_sandbox_with_mounts_but_no_volume_manager_is_a_500) {
    // The mounts are real and cannot be read, so the publication would record
    // a sandbox with no volumes -- and a restore from it would come up without
    // the data the guest had mounted.
    Fixture f;
    Capturable sandbox_tree(&f);
    orchestrator::SandboxMetadata meta = CreatingMeta();
    meta.volume_mounts["/mnt/data"] = "volume-that-cannot-be-read";
    MT_EXPECT_TRUE(f.orch->LaunchSandbox(FreshPlan(meta)).ok());

    ApiImpl impl(f.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>(), sandbox_tree.repository);
    const core::Expected<SnapshotInfoResp, ApiError> r =
        impl.CreateSnapshot(meta.id.ToString(), CreateSnapshotReq());
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_EQ(r.error().code, 500);
    MT_EXPECT_EQ(CommittedCount(sandbox_tree.repository), static_cast<std::size_t>(0));
}

MT_TEST(a_missing_mounted_volume_is_reported_as_a_400) {
    // Rust collapses every non-500 volume error onto a 400 here: a 404 from
    // the volume catalog is not a 404 about the *sandbox*, which is what the
    // caller would read it as.
    Fixture f;
    Capturable sandbox_tree(&f);
    VolumeFixture vf;

    orchestrator::SandboxMetadata meta = CreatingMeta();
    meta.volume_mounts["/mnt/data"] = "no-such-volume";
    MT_EXPECT_TRUE(f.orch->LaunchSandbox(FreshPlan(meta)).ok());

    ApiImpl impl(f.orch, vf.manager, std::shared_ptr<image::ImageResolver>(),
                 sandbox_tree.repository);
    const core::Expected<SnapshotInfoResp, ApiError> r =
        impl.CreateSnapshot(meta.id.ToString(), CreateSnapshotReq());
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_EQ(r.error().code, 400);
    MT_EXPECT_EQ(CommittedCount(sandbox_tree.repository), static_cast<std::size_t>(0));
}

// ---- the route itself ----------------------------------------------------
//
// The handler was a 501 for long enough that the route was spelled
// `/sandboxes/{id}/snapshot` (singular, 204) while this port's own metrics
// labeller folded the plural path. Nothing noticed, because nothing the route
// returned depended on it. Now that it publishes, both halves matter: a
// client cannot find a route under the wrong name, and per-route latency
// would be filed under a path nothing serves.

namespace {

/// A `Server` that only records its route table.
class RecordingServer : public Server {
 public:
    void Route(const std::string& method, const std::string& pattern, Handler h) override {
        patterns.push_back(method + " " + pattern);
        handlers[method + " " + pattern] = h;
    }
    void Use(Middleware) override {}
    core::Expected<core::Unit, core::AnyError> ListenAndServe(const std::string&) override {
        return core::Unit();
    }
    void Stop() override {}

    bool Has(const std::string& key) const {
        for (std::size_t i = 0; i < patterns.size(); ++i) {
            if (patterns[i] == key) return true;
        }
        return false;
    }

    std::vector<std::string> patterns;
    std::map<std::string, Handler> handlers;
};

const char* const kSnapshotRoute = "POST /sandboxes/{sandbox_id}/snapshots";

}  // namespace

MT_TEST(the_snapshot_route_is_registered_where_the_metrics_label_says_it_is) {
    Fixture f;
    Capturable sandbox(&f);
    RecordingServer server;
    RegisterOrchestratorRoutes(&server, f.orch.get(),
                               std::shared_ptr<volume::VolumeManager>(),
                               std::shared_ptr<image::ImageResolver>(), sandbox.repository);

    MT_EXPECT_TRUE(server.Has(kSnapshotRoute));
    // And not under the singular spelling it used to have, which would answer
    // the old path and leave the documented one a 404.
    MT_EXPECT_TRUE(!server.Has("POST /sandboxes/{sandbox_id}/snapshot"));

    // The pattern and the metrics label are the same string. They are derived
    // independently -- one is the route table, the other a hand-written fold
    // in `observability/prometheus.cc` -- so they can and did drift apart.
    MT_EXPECT_EQ(std::string(observability::HttpRouteLabel("/sandboxes/sb-1/snapshots")),
                 std::string("/sandboxes/{sandbox_id}/snapshots"));
}

MT_TEST(the_snapshot_route_answers_201_with_the_published_snapshot) {
    Fixture f;
    Capturable sandbox(&f);
    RecordingServer server;
    RegisterOrchestratorRoutes(&server, f.orch.get(),
                               std::shared_ptr<volume::VolumeManager>(),
                               std::shared_ptr<image::ImageResolver>(), sandbox.repository);
    MT_EXPECT_TRUE(server.Has(kSnapshotRoute));
    if (!server.Has(kSnapshotRoute)) return;

    HttpRequest req;
    req.method = "POST";
    req.query["sandbox_id"] = sandbox.id.ToString();
    req.body = "{\"name\":\"from-the-route\"}";
    HttpResponse resp;
    server.handlers[kSnapshotRoute](req, &resp);

    // 201 and the snapshot in the body. A 204 would leave the caller with no
    // id to launch from, which is what this route used to do.
    MT_EXPECT_EQ(resp.status, 201);
    const core::Expected<core::Json, core::AnyError> body = core::Json::Parse(resp.body);
    MT_EXPECT_TRUE(body.ok());
    if (!body.ok()) return;
    const core::Expected<SnapshotInfoResp, core::AnyError> parsed =
        SnapshotInfoResp::FromJson(body.value());
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;
    MT_EXPECT_EQ(parsed.value().names.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(sandbox.repository->Get(parsed.value().snapshot_id).value().has_value());
}

MT_TEST(the_snapshot_route_rejects_a_missing_body) {
    // Upstream's `requestBody` is `required: true`. Treating an absent body as
    // `{}` would quietly publish unaliased the snapshot a client meant to
    // name but failed to send.
    Fixture f;
    Capturable sandbox(&f);
    RecordingServer server;
    RegisterOrchestratorRoutes(&server, f.orch.get(),
                               std::shared_ptr<volume::VolumeManager>(),
                               std::shared_ptr<image::ImageResolver>(), sandbox.repository);
    if (!server.Has(kSnapshotRoute)) return;

    HttpRequest req;
    req.method = "POST";
    req.query["sandbox_id"] = sandbox.id.ToString();
    HttpResponse resp;
    server.handlers[kSnapshotRoute](req, &resp);

    MT_EXPECT_EQ(resp.status, 400);
    // And nothing was captured or committed for it.
    MT_EXPECT_EQ(f.script.snapshot_calls, 0);
    MT_EXPECT_EQ(CommittedCount(sandbox.repository), static_cast<std::size_t>(0));
}

MT_MAIN
