// SPDX-License-Identifier: MIT
// Rust: src/api/impls/sandbox.rs :: sandboxes_cold_post.
//
// `POST /sandboxes-cold` is the only route that can go from an OCI reference
// to a running Firecracker VM. `POST /sandboxes` cannot: upstream's
// `NewSandbox` schema carries only `templateID` and its own description sends
// callers here. This port had every piece of the cold path -- the resource
// validator, the metrics stage label, the gateway's `new_cold_sandbox`
// scheduling hint, which already matches on this exact path -- and no route,
// so none of it was reachable.
#include "microtest.h"

#include <memory>
#include <string>

#include "agentenv/api/impls.h"
#include "orchestrator_fakes.h"
#include "volume_fakes.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT
using agentenv::testing::Fixture;
using agentenv::testing::VolumeFixture;

namespace {

/// Resolves any reference to a fixed overlaybd config, carrying the OCI
/// context and raw config a real pull would have produced.
class FakeImageResolver : public image::ImageResolver {
 public:
    int                               calls = 0;
    std::string                       last_reference;
    core::Optional<image::ImageError> failure;
    image::ImageBaseContext           base_context;
    core::Optional<core::Json>        raw_config;

    FakeImageResolver() {
        base_context.env_vars["PATH"] = "/usr/local/bin:/usr/bin";
        base_context.workdir          = core::Optional<std::string>(std::string("/app"));
        base_context.user             = core::Optional<std::string>(std::string("1000:1000"));
        base_context.exposed_ports.push_back("8000/tcp");
        std::vector<std::string> cmd;
        cmd.push_back("python3");
        base_context.cmd = core::Optional<std::vector<std::string> >(cmd);
        base_context.labels["org.opencontainers.image.ref.name"] = "python";

        core::Json config;
        config["WorkingDir"] = core::Json(std::string("/app"));
        raw_config = core::Optional<core::Json>(config);
    }

    core::Expected<image::ResolvedBlockImage, image::ImageError> Resolve(
        const std::string& image_ref) override {
        ++calls;
        last_reference = image_ref;
        if (failure.has_value()) return core::make_unexpected(*failure);
        image::ResolvedBlockImage resolved;
        resolved.image_ref             = image_ref;
        resolved.overlaybd_config_path = "/var/lib/agentenv/images/py311/image.json";
        resolved.base_context          = base_context;
        resolved.raw_config            = raw_config;
        return resolved;
    }

    const std::string& DefaultImage() const override { return default_image_; }

 private:
    std::string default_image_ = "ubuntu:24.04";
};

struct ColdFixture {
    Fixture                            orch;
    std::shared_ptr<FakeImageResolver> resolver;

    ColdFixture() : resolver(new FakeImageResolver()) {}

    ApiImpl Impl() {
        return ApiImpl(orch.orch, std::shared_ptr<volume::VolumeManager>(), resolver);
    }
};

/// `ImageError` is a tagged struct with no named constructors, so a test that
/// wants a *specific* class of failure has to build one. The class is what
/// decides 400 against 500, which is the whole point of these assertions.
image::ImageError MakeImageError(image::ImageError::Kind kind, const std::string& reason) {
    image::ImageError error;
    error.kind   = kind;
    error.reason = reason;
    return error;
}

/// The stored record for a sandbox the handler just created.
///
/// Several of the fields under test (`image_configs`, `timeout_action`) are
/// not on `FreshSandboxBuildSpec` -- the orchestrator keeps them on the record
/// and forwards `imageConfigs` to the guest through MMDS -- so the launch spec
/// cannot be used to check them.
core::Optional<orchestrator::SandboxMetadata> StoredRecord(
    Fixture& f, const std::string& sandbox_id) {
    const core::Expected<core::SandboxId, std::string> id =
        core::SandboxId::Parse(sandbox_id);
    if (!id.ok()) return core::Optional<orchestrator::SandboxMetadata>();
    const orchestrator::OrchestratorResult<core::Optional<orchestrator::SandboxMetadata> >
        found = f.orch->GetSandbox(id.value());
    if (!found.ok()) return core::Optional<orchestrator::SandboxMetadata>();
    return found.value();
}

CreateColdSandboxReq RequestFor(const std::string& image) {
    CreateColdSandboxReq req;
    req.image = image;
    return req;
}

/// The sandbox currently holding a volume's lease, or empty for none.
std::string LeaseHolder(VolumeFixture& vf, const std::string& id) {
    const volume::VolumeResult<volume::VolumeRecord> record = vf.manager->Get(id);
    MT_EXPECT_TRUE(record.ok());
    if (!record.ok()) return std::string("<missing>");
    return record.value().reserved_by_sandbox_id.has_value()
               ? *record.value().reserved_by_sandbox_id
               : std::string();
}

/// A cold request mounting one volume at `/mnt/data`.
CreateColdSandboxReq MountingRequest(const std::string& image, const std::string& volume_id) {
    CreateColdSandboxReq req = RequestFor(image);
    std::vector<SandboxVolumeMountModel> mounts;
    SandboxVolumeMountModel mount;
    mount.path = "/mnt/data";
    mount.name = volume_id;
    mounts.push_back(mount);
    req.volume_mounts = core::Optional<std::vector<SandboxVolumeMountModel> >(mounts);
    return req;
}

}  // namespace

