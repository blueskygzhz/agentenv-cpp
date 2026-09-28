// SPDX-License-Identifier: MIT
// Rust: src/sandbox/extra_drive.rs and src/sandbox/manifest.rs test modules.
#include <string>
#include <vector>

#include "agentenv/sandbox/extra_drive.h"
#include "agentenv/sandbox/manifest.h"
#include "agentenv/snapshot/startup_pack.h"
#include "microtest.h"

namespace {

using agentenv::core::Json;
using agentenv::core::JsonObject;
using agentenv::core::Optional;
using agentenv::core::Unit;
namespace sandbox = agentenv::sandbox;
namespace snapshot = agentenv::snapshot;

/// Rust test fixture shape: an `ExtraDrive::Overlaybd` built field-by-field
/// rather than through the validating constructor, which is how the Rust tests
/// construct drives that would not pass validation.
sandbox::ExtraDrive RawDrive(const std::string& drive_id,
                             const Optional<uint64_t>& virtual_size) {
    sandbox::ExtraDrive drive;
    drive.kind = sandbox::ExtraDrive::Kind::Overlaybd;
    drive.drive_id = drive_id;
    drive.image_config_path = "/tmp/" + drive_id + "/image.json";
    drive.read_only = true;
    drive.mount_path = sandbox::ExtraDrive::DefaultMountPath(drive_id);
    drive.virtual_size = virtual_size;
    drive.volume = false;
    return drive;
}

sandbox::SandboxSnapshotManifest ValidManifest() {
    const agentenv::core::Expected<sandbox::SandboxSnapshotManifest, std::string> manifest =
        sandbox::SandboxSnapshotManifest::New(sandbox::kFirecrackerBackend, "vm_state.bin",
                                              "mem_image.json", 4096, "rootfs/image.json",
                                              4096, std::vector<sandbox::ExtraDrive>());
    MT_EXPECT_TRUE(manifest.ok());
    return manifest.value();
}

}  // namespace

// ---------------------------------------------------------------------------
// extra_drive — drive id validation
// ---------------------------------------------------------------------------

MT_TEST(validate_drive_id_accepts_alphanumeric_and_underscore) {
    MT_EXPECT_TRUE(sandbox::ValidateDriveId("data").ok());
    MT_EXPECT_TRUE(sandbox::ValidateDriveId("data_1").ok());
    MT_EXPECT_TRUE(sandbox::ValidateDriveId("DATA").ok());
    MT_EXPECT_TRUE(sandbox::ValidateDriveId("_").ok());
    MT_EXPECT_TRUE(sandbox::ValidateDriveId("9").ok());
}

MT_TEST(validate_drive_id_rejects_empty_and_whitespace) {
    // Rust trims before the emptiness test, so a blank id is empty, not
    // "contains a forbidden character".
    const agentenv::core::Expected<Unit, std::string> empty = sandbox::ValidateDriveId("");
    MT_EXPECT_TRUE(!empty.ok());
    MT_EXPECT_TRUE(empty.error().find("must not be empty") != std::string::npos);

    const agentenv::core::Expected<Unit, std::string> blank = sandbox::ValidateDriveId("   ");
    MT_EXPECT_TRUE(!blank.ok());
    MT_EXPECT_TRUE(blank.error().find("must not be empty") != std::string::npos);
}

MT_TEST(validate_drive_id_rejects_dashes_and_punctuation) {
    // A dash is not allowed: the id becomes part of a Firecracker drive id and
    // a filesystem path, so the accepted set is deliberately narrow.
    const char* const rejected[] = {"data-1", "data.1", "data/1", "data 1", "data:1",
                                    "dat\xc3\xa9"};
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        const agentenv::core::Expected<Unit, std::string> result =
            sandbox::ValidateDriveId(rejected[i]);
        MT_EXPECT_TRUE(!result.ok());
        MT_EXPECT_TRUE(result.error().find("ASCII letters, numbers, and underscores") !=
                       std::string::npos);
    }
}

