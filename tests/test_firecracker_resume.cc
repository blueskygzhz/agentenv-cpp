// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs ::
//   FirecrackerSandbox::{from_snapshot_config_with_override, start_resume}
// and factory.rs :: build_from_paused_state.
//
// The resume path is what turns a persisted record back into a running
// sandbox. Until now nothing could: `DecodePausedState` read the record and
// `BuildFromPausedState` refused it, so a restart left every paused sandbox
// visible and unresumable.
//
// What these cover is the ordering and the overrides, because both fail
// silently. A limiter reconciled after the resume still ends up applied -- the
// guest has simply already run unthrottled. An MMDS document left with the
// captured sandbox id still loads -- the guest just reports as a different
// sandbox, and its logs are filed under that one.
#include "microtest.h"

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/sandbox/firecracker/factory.h"
#include "agentenv/sandbox/firecracker/factory_impl.h"
#include "agentenv/sandbox/firecracker/paused_state.h"
#include "agentenv/sandbox/firecracker/sandbox_backend.h"

using namespace agentenv;
using namespace agentenv::sandbox;
using namespace agentenv::sandbox::firecracker;

namespace {

core::SandboxId CapturedId() {
    const core::Expected<core::SandboxId, std::string> p =
        core::SandboxId::Parse("0192f3a1-4c5d-7e80-9abc-def012345678");
    return p.ok() ? p.value() : core::SandboxId();
}

core::SandboxId ResumedId() {
    const core::Expected<core::SandboxId, std::string> p =
        core::SandboxId::Parse("0192f3a1-4c5d-7e80-9abc-def0aaaabbbb");
    return p.ok() ? p.value() : core::SandboxId();
}

ExtraDrive Drive(const std::string& id, bool volume) {
    ExtraDrive d;
    d.drive_id = id;
    d.image_config_path = "/img/" + id + ".json";
    d.mount_path = "/mnt/" + id;
    d.volume = volume;
    return d;
}

/// A capture with everything a load needs.
SnapshotConfig CapturedConfig() {
    SnapshotConfig config;
    config.vm_state_path = "/snap/vm_state.bin";
    config.mem_overlaybd_config.image_config_path = "/snap/mem.json";
    config.mem_virtual_size = 1024 * 1024;
    config.common.firecracker_binary = "firecracker";
    config.common.tools_drive_version = "1.2.3";
    ublk::OverlaybdDeviceConfig rootfs;
    rootfs.image_config_path = "/img/rootfs.json";
    config.common.rootfs_image_config = core::Optional<ublk::OverlaybdDeviceConfig>(rootfs);
    config.common.rootfs_virtual_size = core::Optional<uint64_t>(2048ull * 1024 * 1024);
    config.common.mmds_metadata =
        core::Optional<MmdsMetadata>(MmdsMetadata(CapturedId(), "snap-1"));
    return config;
}

ResumeStartSpec SpecFor(const SnapshotConfig& config) {
    ResumeStartSpec spec;
    spec.snapshot_config = config;
    spec.sandbox_id = ResumedId();
    spec.mem_device_path = "/dev/ublkb0";
    return spec;
}

std::vector<ResumeStep::Kind> KindsOf(const std::vector<ResumeStep>& steps) {
    std::vector<ResumeStep::Kind> out;
    for (std::size_t i = 0; i < steps.size(); ++i) out.push_back(steps[i].kind);
    return out;
}

int IndexOf(const std::vector<ResumeStep>& steps, ResumeStep::Kind kind) {
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].kind == kind) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

// ============================================================
// from_snapshot_config_with_override
// ============================================================

MT_TEST(a_resume_overrides_the_captured_sandbox_id_in_mmds) {
    // The guest reads its own identity out of MMDS. Left as captured, a
    // resumed sandbox reports as the one that was paused -- and the logs
    // collector files its output under that id, so the two become
    // indistinguishable in exactly the place an operator would look.
    SnapshotConfig config = CapturedConfig();
    MT_EXPECT_EQ(config.common.mmds_metadata->SandboxId(), CapturedId().ToString());

    MT_EXPECT_TRUE(PrepareSnapshotConfigForResume(
                       &config, ResumedId(), core::Optional<EnvdAccessToken>()).ok());
    MT_EXPECT_EQ(config.common.mmds_metadata->SandboxId(), ResumedId().ToString());
}

