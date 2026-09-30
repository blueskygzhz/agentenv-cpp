// SPDX-License-Identifier: MIT
// Rust: src/api/impls/volumes.rs
#include "agentenv/api/volumes.h"

#include <map>
#include <string>

#include "microtest.h"
#include "volume_fakes.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT

namespace {

using agentenv::testing::Fixture;
using agentenv::volume::VolumeError;
using agentenv::volume::VolumeMode;
using agentenv::volume::VolumeRecord;
using agentenv::volume::VolumeStatus;

std::string StringField(const core::Json& json, const char* name) {
    const core::JsonObject& fields = json.as_object();
    core::JsonObject::const_iterator it = fields.find(name);
    MT_EXPECT_TRUE(it != fields.end());
    return it->second.as_string();
}

int64_t IntField(const core::Json& json, const char* name) {
    const core::JsonObject& fields = json.as_object();
    core::JsonObject::const_iterator it = fields.find(name);
    MT_EXPECT_TRUE(it != fields.end());
    return it->second.as_int();
}

bool HasField(const core::Json& json, const char* name) {
    return json.as_object().find(name) != json.as_object().end();
}

}  // namespace

// ---- error mapping ----------------------------------------------------------

MT_TEST(api_volumes_error_status_mapping) {
    MT_EXPECT_EQ(VolumeErrorStatus(VolumeError::InvalidName()), 400);
    MT_EXPECT_EQ(VolumeErrorStatus(VolumeError::MultipleSources()), 400);
    MT_EXPECT_EQ(VolumeErrorStatus(VolumeError::InvalidSize()), 400);
    MT_EXPECT_EQ(VolumeErrorStatus(VolumeError::SizeMismatch()), 400);
    MT_EXPECT_EQ(VolumeErrorStatus(VolumeError::SizeLimitExceeded(1024)), 400);
    MT_EXPECT_EQ(VolumeErrorStatus(VolumeError::TooManyMountedVolumes(24)), 400);
    MT_EXPECT_EQ(VolumeErrorStatus(VolumeError::SourceNotFound("x")), 400);

    MT_EXPECT_EQ(VolumeErrorStatus(VolumeError::NotFound("x")), 404);

    // The volume exists but cannot serve this request now: a conflict, not a
    // malformed request.
    MT_EXPECT_EQ(VolumeErrorStatus(VolumeError::NameConflict("n")), 409);
    MT_EXPECT_EQ(VolumeErrorStatus(VolumeError::Reserved("sb")), 409);
    MT_EXPECT_EQ(VolumeErrorStatus(VolumeError::Uploading("v")), 409);
    MT_EXPECT_EQ(VolumeErrorStatus(VolumeError::Failed("v")), 409);

    MT_EXPECT_EQ(VolumeErrorStatus(VolumeError::Storage("disk")), 500);
}

MT_TEST(api_volumes_error_response_carries_rendered_message) {
    const VolumeApiError error = VolumeErrorResponse(VolumeError::NotFound("vol_1"));
    MT_EXPECT_EQ(error.code, 404);
    MT_EXPECT_EQ(error.message, VolumeError::NotFound("vol_1").ToString());
    MT_EXPECT_TRUE(error.message.find("vol_1") != std::string::npos);
}

// ---- projection -------------------------------------------------------------

MT_TEST(api_volumes_record_projection) {
    VolumeRecord record;
    record.id      = "vol_1";
    record.name    = "data";
    record.mode    = VolumeMode::Exclusive;
    record.size_mb = 2048;
    record.status  = VolumeStatus::Uploading;

    const core::Json json = VolumeRecordToJson(record);
    MT_EXPECT_EQ(StringField(json, "volumeID"), std::string("vol_1"));
    MT_EXPECT_EQ(StringField(json, "name"), std::string("data"));
    MT_EXPECT_EQ(StringField(json, "mode"), std::string("exclusive"));
    MT_EXPECT_EQ(IntField(json, "sizeMB"), static_cast<int64_t>(2048));
    MT_EXPECT_EQ(StringField(json, "status"), std::string("uploading"));
}

MT_TEST(api_volumes_record_projection_uses_api_mode_spelling) {
    VolumeRecord record;
    record.mode = VolumeMode::ReadOnly;
    // The API spells read-only `"ro"`, which is not the serde spelling of
    // `VolumeMode`.
    MT_EXPECT_EQ(StringField(VolumeRecordToJson(record), "mode"), std::string("ro"));
}