MT_TEST(validate_drive_id_rejects_reserved_names) {
    // These would collide with drives AgentENV attaches itself, so a user
    // drive under one of these ids would displace a system drive.
    const char* const reserved[] = {"rootfs", "user_rootfs", "agentenv_volume_slot_0",
                                    "agentenv_volume_slot_"};
    for (std::size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
        const agentenv::core::Expected<Unit, std::string> result =
            sandbox::ValidateDriveId(reserved[i]);
        MT_EXPECT_TRUE(!result.ok());
        MT_EXPECT_TRUE(result.error().find("is reserved") != std::string::npos);
    }
    // Only the exact names and the slot *prefix* are reserved.
    MT_EXPECT_TRUE(sandbox::ValidateDriveId("rootfs2").ok());
    MT_EXPECT_TRUE(sandbox::ValidateDriveId("my_rootfs").ok());
}

// ---------------------------------------------------------------------------
// extra_drive — mount path validation
// ---------------------------------------------------------------------------

MT_TEST(validate_mount_path_requires_an_absolute_non_root_path) {
    MT_EXPECT_TRUE(sandbox::ValidateMountPath("/mnt/data").ok());

    const agentenv::core::Expected<Unit, std::string> relative =
        sandbox::ValidateMountPath("mnt/data");
    MT_EXPECT_TRUE(!relative.ok());
    MT_EXPECT_TRUE(relative.error().find("must be absolute") != std::string::npos);
    MT_EXPECT_TRUE(!sandbox::ValidateMountPath("").ok());

    // Mounting over `/` would replace the guest root.
    const agentenv::core::Expected<Unit, std::string> root = sandbox::ValidateMountPath("/");
    MT_EXPECT_TRUE(!root.ok());
    MT_EXPECT_TRUE(root.error().find("must not be /") != std::string::npos);
}

MT_TEST(validate_mount_path_rejects_whitespace_commas_and_colons) {
    // These are separators in the mount options string the path is spliced
    // into, so allowing them would let one drive inject an extra option.
    const char* const rejected[] = {"/mnt/my data", "/mnt/a,b", "/mnt/a:b", "/mnt/a\tb"};
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        const agentenv::core::Expected<Unit, std::string> result =
            sandbox::ValidateMountPath(rejected[i]);
        MT_EXPECT_TRUE(!result.ok());
        MT_EXPECT_TRUE(result.error().find("whitespace, commas, or colons") !=
                       std::string::npos);
    }
}

MT_TEST(validate_mount_path_rejects_parent_dir_components) {
    const agentenv::core::Expected<Unit, std::string> result =
        sandbox::ValidateMountPath("/mnt/../etc");
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("must not contain '..'") != std::string::npos);

    // Component-wise, so a name that merely contains dots is fine.
    MT_EXPECT_TRUE(sandbox::ValidateMountPath("/mnt/a..b").ok());
    MT_EXPECT_TRUE(sandbox::ValidateMountPath("/mnt/...").ok());
}

MT_TEST(validate_mount_path_rejects_reserved_paths_in_both_directions) {
    // A descendant would overlay the reserved path.
    const char* const descendants[] = {"/proc", "/proc/self", "/sys/fs", "/dev/shm", "/run/x",
                                       "/agentenv/bin", "/opt/agentenv/x"};
    for (std::size_t i = 0; i < sizeof(descendants) / sizeof(descendants[0]); ++i) {
        const agentenv::core::Expected<Unit, std::string> result =
            sandbox::ValidateMountPath(descendants[i]);
        MT_EXPECT_TRUE(!result.ok());
        MT_EXPECT_TRUE(result.error().find("reserved path") != std::string::npos);
    }

    // An ancestor would shadow it: mounting `/opt` hides `/opt/agentenv`.
    const agentenv::core::Expected<Unit, std::string> ancestor =
        sandbox::ValidateMountPath("/opt");
    MT_EXPECT_TRUE(!ancestor.ok());
    MT_EXPECT_TRUE(ancestor.error().find("reserved path") != std::string::npos);
}

MT_TEST(validate_mount_path_compares_reserved_paths_component_wise) {
    // A plain string-prefix check would wrongly reject `/p` as a prefix of
    // `/proc`, and wrongly accept `/proc-data` as unrelated to `/proc`.
    MT_EXPECT_TRUE(sandbox::ValidateMountPath("/p").ok());
    MT_EXPECT_TRUE(sandbox::ValidateMountPath("/procfs").ok());
    MT_EXPECT_TRUE(sandbox::ValidateMountPath("/devices").ok());
    MT_EXPECT_TRUE(sandbox::ValidateMountPath("/opt/agentenv2").ok());
}