MT_TEST(a_resume_replaces_the_captured_access_token) {
    // The token is a live credential. Carrying the captured sandbox's forward
    // would let this one authenticate as that one.
    SnapshotConfig config = CapturedConfig();
    const core::Optional<EnvdAccessToken> captured_token(EnvdAccessToken("captured-secret"));
    config.common.mmds_metadata->SetAccessToken(captured_token);
    const std::string captured_hash = config.common.mmds_metadata->AccessTokenHash();

    const core::Optional<EnvdAccessToken> fresh_token(EnvdAccessToken("fresh-secret"));
    MT_EXPECT_TRUE(PrepareSnapshotConfigForResume(&config, ResumedId(), fresh_token).ok());

    MT_EXPECT_TRUE(config.common.envd_access_token.has_value());
    MT_EXPECT_EQ(config.common.envd_access_token->Expose(), std::string("fresh-secret"));
    // Only the digest is stored, and it is the new one.
    MT_EXPECT_TRUE(config.common.mmds_metadata->AccessTokenHash() != captured_hash);
    MT_EXPECT_EQ(config.common.mmds_metadata->AccessTokenHash(),
                 MmdsMetadata::HashAccessToken("fresh-secret"));
}

MT_TEST(a_capture_without_mmds_gets_a_document) {
    // Rust `get_or_insert_with(|| MmdsMetadata::new(id, "unknown"))`. Without
    // one the guest has no identity to read at all.
    SnapshotConfig config = CapturedConfig();
    config.common.mmds_metadata = core::nullopt;

    MT_EXPECT_TRUE(PrepareSnapshotConfigForResume(
                       &config, ResumedId(), core::Optional<EnvdAccessToken>()).ok());
    MT_EXPECT_TRUE(config.common.mmds_metadata.has_value());
    MT_EXPECT_EQ(config.common.mmds_metadata->SandboxId(), ResumedId().ToString());
    // The snapshot id placeholder upstream uses, rather than a real-looking
    // one this node could not justify.
    MT_EXPECT_EQ(config.common.mmds_metadata->SnapshotId(), std::string("unknown"));
}

MT_TEST(a_legacy_capture_treats_every_drive_as_physical) {
    // A capture from before reserved slots existed lists all drives directly
    // and reserves none. Read literally its volumes would be bound into
    // physical slots and the guest would mount the wrong filesystems.
    SnapshotConfig config = CapturedConfig();
    config.common.extra_drives.push_back(Drive("a", false));
    config.common.extra_drives.push_back(Drive("b", false));
    config.common.volume_drive_slots = 0;
    config.common.physical_extra_drive_count = 0;  // as a legacy record has it

    MT_EXPECT_TRUE(PrepareSnapshotConfigForResume(
                       &config, ResumedId(), core::Optional<EnvdAccessToken>()).ok());
    MT_EXPECT_EQ(config.common.physical_extra_drive_count, static_cast<std::size_t>(2));
}

MT_TEST(an_inconsistent_drive_count_is_refused) {
    // Firecracker reopens every serialized slot path on load, so a record
    // claiming drives it does not carry cannot be loaded. Failing here names
    // the record; failing at load names the load.
    SnapshotConfig config = CapturedConfig();
    config.common.extra_drives.push_back(Drive("a", false));
    config.common.volume_drive_slots = 2;      // non-zero, so no legacy restate
    config.common.physical_extra_drive_count = 3;  // more than the one drive

    const core::Expected<core::Unit, std::string> r = PrepareSnapshotConfigForResume(
        &config, ResumedId(), core::Optional<EnvdAccessToken>());
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().find("3 physical extra drives") != std::string::npos);
    MT_EXPECT_TRUE(r.error().find("1 logical drives") != std::string::npos);
}

MT_TEST(more_volumes_than_reserved_slots_is_refused) {
    SnapshotConfig config = CapturedConfig();
    config.common.extra_drives.push_back(Drive("p", false));
    config.common.extra_drives.push_back(Drive("v1", true));
    config.common.extra_drives.push_back(Drive("v2", true));
    config.common.physical_extra_drive_count = 1;
    config.common.volume_drive_slots = 1;  // but two volumes follow

    const core::Expected<core::Unit, std::string> r = PrepareSnapshotConfigForResume(
        &config, ResumedId(), core::Optional<EnvdAccessToken>());
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().find("2 launch-time volume drives") != std::string::npos);
    MT_EXPECT_TRUE(r.error().find("1 reserved drive slots") != std::string::npos);
}

// ============================================================
// start_resume — the sequence
// ============================================================

