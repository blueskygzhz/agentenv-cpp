// SPDX-License-Identifier: MIT
// Rust: src/api/impls/attached_drives.rs `mod tests`.
#include "agentenv/api/attached_drives.h"

#include <string>
#include <vector>

#include "microtest.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT

namespace {

/// Resolves every reference to a fixed config path, so the tests exercise the
/// validation logic rather than image resolution.
///
/// `Resolve` takes the raw reference string, matching Rust's
/// `resolver.resolve(&drive.image)`. There is no parse step: this port used to
/// have one, which meant these tests asserted on a decomposition Rust never
/// performs.
class FakeResolver : public image::ImageResolver {
 public:
    int         calls = 0;
    /// When set, the next `Resolve` fails with this error. Typed as
    /// `ImageError` rather than a bool plus a string so a test can choose the
    /// *class* of failure, which is what decides 400 versus 500.
    core::Optional<image::ImageError> failure;
    std::string last_reference;
    /// What a pull would have recovered from the image's OCI config. Absent by
    /// default, which is also legal: a bare overlaybd config path has none.
    core::Optional<core::Json> raw_config;

    core::Expected<image::ResolvedBlockImage, image::ImageError> Resolve(
        const std::string& image_ref) override {
        ++calls;
        last_reference = image_ref;
        if (failure.has_value()) return core::make_unexpected(*failure);
        image::ResolvedBlockImage resolved;
        resolved.image_ref             = image_ref;
        resolved.overlaybd_config_path = "/var/lib/agentenv/images/app/image.json";
        resolved.raw_config            = raw_config;
        return resolved;
    }

    const std::string& DefaultImage() const override { return default_image_; }

 private:
    std::string default_image_ = "docker.io/library/alpine:3.20";
};

AttachedDriveRequest Request(const std::string& drive_id, const std::string& image) {
    AttachedDriveRequest request;
    request.drive_id     = drive_id;
    request.source_image = image;
    return request;
}

}  // namespace

MT_TEST(attached_drives_virtual_size_unset_means_inherit) {
    auto size = VirtualSizeFromDiskSizeMb(core::Optional<uint32_t>());
    MT_EXPECT_TRUE(size.ok());
    // Unset stays unset: the ublk daemon resolves the source image's size.
    MT_EXPECT_TRUE(!size.value().has_value());
}

MT_TEST(attached_drives_virtual_size_converts_mib_to_bytes) {
    auto size = VirtualSizeFromDiskSizeMb(core::Optional<uint32_t>(2048));
    MT_EXPECT_TRUE(size.ok());
    MT_EXPECT_TRUE(size.value().has_value());
    MT_EXPECT_EQ(*size.value(), static_cast<uint64_t>(2048) * 1024 * 1024);
}

MT_TEST(attached_drives_virtual_size_rejects_below_minimum_and_unaligned) {
    // Below 1 GiB.
    auto small = VirtualSizeFromDiskSizeMb(core::Optional<uint32_t>(512));
    MT_EXPECT_TRUE(!small.ok());
    MT_EXPECT_EQ(small.error().status, 400);

    // Not a whole number of GiB: the guest filesystem is grown to this size,
    // so a rounded value would silently differ from what was asked for.
    auto unaligned = VirtualSizeFromDiskSizeMb(core::Optional<uint32_t>(1536));
    MT_EXPECT_TRUE(!unaligned.ok());
    MT_EXPECT_EQ(unaligned.error().status, 400);

    // Exactly the minimum is accepted.
    auto minimum = VirtualSizeFromDiskSizeMb(core::Optional<uint32_t>(1024));
    MT_EXPECT_TRUE(minimum.ok());
}

MT_TEST(attached_drives_resolves_single_drive) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "docker.io/library/alpine:3.20"));

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_EQ(resolved.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(resolver.calls, 1);
}

MT_TEST(attached_drives_defaults_read_only_to_true) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "alpine"));

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(resolved.ok());
    // Rust `unwrap_or(true)`: a drive is read-only unless asked otherwise.
    MT_EXPECT_TRUE(resolved.value()[0].drive.read_only);

    drives[0].read_only = core::Optional<bool>(false);
    auto writable = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(writable.ok());
    MT_EXPECT_TRUE(!writable.value()[0].drive.read_only);
}

MT_TEST(attached_drives_carry_the_source_images_oci_config) {
    // Rust `ResolvedAttachedDrive { drive, raw_config }`. `build_image_configs`
    // publishes this object under the drive's id, and the guest reads it from
    // MMDS. The field was declared here and never assigned, so every attached
    // drive's `Env`/`Entrypoint` was dropped between the resolver and the
    // launch -- a defect no test could see while the field had no caller.
    FakeResolver resolver;
    core::Json config;
    config["WorkingDir"] = core::Json(std::string("/srv"));
    resolver.raw_config  = core::Optional<core::Json>(config);

    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "alpine"));
    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(resolved.ok());
    if (!resolved.ok()) return;
    MT_EXPECT_TRUE(resolved.value()[0].raw_config.has_value());
    if (!resolved.value()[0].raw_config.has_value()) return;
    MT_EXPECT_EQ((*resolved.value()[0].raw_config)["WorkingDir"].as_string(),
                 std::string("/srv"));

    // Absent stays absent rather than becoming a present `null`, which a
    // consumer would read as a config that clears every inherited field.
    FakeResolver bare;
    auto without = ResolveAttachedDrives(drives, &bare);
    MT_EXPECT_TRUE(without.ok());
    if (!without.ok()) return;
    MT_EXPECT_TRUE(!without.value()[0].raw_config.has_value());
}

