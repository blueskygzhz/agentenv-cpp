// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs :: start_fresh's host-drive half
// (link_tools_drive, attach_user_rootfs, prepare_volume_drive_slots,
// bind_volume_drive_slot) and src/sandbox/extra_drive.rs ::
// prepare_extra_drives.
//
// These are what make the relative names in `PUT /drives` exist. Firecracker
// accepts a drive whose path is missing only to fail later, and the guest only
// mounts a drive it is told about in `agentenv_drives=`, so both halves are
// asserted on the filesystem and the command line rather than on the calls.
#include "microtest.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/sandbox/firecracker/sandbox_backend.h"
#include "agentenv/storage/ublk-daemon/client.h"
#include "agentenv/storage/ublk-daemon/protocol.h"

using namespace agentenv;
using namespace agentenv::sandbox;
using namespace agentenv::sandbox::firecracker;
namespace daemon_ns = agentenv::storage::ublk_daemon;

namespace {

/// A daemon stand-in: answers runtime-device creation with `/dev/fake<N>`,
/// fails any image named in `fail_images`, and records every delete.
class FakeDaemonTransport : public daemon_ns::Transport {
 public:
    std::vector<std::string> created_images;
    std::vector<std::string> runtime_dirs;
    std::vector<core::Optional<uint64_t> > requested_sizes;
    std::vector<core::Optional<uint64_t> > known_sizes;
    std::vector<bool> allow_shrinks;
    std::vector<uint32_t> deleted;
    std::vector<std::string> fail_images;
    uint32_t next_id = 0;