MT_TEST(api_volumes_record_projection_hides_internal_fields) {
    VolumeRecord record;
    record.id                    = "vol_1";
    record.reserved_by_sandbox_id = core::Optional<std::string>("sb-1");
    record.backing_image_config   = core::Optional<std::string>("/var/lib/x/image.json");
    record.read_only_mounts.push_back("sb-2");
    record.deleting               = true;

    // The reservation owner, the node-local cache path and the layer set are
    // internal: none of them belong in a team-facing response.
    const core::Json json = VolumeRecordToJson(record);
    MT_EXPECT_TRUE(!HasField(json, "reservedBySandboxID"));
    MT_EXPECT_TRUE(!HasField(json, "backingImageConfig"));
    MT_EXPECT_TRUE(!HasField(json, "readOnlyMounts"));
    MT_EXPECT_TRUE(!HasField(json, "deleting"));
    MT_EXPECT_TRUE(!HasField(json, "backingLayers"));
}

// ---- mode parsing -----------------------------------------------------------

MT_TEST(api_volumes_mode_param_defaults_to_exclusive) {
    auto absent = ParseVolumeModeParam(core::Optional<std::string>());
    MT_EXPECT_TRUE(absent.ok());
    MT_EXPECT_TRUE(absent.value() == VolumeMode::Exclusive);

    auto explicit_exclusive =
        ParseVolumeModeParam(core::Optional<std::string>(std::string("exclusive")));
    MT_EXPECT_TRUE(explicit_exclusive.ok());
    MT_EXPECT_TRUE(explicit_exclusive.value() == VolumeMode::Exclusive);

    auto read_only = ParseVolumeModeParam(core::Optional<std::string>(std::string("ro")));
    MT_EXPECT_TRUE(read_only.ok());
    MT_EXPECT_TRUE(read_only.value() == VolumeMode::ReadOnly);
}

MT_TEST(api_volumes_mode_param_rejects_unknown_value) {
    // Silently defaulting an unknown mode could hand out a writable volume to
    // a caller that asked for read-only.
    auto bad = ParseVolumeModeParam(core::Optional<std::string>(std::string("readonly")));
    MT_EXPECT_TRUE(!bad.ok());
    MT_EXPECT_EQ(bad.error().code, 400);
    MT_EXPECT_TRUE(bad.error().message.find("readonly") != std::string::npos);

    MT_EXPECT_TRUE(!ParseVolumeModeParam(core::Optional<std::string>(std::string(""))).ok());
    MT_EXPECT_TRUE(!ParseVolumeModeParam(core::Optional<std::string>(std::string("RO"))).ok());
}

// ---- mount path overlap -----------------------------------------------------

MT_TEST(api_volumes_mount_overlap_is_component_wise) {
    // Nesting shadows, so it overlaps.
    MT_EXPECT_TRUE(MountPathsOverlap("/mnt/a", "/mnt/a/b"));
    MT_EXPECT_TRUE(MountPathsOverlap("/mnt/a/b", "/mnt/a"));
    MT_EXPECT_TRUE(MountPathsOverlap("/mnt/a", "/mnt/a"));

    // Siblings whose names share a string prefix do *not* overlap: `/mnt/ab`
    // is not inside `/mnt/a`.
    MT_EXPECT_TRUE(!MountPathsOverlap("/mnt/a", "/mnt/ab"));
    MT_EXPECT_TRUE(!MountPathsOverlap("/mnt/ab", "/mnt/a"));
    MT_EXPECT_TRUE(!MountPathsOverlap("/mnt/a", "/mnt/b"));
    MT_EXPECT_TRUE(!MountPathsOverlap("/data", "/database"));
}

MT_TEST(api_volumes_mount_overlap_ignores_redundant_separators) {
    MT_EXPECT_TRUE(MountPathsOverlap("/mnt//a", "/mnt/a/b"));
    MT_EXPECT_TRUE(MountPathsOverlap("/mnt/a/", "/mnt/a"));
    // Everything is inside the root.
    MT_EXPECT_TRUE(MountPathsOverlap("/", "/mnt/a"));
}

// ---- mount resolution -------------------------------------------------------

MT_TEST(api_volumes_resolve_empty_mounts_needs_no_manager) {
    auto resolved =
        ResolveVolumeMounts(NULL, std::map<std::string, std::string>(), "sb-1");
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(resolved.value().drives.empty());
    MT_EXPECT_TRUE(resolved.value().normalized_mounts.empty());
}