MT_TEST(attached_drives_rejects_duplicate_drive_id) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "alpine"));
    drives.push_back(Request("data", "busybox"));

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().status, 400);
    // Validation happens before any registry work.
    MT_EXPECT_EQ(resolver.calls, 0);
}

MT_TEST(attached_drives_rejects_duplicate_resolved_mount_path) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("first", "alpine"));
    drives.push_back(Request("second", "busybox"));
    // Point both at the same explicit path.
    drives[0].mount_path = core::Optional<std::string>("/mnt/shared");
    drives[1].mount_path = core::Optional<std::string>("/mnt/shared");

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().status, 400);
}

MT_TEST(attached_drives_distinct_ids_get_distinct_default_paths) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("first", "alpine"));
    drives.push_back(Request("second", "busybox"));

    // Two drives that both omit `mountPath` must not collide, because the
    // default is derived from the drive id.
    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_EQ(resolved.value().size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(resolved.value()[0].drive.mount_path !=
                   resolved.value()[1].drive.mount_path);
}

MT_TEST(attached_drives_blank_mount_path_falls_back_to_default) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "alpine"));
    // Whitespace-only is treated as absent, not as a path.
    drives[0].mount_path = core::Optional<std::string>("   ");

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_EQ(resolved.value()[0].drive.mount_path,
                 sandbox::ExtraDrive::DefaultMountPath("data"));
}

MT_TEST(attached_drives_trims_drive_id_and_image) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("  data  ", "  alpine  "));

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_EQ(resolved.value()[0].drive.drive_id, std::string("data"));
    // The trimmed image is what reaches the resolver, verbatim: the resolver
    // takes the reference string and does its own expansion, so anything
    // rewritten here would be rewritten twice.
    MT_EXPECT_EQ(resolver.last_reference, std::string("alpine"));
}

MT_TEST(attached_drives_rejects_empty_source_image) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "   "));

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().status, 400);
    MT_EXPECT_TRUE(resolved.error().message.find("exactly one image") != std::string::npos);
}

MT_TEST(attached_drives_rejects_invalid_drive_id) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("", "alpine"));

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().status, 400);
    MT_EXPECT_EQ(resolver.calls, 0);
}

MT_TEST(attached_drives_rejects_relative_mount_path) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "alpine"));
    drives[0].mount_path = core::Optional<std::string>("relative/path");

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().status, 400);
}

MT_TEST(attached_drives_size_error_precedes_image_resolution) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "alpine"));
    drives[0].disk_size_mb = core::Optional<uint32_t>(100);

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().status, 400);
    MT_EXPECT_EQ(resolver.calls, 0);
}

MT_TEST(attached_drives_applies_virtual_size) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "alpine"));
    drives[0].disk_size_mb = core::Optional<uint32_t>(2048);

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(resolved.value()[0].drive.virtual_size.has_value());
    MT_EXPECT_EQ(*resolved.value()[0].drive.virtual_size,
                 static_cast<uint64_t>(2048) * 1024 * 1024);
}

MT_TEST(attached_drives_reports_resolver_failure_as_server_error) {
    FakeResolver resolver;
    resolver.failure =
        core::Optional<image::ImageError>(image::OtherError("registry unreachable"));
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "alpine"));

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().status, 500);
    // The drive id is in the message: with several drives, "resolution failed"
    // alone does not say which one.
    MT_EXPECT_TRUE(resolved.error().message.find("data") != std::string::npos);
}

MT_TEST(attached_drives_reports_bad_reference_as_client_error) {
    // The counterpart to the test above, and the reason the resolver reports
    // `ImageError` instead of a string: a mistyped reference and an
    // unreachable registry are both "resolution failed", but only one of them
    // is worth retrying. Reporting the first as 500 tells the client to retry
    // something that can never succeed.
    FakeResolver resolver;
    resolver.failure = core::Optional<image::ImageError>(
        image::InvalidReference("image reference has an empty tag: alpine:"));
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "alpine:"));

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().status, 400);
}

MT_TEST(attached_drives_reports_unsupported_image_as_client_error) {
    // Also 4xx: the image exists, but its layer format is the publisher's or
    // the caller's choice, not a server fault.
    FakeResolver resolver;
    resolver.failure = core::Optional<image::ImageError>(
        image::UnsupportedImage("image is an overlaybd turbo-OCI image"));
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "alpine"));

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().status, 400);
}

MT_TEST(attached_drives_empty_request_needs_no_resolver) {
    // No declarations means no registry work, so a missing resolver is fine.
    auto resolved = ResolveAttachedDrives(std::vector<AttachedDriveRequest>(), NULL);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(resolved.value().empty());
}

MT_TEST(attached_drives_normalizes_sub_path) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "alpine"));
    drives[0].sub_path = core::Optional<std::string>("nested/dir");

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(resolved.value()[0].drive.sub_path.has_value());
}

MT_TEST(attached_drives_rejects_escaping_sub_path) {
    FakeResolver resolver;
    std::vector<AttachedDriveRequest> drives;
    drives.push_back(Request("data", "alpine"));
    drives[0].sub_path = core::Optional<std::string>("../escape");

    auto resolved = ResolveAttachedDrives(drives, &resolver);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_EQ(resolved.error().status, 400);
    MT_EXPECT_EQ(resolver.calls, 0);
}

int main() { return microtest::RunAll(); }