MT_TEST(the_resume_sequence_is_ordered_load_mmds_limiter_resume) {
    const core::Expected<std::vector<ResumeStep>, std::string> built =
        BuildResumeSequence(SpecFor(CapturedConfig()));
    MT_EXPECT_TRUE(built.ok());
    if (!built.ok()) return;
    const std::vector<ResumeStep>& steps = built.value();

    const int load = IndexOf(steps, ResumeStep::Kind::SnapshotLoad);
    const int mmds = IndexOf(steps, ResumeStep::Kind::Mmds);
    const int limiter = IndexOf(steps, ResumeStep::Kind::PatchRateLimiter);
    const int resume = IndexOf(steps, ResumeStep::Kind::Resume);

    MT_EXPECT_TRUE(load >= 0);
    MT_EXPECT_TRUE(mmds > load);
    // The limiter must be patched after the load (no drive exists before it)
    // and before the resume (after it the guest has already issued I/O under
    // whatever limit the capture carried).
    MT_EXPECT_TRUE(limiter > load);
    MT_EXPECT_TRUE(limiter < resume);
    // The resume is last: everything above is configuration of a VM that is
    // loaded but still paused.
    MT_EXPECT_EQ(resume, static_cast<int>(steps.size()) - 1);
}

MT_TEST(a_configured_logger_is_set_before_the_load) {
    // Firecracker accepts `PUT /logger` pre-boot only, and a loaded snapshot
    // is no longer pre-boot.
    SnapshotConfig config = CapturedConfig();
    config.common.firecracker_log_level = core::Optional<std::string>("debug");

    const core::Expected<std::vector<ResumeStep>, std::string> built =
        BuildResumeSequence(SpecFor(config));
    MT_EXPECT_TRUE(built.ok());
    if (!built.ok()) return;

    const int logger = IndexOf(built.value(), ResumeStep::Kind::Logger);
    const int load = IndexOf(built.value(), ResumeStep::Kind::SnapshotLoad);
    MT_EXPECT_EQ(logger, 0);
    MT_EXPECT_TRUE(load > logger);
}

MT_TEST(a_blank_log_level_adds_no_logger_step) {
    // `LoggingEnabled` treats a whitespace-only level as off, so a config with
    // one does not create an empty log file.
    SnapshotConfig config = CapturedConfig();
    config.common.firecracker_log_level = core::Optional<std::string>("   ");

    const core::Expected<std::vector<ResumeStep>, std::string> built =
        BuildResumeSequence(SpecFor(config));
    MT_EXPECT_TRUE(built.ok());
    if (built.ok()) {
        MT_EXPECT_EQ(IndexOf(built.value(), ResumeStep::Kind::Logger), -1);
    }
}

MT_TEST(the_limiter_is_reconciled_with_both_buckets_present) {
    // A restored VM inherits whatever limiter was active when it was paused.
    // `PATCH /drives` reads an absent bucket as "leave unchanged", so a
    // dimension this node leaves unset must be written as an explicit disable
    // or the inherited limit survives.
    SnapshotConfig config = CapturedConfig();
    config.common.disk_rate_limit.enabled = false;  // node configures no limit

    const core::Expected<std::vector<ResumeStep>, std::string> built =
        BuildResumeSequence(SpecFor(config));
    MT_EXPECT_TRUE(built.ok());
    if (!built.ok()) return;

    const int at = IndexOf(built.value(), ResumeStep::Kind::PatchRateLimiter);
    MT_EXPECT_TRUE(at >= 0);
    if (at < 0) return;
    const ResumeStep& step = built.value()[static_cast<std::size_t>(at)];
    MT_EXPECT_EQ(step.id, std::string(kUserRootfsDriveId));
    MT_EXPECT_TRUE(step.rate_limiter.bandwidth.has_value());
    MT_EXPECT_TRUE(step.rate_limiter.ops.has_value());
    // The explicit disable sentinel: both fields zero.
    MT_EXPECT_EQ(step.rate_limiter.bandwidth->size, static_cast<int64_t>(0));
    MT_EXPECT_EQ(step.rate_limiter.bandwidth->refill_time, static_cast<int64_t>(0));
}

MT_TEST(new_volumes_are_patched_in_after_the_load) {
    // A volume absent from the source state loaded as a placeholder, so its
    // real path is patched once the VM exists.
    SnapshotConfig config = CapturedConfig();
    config.common.extra_drives.push_back(Drive("phys", false));
    config.common.extra_drives.push_back(Drive("vol", true));
    config.common.physical_extra_drive_count = 1;
    config.common.volume_drive_slots = 2;

    const core::Expected<std::vector<ResumeStep>, std::string> built =
        BuildResumeSequence(SpecFor(config));
    MT_EXPECT_TRUE(built.ok());
    if (!built.ok()) return;

    const int load = IndexOf(built.value(), ResumeStep::Kind::SnapshotLoad);
    const int patch = IndexOf(built.value(), ResumeStep::Kind::PatchDrivePath);
    MT_EXPECT_TRUE(patch > load);
    if (patch < 0) return;
    const ResumeStep& step = built.value()[static_cast<std::size_t>(patch)];
    // Bound through the reserved slot, not under the volume's own id: the
    // slot is what the loaded state knows about.
    MT_EXPECT_EQ(step.id, VolumeDriveSlotId(0));
    MT_EXPECT_EQ(step.path, std::string("/img/vol.json"));
}