MT_TEST(a_cold_create_resolves_the_image_and_boots_from_it) {
    ColdFixture f;
    ApiImpl impl = f.Impl();

    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateColdSandbox(RequestFor("python:3.11-slim"));
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;

    // The reference reaches the resolver verbatim: no parse step, as Rust's
    // `resolver.resolve(&body.image)` has none.
    MT_EXPECT_EQ(f.resolver->calls, 1);
    MT_EXPECT_EQ(f.resolver->last_reference, std::string("python:3.11-slim"));

    // The fresh branch, not the snapshot one -- and with a non-empty rootfs
    // config path, which is the thing `POST /sandboxes` could never produce.
    MT_EXPECT_EQ(f.orch.factory->build_calls, 1);
    MT_EXPECT_EQ(f.orch.factory->build_from_snapshot_calls, 0);
    MT_EXPECT_EQ(f.orch.factory->last_build_spec.image_config_path,
                 std::string("/var/lib/agentenv/images/py311/image.json"));
}

MT_TEST(the_launch_inherits_the_images_oci_context) {
    // Rust `resolved_rootfs.base_context.into()`. Dropping any of these would
    // boot the guest in the wrong directory, as the wrong user, or without the
    // image's own command -- all silently.
    ColdFixture f;
    ApiImpl impl = f.Impl();
    MT_EXPECT_TRUE(impl.CreateColdSandbox(RequestFor("python:3.11-slim")).ok());

    const snapshot::CommandContext& ctx = f.orch.factory->last_build_spec.context;
    MT_EXPECT_EQ(ctx.workdir, std::string("/app"));
    MT_EXPECT_TRUE(ctx.user.has_value() && *ctx.user == std::string("1000:1000"));
    MT_EXPECT_EQ(ctx.env_vars.find("PATH")->second,
                 std::string("/usr/local/bin:/usr/bin"));
    MT_EXPECT_TRUE(ctx.cmd.has_value() && ctx.cmd->size() == 1);
    MT_EXPECT_TRUE(!ctx.entrypoint.has_value());
    MT_EXPECT_EQ(ctx.exposed_ports.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(ctx.labels.find("org.opencontainers.image.ref.name")->second,
                 std::string("python"));
}

MT_TEST(a_blank_image_workdir_normalizes_to_root) {
    // `CommandContext::FromEnvAndWorkdir` does this, and it is why the
    // conversion is not a field-by-field copy: an image config carrying an
    // empty `WorkingDir` would otherwise become a `cd ''`.
    ColdFixture f;
    f.resolver->base_context.workdir = core::Optional<std::string>(std::string(""));
    ApiImpl impl = f.Impl();
    MT_EXPECT_TRUE(impl.CreateColdSandbox(RequestFor("scratch:latest")).ok());
    MT_EXPECT_EQ(f.orch.factory->last_build_spec.context.workdir, std::string("/"));
}

MT_TEST(the_rootfs_oci_config_is_recorded_for_later_capture) {
    // Rust `build_image_configs`: an unset `drive_id` at mount path `/`. Both
    // are what `ImageConfigs::RootfsConfig` matches on, and a snapshot taken
    // from this sandbox republishes that object as its own `Env`/`Entrypoint`.
    ColdFixture f;
    ApiImpl impl = f.Impl();
    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateColdSandbox(RequestFor("python:3.11-slim"));
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;
    const core::Optional<orchestrator::SandboxMetadata> record =
        StoredRecord(f.orch, created.value().sandbox_id);
    MT_EXPECT_TRUE(record.has_value());
    if (!record.has_value()) return;

    const tpl::ImageConfigs& configs = record->image_configs;
    MT_EXPECT_EQ(configs.size(), static_cast<std::size_t>(1));
    if (configs.size() != 1) return;
    MT_EXPECT_TRUE(!configs.entries()[0].drive_id.has_value());
    MT_EXPECT_EQ(configs.entries()[0].mount_path, std::string("/"));
    MT_EXPECT_TRUE(configs.RootfsConfig().has_value());
}

MT_TEST(an_image_without_an_oci_config_records_no_entry) {
    // Upstream's `if let Some(config)`. An entry holding JSON `null` would
    // read as a rootfs config that explicitly clears every inherited field.
    ColdFixture f;
    f.resolver->raw_config = core::Optional<core::Json>();
    ApiImpl impl = f.Impl();
    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateColdSandbox(RequestFor("scratch:latest"));
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;
    const core::Optional<orchestrator::SandboxMetadata> record =
        StoredRecord(f.orch, created.value().sandbox_id);
    MT_EXPECT_TRUE(record.has_value() && record->image_configs.IsEmpty());
}

MT_TEST(cold_resources_come_from_the_request) {
    // `ColdStartResources` was ported, tested, and had no caller until this
    // route existed. These are the fields that make the route useful at all:
    // without them every cold sandbox would get the node's defaults.
    ColdFixture f;
    CreateColdSandboxReq req = RequestFor("python:3.11-slim");
    req.resources.cpu_count    = core::Optional<uint32_t>(static_cast<uint32_t>(4));
    req.resources.memory_mb    = core::Optional<uint32_t>(static_cast<uint32_t>(2048));
    req.resources.disk_size_mb = core::Optional<uint32_t>(static_cast<uint32_t>(4096));

    ApiImpl impl = f.Impl();
    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateColdSandbox(req);
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;
    MT_EXPECT_EQ(f.orch.factory->last_build_spec.resources.cpu_count,
                 static_cast<uint32_t>(4));
    MT_EXPECT_EQ(f.orch.factory->last_build_spec.resources.memory_mib,
                 static_cast<uint32_t>(2048));
    MT_EXPECT_EQ(f.orch.factory->last_build_spec.resources.disk_size_mib,
                 static_cast<uint32_t>(4096));
}

MT_TEST(invalid_cold_resources_are_rejected_before_anything_boots) {
    ColdFixture f;
    CreateColdSandboxReq req = RequestFor("python:3.11-slim");
    // Not a whole number of gibibytes; the block layer would round it to
    // something the caller did not ask for.
    req.resources.disk_size_mb = core::Optional<uint32_t>(static_cast<uint32_t>(1500));

    ApiImpl impl = f.Impl();
    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateColdSandbox(req);
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 400);
    MT_EXPECT_EQ(f.orch.factory->build_calls, 0);
}