MT_TEST(validate_mount_path_allows_tmp) {
    // Deliberately not reserved: /tmp belongs to the guest filesystem and
    // replacing it hides no control-plane files.
    MT_EXPECT_TRUE(sandbox::ValidateMountPath("/tmp").ok());
    MT_EXPECT_TRUE(sandbox::ValidateMountPath("/tmp/data").ok());
}

// ---------------------------------------------------------------------------
// extra_drive — sub path validation
// ---------------------------------------------------------------------------

MT_TEST(validate_sub_path_requires_a_non_empty_relative_path) {
    MT_EXPECT_EQ(sandbox::ValidateSubPath("nested/dir").value(), std::string("nested/dir"));

    const agentenv::core::Expected<std::string, std::string> empty =
        sandbox::ValidateSubPath("");
    MT_EXPECT_TRUE(!empty.ok());
    MT_EXPECT_TRUE(empty.error().find("must not be empty") != std::string::npos);

    const agentenv::core::Expected<std::string, std::string> absolute =
        sandbox::ValidateSubPath("/nested");
    MT_EXPECT_TRUE(!absolute.ok());
    MT_EXPECT_TRUE(absolute.error().find("must be a relative path") != std::string::npos);
}

MT_TEST(validate_sub_path_rejects_escapes_and_separators) {
    // A `..` here would bind a directory from outside the drive root onto the
    // mount point.
    const agentenv::core::Expected<std::string, std::string> escape =
        sandbox::ValidateSubPath("../escape");
    MT_EXPECT_TRUE(!escape.ok());
    MT_EXPECT_TRUE(escape.error().find("must not contain '..'") != std::string::npos);

    MT_EXPECT_TRUE(!sandbox::ValidateSubPath("a b").ok());
    MT_EXPECT_TRUE(!sandbox::ValidateSubPath("a,b").ok());
    MT_EXPECT_TRUE(!sandbox::ValidateSubPath("a:b").ok());
    // Dots inside a name are fine.
    MT_EXPECT_TRUE(sandbox::ValidateSubPath("a..b").ok());
}

// ---------------------------------------------------------------------------
// extra_drive — normalisation
// ---------------------------------------------------------------------------

MT_TEST(normalize_mount_path_rebuilds_from_components) {
    // Without this, `/mnt//data/.` and `/mnt/data` would be two stored values
    // for one mount point.
    MT_EXPECT_EQ(sandbox::NormalizeMountPath("/mnt//data/.").value(),
                 std::string("/mnt/data"));
    MT_EXPECT_EQ(sandbox::NormalizeMountPath("/mnt/data/").value(), std::string("/mnt/data"));
    MT_EXPECT_EQ(sandbox::NormalizeMountPath("/mnt/data").value(), std::string("/mnt/data"));
}

MT_TEST(normalize_mount_path_validates_first) {
    // Normalising before validating would let `/mnt/../proc` collapse to
    // `/proc` and pass.
    MT_EXPECT_TRUE(!sandbox::NormalizeMountPath("/mnt/../proc").ok());
    MT_EXPECT_TRUE(!sandbox::NormalizeMountPath("/").ok());
    MT_EXPECT_TRUE(!sandbox::NormalizeMountPath("relative").ok());
}

MT_TEST(normalize_mount_path_for_drive_substitutes_the_default) {
    // An empty mount path is the API's way of saying "use the default".
    MT_EXPECT_EQ(sandbox::NormalizeMountPathForDrive("data", "").value(),
                 std::string("/mnt/data"));
    MT_EXPECT_EQ(sandbox::NormalizeMountPathForDrive("data", "/custom//path").value(),
                 std::string("/custom/path"));
}

MT_TEST(default_mount_path_is_under_the_mount_root) {
    MT_EXPECT_EQ(sandbox::ExtraDrive::DefaultMountPath("data"), std::string("/mnt/data"));
}

// ---------------------------------------------------------------------------
// extra_drive — constructors and derived copies
// ---------------------------------------------------------------------------

MT_TEST(try_new_overlaybd_validates_and_normalizes) {
    const agentenv::core::Expected<sandbox::ExtraDrive, std::string> drive =
        sandbox::ExtraDrive::TryNewOverlaybd("data", "/images/data.json", true);
    MT_EXPECT_TRUE(drive.ok());
    MT_EXPECT_EQ(drive.value().drive_id, std::string("data"));
    MT_EXPECT_EQ(drive.value().mount_path, std::string("/mnt/data"));
    MT_EXPECT_TRUE(drive.value().read_only);
    // A fresh drive has no size yet; the daemon reads it from the image.
    MT_EXPECT_TRUE(!drive.value().virtual_size.has_value());
    MT_EXPECT_TRUE(!drive.value().sub_path.has_value());
    MT_EXPECT_TRUE(!drive.value().volume);

    MT_EXPECT_TRUE(!sandbox::ExtraDrive::TryNewOverlaybd("root-fs", "/i.json", true).ok());
    MT_EXPECT_TRUE(!sandbox::ExtraDrive::TryNewOverlaybd("rootfs", "/i.json", true).ok());
}