    core::Expected<core::Json, std::string> Call(const core::Json& request,
                                                  int64_t /*timeout_ms*/) override {
        const core::Expected<daemon_ns::DaemonRequest, std::string> req =
            daemon_ns::DaemonRequest::FromJson(request);
        if (!req.ok()) return core::make_unexpected(req.error());
        daemon_ns::DaemonResponse resp;
        switch (req.value().kind) {
        case daemon_ns::DaemonRequest::Kind::CreateOverlaybdRuntimeDevice: {
            const std::string& image = req.value().source_image_config;
            for (std::size_t i = 0; i < fail_images.size(); ++i) {
                if (fail_images[i] == image) {
                    resp.status = daemon_ns::DaemonResponse::Status::Error;
                    resp.message = "refused " + image;
                    return resp.ToJson();
                }
            }
            created_images.push_back(image);
            runtime_dirs.push_back(req.value().runtime_dir);
            requested_sizes.push_back(req.value().requested_virtual_size);
            known_sizes.push_back(req.value().known_source_virtual_size);
            allow_shrinks.push_back(req.value().allow_shrink);
            resp.status = daemon_ns::DaemonResponse::Status::OverlaybdRuntimeDeviceCreated;
            resp.dev_id = next_id;
            resp.device_path = "/dev/fake" + std::to_string(next_id);
            resp.actual_virtual_size = 4096;
            ++next_id;
            break;
        }
        case daemon_ns::DaemonRequest::Kind::Delete:
            deleted.push_back(req.value().dev_id);
            resp.status = daemon_ns::DaemonResponse::Status::Deleted;
            break;
        default:
            resp.status = daemon_ns::DaemonResponse::Status::Error;
            resp.message = "unexpected request";
            break;
        }
        return resp.ToJson();
    }
};

std::string MakeDir() {
    char tmpl[] = "/tmp/aenv-fresh-drives-XXXXXX";
    char* p = ::mkdtemp(tmpl);
    return p ? std::string(p) : std::string();
}

std::string ReadLink(const std::string& path) {
    char buf[PATH_MAX];
    const ssize_t n = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (n < 0) return std::string();
    buf[n] = '\0';
    return std::string(buf);
}

bool IsSymlink(const std::string& path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

bool Exists(const std::string& path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
}

ExtraDrive Drive(const std::string& id, const std::string& image, const std::string& mount) {
    const core::Expected<ExtraDrive, std::string> d = ExtraDrive::TryNewOverlaybdWithMountPath(
        id, image, false, mount, core::Optional<std::string>());
    return d.ok() ? d.value() : ExtraDrive();
}

struct Fixture {
    std::string dir;
    std::string work_dir;
    std::shared_ptr<FakeDaemonTransport> transport;
    FreshDrivePlan plan;

    Fixture() : dir(MakeDir()), transport(new FakeDaemonTransport()) {
        if (dir.empty()) return;
        work_dir = dir + "/work";
        ::mkdir(work_dir.c_str(), 0700);
        { std::ofstream f((dir + "/tools.ext4").c_str()); f << "tools"; }
        std::shared_ptr<daemon_ns::DaemonClient> client(
            new daemon_ns::DaemonClient(transport, dir + "/ublk.sock"));
        plan.device_manager.reset(new ublk::DeviceManager(client, /*pool_enabled=*/false));
        plan.tools_drive_path = dir + "/tools.ext4";
        plan.rootfs_image_config.image_config_path = dir + "/user.json";
        plan.overlaybd_global_config = dir + "/global.json";
    }
};

}  // namespace

// ============================================================
// boot args
// ============================================================

MT_TEST(fresh_boot_args_carry_init_then_the_guest_mount_table) {
    std::vector<ExtraDrive> drives;
    drives.push_back(Drive("data", "/img/data.json", "/mnt/data"));
    drives.push_back(Drive("logs", "/img/logs.json", "/var/log/app"));
    const core::Optional<std::string> args = ComposeFreshBootArgs(
        core::Optional<std::string>("console=ttyS0"), drives, 1024, "x86_64");
    MT_EXPECT_TRUE(args.has_value());
    if (!args.has_value()) return;
    const std::string& s = *args;
    const std::size_t init = s.find("init=/init");
    const std::size_t table = s.find("agentenv_drives=vdc:/mnt/data,vdd:/var/log/app");
    MT_EXPECT_TRUE(s.compare(0, 13, "console=ttyS0") == 0);
    MT_EXPECT_TRUE(init != std::string::npos);
    MT_EXPECT_TRUE(table != std::string::npos);
    MT_EXPECT_TRUE(init < table);
    // DAMON region comes last and is computed for this VM's memory size.
    MT_EXPECT_TRUE(s.find("damon_reclaim.monitor_region_end=") != std::string::npos);
}

MT_TEST(network_boot_args_go_after_the_mount_table_and_before_damon) {
    // Rust `start_fresh`: drives, then `slot.build_ip_boot_arg()`, then
    // `GUEST_ARP_NOTIFY_BOOT_ARG`, then the DAMON region.
    std::vector<ExtraDrive> drives;
    drives.push_back(Drive("data", "/img/data.json", "/mnt/data"));
    std::vector<std::string> network;
    network.push_back("ip=169.254.0.21::169.254.0.22:255.255.255.252:instance:eth0:off:8.8.8.8");
    network.push_back(kGuestArpNotifyBootArg);
    const core::Optional<std::string> args = ComposeFreshBootArgs(
        core::Optional<std::string>("console=ttyS0"), drives, 1024, "x86_64", network);
    MT_EXPECT_TRUE(args.has_value());
    if (!args.has_value()) return;
    const std::string& s = *args;
    const std::size_t table = s.find("agentenv_drives=");
    const std::size_t ip = s.find("ip=169.254.0.21::169.254.0.22:");
    const std::size_t arp = s.find("sysctl.net.ipv4.conf.all.arp_notify=1");
    const std::size_t damon = s.find("damon_reclaim.");
    MT_EXPECT_TRUE(table != std::string::npos);
    MT_EXPECT_TRUE(ip != std::string::npos);
    MT_EXPECT_TRUE(arp != std::string::npos);
    MT_EXPECT_TRUE(damon != std::string::npos);
    MT_EXPECT_TRUE(table < ip);
    MT_EXPECT_TRUE(ip < arp);
    MT_EXPECT_TRUE(arp < damon);
}

MT_TEST(no_network_args_compose_the_same_as_before) {
    std::vector<ExtraDrive> drives;
    drives.push_back(Drive("data", "/img/data.json", "/mnt/data"));
    const core::Optional<std::string> without =
        ComposeFreshBootArgs(core::nullopt, drives, 1024, "x86_64");
    const core::Optional<std::string> empty = ComposeFreshBootArgs(
        core::nullopt, drives, 1024, "x86_64", std::vector<std::string>());
    MT_EXPECT_TRUE(without.has_value() && empty.has_value());
    if (!without.has_value() || !empty.has_value()) return;
    MT_EXPECT_EQ(*without, *empty);
    MT_EXPECT_TRUE(without->find("ip=") == std::string::npos);
}

MT_TEST(an_operator_init_is_not_overridden) {
    const core::Optional<std::string> args = ComposeFreshBootArgs(
        core::Optional<std::string>("init=/sbin/custom"), std::vector<ExtraDrive>(), 512, "");
    MT_EXPECT_TRUE(args.has_value());
    if (!args.has_value()) return;
    MT_EXPECT_EQ(*args, std::string("init=/sbin/custom"));
}

MT_TEST(no_extra_drives_means_no_mount_table) {
    const core::Optional<std::string> args =
        ComposeFreshBootArgs(core::Optional<std::string>(), std::vector<ExtraDrive>(), 512, "");
    MT_EXPECT_TRUE(args.has_value());
    if (!args.has_value()) return;
    MT_EXPECT_EQ(*args, std::string("init=/init"));
}

// ============================================================
// volume slots
// ============================================================

MT_TEST(volume_slots_are_placeholders_until_bound_to_a_volume) {
    const std::string dir = MakeDir();
    if (dir.empty()) return;
    std::vector<DriveMount> volumes(1);
    volumes[0].drive_id = "vol";
    volumes[0].attachment_path = "extra-drive-vol";

    MT_EXPECT_TRUE(PrepareVolumeDriveSlots(dir, 3, volumes).ok());
    // Bound slot: a relative link to the volume's attachment, resolved inside
    // the work dir, as Rust links `drive.attachment_path`.
    const std::string slot0 = dir + "/" + VolumeDriveSlotId(0) + ".img";
    MT_EXPECT_TRUE(IsSymlink(slot0));
    MT_EXPECT_EQ(ReadLink(slot0), std::string("extra-drive-vol"));
    // Unbound slots: 4 KiB placeholder files a later restore can rebind.
    for (std::size_t i = 1; i < 3; ++i) {
        const std::string slot = dir + "/" + VolumeDriveSlotId(i) + ".img";
        struct stat st;
        MT_EXPECT_TRUE(::lstat(slot.c_str(), &st) == 0 && S_ISREG(st.st_mode));
        MT_EXPECT_EQ(static_cast<uint64_t>(st.st_size), kVolumeDrivePlaceholderSize);
    }
}

MT_TEST(more_volumes_than_slots_is_refused_before_touching_disk) {
    const std::string dir = MakeDir();
    if (dir.empty()) return;
    std::vector<DriveMount> volumes(2);
    const core::Expected<core::Unit, std::string> r = PrepareVolumeDriveSlots(dir, 1, volumes);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(!Exists(dir + "/" + VolumeDriveSlotId(0) + ".img"));
}

MT_TEST(an_existing_slot_file_is_not_reused) {
    const std::string dir = MakeDir();
    if (dir.empty()) return;
    { std::ofstream f((dir + "/" + VolumeDriveSlotId(0) + ".img").c_str()); f << "x"; }
    MT_EXPECT_TRUE(!PrepareVolumeDriveSlots(dir, 1, std::vector<DriveMount>()).ok());
}

// ============================================================
// tools / user rootfs / extra drives
// ============================================================

MT_TEST(fresh_drives_link_every_device_into_the_work_dir) {
    Fixture fx;
    if (fx.dir.empty()) return;
    fx.plan.extra_drives.push_back(Drive("data", fx.dir + "/data.json", "/mnt/data"));

    FreshDriveResources resources;
    const core::Expected<std::vector<DriveMount>, std::string> mounts =
        PrepareFreshDrives(fx.work_dir, fx.plan, &resources);
    MT_EXPECT_TRUE(mounts.ok());
    if (!mounts.ok()) return;

    // vda: the tools drive, canonicalized.
    char resolved[PATH_MAX];
    MT_EXPECT_TRUE(::realpath((fx.dir + "/tools.ext4").c_str(), resolved) != NULL);
    MT_EXPECT_EQ(ReadLink(fx.work_dir + "/" + kRootfsDrivePath), std::string(resolved));
    // vdb: the user image's ublk device.
    MT_EXPECT_EQ(ReadLink(fx.work_dir + "/" + kUserRootfsDrivePath), std::string("/dev/fake0"));
    // vdc: the extra drive's ublk device, under its attachment name.
    MT_EXPECT_EQ(ReadLink(fx.work_dir + "/extra-drive-data"), std::string("/dev/fake1"));

    MT_EXPECT_EQ(mounts.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(mounts.value()[0].drive_id, std::string("data"));
    MT_EXPECT_EQ(mounts.value()[0].attachment_path, std::string("extra-drive-data"));

    // The daemon materializes each runtime in its own directory.
    MT_EXPECT_EQ(fx.transport->runtime_dirs.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(fx.transport->runtime_dirs[0], fx.work_dir + "/overlaybd");
    MT_EXPECT_EQ(fx.transport->runtime_dirs[1], fx.work_dir + "/extra-drive-runtime-data");

    // Released rootfs first, then extra drives; pooling off means delete.
    resources.Release();
    MT_EXPECT_EQ(fx.transport->deleted.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(fx.transport->deleted[0], static_cast<uint32_t>(0));
    MT_EXPECT_EQ(fx.transport->deleted[1], static_cast<uint32_t>(1));
    // Idempotent: a second release frees nothing twice.
    resources.Release();
    MT_EXPECT_EQ(fx.transport->deleted.size(), static_cast<std::size_t>(2));
}

MT_TEST(a_failed_extra_drive_rolls_back_links_but_keeps_devices_for_release) {
    Fixture fx;
    if (fx.dir.empty()) return;
    fx.plan.extra_drives.push_back(Drive("a", fx.dir + "/a.json", "/mnt/a"));
    fx.plan.extra_drives.push_back(Drive("b", fx.dir + "/b.json", "/mnt/b"));
    fx.transport->fail_images.push_back(fx.dir + "/b.json");

    FreshDriveResources resources;
    const core::Expected<std::vector<DriveMount>, std::string> mounts =
        PrepareFreshDrives(fx.work_dir, fx.plan, &resources);
    MT_EXPECT_TRUE(!mounts.ok());
    if (mounts.ok()) return;
    MT_EXPECT_TRUE(mounts.error().find("refused") != std::string::npos);
    // Rust `PreparedDrives::cleanup` removes the attachments it made...
    MT_EXPECT_TRUE(!Exists(fx.work_dir + "/extra-drive-a"));
    // ...and every device that exists is still held, so none leaks.
    MT_EXPECT_TRUE(resources.rootfs() != NULL);
    MT_EXPECT_EQ(resources.extra_drives().size(), static_cast<std::size_t>(1));
    resources.Release();
    MT_EXPECT_EQ(fx.transport->deleted.size(), static_cast<std::size_t>(2));
}

MT_TEST(a_missing_tools_drive_fails_before_any_device_is_made) {
    Fixture fx;
    if (fx.dir.empty()) return;
    fx.plan.tools_drive_path = fx.dir + "/no-such-tools.ext4";
    FreshDriveResources resources;
    const core::Expected<std::vector<DriveMount>, std::string> mounts =
        PrepareFreshDrives(fx.work_dir, fx.plan, &resources);
    MT_EXPECT_TRUE(!mounts.ok());
    MT_EXPECT_TRUE(fx.transport->created_images.empty());
    MT_EXPECT_TRUE(resources.empty());
}

MT_TEST(no_daemon_client_is_refused_by_name) {
    Fixture fx;
    if (fx.dir.empty()) return;
    fx.plan.device_manager.reset(
        new ublk::DeviceManager(std::shared_ptr<daemon_ns::DaemonClient>(), false));
    FreshDriveResources resources;
    const core::Expected<std::vector<DriveMount>, std::string> mounts =
        PrepareFreshDrives(fx.work_dir, fx.plan, &resources);
    MT_EXPECT_TRUE(!mounts.ok());
    if (mounts.ok()) return;
    MT_EXPECT_TRUE(mounts.error().find("ublk daemon") != std::string::npos);
}

MT_TEST(start_with_host_drives_needs_a_resource_holder) {
    FreshStartSpec spec;
    spec.drives = FreshDrivePlan();
    std::unique_ptr<FirecrackerInstance> out;
    const core::Expected<core::Unit, std::string> r =
        StartFreshInstance(spec, std::shared_ptr<Connector>(), &out, NULL);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(out.get() == NULL);
}

// ============================================================
// resume (Rust start_resume / prepare_resume_drives /
// attach_new_volume_drives)
// ============================================================

namespace {

ExtraDrive SizedDrive(const std::string& id, const std::string& image, bool volume,
                      uint64_t size) {
    ExtraDrive d = Drive(id, image, "/mnt/" + id);
    d.volume = volume;
    d.virtual_size = core::Optional<uint64_t>(size);
    return d;
}

ResumeDrivePlan ResumePlanOf(const Fixture& fx) {
    ResumeDrivePlan plan;
    plan.device_manager = fx.plan.device_manager;
    plan.tools_drive_path = fx.plan.tools_drive_path;
    plan.overlaybd_global_config = fx.plan.overlaybd_global_config;
    plan.extra_drive_allow_shrink = true;
    return plan;
}

CommonConfig ResumeCommon(const Fixture& fx) {
    CommonConfig common;
    ublk::OverlaybdDeviceConfig rootfs;
    rootfs.image_config_path = fx.dir + "/user.json";
    common.rootfs_image_config = core::Optional<ublk::OverlaybdDeviceConfig>(rootfs);
    common.rootfs_virtual_size = core::Optional<uint64_t>(8192);
    common.ublk_config = core::Optional<ublk::UblkConfig>(
        ublk::UblkConfig::OverlaybdWithRuntimeUpperMode(
            rootfs.image_config_path, false, storage::overlaybd::UpperMode::LogStructured));
    return common;
}

std::string SlotPath(const std::string& work_dir, std::size_t i) {
    return work_dir + "/" + VolumeDriveSlotId(i) + ".img";
}

bool IsPlaceholder(const std::string& path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           static_cast<uint64_t>(st.st_size) == kVolumeDrivePlaceholderSize;
}

}  // namespace

MT_TEST(resume_binds_restored_volumes_before_the_load_and_defers_new_ones) {
    Fixture fx;
    if (fx.dir.empty()) return;
    CommonConfig common = ResumeCommon(fx);
    // physical | in-state volume | new launch-slot drive | new volume
    common.extra_drives.push_back(SizedDrive("phys", fx.dir + "/phys.json", false, 100));
    common.extra_drives.push_back(SizedDrive("old", fx.dir + "/old.json", true, 200));
    common.extra_drives.push_back(SizedDrive("slot", fx.dir + "/slot.json", false, 300));
    common.extra_drives.push_back(SizedDrive("new", fx.dir + "/new.json", true, 400));
    common.physical_extra_drive_count = 1;
    common.volume_drive_slots = 4;
    std::vector<std::size_t> new_indices;
    new_indices.push_back(2);
    new_indices.push_back(3);

    FreshDriveResources resources;
    const core::Expected<std::vector<VolumeSlotBind>, std::string> binds =
        PrepareResumeDrives(fx.work_dir, ResumePlanOf(fx), common, new_indices, &resources);
    MT_EXPECT_TRUE(binds.ok());
    if (!binds.ok()) {
        MT_EXPECT_EQ(binds.error(), std::string(""));
        return;
    }

    // The user rootfs is restored at its recorded size, never shrunk.
    MT_EXPECT_EQ(fx.transport->created_images.size(), static_cast<std::size_t>(5));
    if (fx.transport->created_images.size() != 5) return;
    MT_EXPECT_EQ(ReadLink(fx.work_dir + "/" + kUserRootfsDrivePath), std::string("/dev/fake0"));
    MT_EXPECT_TRUE(fx.transport->requested_sizes[0].has_value() &&
                   *fx.transport->requested_sizes[0] == 8192);
    MT_EXPECT_TRUE(fx.transport->known_sizes[0].has_value() &&
                   *fx.transport->known_sizes[0] == 8192);
    MT_EXPECT_TRUE(!fx.transport->allow_shrinks[0]);

    // Recorded drives (physical, in-state volume) and the new volume restore
    // at their known size; only the new launch-slot drive is sized as fresh.
    MT_EXPECT_TRUE(fx.transport->known_sizes[1].has_value());   // phys
    MT_EXPECT_TRUE(fx.transport->known_sizes[2].has_value());   // old
    MT_EXPECT_TRUE(!fx.transport->known_sizes[3].has_value());  // slot: fresh
    MT_EXPECT_TRUE(fx.transport->allow_shrinks[3]);
    MT_EXPECT_TRUE(fx.transport->known_sizes[4].has_value());   // new volume
    MT_EXPECT_TRUE(!fx.transport->allow_shrinks[4]);

    // Slot 0 holds the in-state volume and is bound before the load.
    MT_EXPECT_EQ(ReadLink(SlotPath(fx.work_dir, 0)), std::string("extra-drive-old"));
    // Slots 1 and 2 are new: still placeholders, so the load opens them.
    MT_EXPECT_TRUE(IsPlaceholder(SlotPath(fx.work_dir, 1)));
    MT_EXPECT_TRUE(IsPlaceholder(SlotPath(fx.work_dir, 2)));
    // Slot 3 is reserved but unused.
    MT_EXPECT_TRUE(IsPlaceholder(SlotPath(fx.work_dir, 3)));

    MT_EXPECT_EQ(binds.value().size(), static_cast<std::size_t>(2));
    if (binds.value().size() != 2) return;
    MT_EXPECT_EQ(binds.value()[0].slot.drive_id, VolumeDriveSlotId(1));
    MT_EXPECT_EQ(binds.value()[0].drive.attachment_path, std::string("extra-drive-slot"));
    MT_EXPECT_EQ(binds.value()[1].slot.drive_id, VolumeDriveSlotId(2));
    MT_EXPECT_EQ(binds.value()[1].slot.attachment_path, VolumeDriveSlotId(2) + ".img");
    MT_EXPECT_EQ(binds.value()[1].drive.attachment_path, std::string("extra-drive-new"));

    resources.Release();
    MT_EXPECT_EQ(fx.transport->deleted.size(), static_cast<std::size_t>(5));
}

MT_TEST(resume_without_a_ublk_config_attaches_no_user_rootfs) {
    // Rust attaches the user rootfs only `if config.common.ublk_config.is_some()`.
    Fixture fx;
    if (fx.dir.empty()) return;
    CommonConfig common = ResumeCommon(fx);
    common.ublk_config = core::nullopt;

    FreshDriveResources resources;
    MT_EXPECT_TRUE(PrepareResumeDrives(fx.work_dir, ResumePlanOf(fx), common,
                                       std::vector<std::size_t>(), &resources).ok());
    MT_EXPECT_TRUE(!Exists(fx.work_dir + "/" + kUserRootfsDrivePath));
    MT_EXPECT_TRUE(resources.rootfs() == NULL);
    // The tools drive is linked regardless.
    MT_EXPECT_TRUE(IsSymlink(fx.work_dir + "/" + kRootfsDrivePath));
}

MT_TEST(resume_refuses_more_slot_drives_than_reserved_slots) {
    Fixture fx;
    if (fx.dir.empty()) return;
    CommonConfig common = ResumeCommon(fx);
    common.extra_drives.push_back(SizedDrive("a", fx.dir + "/a.json", true, 1));
    common.extra_drives.push_back(SizedDrive("b", fx.dir + "/b.json", true, 1));
    common.physical_extra_drive_count = 0;
    common.volume_drive_slots = 1;

    FreshDriveResources resources;
    const core::Expected<std::vector<VolumeSlotBind>, std::string> r = PrepareResumeDrives(
        fx.work_dir, ResumePlanOf(fx), common, std::vector<std::size_t>(), &resources);
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().find("reserved volume slots") != std::string::npos);
}

MT_TEST(volume_slots_beyond_the_drive_letters_are_refused_before_touching_disk) {
    const std::string dir = MakeDir();
    if (dir.empty()) return;
    const core::Expected<std::vector<DriveMount>, std::string> r =
        CreateVolumeDriveSlots(dir, 2, kMaxExtraDrives - 1);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(!Exists(SlotPath(dir, 0)));
}

namespace {

struct RecordedCall {
    std::string method;
    std::string path;
    std::string body;
    bool slot_was_bound;  // observed at the moment the request arrived
};

class RecordingSocket : public Socket {
 public:
    RecordingSocket(std::vector<RecordedCall>* log, std::string slot_path)
        : log_(log), slot_path_(std::move(slot_path)) {}
    core::Expected<std::string, std::string> Request(const std::string& method,
                                                     const std::string& path,
                                                     const std::string& body) override {
        RecordedCall c;
        c.method = method;
        c.path = path;
        c.body = body;
        c.slot_was_bound = IsSymlink(slot_path_);
        log_->push_back(c);
        return std::string();
    }
    std::future<core::Expected<std::string, std::string> >
    RequestAsync(const std::string& method, const std::string& path,
                 const std::string& body) override {
        std::promise<core::Expected<std::string, std::string> > p;
        p.set_value(Request(method, path, body));
        return p.get_future();
    }

 private:
    std::vector<RecordedCall>* log_;
    std::string slot_path_;
};

class RecordingConnector : public Connector {
 public:
    explicit RecordingConnector(std::string slot_path) : slot_path_(std::move(slot_path)) {}
    core::Expected<std::shared_ptr<Socket>, std::string> Connect(const std::string&) override {
        return std::shared_ptr<Socket>(new RecordingSocket(&calls, slot_path_));
    }
    std::vector<RecordedCall> calls;

 private:
    std::string slot_path_;
};

std::vector<ResumeStep> NewVolumePatch() {
    std::vector<ResumeStep> steps(1);
    steps[0].kind = ResumeStep::Kind::PatchDrivePath;
    steps[0].id = VolumeDriveSlotId(0);
    steps[0].path = VolumeDriveSlotId(0) + ".img";
    steps[0].text = "extra-drive-vol";
    return steps;
}

}  // namespace

MT_TEST(a_new_volume_is_bound_then_patched_with_the_slot_path) {
    // Rust `attach_new_volume_drives`: the slot is rebound first, then PATCHed
    // with the slot's own path so Firecracker reopens it and sees the volume's
    // real capacity rather than the 4 KiB placeholder.
    const std::string dir = MakeDir();
    if (dir.empty()) return;
    MT_EXPECT_TRUE(CreateVolumeDriveSlots(dir, 0, 1).ok());

    std::shared_ptr<RecordingConnector> conn(new RecordingConnector(SlotPath(dir, 0)));
    FirecrackerInstance instance(dir, conn);
    const core::Expected<core::Unit, std::string> r =
        ApplyResumeSequence(&instance, NewVolumePatch());
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) {
        MT_EXPECT_EQ(r.error(), std::string(""));
        return;
    }

    MT_EXPECT_EQ(ReadLink(SlotPath(dir, 0)), std::string("extra-drive-vol"));
    MT_EXPECT_EQ(conn->calls.size(), static_cast<std::size_t>(1));
    if (conn->calls.size() != 1) return;
    MT_EXPECT_EQ(conn->calls[0].method, std::string("PATCH"));
    MT_EXPECT_EQ(conn->calls[0].path, std::string("/drives/") + VolumeDriveSlotId(0));
    MT_EXPECT_TRUE(conn->calls[0].body.find(VolumeDriveSlotId(0) + ".img") !=
                   std::string::npos);
    // Bound before the PATCH, or Firecracker would reopen the placeholder.
    MT_EXPECT_TRUE(conn->calls[0].slot_was_bound);
}

MT_TEST(a_missing_slot_fails_the_bind_without_patching) {
    const std::string dir = MakeDir();
    if (dir.empty()) return;
    std::shared_ptr<RecordingConnector> conn(new RecordingConnector(SlotPath(dir, 0)));
    FirecrackerInstance instance(dir, conn);
    const core::Expected<core::Unit, std::string> r =
        ApplyResumeSequence(&instance, NewVolumePatch());
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().find("bind new volume") != std::string::npos);
    MT_EXPECT_TRUE(conn->calls.empty());
}

MT_TEST(fresh_with_a_network_plan_needs_a_resource_holder) {
    // The slot is stored in the holder before any fallible step; without one
    // a failure after allocation would leak a namespace and a veth pair.
    FreshStartSpec spec;
    spec.network = FreshNetworkPlan();
    std::unique_ptr<FirecrackerInstance> out;
    MT_EXPECT_TRUE(!StartFreshInstance(spec, std::shared_ptr<Connector>(), &out, NULL).ok());
    MT_EXPECT_TRUE(out.get() == NULL);
}

MT_TEST(fresh_with_a_network_plan_but_no_manager_fails_before_spawning) {
    const std::string dir = MakeDir();
    if (dir.empty()) return;
    FreshStartSpec spec;
    spec.chroot_base_dir = dir;
    // A binary that would fail the spawn: the error must be the missing
    // manager, proving the VMM was never reached.
    spec.firecracker_bin = dir + "/no-such-firecracker";
    spec.network = FreshNetworkPlan();
    FreshDriveResources resources;
    std::unique_ptr<FirecrackerInstance> out;
    const core::Expected<core::Unit, std::string> r =
        StartFreshInstance(spec, std::shared_ptr<Connector>(), &out, &resources);
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().find("no network manager") != std::string::npos);
    MT_EXPECT_TRUE(out.get() == NULL);
    MT_EXPECT_TRUE(resources.network_slot() == NULL);
}