MT_TEST(a_user_image_error_is_a_400_and_a_registry_fault_a_500) {
    // Rust: `err.is_user_error()` decides. A caller must be able to tell a
    // typo in the reference from a registry outage.
    ColdFixture f;
    f.resolver->failure = core::Optional<image::ImageError>(
        MakeImageError(image::ImageError::kNotFound, "no such tag"));
    ApiImpl impl = f.Impl();
    const core::Expected<CreateSandboxResp, ApiError> bad =
        impl.CreateColdSandbox(RequestFor("python:nope"));
    MT_EXPECT_TRUE(!bad.ok());
    if (bad.ok()) return;
    MT_EXPECT_EQ(bad.error().code, 400);
    MT_EXPECT_TRUE(bad.error().message.find("python:nope") != std::string::npos);

    f.resolver->failure =
        core::Optional<image::ImageError>(MakeImageError(image::ImageError::kOther, "connection reset"));
    const core::Expected<CreateSandboxResp, ApiError> faulted =
        impl.CreateColdSandbox(RequestFor("python:3.11-slim"));
    MT_EXPECT_TRUE(!faulted.ok());
    if (faulted.ok()) return;
    MT_EXPECT_EQ(faulted.error().code, 500);
    MT_EXPECT_EQ(f.orch.factory->build_calls, 0);
}

MT_TEST(a_node_without_an_image_resolver_reports_501) {
    ColdFixture f;
    ApiImpl impl(f.orch.orch, std::shared_ptr<volume::VolumeManager>(),
                 std::shared_ptr<image::ImageResolver>());
    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateColdSandbox(RequestFor("python:3.11-slim"));
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 501);
}