MT_TEST(api_volumes_resolve_single_mount) {
    Fixture fixture;
    const VolumeRecord volume = fixture.PublishedVolume();

    std::map<std::string, std::string> mounts;
    mounts["/mnt/data"] = volume.id;

    auto resolved = ResolveVolumeMounts(fixture.manager.get(), mounts, "sb-1");
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_EQ(resolved.value().drives.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(resolved.value().drives[0].drive_id, volume.id);
    MT_EXPECT_EQ(resolved.value().drives[0].mount_path, std::string("/mnt/data"));
    MT_EXPECT_EQ(resolved.value().normalized_mounts.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(resolved.value().normalized_mounts["/mnt/data"], volume.id);
}

MT_TEST(api_volumes_resolve_sets_virtual_size_from_record) {
    Fixture fixture;
    const VolumeRecord volume = fixture.PublishedVolume();

    std::map<std::string, std::string> mounts;
    mounts["/mnt/data"] = volume.id;

    auto resolved = ResolveVolumeMounts(fixture.manager.get(), mounts, "sb-1");
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(resolved.value().drives[0].virtual_size.has_value());
    MT_EXPECT_EQ(*resolved.value().drives[0].virtual_size,
                 volume.size_mb * agentenv::volume::kBytesPerMb);
}

MT_TEST(api_volumes_exclusive_mount_gets_snapshot_output_dir) {
    Fixture fixture;
    const VolumeRecord volume = fixture.PublishedVolume();  // Exclusive

    std::map<std::string, std::string> mounts;
    mounts["/mnt/data"] = volume.id;

    // Only an exclusive volume captures its changes back out.
    auto resolved = ResolveVolumeMounts(fixture.manager.get(), mounts, "sb-1");
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(resolved.value().drives[0].snapshot_output_dir.has_value());
    MT_EXPECT_TRUE(resolved.value().drives[0].volume);
    MT_EXPECT_TRUE(!resolved.value().drives[0].read_only);
}

MT_TEST(api_volumes_reserves_the_mounted_volume) {
    Fixture fixture;
    const VolumeRecord volume = fixture.PublishedVolume();

    std::map<std::string, std::string> mounts;
    mounts["/mnt/data"] = volume.id;
    MT_EXPECT_TRUE(ResolveVolumeMounts(fixture.manager.get(), mounts, "sb-1").ok());

    // A successful resolution leaves the lease in place for the launch.
    auto reloaded = fixture.manager->Get(volume.id);
    MT_EXPECT_TRUE(reloaded.ok());
    MT_EXPECT_TRUE(reloaded.value().MountedBy("sb-1"));
}

MT_TEST(api_volumes_rejects_overlapping_mount_paths) {
    Fixture fixture;
    const VolumeRecord volume = fixture.PublishedVolume();

    std::map<std::string, std::string> mounts;
    mounts["/mnt/data"]       = volume.id;
    mounts["/mnt/data/inner"] = volume.id;

    auto resolved = ResolveVolumeMounts(fixture.manager.get(), mounts, "sb-1");
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().code, 400);
    MT_EXPECT_TRUE(resolved.error().message.find("overlaps") != std::string::npos);
}

MT_TEST(api_volumes_rejects_same_volume_mounted_twice) {
    Fixture fixture;
    const VolumeRecord volume = fixture.PublishedVolume();

    // Two non-overlapping paths, one volume: the guest would get two views of
    // a single device.
    std::map<std::string, std::string> mounts;
    mounts["/mnt/one"] = volume.id;
    mounts["/mnt/two"] = volume.id;

    auto resolved = ResolveVolumeMounts(fixture.manager.get(), mounts, "sb-1");
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().code, 400);
    MT_EXPECT_TRUE(resolved.error().message.find("more than once") != std::string::npos);
}

MT_TEST(api_volumes_releases_reservations_when_resolution_fails) {
    Fixture fixture;
    const VolumeRecord volume = fixture.PublishedVolume();

    std::map<std::string, std::string> mounts;
    // `/mnt/one` resolves and reserves; `/mnt/two` then fails the duplicate
    // check, which must roll the first reservation back.
    mounts["/mnt/one"] = volume.id;
    mounts["/mnt/two"] = volume.id;

    MT_EXPECT_TRUE(!ResolveVolumeMounts(fixture.manager.get(), mounts, "sb-1").ok());

    // Nothing may stay leased to a sandbox that never launched.
    auto reloaded = fixture.manager->Get(volume.id);
    MT_EXPECT_TRUE(reloaded.ok());
    MT_EXPECT_TRUE(!reloaded.value().MountedBy("sb-1"));

    // And the volume is mountable again by the next request.
    std::map<std::string, std::string> retry;
    retry["/mnt/data"] = volume.id;
    MT_EXPECT_TRUE(ResolveVolumeMounts(fixture.manager.get(), retry, "sb-2").ok());
}

