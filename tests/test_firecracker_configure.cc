// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs ::
//   FirecrackerSandbox::{configure_microvm, configure_extra_drives}
//
// The assertions here are about ORDER and about the two `ensure!`s. Order is
// what a per-call test cannot catch: each request in a wrong sequence is still
// well-formed, so Firecracker accepts it and the damage appears later -- as a
// guest mounting the wrong filesystem, or a `PUT` refused because the VM has
// already booted.
#include "microtest.h"

#include <string>
#include <vector>

#include "agentenv/sandbox/firecracker/sandbox.h"

using namespace agentenv;
using namespace agentenv::sandbox;
using namespace agentenv::sandbox::firecracker;

namespace {

using Kind = ConfigureStep::Kind;

ConfigurePlan BasePlan() {
    ConfigurePlan plan;
    plan.mem_size_mib = 2048;
    plan.vcpu_count = 4;
    plan.track_dirty_pages = true;
    plan.kernel_image = "/opt/agentenv/vmlinux";
    return plan;
}

DriveMount Mount(const std::string& id, const std::string& path, bool read_only) {
    DriveMount m;
    m.drive_id = id;
    m.attachment_path = path;
    m.read_only = read_only;
    return m;
}

/// Index of the first step of `kind`, or -1.
int IndexOf(const std::vector<ConfigureStep>& steps, Kind kind) {
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].kind == kind) return static_cast<int>(i);
    }
    return -1;
}

std::vector<std::string> DriveIds(const std::vector<ConfigureStep>& steps) {
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].kind == Kind::Drive) ids.push_back(steps[i].id);
    }
    return ids;
}

const ConfigureStep* FindDrive(const std::vector<ConfigureStep>& steps,
                               const std::string& id) {
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].kind == Kind::Drive && steps[i].id == id) return &steps[i];
    }
    return NULL;
}

}  // namespace

// ============================================================
// order
// ============================================================

MT_TEST(sequence_follows_the_rust_order) {
    ConfigurePlan plan = BasePlan();
    plan.log_path = "/var/log/fc.log";
    plan.log_level = "Info";
    plan.cpu_config_json = "{\"cpuid_modifiers\":[]}";
    plan.has_network_slot = true;
    plan.mmds_json = "{\"latest\":{}}";

    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    const std::vector<ConfigureStep>& steps = seq.value();

    // Logger first: it is accepted pre-boot and only once, so it must precede
    // anything that can fail -- a later failure is what the log was for.
    MT_EXPECT_EQ(IndexOf(steps, Kind::Logger), 0);
    MT_EXPECT_TRUE(IndexOf(steps, Kind::MachineConfig) < IndexOf(steps, Kind::CpuConfig));
    MT_EXPECT_TRUE(IndexOf(steps, Kind::CpuConfig) < IndexOf(steps, Kind::BootSource));
    MT_EXPECT_TRUE(IndexOf(steps, Kind::BootSource) < IndexOf(steps, Kind::Drive));
    MT_EXPECT_TRUE(IndexOf(steps, Kind::Drive) < IndexOf(steps, Kind::NetworkInterface));
    MT_EXPECT_TRUE(IndexOf(steps, Kind::NetworkInterface) < IndexOf(steps, Kind::MmdsConfig));
    MT_EXPECT_TRUE(IndexOf(steps, Kind::MmdsConfig) < IndexOf(steps, Kind::Mmds));
    // Balloon last.
    MT_EXPECT_EQ(IndexOf(steps, Kind::Balloon), static_cast<int>(steps.size()) - 1);
}

MT_TEST(drives_are_attached_in_guest_slot_order) {
    ConfigurePlan plan = BasePlan();
    plan.extra_drive_attachments.push_back(Mount("extra0", "extra0.img", true));
    plan.extra_drive_attachments.push_back(Mount("extra1", "extra1.img", false));
    plan.physical_extra_drive_count = 2;
    plan.volume_drive_slots = 2;

    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;

    // The guest names virtio-blk devices positionally, so this order IS the
    // guest's vda/vdb/vdc/... layout. Any permutation still satisfies the API.
    const std::vector<std::string> ids = DriveIds(seq.value());
    MT_EXPECT_EQ(ids.size(), static_cast<std::size_t>(6));
    MT_EXPECT_EQ(ids[0], std::string(kRootfsDriveId));      // vda
    MT_EXPECT_EQ(ids[1], std::string(kUserRootfsDriveId));  // vdb
    MT_EXPECT_EQ(ids[2], std::string("extra0"));            // vdc
    MT_EXPECT_EQ(ids[3], std::string("extra1"));            // vdd
    MT_EXPECT_EQ(ids[4], VolumeDriveSlotId(2));             // vde
    MT_EXPECT_EQ(ids[5], VolumeDriveSlotId(3));             // vdf
}

// ============================================================
// per-drive flags
// ============================================================