MT_TEST(only_an_explicit_auto_pause_false_switches_to_delete) {
    // Rust `match body.auto_pause { Some(false) => Delete, _ => Pause }`.
    // Absent and `true` must behave identically, or an omitted field would
    // start deleting sandboxes on timeout.
    ColdFixture f;
    ApiImpl impl = f.Impl();

    const char* kImage = "python:3.11-slim";
    const core::Expected<CreateSandboxResp, ApiError> absent =
        impl.CreateColdSandbox(RequestFor(kImage));
    MT_EXPECT_TRUE(absent.ok());
    if (!absent.ok()) return;
    const core::Optional<orchestrator::SandboxMetadata> absent_record =
        StoredRecord(f.orch, absent.value().sandbox_id);
    MT_EXPECT_TRUE(absent_record.has_value());
    if (!absent_record.has_value()) return;
    MT_EXPECT_TRUE(absent_record->timeout_action ==
                   orchestrator::SandboxTimeoutAction::Pause);

    CreateColdSandboxReq paused = RequestFor(kImage);
    paused.auto_pause = core::Optional<bool>(true);
    const core::Expected<CreateSandboxResp, ApiError> on =
        impl.CreateColdSandbox(paused);
    MT_EXPECT_TRUE(on.ok());
    if (!on.ok()) return;
    const core::Optional<orchestrator::SandboxMetadata> on_record =
        StoredRecord(f.orch, on.value().sandbox_id);
    MT_EXPECT_TRUE(on_record.has_value());
    if (!on_record.has_value()) return;
    MT_EXPECT_TRUE(on_record->timeout_action == orchestrator::SandboxTimeoutAction::Pause);

    CreateColdSandboxReq deleted = RequestFor(kImage);
    deleted.auto_pause = core::Optional<bool>(false);
    const core::Expected<CreateSandboxResp, ApiError> off =
        impl.CreateColdSandbox(deleted);
    MT_EXPECT_TRUE(off.ok());
    if (!off.ok()) return;
    const core::Optional<orchestrator::SandboxMetadata> off_record =
        StoredRecord(f.orch, off.value().sandbox_id);
    MT_EXPECT_TRUE(off_record.has_value());
    if (!off_record.has_value()) return;
    MT_EXPECT_TRUE(off_record->timeout_action == orchestrator::SandboxTimeoutAction::Delete);
}

MT_TEST(cold_auto_resume_reaches_the_stored_record) {
    ColdFixture f;
    ApiImpl impl = f.Impl();

    CreateColdSandboxReq req = RequestFor("python:3.11-slim");
    req.auto_resume = core::Optional<bool>(true);
    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateColdSandbox(req);
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;
    const core::Optional<orchestrator::SandboxMetadata> record =
        StoredRecord(f.orch, created.value().sandbox_id);
    MT_EXPECT_TRUE(record.has_value());
    if (!record.has_value()) return;
    MT_EXPECT_TRUE(record->auto_resume);

    const core::Expected<CreateSandboxResp, ApiError> absent =
        impl.CreateColdSandbox(RequestFor("python:3.11-slim"));
    MT_EXPECT_TRUE(absent.ok());
    if (!absent.ok()) return;
    const core::Optional<orchestrator::SandboxMetadata> absent_record =
        StoredRecord(f.orch, absent.value().sandbox_id);
    MT_EXPECT_TRUE(absent_record.has_value() && !absent_record->auto_resume);
}

MT_TEST(cold_custom_extension_params_without_an_extension_are_refused) {
    // Rust `validate_custom_extension_params`. No extension is configured in
    // these tests, so non-empty params are a 400 rather than a sandbox that
    // starts with the params silently dropped.
    ColdFixture f;
    ApiImpl impl = f.Impl();

    core::Json params;
    params["tier"] = core::Json(std::string("gold"));
    CreateColdSandboxReq req = RequestFor("python:3.11-slim");
    req.custom_extension_params = core::Optional<core::Json>(params);

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateColdSandbox(req);
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 400);
    MT_EXPECT_EQ(f.orch.factory->build_calls, 0);
}

MT_TEST(the_cold_network_config_shapes_the_policy) {
    // The `network` half of the body used to be absent from the DTO, so the
    // allow/deny lists and `allowPublicTraffic` could not be set per request.
    ColdFixture f;
    ApiImpl impl = f.Impl();

    CreateColdSandboxReq req = RequestFor("python:3.11-slim");
    SandboxNetworkConfigModel network;
    network.allow_public_traffic = core::Optional<bool>(false);
    req.network = core::Optional<SandboxNetworkConfigModel>(network);

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateColdSandbox(req);
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;
    // Only a sandbox that disallows public traffic gets a traffic token.
    MT_EXPECT_TRUE(created.value().traffic_access_token.has_value());

    const core::Expected<CreateSandboxResp, ApiError> open =
        impl.CreateColdSandbox(RequestFor("python:3.11-slim"));
    MT_EXPECT_TRUE(open.ok());
    if (!open.ok()) return;
    MT_EXPECT_TRUE(!open.value().traffic_access_token.has_value());
}

MT_TEST(an_invalid_cold_network_config_is_a_400) {
    ColdFixture f;
    ApiImpl impl = f.Impl();

    CreateColdSandboxReq req = RequestFor("python:3.11-slim");
    SandboxNetworkConfigModel network;
    std::vector<std::string> allow;
    allow.push_back("2001:db8::/32");
    network.allow_out = core::Optional<std::vector<std::string> >(allow);
    req.network = core::Optional<SandboxNetworkConfigModel>(network);

    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateColdSandbox(req);
    MT_EXPECT_TRUE(!created.ok());
    if (created.ok()) return;
    MT_EXPECT_EQ(created.error().code, 400);
    MT_EXPECT_EQ(f.orch.factory->build_calls, 0);
}