MT_TEST(a_resume_without_a_memory_device_is_refused_up_front) {
    // Rust's `ensure!(UblkDeviceManager::global().is_available())`, which it
    // checks before allocating anything: memory restore reads through a block
    // device, and without one the load would name a path that is not one.
    ResumeStartSpec spec = SpecFor(CapturedConfig());
    spec.mem_device_path.clear();

    const core::Expected<std::vector<ResumeStep>, std::string> built =
        BuildResumeSequence(spec);
    MT_EXPECT_TRUE(!built.ok());
    if (built.ok()) return;
    MT_EXPECT_TRUE(built.error().find("memory block device") != std::string::npos);
}

MT_TEST(a_capture_missing_its_rootfs_is_refused_up_front) {
    // Each of these would otherwise surface as a VM that loaded and then could
    // not find its disk, by which point the error names the load.
    SnapshotConfig no_rootfs = CapturedConfig();
    no_rootfs.common.rootfs_image_config = core::nullopt;
    MT_EXPECT_TRUE(!BuildResumeSequence(SpecFor(no_rootfs)).ok());

    SnapshotConfig no_size = CapturedConfig();
    no_size.common.rootfs_virtual_size = core::nullopt;
    const core::Expected<std::vector<ResumeStep>, std::string> r =
        BuildResumeSequence(SpecFor(no_size));
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) {
        MT_EXPECT_TRUE(r.error().find("rootfs virtual size") != std::string::npos);
    }

    SnapshotConfig no_state = CapturedConfig();
    no_state.vm_state_path.clear();
    MT_EXPECT_TRUE(!BuildResumeSequence(SpecFor(no_state)).ok());
}

// ============================================================
// build_from_paused_state
// ============================================================

MT_TEST(a_paused_state_from_another_backend_is_refused) {
    // Rust `downcast_ref::<FirecrackerPausedState>().context(..)`. Another
    // backend's capture has a different shape entirely, so it is refused
    // rather than reinterpreted.
    class ForeignState : public PausedSandboxState {
     public:
        std::string Encode() const override { return "{}"; }
        RuntimeArtifactSet RuntimeArtifacts() const override {
            return RuntimeArtifactSet::Empty();
        }
    };

    Config cfg;
    FirecrackerSandboxFactory factory(cfg);
    ForeignState foreign;
    const core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError> r =
        factory.BuildFromPausedState(ResumedId(), foreign,
                                     core::Optional<EnvdAccessToken>());
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().chain().find("not a Firecracker paused state") !=
                   std::string::npos);
}

MT_TEST(a_firecracker_paused_state_yields_a_resuming_backend) {
    // The gap this closes: `DecodePausedState` could already read a persisted
    // record, and `BuildFromPausedState` then refused it -- so a restart left
    // every paused sandbox listed and unresumable.
    Config cfg;
    FirecrackerSandboxFactory factory(cfg);
    FirecrackerPausedState paused(CapturedConfig());

    const core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError> r =
        factory.BuildFromPausedState(ResumedId(), paused,
                                     core::Optional<EnvdAccessToken>());
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) {
        MT_EXPECT_EQ(r.error().chain(), std::string(""));
        return;
    }
    MT_EXPECT_TRUE(r.value() != NULL);
}

MT_TEST(an_inconsistent_record_is_refused_at_build_not_at_start) {
    // The drive counts are a property of the record, so they are checked while
    // constructing the backend. Deferring to start would spawn a VMM first and
    // kill it again for something knowable from the record alone.
    SnapshotConfig bad = CapturedConfig();
    bad.common.extra_drives.push_back(Drive("a", false));
    bad.common.volume_drive_slots = 2;
    bad.common.physical_extra_drive_count = 9;

    Config cfg;
    FirecrackerSandboxFactory factory(cfg);
    FirecrackerPausedState paused(bad);

    const core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError> r =
        factory.BuildFromPausedState(ResumedId(), paused,
                                     core::Optional<EnvdAccessToken>());
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().chain().find("physical extra drives") != std::string::npos);
}

MT_TEST(a_resume_without_the_ublk_daemon_fails_at_start) {
    // Construction succeeds, as it does in Rust: the memory device is acquired
    // in `start_resume`, where Rust's own `ensure!` fires. What must not
    // happen is a resume that reports success without having loaded anything.
    Config cfg;
    FirecrackerSandboxFactory factory(cfg);
    FirecrackerPausedState paused(CapturedConfig());

    core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError> built =
        factory.BuildFromPausedState(ResumedId(), paused,
                                     core::Optional<EnvdAccessToken>());
    MT_EXPECT_TRUE(built.ok());
    if (!built.ok()) return;

    const core::Expected<core::Unit, core::AnyError> started = built.value()->StartNowait();
    MT_EXPECT_TRUE(!started.ok());
    if (started.ok()) return;
    MT_EXPECT_TRUE(started.error().chain().find("memory block device") != std::string::npos);
}