MT_TEST(tools_drive_is_root_and_read_only) {
    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(BasePlan());
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    const ConfigureStep* vda = FindDrive(seq.value(), kRootfsDriveId);
    MT_EXPECT_TRUE(vda != NULL);
    if (vda == NULL) return;
    MT_EXPECT_TRUE(vda->is_root);
    MT_EXPECT_TRUE(vda->read_only);
    // Small, read-only and on the boot path: Sync, no direct, no limiter.
    MT_EXPECT_TRUE(!vda->direct);
    MT_EXPECT_TRUE(vda->io_engine == IoEngine::Sync);
    MT_EXPECT_TRUE(!vda->rate_limiter.has_value());
    MT_EXPECT_EQ(vda->path, std::string(kRootfsDrivePath));
}

MT_TEST(user_drive_is_writable_async_and_direct) {
    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(BasePlan());
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    const ConfigureStep* vdb = FindDrive(seq.value(), kUserRootfsDriveId);
    MT_EXPECT_TRUE(vdb != NULL);
    if (vdb == NULL) return;
    MT_EXPECT_TRUE(!vdb->is_root);
    MT_EXPECT_TRUE(!vdb->read_only);
    MT_EXPECT_TRUE(vdb->direct);
    MT_EXPECT_TRUE(vdb->io_engine == IoEngine::Async);
}

MT_TEST(disk_limiter_rides_on_the_pre_boot_user_drive) {
    // Attached as pre-boot drive config rather than PATCHed after start, so
    // throttling is in force from the guest's first I/O instead of after a
    // brief unthrottled window.
    ConfigurePlan plan = BasePlan();
    plan.disk_rate_limit.enabled = true;
    plan.disk_rate_limit.bandwidth_bytes_per_sec = 1048576;

    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    const ConfigureStep* vdb = FindDrive(seq.value(), kUserRootfsDriveId);
    MT_EXPECT_TRUE(vdb != NULL);
    if (vdb == NULL) return;
    MT_EXPECT_TRUE(vdb->rate_limiter.has_value());
    // The tools drive is never throttled.
    const ConfigureStep* vda = FindDrive(seq.value(), kRootfsDriveId);
    MT_EXPECT_TRUE(vda != NULL && !vda->rate_limiter.has_value());
}

MT_TEST(extra_drives_keep_their_read_only_flag_and_are_never_root) {
    ConfigurePlan plan = BasePlan();
    plan.extra_drive_attachments.push_back(Mount("ro", "ro.img", true));
    plan.extra_drive_attachments.push_back(Mount("rw", "rw.img", false));
    plan.physical_extra_drive_count = 2;

    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    const ConfigureStep* ro = FindDrive(seq.value(), "ro");
    const ConfigureStep* rw = FindDrive(seq.value(), "rw");
    MT_EXPECT_TRUE(ro != NULL && rw != NULL);
    if (ro == NULL || rw == NULL) return;
    MT_EXPECT_TRUE(ro->read_only);
    MT_EXPECT_TRUE(!rw->read_only);
    // Only `read_only` varies; everything else is fixed for an extra drive.
    MT_EXPECT_TRUE(!ro->is_root && !rw->is_root);
    MT_EXPECT_TRUE(ro->direct && rw->direct);
    MT_EXPECT_TRUE(ro->io_engine == IoEngine::Async);
}

// ============================================================
// the two ensure!s
// ============================================================

MT_TEST(a_short_prepared_drive_list_is_an_error) {
    // Rust splits one prepared list at `physical_extra_drive_count`. Splitting
    // a shorter list would silently reclassify a physical drive as a volume
    // slot, so this must fail rather than truncate.
    ConfigurePlan plan = BasePlan();
    plan.extra_drive_attachments.push_back(Mount("extra0", "extra0.img", false));
    plan.physical_extra_drive_count = 3;

    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(!seq.ok());
    MT_EXPECT_TRUE(seq.error().find("physical extra drives") != std::string::npos);
}

MT_TEST(more_volumes_than_reserved_slots_is_an_error) {
    // Firecracker cannot add a virtio-block device after a snapshot restore,
    // so a missing slot cannot be fixed later. It is permanent, and must fail
    // now rather than at the first restore.
    ConfigurePlan plan = BasePlan();
    plan.extra_drive_attachments.push_back(Mount("vol0", "vol0.img", false));
    plan.extra_drive_attachments.push_back(Mount("vol1", "vol1.img", false));
    plan.physical_extra_drive_count = 0;
    plan.volume_drive_slots = 1;

    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(!seq.ok());
    MT_EXPECT_TRUE(seq.error().find("reserved volume slots") != std::string::npos);
}

MT_TEST(volume_slots_are_reserved_even_when_no_volume_uses_them) {
    // The slots exist from the first boot precisely so a later restore can
    // bind a volume into one. Reserving none would make that impossible.
    ConfigurePlan plan = BasePlan();
    plan.volume_drive_slots = 3;

    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    const std::vector<std::string> ids = DriveIds(seq.value());
    MT_EXPECT_EQ(ids.size(), static_cast<std::size_t>(5));  // vda + vdb + 3 slots
    MT_EXPECT_EQ(ids[2], VolumeDriveSlotId(0));
    MT_EXPECT_EQ(ids[4], VolumeDriveSlotId(2));
}