// ---- volumes ---------------------------------------------------------------

MT_TEST(cold_volume_drives_travel_on_the_image_source) {
    // A cold boot binds every drive while building the VM, so the volumes
    // belong to the Image launch source -- `FreshSandboxBuildSpec::extra_drives`
    // carries them and `SandboxLaunchConfig::extra_drives` stays empty.
    // `request.extra_drives` is for drives added to a VM that already exists
    // in a snapshot; putting them there would leave the guest without them,
    // and nothing else in this suite would have noticed.
    ColdFixture f;
    VolumeFixture vf;
    const volume::VolumeRecord volume = vf.PublishedVolume();

    ApiImpl impl(f.orch.orch, vf.manager, f.resolver);
    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateColdSandbox(MountingRequest("python:3.11-slim", volume.id));
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;

    MT_EXPECT_EQ(f.orch.factory->last_build_spec.extra_drives.size(),
                 static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(f.orch.factory->last_launch_config.extra_drives.empty());

    // And the lease moved from the `pending-` placeholder to the real sandbox;
    // left on the placeholder nothing would ever release it.
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), created.value().sandbox_id);
}

MT_TEST(a_failed_cold_launch_releases_the_volume_lease) {
    ColdFixture f;
    VolumeFixture vf;
    const volume::VolumeRecord volume = vf.PublishedVolume();
    f.orch.factory->fail_build = true;

    ApiImpl impl(f.orch.orch, vf.manager, f.resolver);
    MT_EXPECT_TRUE(
        !impl.CreateColdSandbox(MountingRequest("python:3.11-slim", volume.id)).ok());

    // Released, not deleted: the volume pre-existed the request.
    MT_EXPECT_EQ(LeaseHolder(vf, volume.id), std::string());
}

// ---- attachedDrives --------------------------------------------------------

MT_TEST(cold_attached_drives_lead_the_image_sources_drives) {
    // Rust `let mut extra_drives = resolved_attached...; extra_drives.extend(
    // volume_drives);`. Order first, because reserved slots are consumed
    // positionally: a volume taking the slot a drive expected would bind the
    // guest's mounts to the wrong backing devices.
    ColdFixture f;
    VolumeFixture vf;
    const volume::VolumeRecord volume = vf.PublishedVolume();

    CreateColdSandboxReq req = MountingRequest("python:3.11-slim", volume.id);
    std::vector<AttachedDriveRequest> drives;
    AttachedDriveRequest drive;
    drive.drive_id     = "cache";
    drive.source_image = "registry.example.com/cache:v1";
    drive.mount_path   = core::Optional<std::string>(std::string("/mnt/cache"));
    drives.push_back(drive);
    req.attached_drives = core::Optional<std::vector<AttachedDriveRequest> >(drives);

    ApiImpl impl(f.orch.orch, vf.manager, f.resolver);
    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateColdSandbox(req);
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;

    // On the *image source*, as the volumes already are: a cold boot binds
    // every drive while building the VM, so `SandboxLaunchConfig::extra_drives`
    // stays empty and putting them there would leave the guest without them.
    const std::vector<sandbox::ExtraDrive>& extras =
        f.orch.factory->last_build_spec.extra_drives;
    MT_EXPECT_TRUE(f.orch.factory->last_launch_config.extra_drives.empty());
    MT_EXPECT_EQ(extras.size(), static_cast<std::size_t>(2));
    if (extras.size() != 2) return;
    MT_EXPECT_EQ(extras[0].drive_id, std::string("cache"));
    MT_EXPECT_EQ(extras[0].mount_path, std::string("/mnt/cache"));
    MT_EXPECT_TRUE(!extras[0].IsVolume());
    MT_EXPECT_TRUE(extras[1].IsVolume());

    // Two resolves: the rootfs and the drive. One would mean the drive's image
    // reference never left the request.
    MT_EXPECT_EQ(f.resolver->calls, 2);
    MT_EXPECT_EQ(f.resolver->last_reference, std::string("registry.example.com/cache:v1"));
}