MT_TEST(try_new_overlaybd_with_mount_path_validates_the_sub_path) {
    const agentenv::core::Expected<sandbox::ExtraDrive, std::string> drive =
        sandbox::ExtraDrive::TryNewOverlaybdWithMountPath(
            "data", "/images/data.json", false, "/srv//data",
            Optional<std::string>(std::string("inner/dir")));
    MT_EXPECT_TRUE(drive.ok());
    MT_EXPECT_EQ(drive.value().mount_path, std::string("/srv/data"));
    MT_EXPECT_EQ(*drive.value().sub_path, std::string("inner/dir"));

    // A bad sub path fails the whole construction rather than being dropped.
    MT_EXPECT_TRUE(!sandbox::ExtraDrive::TryNewOverlaybdWithMountPath(
                        "data", "/i.json", false, "/srv/data",
                        Optional<std::string>(std::string("../escape")))
                        .ok());
    // A bad mount path likewise.
    MT_EXPECT_TRUE(!sandbox::ExtraDrive::TryNewOverlaybdWithMountPath(
                        "data", "/i.json", false, "/proc", Optional<std::string>())
                        .ok());
}

MT_TEST(runtime_dir_and_symlink_name_are_derived_from_the_drive_id) {
    const sandbox::ExtraDrive drive = RawDrive("data", Optional<uint64_t>());
    MT_EXPECT_EQ(drive.RuntimeDir("/run/sandbox-1"),
                 std::string("/run/sandbox-1/extra-drive-runtime-data"));
    MT_EXPECT_EQ(drive.AttachmentSymlinkName(), std::string("extra-drive-data"));
}

MT_TEST(with_volume_snapshot_output_dir_also_marks_the_drive_a_volume) {
    // Rust sets `volume: true` unconditionally in this constructor, so the two
    // cannot drift apart.
    const sandbox::ExtraDrive drive = RawDrive("data", Optional<uint64_t>());
    MT_EXPECT_TRUE(!drive.volume);

    const sandbox::ExtraDrive with_dir = drive.WithVolumeSnapshotOutputDir(
        Optional<std::string>(std::string("/snapshots/data")));
    MT_EXPECT_TRUE(with_dir.volume);
    MT_EXPECT_EQ(*with_dir.snapshot_output_dir, std::string("/snapshots/data"));

    // Even with no output directory, the drive is still a volume.
    const sandbox::ExtraDrive without_dir =
        drive.WithVolumeSnapshotOutputDir(Optional<std::string>());
    MT_EXPECT_TRUE(without_dir.volume);
    MT_EXPECT_TRUE(!without_dir.snapshot_output_dir.has_value());
}

MT_TEST(with_image_config_path_preserves_every_other_field) {
    sandbox::ExtraDrive drive = RawDrive("data", Optional<uint64_t>(static_cast<uint64_t>(1)));
    drive.sub_path = Optional<std::string>(std::string("inner"));
    drive.volume = true;

    const sandbox::ExtraDrive updated = drive.WithImageConfigPath("/new/image.json");
    MT_EXPECT_EQ(updated.image_config_path, std::string("/new/image.json"));
    MT_EXPECT_EQ(updated.drive_id, drive.drive_id);
    MT_EXPECT_EQ(updated.mount_path, drive.mount_path);
    MT_EXPECT_EQ(*updated.sub_path, std::string("inner"));
    MT_EXPECT_TRUE(updated.volume);
    MT_EXPECT_EQ(*updated.virtual_size, static_cast<uint64_t>(1));
}

MT_TEST(try_with_virtual_size_rejects_zero) {
    const sandbox::ExtraDrive drive = RawDrive("data", Optional<uint64_t>());
    MT_EXPECT_EQ(*drive.TryWithVirtualSize(4096).value().virtual_size,
                 static_cast<uint64_t>(4096));

    // Zero would reach the daemon as a request for an empty device.
    const agentenv::core::Expected<sandbox::ExtraDrive, std::string> zero =
        drive.TryWithVirtualSize(0);
    MT_EXPECT_TRUE(!zero.ok());
    MT_EXPECT_TRUE(zero.error().find("must be non-zero") != std::string::npos);
}