// ============================================================
// the resume sequence against a stand-in VMM
//
// Everything above asserts the plan. This runs it: the plan being right and
// the dispatch issuing it are different claims, and a `PATCH` sent as a `PUT`
// satisfies the first while failing the second.
// ============================================================

namespace {

struct Call {
    std::string method;
    std::string path;
    std::string body;
};

class ResumeSocket : public Socket {
 public:
    ResumeSocket(std::vector<Call>* log, const std::string& fail_path)
        : log_(log), fail_path_(fail_path) {}

    core::Expected<std::string, std::string> Request(const std::string& method,
                                                     const std::string& path,
                                                     const std::string& body) override {
        Call call;
        call.method = method;
        call.path = path;
        call.body = body;
        log_->push_back(call);
        if (!fail_path_.empty() && path == fail_path_) {
            return core::make_unexpected(std::string("HTTP 400: fault injected"));
        }
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
    std::vector<Call>* log_;
    std::string fail_path_;
};

class ResumeConnector : public Connector {
 public:
    explicit ResumeConnector(const std::string& fail_path = std::string())
        : fail_path_(fail_path) {}

    core::Expected<std::shared_ptr<Socket>, std::string>
    Connect(const std::string&) override {
        return std::shared_ptr<Socket>(new ResumeSocket(&calls, fail_path_));
    }

    std::vector<Call> calls;

 private:
    std::string fail_path_;
};

std::string MakeTempDir() {
    char tmpl[] = "/tmp/aenv-resume-XXXXXX";
    char* p = ::mkdtemp(tmpl);
    return p ? std::string(p) : std::string();
}

/// A spec pointing at the stand-in VMM, with a memory device path that only
/// has to be a string here -- nothing in this port opens it.
///
/// The config goes through `PrepareSnapshotConfigForResume` first, as the real
/// caller does: `StartResumeInstance` only reads the config, exactly as Rust's
/// `start_resume` does, and the identity override belongs to the step before
/// it. Skipping it here would assert the sequence against a config no real
/// resume ever issues.
ResumeStartSpec RunnableSpecIn(const std::string& dir) {
    SnapshotConfig config = CapturedConfig();
    PrepareSnapshotConfigForResume(&config, ResumedId(),
                                   core::Optional<EnvdAccessToken>());
    ResumeStartSpec spec = SpecFor(config);
    spec.firecracker_bin = AGENTENV_FAKE_FIRECRACKER;
    spec.chroot_base_dir = dir + "/chroot";
    spec.socket_timeout_ms = 300;
    spec.socket_poll_ms = 5;
    return spec;
}

}  // namespace

MT_TEST(the_resume_sequence_reaches_the_vmm_in_order) {
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;

    // Held locally so the call log outlives whatever happens to the instance.
    std::shared_ptr<ResumeConnector> conn(new ResumeConnector());
    std::unique_ptr<FirecrackerInstance> instance;
    const core::Expected<core::Unit, std::string> r = StartResumeInstance(
        RunnableSpecIn(dir), conn, &instance);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) {
        MT_EXPECT_EQ(r.error(), std::string(""));
        return;
    }

    MT_EXPECT_EQ(conn->calls.size(), static_cast<std::size_t>(4));
    if (conn->calls.size() != 4) return;

    // Load, then MMDS, then the limiter, then the resume.
    MT_EXPECT_EQ(conn->calls[0].method, std::string("PUT"));
    MT_EXPECT_EQ(conn->calls[0].path, std::string("/snapshot/load"));
    // `resume_vm: false` -- the VM is left loaded-but-paused so the limiter
    // can be reconciled before the guest issues any I/O.
    MT_EXPECT_TRUE(conn->calls[0].body.find("\"resume_vm\":false") != std::string::npos);
    MT_EXPECT_TRUE(conn->calls[0].body.find("/dev/ublkb0") != std::string::npos);

    MT_EXPECT_EQ(conn->calls[1].path, std::string("/mmds"));
    // The resumed id, not the captured one.
    MT_EXPECT_TRUE(conn->calls[1].body.find(ResumedId().ToString()) != std::string::npos);

    // A PATCH, not a PUT: `PUT /drives` is pre-boot only and would be refused
    // on a loaded VM.
    MT_EXPECT_EQ(conn->calls[2].method, std::string("PATCH"));
    MT_EXPECT_EQ(conn->calls[2].path, std::string("/drives/") + kUserRootfsDriveId);