MT_TEST(an_attached_drives_oci_config_is_recorded_under_its_drive_id) {
    // Rust `build_image_configs(&resolved_rootfs, &resolved_attached)`. The
    // attached half was dropped twice over: the field was unported, and
    // `ResolvedAttachedDriveSpec::raw_config` was declared `std::string` and
    // never assigned, so even a wired-up caller would have published nothing.
    ColdFixture f;
    CreateColdSandboxReq req = RequestFor("python:3.11-slim");
    std::vector<AttachedDriveRequest> drives;
    AttachedDriveRequest drive;
    drive.drive_id     = "cache";
    drive.source_image = "registry.example.com/cache:v1";
    drive.mount_path   = core::Optional<std::string>(std::string("/mnt/cache"));
    drives.push_back(drive);
    req.attached_drives = core::Optional<std::vector<AttachedDriveRequest> >(drives);

    ApiImpl impl = f.Impl();
    const core::Expected<CreateSandboxResp, ApiError> created = impl.CreateColdSandbox(req);
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;
    const core::Optional<orchestrator::SandboxMetadata> record =
        StoredRecord(f.orch, created.value().sandbox_id);
    MT_EXPECT_TRUE(record.has_value());
    if (!record.has_value()) return;

    const tpl::ImageConfigs& configs = record->image_configs;
    MT_EXPECT_EQ(configs.size(), static_cast<std::size_t>(2));
    if (configs.size() != 2) return;
    // The rootfs entry stays first and stays keyless, which is what
    // `RootfsConfig` matches on. An attached drive mis-keyed as the rootfs
    // would make a snapshot taken from this sandbox publish the drive's
    // `Env`/`Entrypoint` as its own.
    MT_EXPECT_TRUE(!configs.entries()[0].drive_id.has_value());
    MT_EXPECT_EQ(configs.entries()[0].mount_path, std::string("/"));
    MT_EXPECT_TRUE(configs.entries()[1].drive_id.has_value());
    MT_EXPECT_EQ(*configs.entries()[1].drive_id, std::string("cache"));
    MT_EXPECT_EQ(configs.entries()[1].mount_path, std::string("/mnt/cache"));
    MT_EXPECT_TRUE(configs.RootfsConfig().has_value());

    // An image carrying no OCI config contributes no entry, as upstream's
    // `if let Some(config)` does -- an entry holding JSON `null` would read as
    // a config that explicitly clears every inherited field.
    ColdFixture bare;
    bare.resolver->raw_config = core::Optional<core::Json>();
    ApiImpl bare_impl = bare.Impl();
    const core::Expected<CreateSandboxResp, ApiError> bare_created =
        bare_impl.CreateColdSandbox(req);
    MT_EXPECT_TRUE(bare_created.ok());
    if (!bare_created.ok()) return;
    const core::Optional<orchestrator::SandboxMetadata> bare_record =
        StoredRecord(bare.orch, bare_created.value().sandbox_id);
    MT_EXPECT_TRUE(bare_record.has_value() && bare_record->image_configs.IsEmpty());
}

MT_TEST(a_malformed_cold_attached_drive_is_a_400_before_the_boot) {
    // The validation is `ResolveAttachedDrives`', already unit-tested; what is
    // checked here is that the handler forwards its status rather than
    // flattening everything to 500, and that it refuses before the VM is
    // built.
    ColdFixture f;
    ApiImpl impl = f.Impl();

    AttachedDriveRequest base;
    base.drive_id     = "data";
    base.source_image = "registry.example.com/data:v1";

    // A reserved mount path, a duplicate id, and a blank image reference --
    // one case per validation pass in request order.
    std::vector<std::vector<AttachedDriveRequest> > cases;
    {
        AttachedDriveRequest reserved = base;
        reserved.mount_path = core::Optional<std::string>(std::string("/proc/data"));
        cases.push_back(std::vector<AttachedDriveRequest>(1, reserved));
    }
    {
        std::vector<AttachedDriveRequest> duplicate;
        duplicate.push_back(base);
        AttachedDriveRequest second = base;
        second.mount_path = core::Optional<std::string>(std::string("/mnt/other"));
        duplicate.push_back(second);
        cases.push_back(duplicate);
    }
    {
        AttachedDriveRequest blank = base;
        blank.source_image = "   ";
        cases.push_back(std::vector<AttachedDriveRequest>(1, blank));
    }

    for (std::size_t i = 0; i < cases.size(); ++i) {
        CreateColdSandboxReq req = RequestFor("python:3.11-slim");
        req.attached_drives = core::Optional<std::vector<AttachedDriveRequest> >(cases[i]);
        const core::Expected<CreateSandboxResp, ApiError> created =
            impl.CreateColdSandbox(req);
        MT_EXPECT_TRUE(!created.ok());
        if (created.ok()) return;
        MT_EXPECT_EQ(created.error().code, 400);
    }
    MT_EXPECT_EQ(f.orch.factory->build_calls, 0);
}