// ---------------------------------------------------------------------------
// extra_drive — prepare mode
// ---------------------------------------------------------------------------

MT_TEST(prepare_mode_reads_virtual_size_differently_per_phase) {
    const sandbox::ExtraDrive drive =
        RawDrive("data", Optional<uint64_t>(static_cast<uint64_t>(8192)));

    Optional<uint64_t> target;
    Optional<uint64_t> base;

    // Fresh: the size is a requested target, and the base is left unknown so
    // the daemon reads it from the source image.
    sandbox::ExtraDrivePrepareMode::MakeFresh(true).DeviceSizes(drive, &target, &base);
    MT_EXPECT_EQ(*target, static_cast<uint64_t>(8192));
    MT_EXPECT_TRUE(!base.has_value());

    // Resume: the size is the recorded actual device size, so it is both.
    sandbox::ExtraDrivePrepareMode::MakeResume().DeviceSizes(drive, &target, &base);
    MT_EXPECT_EQ(*target, static_cast<uint64_t>(8192));
    MT_EXPECT_EQ(*base, static_cast<uint64_t>(8192));
}

MT_TEST(prepare_mode_never_allows_shrink_on_resume) {
    MT_EXPECT_TRUE(sandbox::ExtraDrivePrepareMode::MakeFresh(true).AllowShrink());
    MT_EXPECT_TRUE(!sandbox::ExtraDrivePrepareMode::MakeFresh(false).AllowShrink());
    // Shrinking here would truncate a device the snapshot already sized.
    MT_EXPECT_TRUE(!sandbox::ExtraDrivePrepareMode::MakeResume().AllowShrink());
}

// ---------------------------------------------------------------------------
// manifest — construction
// ---------------------------------------------------------------------------

MT_TEST(new_rejects_an_unknown_backend) {
    // An unknown name would reach a published record that no restore could
    // act on, so it is refused at construction.
    const agentenv::core::Expected<sandbox::SandboxSnapshotManifest, std::string> manifest =
        sandbox::SandboxSnapshotManifest::New("qemu", "vm_state.bin", "mem.json", 0,
                                              "rootfs.json", 4096,
                                              std::vector<sandbox::ExtraDrive>());
    MT_EXPECT_TRUE(!manifest.ok());
    MT_EXPECT_TRUE(manifest.error().find("names no VMM a restore can reach") !=
                   std::string::npos);
    MT_EXPECT_TRUE(sandbox::IsKnownBackend("firecracker"));
    MT_EXPECT_TRUE(!sandbox::IsKnownBackend(""));
}

MT_TEST(new_rejects_attached_drive_without_virtual_size) {
    // Rust: `new_rejects_attached_drive_without_virtual_size`.
    std::vector<sandbox::ExtraDrive> drives;
    drives.push_back(RawDrive("data", Optional<uint64_t>()));

    const agentenv::core::Expected<sandbox::SandboxSnapshotManifest, std::string> manifest =
        sandbox::SandboxSnapshotManifest::New(sandbox::kFirecrackerBackend, "vm_state.bin",
                                              "mem_image.json", 4096, "rootfs/image.json",
                                              4096, drives);
    MT_EXPECT_TRUE(!manifest.ok());
    MT_EXPECT_TRUE(manifest.error().find("virtual size must be known") != std::string::npos);
}

MT_TEST(with_extra_drives_rejects_zero_virtual_size) {
    // Rust: `with_extra_drives_rejects_zero_virtual_size`.
    std::vector<sandbox::ExtraDrive> drives;
    drives.push_back(RawDrive("data", Optional<uint64_t>(static_cast<uint64_t>(0))));

    const agentenv::core::Expected<sandbox::SandboxSnapshotManifest, std::string> updated =
        ValidManifest().WithExtraDrives(drives);
    MT_EXPECT_TRUE(!updated.ok());
    MT_EXPECT_TRUE(updated.error().find("virtual size must be non-zero") !=
                   std::string::npos);
}