    MT_EXPECT_EQ(conn->calls[3].method, std::string("PATCH"));
    MT_EXPECT_EQ(conn->calls[3].path, std::string("/vm"));
    MT_EXPECT_TRUE(conn->calls[3].body.find("Resumed") != std::string::npos);

    // No InstanceStart anywhere: the VM is already running from the loaded
    // state, and a start would boot a second machine over the restored one.
    for (std::size_t i = 0; i < conn->calls.size(); ++i) {
        MT_EXPECT_TRUE(conn->calls[i].path != std::string("/actions"));
    }
}

MT_TEST(a_failed_resume_does_not_leave_a_running_vmm) {
    // An error returned alongside a live Firecracker leaks both it and the
    // work directory, and the next resume for this sandbox collides with it.
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;

    // The MMDS write fails, which is past the spawn and past the load.
    //
    // Held locally for the duration: on failure `StartResumeInstance` drops
    // the instance it built, which is the only other owner of the connector.
    // Reading the call log through a raw pointer afterwards would be a
    // use-after-free -- and would report whatever the freed memory held.
    std::shared_ptr<ResumeConnector> conn(new ResumeConnector("/mmds"));
    std::unique_ptr<FirecrackerInstance> instance;
    const core::Expected<core::Unit, std::string> r = StartResumeInstance(
        RunnableSpecIn(dir), conn, &instance);

    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    // The error names the failing call, not just the transport fault.
    MT_EXPECT_TRUE(r.error().find("/mmds") != std::string::npos);
    // Ownership is all-or-nothing: no instance is handed back on failure, so
    // the unique_ptr inside unwound and stopped the VMM.
    MT_EXPECT_TRUE(!instance);
    // The limiter and the resume were never attempted.
    for (std::size_t i = 0; i < conn->calls.size(); ++i) {
        MT_EXPECT_TRUE(conn->calls[i].path != std::string("/vm"));
    }
}

MT_TEST(a_backend_built_from_a_paused_state_resumes_end_to_end) {
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;

    Config cfg;
    cfg.firecracker_bin = AGENTENV_FAKE_FIRECRACKER;
    cfg.chroot_base_dir = dir + "/chroot";
    FirecrackerSandboxFactory factory(cfg);

    std::shared_ptr<ResumeConnector> conn(new ResumeConnector());
    factory.SetResumeDepsForTesting("/dev/ublkb0", conn);

    // The capture names no binary, so the factory's own config supplies it.
    SnapshotConfig captured = CapturedConfig();
    captured.common.firecracker_binary.clear();
    captured.common.runtime_policy.socket_timeout_ms = 300;
    captured.common.runtime_policy.socket_poll_interval_ms = 5;
    FirecrackerPausedState paused(captured);

    core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError> built =
        factory.BuildFromPausedState(ResumedId(), paused,
                                     core::Optional<EnvdAccessToken>(
                                         EnvdAccessToken("resume-token")));
    MT_EXPECT_TRUE(built.ok());
    if (!built.ok()) {
        MT_EXPECT_EQ(built.error().chain(), std::string(""));
        return;
    }

    const core::Expected<core::Unit, core::AnyError> started = built.value()->StartNowait();
    MT_EXPECT_TRUE(started.ok());
    if (!started.ok()) {
        MT_EXPECT_EQ(started.error().chain(), std::string(""));
        return;
    }

    // The whole point: a persisted record became a loaded, running VM.
    MT_EXPECT_EQ(conn->calls.size(), static_cast<std::size_t>(4));
    if (conn->calls.size() != 4) return;
    MT_EXPECT_EQ(conn->calls[0].path, std::string("/snapshot/load"));
    MT_EXPECT_EQ(conn->calls[3].path, std::string("/vm"));

    // The launch's token reached the guest document, hashed -- never the token
    // itself.
    MT_EXPECT_TRUE(conn->calls[1].body.find(
                       MmdsMetadata::HashAccessToken("resume-token")) != std::string::npos);
    MT_EXPECT_TRUE(conn->calls[1].body.find("resume-token") == std::string::npos);

    MT_EXPECT_TRUE(built.value()->Stop().ok());
}

// ============================================================
// Rust `snapshot_config_for_launch` + `build_from_snapshot`
//
// A committed snapshot is shared, so launching one is not the same as
// reviving this node's own paused capture: the launch supplies the whole of
// the new sandbox's identity, not just a corrected id.
// ============================================================

namespace {

SandboxLaunchConfig LaunchConfigFor(const core::SandboxId& id) {
    SandboxLaunchConfig config;
    config.sandbox_id = id;
    config.snapshot_id = "snap-committed";
    return config;
}

/// A recorded config with room for two launch-time volumes.
SnapshotConfig CommittedConfig() {
    SnapshotConfig config = CapturedConfig();
    config.common.volume_drive_slots = 2;
    config.common.physical_extra_drive_count = 0;
    return config;
}

}  // namespace