MT_TEST(the_cold_body_reads_its_attached_drives) {
    // The wire shape, which is shared with `NewSandbox`: `source.image` is
    // nested, and the four optionals must stay unset rather than defaulted so
    // `ResolveAttachedDrives` applies its own defaults.
    core::Json drive;
    drive["driveID"] = core::Json(std::string("data"));
    core::Json source;
    source["image"] = core::Json(std::string("registry.example.com/data:v1"));
    drive["source"] = source;
    core::JsonArray drives;
    drives.push_back(drive);
    core::Json j;
    j["image"]          = core::Json(std::string("python:3.11-slim"));
    j["attachedDrives"] = core::Json(drives);

    const core::Expected<CreateColdSandboxReq, core::AnyError> parsed =
        CreateColdSandboxReq::FromJson(j);
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;
    MT_EXPECT_TRUE(parsed.value().attached_drives.has_value());
    if (!parsed.value().attached_drives.has_value()) return;
    MT_EXPECT_EQ(parsed.value().attached_drives->size(), static_cast<std::size_t>(1));
    if (parsed.value().attached_drives->size() != 1) return;
    const AttachedDriveRequest& got = (*parsed.value().attached_drives)[0];
    MT_EXPECT_EQ(got.drive_id, std::string("data"));
    MT_EXPECT_EQ(got.source_image, std::string("registry.example.com/data:v1"));
    MT_EXPECT_TRUE(!got.read_only.has_value());
    MT_EXPECT_TRUE(!got.mount_path.has_value());
    MT_EXPECT_TRUE(!got.sub_path.has_value());
    MT_EXPECT_TRUE(!got.disk_size_mb.has_value());

    // Absent and `[]` stay distinct, for the same reason they do for
    // `volumeMounts`: on the snapshot route an explicit `[]` refuses a
    // capture's drives while omitting the key restores them.
    core::Json without;
    without["image"] = core::Json(std::string("python:3.11-slim"));
    const core::Expected<CreateColdSandboxReq, core::AnyError> omitted =
        CreateColdSandboxReq::FromJson(without);
    MT_EXPECT_TRUE(omitted.ok());
    if (!omitted.ok()) return;
    MT_EXPECT_TRUE(!omitted.value().attached_drives.has_value());

    core::Json emptied;
    emptied["image"]          = core::Json(std::string("python:3.11-slim"));
    emptied["attachedDrives"] = core::Json(core::JsonArray());
    const core::Expected<CreateColdSandboxReq, core::AnyError> empty =
        CreateColdSandboxReq::FromJson(emptied);
    MT_EXPECT_TRUE(empty.ok());
    if (!empty.ok()) return;
    MT_EXPECT_TRUE(empty.value().attached_drives.has_value());
    MT_EXPECT_TRUE(empty.value().attached_drives->empty());
}

// ---- body parsing ----------------------------------------------------------

MT_TEST(the_cold_body_requires_an_image) {
    core::Json empty;
    MT_EXPECT_TRUE(!CreateColdSandboxReq::FromJson(empty).ok());

    // An explicitly empty reference too: resolving "" would reach the registry
    // and come back as a confusing 404 instead of this clear 400.
    core::Json blank;
    blank["image"] = core::Json(std::string(""));
    MT_EXPECT_TRUE(!CreateColdSandboxReq::FromJson(blank).ok());
}

MT_TEST(the_cold_body_reads_its_timeout_in_seconds) {
    // Both create bodies spell this `timeout` and both mean seconds. An
    // earlier note here claimed `NewSandbox` carried milliseconds, which was
    // this port's own invention; the record stores milliseconds, so the
    // conversion is the handler's and reading one as the other is a factor of
    // 1000 in how long a sandbox lives.
    core::Json j;
    j["image"]   = core::Json(std::string("python:3.11-slim"));
    j["timeout"] = core::Json(static_cast<int64_t>(60));
    const core::Expected<CreateColdSandboxReq, core::AnyError> parsed =
        CreateColdSandboxReq::FromJson(j);
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;
    MT_EXPECT_TRUE(parsed.value().timeout_secs.has_value());
    MT_EXPECT_EQ(*parsed.value().timeout_secs, static_cast<int64_t>(60));

    ColdFixture f;
    ApiImpl impl = f.Impl();
    const core::Expected<CreateSandboxResp, ApiError> created =
        impl.CreateColdSandbox(parsed.value());
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;
    const core::Optional<orchestrator::SandboxMetadata> record =
        StoredRecord(f.orch, created.value().sandbox_id);
    MT_EXPECT_TRUE(record.has_value());
    if (!record.has_value()) return;
    MT_EXPECT_TRUE(record->timeout_ms.has_value());
    if (!record->timeout_ms.has_value()) return;
    MT_EXPECT_EQ(*record->timeout_ms, static_cast<int64_t>(60000));
}