MT_TEST(with_extra_drives_leaves_the_original_untouched) {
    const sandbox::SandboxSnapshotManifest original = ValidManifest();
    MT_EXPECT_EQ(original.attached_drives.size(), static_cast<std::size_t>(0));

    std::vector<sandbox::ExtraDrive> drives;
    drives.push_back(RawDrive("data", Optional<uint64_t>(static_cast<uint64_t>(4096))));
    const sandbox::SandboxSnapshotManifest updated =
        original.WithExtraDrives(drives).value();

    MT_EXPECT_EQ(updated.attached_drives.size(), static_cast<std::size_t>(1));
    // A rejected or accepted call must not mutate the receiver.
    MT_EXPECT_EQ(original.attached_drives.size(), static_cast<std::size_t>(0));
}

MT_TEST(new_counts_physical_drives_from_its_input) {
    std::vector<sandbox::ExtraDrive> drives;
    drives.push_back(RawDrive("data", Optional<uint64_t>(static_cast<uint64_t>(4096))));
    drives.push_back(RawDrive("logs", Optional<uint64_t>(static_cast<uint64_t>(8192))));

    const sandbox::SandboxSnapshotManifest manifest =
        sandbox::SandboxSnapshotManifest::New(sandbox::kFirecrackerBackend, "vm_state.bin",
                                              "mem_image.json", 0, "rootfs/image.json", 4096,
                                              drives)
            .value();
    MT_EXPECT_EQ(manifest.physical_extra_drive_count, static_cast<std::size_t>(2));
    MT_EXPECT_EQ(manifest.volume_drive_slots, static_cast<std::size_t>(0));
    MT_EXPECT_EQ(manifest.version, sandbox::kManifestFormatVersion);
}

// ---------------------------------------------------------------------------
// manifest — round trip to ExtraDrive
// ---------------------------------------------------------------------------