MT_TEST(volume_slot_ids_continue_after_the_physical_drives) {
    // Slot ids are offset by the physical count, so a slot never collides with
    // a physical drive's position in the guest's device order.
    ConfigurePlan plan = BasePlan();
    plan.extra_drive_attachments.push_back(Mount("p0", "p0.img", false));
    plan.physical_extra_drive_count = 1;
    plan.volume_drive_slots = 2;

    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    const std::vector<std::string> ids = DriveIds(seq.value());
    MT_EXPECT_EQ(ids[3], VolumeDriveSlotId(1));
    MT_EXPECT_EQ(ids[4], VolumeDriveSlotId(2));
}

// ============================================================
// conditional steps
// ============================================================

MT_TEST(network_mmds_config_and_mmds_move_together) {
    // MMDS is served over the interface, so the document without an interface
    // leaves the guest unable to read its own metadata.
    ConfigurePlan plan = BasePlan();
    plan.has_network_slot = false;
    plan.mmds_json = "{\"latest\":{}}";

    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    MT_EXPECT_EQ(IndexOf(seq.value(), Kind::NetworkInterface), -1);
    MT_EXPECT_EQ(IndexOf(seq.value(), Kind::MmdsConfig), -1);
    // Present mmds_json must not leak in without a slot.
    MT_EXPECT_EQ(IndexOf(seq.value(), Kind::Mmds), -1);

    plan.has_network_slot = true;
    seq = BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    MT_EXPECT_TRUE(IndexOf(seq.value(), Kind::NetworkInterface) >= 0);
    MT_EXPECT_TRUE(IndexOf(seq.value(), Kind::MmdsConfig) >= 0);
    MT_EXPECT_TRUE(IndexOf(seq.value(), Kind::Mmds) >= 0);
}

MT_TEST(an_empty_cpu_template_is_omitted_not_sent_empty) {
    // Rust checks `!cpu_json.is_empty()` inside the `Some` arm: a
    // present-but-empty string is not a request for an empty template, which
    // Firecracker would reject.
    ConfigurePlan plan = BasePlan();
    plan.cpu_config_json = "";
    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    MT_EXPECT_EQ(IndexOf(seq.value(), Kind::CpuConfig), -1);

    plan.cpu_config_json = "{}";
    seq = BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    MT_EXPECT_TRUE(IndexOf(seq.value(), Kind::CpuConfig) >= 0);
}

MT_TEST(a_blank_log_level_turns_logging_off) {
    // `LoggingEnabled`: a whitespace-only config value must not create an empty
    // log file, and must not burn the one logger the VM accepts.
    ConfigurePlan plan = BasePlan();
    plan.log_path = "/var/log/fc.log";
    plan.log_level = "   ";
    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    MT_EXPECT_EQ(IndexOf(seq.value(), Kind::Logger), -1);

    plan.log_level = "Debug";
    seq = BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    MT_EXPECT_EQ(IndexOf(seq.value(), Kind::Logger), 0);
}

MT_TEST(machine_config_carries_the_plan_and_pins_smt_off) {
    ConfigurePlan plan = BasePlan();
    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    const int i = IndexOf(seq.value(), Kind::MachineConfig);
    MT_EXPECT_TRUE(i >= 0);
    if (i < 0) return;
    const ConfigureStep& step = seq.value()[static_cast<std::size_t>(i)];
    MT_EXPECT_EQ(step.mem_size_mib, static_cast<uint32_t>(2048));
    MT_EXPECT_EQ(step.vcpu_count, static_cast<uint32_t>(4));
    MT_EXPECT_TRUE(step.track_dirty_pages);
    // Rust passes `false` positionally; SMT is never enabled on this path.
    MT_EXPECT_TRUE(!step.smt);
}

MT_TEST(boot_source_omits_absent_boot_args) {
    ConfigurePlan plan = BasePlan();
    core::Expected<std::vector<ConfigureStep>, std::string> seq =
        BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    int i = IndexOf(seq.value(), Kind::BootSource);
    MT_EXPECT_TRUE(i >= 0);
    if (i < 0) return;
    MT_EXPECT_EQ(seq.value()[static_cast<std::size_t>(i)].path,
                 std::string("/opt/agentenv/vmlinux"));
    MT_EXPECT_TRUE(seq.value()[static_cast<std::size_t>(i)].text.empty());

    plan.boot_args = core::Optional<std::string>("console=ttyS0 reboot=k");
    seq = BuildConfigureSequence(plan);
    MT_EXPECT_TRUE(seq.ok());
    if (!seq.ok()) return;
    i = IndexOf(seq.value(), Kind::BootSource);
    MT_EXPECT_EQ(seq.value()[static_cast<std::size_t>(i)].text,
                 std::string("console=ttyS0 reboot=k"));
}

MT_MAIN