MT_TEST(an_absent_cold_timeout_stays_absent) {
    // Unset means the node default, so there is no sentinel to substitute:
    // zero is a legal explicit value upstream.
    core::Json j;
    j["image"] = core::Json(std::string("python:3.11-slim"));
    const core::Expected<CreateColdSandboxReq, core::AnyError> parsed =
        CreateColdSandboxReq::FromJson(j);
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;
    MT_EXPECT_TRUE(!parsed.value().timeout_secs.has_value());
    MT_EXPECT_TRUE(!parsed.value().resources.cpu_count.has_value());
    MT_EXPECT_TRUE(!parsed.value().resources.disk_size_mb.has_value());
}

MT_TEST(a_present_non_boolean_cold_flag_is_rejected) {
    // Not read as the field's default: for `autoPause` that default is the
    // opposite of what a caller writing `"autoPause": "false"` meant.
    core::Json j;
    j["image"]     = core::Json(std::string("python:3.11-slim"));
    j["autoPause"] = core::Json(std::string("false"));
    MT_EXPECT_TRUE(!CreateColdSandboxReq::FromJson(j).ok());

    core::Json secure;
    secure["image"]  = core::Json(std::string("python:3.11-slim"));
    secure["secure"] = core::Json(static_cast<int64_t>(1));
    MT_EXPECT_TRUE(!CreateColdSandboxReq::FromJson(secure).ok());
}

MT_TEST(the_cold_body_round_trips_through_json) {
    CreateColdSandboxReq req = RequestFor("python:3.11-slim");
    req.resources.cpu_count = core::Optional<uint32_t>(static_cast<uint32_t>(4));
    req.resources.memory_mb = core::Optional<uint32_t>(static_cast<uint32_t>(2048));
    req.timeout_secs        = core::Optional<int64_t>(static_cast<int64_t>(90));
    req.secure              = true;
    req.auto_pause          = core::Optional<bool>(false);
    req.extra_boot_args     = core::Optional<std::string>(std::string("quiet"));
    req.metadata["team"]    = "infra";
    req.env_vars.push_back("LOG=debug");
    std::vector<SandboxVolumeMountModel> mounts;
    SandboxVolumeMountModel mount;
    mount.name = "vol-1";
    mount.path = "/mnt/data";
    mounts.push_back(mount);
    req.volume_mounts = core::Optional<std::vector<SandboxVolumeMountModel> >(mounts);

    const core::Expected<CreateColdSandboxReq, core::AnyError> back =
        CreateColdSandboxReq::FromJson(req.ToJson());
    MT_EXPECT_TRUE(back.ok());
    if (!back.ok()) return;
    MT_EXPECT_EQ(back.value().image, req.image);
    MT_EXPECT_EQ(*back.value().resources.cpu_count, static_cast<uint32_t>(4));
    MT_EXPECT_EQ(*back.value().resources.memory_mb, static_cast<uint32_t>(2048));
    MT_EXPECT_TRUE(!back.value().resources.disk_size_mb.has_value());
    MT_EXPECT_EQ(*back.value().timeout_secs, static_cast<int64_t>(90));
    MT_EXPECT_TRUE(back.value().secure);
    MT_EXPECT_TRUE(back.value().auto_pause.has_value() && !*back.value().auto_pause);
    MT_EXPECT_EQ(*back.value().extra_boot_args, std::string("quiet"));
    MT_EXPECT_EQ(back.value().metadata.find("team")->second, std::string("infra"));
    MT_EXPECT_EQ(back.value().env_vars.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(back.value().volume_mounts.has_value());
    if (!back.value().volume_mounts.has_value()) return;
    MT_EXPECT_EQ((*back.value().volume_mounts)[0].name, std::string("vol-1"));
    MT_EXPECT_EQ((*back.value().volume_mounts)[0].path, std::string("/mnt/data"));
}

MT_TEST(an_absent_and_an_empty_mount_list_stay_distinct) {
    // The whole reason `volumeMounts` is an optional. Collapsing the two makes
    // a snapshot restore indistinguishable from an explicit refusal of every
    // volume -- which `CreateSandbox` now depends on telling apart.
    core::Json absent;
    absent["image"] = core::Json(std::string("python:3.11-slim"));
    const core::Expected<CreateColdSandboxReq, core::AnyError> a =
        CreateColdSandboxReq::FromJson(absent);
    MT_EXPECT_TRUE(a.ok() && !a.value().volume_mounts.has_value());

    core::Json emptied;
    emptied["image"]        = core::Json(std::string("python:3.11-slim"));
    emptied["volumeMounts"] = core::Json(core::JsonArray());
    const core::Expected<CreateColdSandboxReq, core::AnyError> e =
        CreateColdSandboxReq::FromJson(emptied);
    MT_EXPECT_TRUE(e.ok());
    if (!e.ok()) return;
    MT_EXPECT_TRUE(e.value().volume_mounts.has_value());
    MT_EXPECT_TRUE(e.value().volume_mounts->empty());
}

MT_MAIN