MT_TEST(attached_drive_virtual_size_is_mapped_to_runtime_input) {
    // Rust: `attached_drive_virtual_size_is_serialized_and_mapped_to_runtime_input`.
    sandbox::SnapshotAttachedDriveArtifacts known;
    known.drive_id = "data";
    known.read_only = true;
    known.mount_path = "/mnt/data";
    known.virtual_size = 4096;
    known.image_config_path = "drives/data/image.json";

    const Json known_json = known.ToJson();
    MT_EXPECT_EQ(known_json.as_object().find("virtualSize")->second.as_int(),
                 static_cast<int64_t>(4096));

    sandbox::SandboxSnapshotManifest manifest = ValidManifest();
    manifest.attached_drives.push_back(known);
    manifest.physical_extra_drive_count = 1;

    const std::vector<sandbox::ExtraDrive> drives = manifest.ExtraDrives();
    MT_EXPECT_EQ(drives.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(*drives[0].virtual_size, static_cast<uint64_t>(4096));
    MT_EXPECT_EQ(drives[0].image_config_path, std::string("drives/data/image.json"));
    MT_EXPECT_TRUE(drives[0].read_only);
    // A drive read back from a manifest is never a volume: volumes are
    // omitted from committed manifests and rebound through reserved slots.
    MT_EXPECT_TRUE(!drives[0].volume);
    MT_EXPECT_TRUE(!drives[0].snapshot_output_dir.has_value());
}

MT_TEST(extra_drives_falls_back_to_the_default_mount_path) {
    // A stored mount path that no longer validates must not make an existing
    // snapshot unusable.
    sandbox::SnapshotAttachedDriveArtifacts stored;
    stored.drive_id = "data";
    stored.virtual_size = 4096;
    stored.mount_path = "/proc";  // reserved today

    sandbox::SandboxSnapshotManifest manifest = ValidManifest();
    manifest.attached_drives.push_back(stored);

    MT_EXPECT_EQ(manifest.ExtraDrives()[0].mount_path, std::string("/mnt/data"));

    // An empty stored path takes the default through the same path.
    manifest.attached_drives[0].mount_path = "";
    MT_EXPECT_EQ(manifest.ExtraDrives()[0].mount_path, std::string("/mnt/data"));
}

MT_TEST(extra_drives_normalizes_a_valid_stored_mount_path) {
    sandbox::SnapshotAttachedDriveArtifacts stored;
    stored.drive_id = "data";
    stored.virtual_size = 4096;
    stored.mount_path = "/srv//data/.";

    sandbox::SandboxSnapshotManifest manifest = ValidManifest();
    manifest.attached_drives.push_back(stored);
    MT_EXPECT_EQ(manifest.ExtraDrives()[0].mount_path, std::string("/srv/data"));
}

// ---------------------------------------------------------------------------
// manifest — serialisation
// ---------------------------------------------------------------------------

MT_TEST(attached_drive_virtual_size_is_required) {
    // Rust: `attached_drive_virtual_size_is_required`. A drive whose size is
    // unknown cannot be restored, so there is no serde default for it.
    const agentenv::core::Expected<Json, agentenv::core::AnyError> json = Json::Parse(
        "{\"driveId\":\"data\",\"readOnly\":true,\"mountPath\":\"/mnt/data\"}");
    MT_EXPECT_TRUE(json.ok());

    const agentenv::core::Expected<sandbox::SnapshotAttachedDriveArtifacts, std::string>
        parsed = sandbox::SnapshotAttachedDriveArtifacts::FromJson(json.value());
    MT_EXPECT_TRUE(!parsed.ok());
    MT_EXPECT_TRUE(parsed.error().find("virtualSize") != std::string::npos);
}

MT_TEST(manifest_without_memory_startup_pack_parses_as_none) {
    // Rust: `manifest_without_memory_startup_pack_parses_as_none`. Older
    // records carry no pack, and that is a normal resume, not an error.
    const agentenv::core::Expected<Json, agentenv::core::AnyError> json =
        Json::Parse("{\"version\":1,\"vmState\":{},\"memory\":{\"virtualSize\":4096},"
                    "\"rootfs\":{\"virtualSize\":4096},\"attachedDrives\":[]}");
    MT_EXPECT_TRUE(json.ok());

    const agentenv::core::Expected<sandbox::SandboxSnapshotManifest, std::string> manifest =
        sandbox::SandboxSnapshotManifest::FromJson(json.value());
    MT_EXPECT_TRUE(manifest.ok());
    MT_EXPECT_TRUE(!manifest.value().memory_startup_pack.has_value());
    // A record written before `backend` existed holds a Firecracker capture.
    MT_EXPECT_EQ(manifest.value().backend, std::string("firecracker"));
    MT_EXPECT_EQ(manifest.value().volume_drive_slots, static_cast<std::size_t>(0));
}

MT_TEST(manifest_round_trips_through_json) {
    sandbox::SandboxSnapshotManifest manifest = ValidManifest();
    manifest.volume_drive_slots = 4;
    manifest.physical_extra_drive_count = 1;

    sandbox::SnapshotAttachedDriveArtifacts drive;
    drive.drive_id = "data";
    drive.read_only = false;
    drive.mount_path = "/mnt/data";
    drive.sub_path = Optional<std::string>(std::string("inner"));
    drive.virtual_size = 8192;
    manifest.attached_drives.push_back(drive);

    snapshot::ResolvedStartupPack pack;
    pack.url = "https://example.com/pack";
    pack.pack_size = 1024;
    pack.index_sha256 = "abc123";
    pack.mem_virtual_size = 4096;
    manifest.memory_startup_pack = Optional<snapshot::ResolvedStartupPack>(pack);

    const agentenv::core::Expected<Json, agentenv::core::AnyError> reparsed =
        Json::Parse(manifest.ToJson().ToString());
    MT_EXPECT_TRUE(reparsed.ok());
    const agentenv::core::Expected<sandbox::SandboxSnapshotManifest, std::string> parsed =
        sandbox::SandboxSnapshotManifest::FromJson(reparsed.value());
    MT_EXPECT_TRUE(parsed.ok());

    MT_EXPECT_EQ(parsed.value().volume_drive_slots, static_cast<std::size_t>(4));
    MT_EXPECT_EQ(parsed.value().physical_extra_drive_count, static_cast<std::size_t>(1));
    MT_EXPECT_EQ(parsed.value().attached_drives.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(*parsed.value().attached_drives[0].sub_path, std::string("inner"));
    MT_EXPECT_EQ(parsed.value().attached_drives[0].virtual_size,
                 static_cast<uint64_t>(8192));
    MT_EXPECT_TRUE(parsed.value().memory_startup_pack.has_value());
    MT_EXPECT_EQ(parsed.value().memory_startup_pack->url,
                 std::string("https://example.com/pack"));

    // The `#[serde(skip)]` paths do not survive serialisation, by design: they
    // are local locations hydrated at resolve time.
    MT_EXPECT_EQ(parsed.value().memory.image_config_path, std::string(""));
    MT_EXPECT_EQ(parsed.value().vm_state.path, std::string(""));
}

MT_TEST(manifest_omits_an_absent_startup_pack_rather_than_writing_null) {
    // `skip_serializing_if = "Option::is_none"`, so the key must be missing.
    const sandbox::SandboxSnapshotManifest manifest = ValidManifest();
    // Held in a named value: `as_object()` returns a reference into the Json,
    // which a temporary would not keep alive.
    const Json json = manifest.ToJson();
    const JsonObject& fields = json.as_object();
    MT_EXPECT_TRUE(fields.find("memoryStartupPack") == fields.end());
}

MT_TEST(manifest_rejects_structurally_invalid_json) {
    const char* const rejected[] = {
        "{}",
        "[]",
        // Missing memory / rootfs / attachedDrives.
        "{\"version\":1}",
        "{\"version\":1,\"memory\":{\"virtualSize\":1}}",
        "{\"version\":1,\"memory\":{\"virtualSize\":1},\"rootfs\":{\"virtualSize\":1}}",
        // A negative size would wrap into an enormous value.
        "{\"version\":1,\"memory\":{\"virtualSize\":-1},\"rootfs\":{\"virtualSize\":1},"
        "\"attachedDrives\":[]}",
    };
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        const agentenv::core::Expected<Json, agentenv::core::AnyError> json =
            Json::Parse(rejected[i]);
        MT_EXPECT_TRUE(json.ok());
        MT_EXPECT_TRUE(!sandbox::SandboxSnapshotManifest::FromJson(json.value()).ok());
    }
}

// ---------------------------------------------------------------------------
// startup pack
// ---------------------------------------------------------------------------

MT_TEST(resolve_startup_pack_ref_gates_on_consumption_and_presence) {
    snapshot::MemoryStartupPackInfo info;
    info.pack_size = 1024;
    info.mem_virtual_size = 4096;
    info.index_sha256 = "abc";

    const Optional<snapshot::ResolvedStartupPack> resolved =
        snapshot::ResolveStartupPackRef(Optional<snapshot::MemoryStartupPackInfo>(info), true,
                                        "https://example.com/pack");
    MT_EXPECT_TRUE(resolved.has_value());
    MT_EXPECT_EQ(resolved->pack_size, static_cast<uint64_t>(1024));
    MT_EXPECT_EQ(resolved->index_sha256, std::string("abc"));
    MT_EXPECT_EQ(resolved->mem_virtual_size, static_cast<uint64_t>(4096));

    // Both gates fall back to a plain on-demand resume rather than an error.
    MT_EXPECT_TRUE(!snapshot::ResolveStartupPackRef(
                        Optional<snapshot::MemoryStartupPackInfo>(info), false, "url")
                        .has_value());
    MT_EXPECT_TRUE(!snapshot::ResolveStartupPackRef(
                        Optional<snapshot::MemoryStartupPackInfo>(), true, "url")
                        .has_value());
}

MT_TEST(startup_pack_descriptors_round_trip) {
    snapshot::MemoryStartupPackInfo info;
    info.pack_size = 7;
    info.mem_virtual_size = 8;
    info.index_sha256 = "deadbeef";
    const agentenv::core::Expected<snapshot::MemoryStartupPackInfo, std::string> parsed_info =
        snapshot::MemoryStartupPackInfo::FromJson(info.ToJson());
    MT_EXPECT_TRUE(parsed_info.ok());
    MT_EXPECT_TRUE(parsed_info.value() == info);

    snapshot::ResolvedStartupPack pack;
    pack.url = "https://example.com/p";
    pack.pack_size = 9;
    pack.index_sha256 = "cafe";
    pack.mem_virtual_size = 10;
    const agentenv::core::Expected<snapshot::ResolvedStartupPack, std::string> parsed_pack =
        snapshot::ResolvedStartupPack::FromJson(pack.ToJson());
    MT_EXPECT_TRUE(parsed_pack.ok());
    MT_EXPECT_TRUE(parsed_pack.value() == pack);

    // Incomplete objects must not parse into something that looks usable.
    MT_EXPECT_TRUE(!snapshot::ResolvedStartupPack::FromJson(Json(JsonObject())).ok());
    MT_EXPECT_TRUE(!snapshot::MemoryStartupPackInfo::FromJson(Json(JsonObject())).ok());
}

int main() { return microtest::RunAll(); }