MT_TEST(resume_with_a_network_manager_needs_a_resource_holder) {
    ResumeStartSpec spec;
    // Any non-NULL pointer: the holder check runs before the manager is
    // touched, and building a real one would install host iptables rules.
    spec.network_manager = reinterpret_cast<network::SlotNetworkManager*>(&spec);
    std::unique_ptr<FirecrackerInstance> out;
    const core::Expected<core::Unit, std::string> r =
        StartResumeInstance(spec, std::shared_ptr<Connector>(), &out, NULL);
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().find("network slot needs a resource holder") != std::string::npos);
}

MT_TEST(releasing_without_a_network_slot_succeeds) {
    FreshDriveResources resources;
    MT_EXPECT_TRUE(resources.Release().ok());
    MT_EXPECT_TRUE(resources.Release().ok());  // idempotent
    MT_EXPECT_TRUE(resources.empty());
}

MT_TEST(resume_with_host_drives_needs_a_resource_holder) {
    ResumeStartSpec spec;
    spec.drives = ResumeDrivePlan();
    std::unique_ptr<FirecrackerInstance> out;
    const core::Expected<core::Unit, std::string> r =
        StartResumeInstance(spec, std::shared_ptr<Connector>(), &out, NULL);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(out.get() == NULL);
}

MT_MAIN