MT_TEST(a_launch_replaces_the_committed_snapshots_identity) {
    // The document the snapshot recorded names whichever sandbox captured it.
    // A committed snapshot is launched by many sandboxes, so the identity is
    // replaced wholesale rather than patched.
    SnapshotConfig config = CommittedConfig();
    SandboxLaunchConfig launch = LaunchConfigFor(ResumedId());
    launch.envd_access_token =
        core::Optional<EnvdAccessToken>(EnvdAccessToken("launch-token"));

    MT_EXPECT_TRUE(ApplyLaunchToSnapshotConfig(&config, launch,
                                               core::Optional<std::string>()).ok());

    MT_EXPECT_TRUE(config.common.mmds_metadata.has_value());
    MT_EXPECT_EQ(config.common.mmds_metadata->SandboxId(), ResumedId().ToString());
    // The launch's snapshot id, not the capture's -- this is what the guest
    // reports as the image it came from.
    MT_EXPECT_EQ(config.common.mmds_metadata->SnapshotId(), std::string("snap-committed"));
    MT_EXPECT_EQ(config.common.mmds_metadata->AccessTokenHash(),
                 MmdsMetadata::HashAccessToken("launch-token"));
    MT_EXPECT_TRUE(config.common.envd_access_token.has_value());
}

MT_TEST(a_launch_asking_for_more_volumes_than_slots_is_refused) {
    // Firecracker cannot add a virtio-block device after a restore, so the
    // shortfall can never be made up. Dropping the surplus would boot a
    // sandbox whose caller finds its mount points empty.
    SnapshotConfig config = CommittedConfig();
    config.common.volume_drive_slots = 1;

    SandboxLaunchConfig launch = LaunchConfigFor(ResumedId());
    launch.extra_drives.push_back(Drive("v1", true));
    launch.extra_drives.push_back(Drive("v2", true));

    const core::Expected<core::Unit, std::string> r =
        ApplyLaunchToSnapshotConfig(&config, launch, core::Optional<std::string>());
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().find("at most 1") != std::string::npos);
    MT_EXPECT_TRUE(r.error().find("requested 2") != std::string::npos);
}

MT_TEST(launch_volumes_are_appended_after_the_snapshots_own_drives) {
    // The snapshot's drives keep their slots; the launch's take the reserved
    // ones after them. Prepending would shift every recorded drive by one and
    // the guest would mount each at its neighbour's path.
    SnapshotConfig config = CommittedConfig();
    config.common.extra_drives.push_back(Drive("recorded", false));
    config.common.physical_extra_drive_count = 1;

    SandboxLaunchConfig launch = LaunchConfigFor(ResumedId());
    launch.extra_drives.push_back(Drive("launch-vol", true));

    MT_EXPECT_TRUE(ApplyLaunchToSnapshotConfig(&config, launch,
                                               core::Optional<std::string>()).ok());
    MT_EXPECT_EQ(config.common.extra_drives.size(), static_cast<std::size_t>(2));
    if (config.common.extra_drives.size() != 2) return;
    MT_EXPECT_EQ(config.common.extra_drives[0].drive_id, std::string("recorded"));
    MT_EXPECT_EQ(config.common.extra_drives[1].drive_id, std::string("launch-vol"));
}

MT_TEST(launch_env_vars_merge_over_the_snapshots_own) {
    // Rust `get_or_insert_default().extend(..)`. Substituting instead of
    // merging would drop the image's defaults for every key the launch does
    // not name -- including the ones the guest needs to start at all.
    SnapshotConfig config = CommittedConfig();
    std::map<std::string, std::string> recorded;
    recorded["PATH"] = "/usr/bin";
    recorded["LANG"] = "C";
    config.common.env_vars =
        core::Optional<std::map<std::string, std::string> >(recorded);

    SandboxLaunchConfig launch = LaunchConfigFor(ResumedId());
    std::map<std::string, std::string> overrides;
    overrides["LANG"] = "en_US.UTF-8";   // overrides
    overrides["EXTRA"] = "1";            // adds
    launch.env_vars =
        core::Optional<std::map<std::string, std::string> >(overrides);

    MT_EXPECT_TRUE(ApplyLaunchToSnapshotConfig(&config, launch,
                                               core::Optional<std::string>()).ok());
    MT_EXPECT_TRUE(config.common.env_vars.has_value());
    if (!config.common.env_vars.has_value()) return;
    MT_EXPECT_EQ((*config.common.env_vars)["PATH"], std::string("/usr/bin"));
    MT_EXPECT_EQ((*config.common.env_vars)["LANG"], std::string("en_US.UTF-8"));
    MT_EXPECT_EQ((*config.common.env_vars)["EXTRA"], std::string("1"));
}