MT_TEST(api_volumes_rejects_unknown_volume_reference) {
    Fixture fixture;
    std::map<std::string, std::string> mounts;
    mounts["/mnt/data"] = "vol_missing";

    auto resolved = ResolveVolumeMounts(fixture.manager.get(), mounts, "sb-1");
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().code, 404);
}

MT_TEST(api_volumes_rejects_invalid_mount_path) {
    Fixture fixture;
    const VolumeRecord volume = fixture.PublishedVolume();

    std::map<std::string, std::string> mounts;
    mounts["relative/path"] = volume.id;

    auto resolved = ResolveVolumeMounts(fixture.manager.get(), mounts, "sb-1");
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().code, 400);

    // The bad path was rejected before anything was reserved.
    auto reloaded = fixture.manager->Get(volume.id);
    MT_EXPECT_TRUE(reloaded.ok());
    MT_EXPECT_TRUE(!reloaded.value().MountedBy("sb-1"));
}

MT_TEST(api_volumes_rejects_too_many_mounts) {
    Fixture fixture;
    const VolumeRecord volume = fixture.PublishedVolume();

    std::map<std::string, std::string> mounts;
    // The ceiling is hardware-shaped (/dev/vdc..vdz), not a policy knob.
    for (std::size_t i = 0; i <= agentenv::volume::kMaxVolumeMounts; ++i) {
        std::ostringstream path;
        path << "/mnt/v" << i;
        mounts[path.str()] = volume.id;
    }

    auto resolved = ResolveVolumeMounts(fixture.manager.get(), mounts, "sb-1");
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().code, 400);
    // Refused up front, so nothing was reserved.
    auto reloaded = fixture.manager->Get(volume.id);
    MT_EXPECT_TRUE(reloaded.ok());
    MT_EXPECT_TRUE(!reloaded.value().MountedBy("sb-1"));
}

MT_TEST(api_volumes_rejects_volume_over_size_limit) {
    agentenv::testing::Fixture fixture;
    const VolumeRecord volume = fixture.PublishedVolume();

    // Reopen the manager with a limit below the volume's size.
    agentenv::volume::VolumeLimits limits;
    limits.max_size_mb = volume.size_mb - 1;
    limits.max_mounts  = 8;
    const agentenv::volume::VolumeResult<std::shared_ptr<agentenv::volume::VolumeManager> >
        reopened = agentenv::volume::VolumeManager::OpenWithRepositoryAndLimits(
            agentenv::core::fs::Join(fixture.root, "volumes/catalog"), fixture.repository,
            limits);
    MT_EXPECT_TRUE(reopened.ok());

    std::map<std::string, std::string> mounts;
    mounts["/mnt/data"] = volume.id;

    auto resolved = ResolveVolumeMounts(reopened.value().get(), mounts, "sb-1");
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().code, 400);
}

MT_TEST(api_volumes_resolution_is_ordered_by_mount_path) {
    Fixture fixture;
    const VolumeRecord first  = fixture.PublishedVolume();
    const agentenv::volume::VolumeResult<VolumeRecord> second =
        fixture.manager->Create("second", VolumeMode::Exclusive,
                                core::Optional<std::string>(first.id),
                                core::Optional<std::string>(), first.size_mb);
    MT_EXPECT_TRUE(second.ok());

    std::map<std::string, std::string> mounts;
    mounts["/mnt/zebra"] = first.id;
    mounts["/mnt/alpha"] = second.value().id;

    // Rust sorts the mounts before reserving so the order is deterministic.
    auto resolved = ResolveVolumeMounts(fixture.manager.get(), mounts, "sb-1");
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_EQ(resolved.value().drives.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(resolved.value().drives[0].mount_path, std::string("/mnt/alpha"));
    MT_EXPECT_EQ(resolved.value().drives[1].mount_path, std::string("/mnt/zebra"));
}

int main() { return microtest::RunAll(); }