MT_TEST(launch_custom_params_override_the_snapshots_otherwise_inherit) {
    SnapshotConfig config = CommittedConfig();
    SandboxLaunchConfig launch = LaunchConfigFor(ResumedId());

    // Absent on the launch: the snapshot's value is inherited.
    MT_EXPECT_TRUE(ApplyLaunchToSnapshotConfig(
                       &config, launch,
                       core::Optional<std::string>(std::string("{\"from\":\"snapshot\"}")))
                       .ok());
    MT_EXPECT_TRUE(config.common.custom_extension_params_json.has_value());
    MT_EXPECT_EQ(*config.common.custom_extension_params_json,
                 std::string("{\"from\":\"snapshot\"}"));

    // Present on the launch: it wins.
    custom_extension::Params params;
    params.json_bytes = "{\"from\":\"launch\"}";
    launch.custom_extension_params = core::Optional<custom_extension::Params>(params);
    MT_EXPECT_TRUE(ApplyLaunchToSnapshotConfig(
                       &config, launch,
                       core::Optional<std::string>(std::string("{\"from\":\"snapshot\"}")))
                       .ok());
    MT_EXPECT_EQ(*config.common.custom_extension_params_json,
                 std::string("{\"from\":\"launch\"}"));
}

MT_TEST(initial_guest_drive_mounts_start_after_the_physical_drives) {
    // Slot indices begin at `physical_extra_drive_count`: a volume bound into
    // a physical slot is mounted as the wrong filesystem.
    SnapshotConfig config = CommittedConfig();
    config.common.physical_extra_drive_count = 2;

    SandboxLaunchConfig launch = LaunchConfigFor(ResumedId());
    launch.extra_drives.push_back(Drive("v1", true));
    launch.extra_drives.push_back(Drive("v2", true));

    const std::vector<std::pair<std::size_t, ExtraDrive> > mounts =
        InitialGuestDriveMountsForSnapshotLaunch(config, launch);
    MT_EXPECT_EQ(mounts.size(), static_cast<std::size_t>(2));
    if (mounts.size() != 2) return;
    MT_EXPECT_EQ(mounts[0].first, static_cast<std::size_t>(2));
    MT_EXPECT_EQ(mounts[1].first, static_cast<std::size_t>(3));
    MT_EXPECT_EQ(mounts[0].second.drive_id, std::string("v1"));
}

MT_TEST(drives_already_in_the_snapshot_need_no_patch) {
    // They were bound before the load, so patching them afterwards would
    // repoint a drive Firecracker has already opened.
    SnapshotConfig config = CommittedConfig();
    SandboxLaunchConfig launch = LaunchConfigFor(ResumedId());
    launch.extra_drives.push_back(Drive("v1", true));
    launch.extra_drives_in_snapshot = true;

    MT_EXPECT_TRUE(InitialGuestDriveMountsForSnapshotLaunch(config, launch).empty());
}

MT_TEST(build_from_snapshot_refuses_another_backends_capture) {
    // A capture is restored by the VMM that took it, so this is not merely
    // unsupported -- the vm_state is unreadable here.
    Config cfg;
    cfg.firecracker_bin = "/bin/sh";
    cfg::AppConfig app;
    app.firecracker.binary_path = std::string("/bin/sh");
    app.kernel.image_path = std::string("vmlinux.bin");
    app.tools.drive_path = std::string("tools.ext4");
    app.tools.version = std::string("1.2.3");
    FirecrackerSandboxFactory factory(cfg, app);

    RunnableSnapshotView snapshot;
    snapshot.snapshot_id = "snap-committed";
    snapshot.tools_drive_version = "1.2.3";
    SnapshotManifestView manifest;
    manifest.backend = "cloud-hypervisor";
    manifest.vm_state_path = "/snap/vm_state.bin";

    const core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError> r =
        factory.BuildFromSnapshot(snapshot, manifest, LaunchConfigFor(ResumedId()));
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().chain().find("cloud-hypervisor") != std::string::npos);
}

MT_TEST(build_from_snapshot_without_an_app_config_says_so) {
    // The projection reads the node's own defaults (tools drive, kernel), so
    // a factory without them can only refuse -- rather than invent paths an
    // operator is supposed to choose.
    Config cfg;
    FirecrackerSandboxFactory factory(cfg);

    RunnableSnapshotView snapshot;
    snapshot.snapshot_id = "snap-committed";
    SnapshotManifestView manifest;
    manifest.backend = "firecracker";

    const core::Expected<std::unique_ptr<SandboxBackend>, core::AnyError> r =
        factory.BuildFromSnapshot(snapshot, manifest, LaunchConfigFor(ResumedId()));
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().chain().find("AppConfig") != std::string::npos);
}

MT_MAIN
