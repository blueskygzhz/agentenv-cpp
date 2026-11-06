// SPDX-License-Identifier: MIT
// Rust: src/sandbox/guest_volumes.rs.
//
// Upstream ships no inline tests for this file, so the judgements under test
// are the ones its comments claim: the read-only mount options, the
// stage/bind/detach ordering for a sub-path, the unmount-and-retry that
// refuses a stale mount at the target, the freeze script's single opened fd
// and asymmetric validation, and the absent-vs-empty distinction on the
// recorded frozen device.
//
// The executor double records commands and replays scripted exit codes; the
// code under test is the real `MountInitialGuestDrives` /
// `SyncWritableVolumeFilesystems` / `SetVolumeFreeze`.
#include "agentenv/sandbox/guest_volumes.h"

#include <string>
#include <vector>

#include "microtest.h"

using namespace agentenv;           // NOLINT
using namespace agentenv::sandbox;  // NOLINT

namespace {

struct RecordedCall {
    std::string                 op;   // the command, or "create_dir_all"
    std::vector<std::string>    args;
    core::Optional<std::string> cwd;
    core::Optional<int64_t>     timeout_ms;
};

/// Replays scripted results keyed by a substring of the joined argv, so a
/// test can fail exactly the mount it cares about and let the rest succeed.
class ScriptedGuest : public Executor {
 public:
    struct Rule {
        std::string match;      // substring of the joined argv
        int32_t     exit_code;
        std::string stderr_data;
        std::string stdout_data;
        bool        transport_error;

        Rule() : exit_code(0), transport_error(false) {}
    };

    ScriptedGuest() : mkdir_fails_for_() {}

    core::Expected<ProcessOutput, std::string> RunCommandWithOpts(
        const std::string& cmd, const std::vector<std::string>& args,
        const ProcessOpts& opts) override {
        RecordedCall call;
        call.op         = cmd;
        call.args       = args;
        call.cwd        = opts.cwd;
        call.timeout_ms = opts.timeout_ms;
        calls_.push_back(call);

        const std::string joined = Join(args);
        for (std::size_t i = 0; i < rules_.size(); ++i) {
            if (joined.find(rules_[i].match) == std::string::npos) continue;
            if (rules_[i].transport_error) {
                return core::make_unexpected(std::string("transport closed"));
            }
            ProcessOutput out;
            out.exit_code   = rules_[i].exit_code;
            out.stderr_data = rules_[i].stderr_data;
            out.stdout_data = rules_[i].stdout_data;
            // One-shot: a rule fires once so "fail, then succeed on retry" is
            // expressible, which is the behaviour the retry path hinges on.
            rules_.erase(rules_.begin() + static_cast<std::ptrdiff_t>(i));
            return out;
        }
        return ProcessOutput();
    }

    core::Expected<core::Unit, std::string> CreateDirAll(const std::string& path) override {
        RecordedCall call;
        call.op = "create_dir_all";
        call.args.push_back(path);
        calls_.push_back(call);
        if (!mkdir_fails_for_.empty() && path.find(mkdir_fails_for_) != std::string::npos) {
            return core::make_unexpected(std::string("permission denied"));
        }
        return core::Unit();
    }

    core::Expected<std::unique_ptr<ProcessHandle>, std::string> StartProcess(
        const std::string&, const std::vector<std::string>&, const ProcessOpts&) override {
        return core::make_unexpected(std::string("no volume operation starts a process"));
    }

    void FailOnce(const std::string& match, int32_t exit_code, const std::string& err) {
        Rule rule;
        rule.match       = match;
        rule.exit_code   = exit_code;
        rule.stderr_data = err;
        rules_.push_back(rule);
    }

    void SucceedWithStdout(const std::string& match, const std::string& out) {
        Rule rule;
        rule.match       = match;
        rule.stdout_data = out;
        rules_.push_back(rule);
    }

    void TransportErrorOnce(const std::string& match) {
        Rule rule;
        rule.match           = match;
        rule.transport_error = true;
        rules_.push_back(rule);
    }

    void FailMkdirFor(const std::string& fragment) { mkdir_fails_for_ = fragment; }

    const std::vector<RecordedCall>& calls() const { return calls_; }

    /// Joined argv of every recorded *command* (directory creations keep their
    /// "create_dir_all" op so the two kinds stay distinguishable).
    std::vector<std::string> Trace() const {
        std::vector<std::string> trace;
        for (std::size_t i = 0; i < calls_.size(); ++i) {
            if (calls_[i].op == "create_dir_all") {
                trace.push_back("mkdir " + calls_[i].args[0]);
            } else {
                trace.push_back(Join(calls_[i].args));
            }
        }
        return trace;
    }

    static std::string Join(const std::vector<std::string>& args) {
        std::string joined;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i != 0) joined += " ";
            joined += args[i];
        }
        return joined;
    }

 private:
    std::vector<RecordedCall> calls_;
    std::vector<Rule>         rules_;
    std::string               mkdir_fails_for_;
};

ExtraDrive Drive(const std::string& id, const std::string& mount_path, bool read_only) {
    ExtraDrive drive;
    drive.drive_id    = id;
    drive.mount_path  = mount_path;
    drive.read_only   = read_only;
    drive.image_config_path = "/var/lib/agentenv/" + id + ".json";
    return drive;
}

std::vector<IndexedGuestDrive> One(std::size_t index, const ExtraDrive& drive) {
    std::vector<IndexedGuestDrive> drives;
    drives.push_back(IndexedGuestDrive(index, drive));
    return drives;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::size_t IndexOf(const std::vector<std::string>& trace, const std::string& needle) {
    for (std::size_t i = 0; i < trace.size(); ++i) {
        if (Contains(trace[i], needle)) return i;
    }
    return trace.size();
}

}  // namespace

// ---------------------------------------------------------------------------
// device naming
// ---------------------------------------------------------------------------

MT_TEST(extra_drives_are_named_from_vdc_upwards) {
    // vda is the rootfs and vdb the tools drive; starting extra drives at vdc
    // is what keeps a volume from being mounted over the root filesystem.
    MT_EXPECT_TRUE(GuestDeviceForDriveIndex(0) == "/dev/vdc");
    MT_EXPECT_TRUE(GuestDeviceForDriveIndex(1) == "/dev/vdd");
    MT_EXPECT_TRUE(GuestDeviceForDriveIndex(23) == "/dev/vdz");
}

MT_TEST(a_filtered_drive_subset_keeps_its_original_device_names) {
    // The index travels with the drive precisely so that mounting only the
    // drives init skipped does not renumber them. If the device came from the
    // loop position, drive 2 below would be mounted as /dev/vdc — the first
    // extra drive, which belongs to someone else.
    ScriptedGuest guest;
    std::vector<IndexedGuestDrive> drives;
    drives.push_back(IndexedGuestDrive(2, Drive("vol-a", "/mnt/a", true)));
    drives.push_back(IndexedGuestDrive(5, Drive("vol-b", "/mnt/b", false)));
    MT_EXPECT_TRUE(MountInitialGuestDrives(&guest, drives).ok());

    const std::vector<std::string> trace = guest.Trace();
    MT_EXPECT_TRUE(IndexOf(trace, "/dev/vde /mnt/a") < trace.size());
    MT_EXPECT_TRUE(IndexOf(trace, "/dev/vdh /mnt/b") < trace.size());
}

// ---------------------------------------------------------------------------
// plain mounts
// ---------------------------------------------------------------------------

MT_TEST(a_writable_drive_mounts_with_no_options_after_its_target_exists) {
    ScriptedGuest guest;
    MT_EXPECT_TRUE(MountInitialGuestDrives(&guest, One(0, Drive("d", "/mnt/d", false))).ok());

    const std::vector<std::string> trace = guest.Trace();
    MT_EXPECT_TRUE(trace.size() == 2);
    // The directory must exist before the mount: mounting onto a missing path
    // fails, and the guest image is not required to ship the mount point.
    MT_EXPECT_TRUE(trace[0] == "mkdir /mnt/d");
    MT_EXPECT_TRUE(trace[1] == "mount -n /dev/vdc /mnt/d");
    MT_EXPECT_TRUE(guest.calls()[1].op == std::string(kGuestBusyboxPath));
}

MT_TEST(a_read_only_drive_suppresses_journal_replay_and_forbids_writes) {
    // Both halves of `ro,noload` matter: a crash-consistent snapshot still
    // carries ext4's needs-recovery flag, and a read-only OverlayBD device can
    // neither replay the journal nor accept the write that replay would make.
    ScriptedGuest guest;
    MT_EXPECT_TRUE(MountInitialGuestDrives(&guest, One(1, Drive("ro", "/mnt/ro", true))).ok());

    const std::vector<std::string> trace = guest.Trace();
    MT_EXPECT_TRUE(trace.size() == 2);
    MT_EXPECT_TRUE(trace[1] == "mount -n -o ro,noload /dev/vdd /mnt/ro");
}

MT_TEST(every_guest_command_runs_from_the_root_directory) {
    // envd's default workdir comes from the image and may sit inside a path
    // being mounted over or lazily unmounted, where it would hold the
    // directory busy.
    ScriptedGuest guest;
    ExtraDrive drive = Drive("sub", "/mnt/sub", false);
    drive.sub_path = core::Optional<std::string>(std::string("inner"));
    MT_EXPECT_TRUE(MountInitialGuestDrives(&guest, One(0, drive)).ok());

    bool saw_command = false;
    for (std::size_t i = 0; i < guest.calls().size(); ++i) {
        if (guest.calls()[i].op == "create_dir_all") continue;
        saw_command = true;
        MT_EXPECT_TRUE(guest.calls()[i].cwd.has_value());
        if (guest.calls()[i].cwd.has_value()) {
            MT_EXPECT_TRUE(*guest.calls()[i].cwd == "/");
        }
    }
    MT_EXPECT_TRUE(saw_command);
}

MT_TEST(a_failed_target_directory_stops_before_any_mount) {
    ScriptedGuest guest;
    guest.FailMkdirFor("/mnt/d");
    const core::Expected<core::Unit, std::string> r =
        MountInitialGuestDrives(&guest, One(0, Drive("d", "/mnt/d", false)));
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(Contains(r.error(), "permission denied"));
    MT_EXPECT_TRUE(guest.calls().size() == 1);
}

MT_TEST(a_failing_drive_aborts_the_remaining_drives) {
    // Sequential and fail-fast: a later drive may target a path under an
    // earlier one, so continuing past a failure would mount it onto a
    // directory whose backing filesystem is not the one that was intended.
    ScriptedGuest guest;
    guest.FailOnce("/dev/vdc /mnt/a", 32, "no such device");
    guest.FailOnce("/dev/vdc /mnt/a", 32, "no such device");  // the retry
    std::vector<IndexedGuestDrive> drives;
    drives.push_back(IndexedGuestDrive(0, Drive("a", "/mnt/a", false)));
    drives.push_back(IndexedGuestDrive(1, Drive("b", "/mnt/a/b", false)));

    const core::Expected<core::Unit, std::string> r = MountInitialGuestDrives(&guest, drives);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(IndexOf(guest.Trace(), "/mnt/a/b") == guest.Trace().size());
}

// ---------------------------------------------------------------------------
// stale mount at the target
// ---------------------------------------------------------------------------

MT_TEST(a_stale_mount_at_the_target_is_cleared_and_the_request_retried) {
    // "Already mounted" must not count as success: a resumed guest can carry
    // a previous incarnation's device at this path, and accepting it would
    // give the sandbox a filesystem backed by the wrong drive while every
    // lookup appeared to work.
    ScriptedGuest guest;
    guest.FailOnce("mount -n /dev/vdc /mnt/d", 32, "/mnt/d already mounted");

    MT_EXPECT_TRUE(MountInitialGuestDrives(&guest, One(0, Drive("d", "/mnt/d", false))).ok());

    const std::vector<std::string> trace = guest.Trace();
    MT_EXPECT_TRUE(trace.size() == 4);
    MT_EXPECT_TRUE(trace[1] == "mount -n /dev/vdc /mnt/d");
    MT_EXPECT_TRUE(trace[2] == "umount -l /mnt/d");
    MT_EXPECT_TRUE(trace[3] == "mount -n /dev/vdc /mnt/d");
}

MT_TEST(the_retry_happens_exactly_once_and_reports_the_second_failure) {
    ScriptedGuest guest;
    guest.FailOnce("mount -n /dev/vdc /mnt/d", 1, "first");
    guest.FailOnce("mount -n /dev/vdc /mnt/d", 32, "  wrong fs type  ");

    const core::Expected<core::Unit, std::string> r =
        MountInitialGuestDrives(&guest, One(0, Drive("d", "/mnt/d", false)));
    MT_EXPECT_TRUE(!r.ok());
    // The reported status and stderr come from the retry, not the first
    // attempt: the first failure is expected whenever a stale mount is being
    // cleared, so surfacing it would make every successful recovery look like
    // the cause of a later problem.
    MT_EXPECT_TRUE(Contains(r.error(), "exit code 32"));
    MT_EXPECT_TRUE(Contains(r.error(), "wrong fs type"));
    MT_EXPECT_TRUE(!Contains(r.error(), "first"));
    // Trimmed, so the message does not carry the applet's trailing newline.
    MT_EXPECT_TRUE(!Contains(r.error(), "  wrong fs type"));
    MT_EXPECT_TRUE(Contains(r.error(), "failed to mount /dev/vdc at /mnt/d"));
    // mkdir, mount, umount, mount — and no third attempt.
    MT_EXPECT_TRUE(guest.Trace().size() == 4);
}

MT_TEST(a_transport_failure_is_not_retried_as_a_mount_failure) {
    // A non-zero exit says the guest answered; a transport error says it did
    // not, so the mount's outcome is unknown and clearing the target could
    // unmount a filesystem that was just mounted successfully.
    ScriptedGuest guest;
    guest.TransportErrorOnce("mount -n /dev/vdc /mnt/d");

    const core::Expected<core::Unit, std::string> r =
        MountInitialGuestDrives(&guest, One(0, Drive("d", "/mnt/d", false)));
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(Contains(r.error(), "transport closed"));
    MT_EXPECT_TRUE(IndexOf(guest.Trace(), "umount") == guest.Trace().size());
}

// ---------------------------------------------------------------------------
// sub_path
// ---------------------------------------------------------------------------

MT_TEST(a_sub_path_drive_stages_binds_then_detaches_the_stage) {
    ScriptedGuest guest;
    ExtraDrive drive = Drive("vol-7", "/mnt/data", true);
    drive.sub_path = core::Optional<std::string>(std::string("project/src"));

    MT_EXPECT_TRUE(MountInitialGuestDrives(&guest, One(0, drive)).ok());

    const std::vector<std::string> trace = guest.Trace();
    MT_EXPECT_TRUE(trace.size() == 5);
    MT_EXPECT_TRUE(trace[0] == "mkdir /mnt/data");
    // Keyed by drive id, so two drives with different sub-paths cannot
    // collide on one staging directory.
    MT_EXPECT_TRUE(trace[1] == "mkdir /run/agentenv-drive-vol-7");
    // The read-only options apply to the *device* mount; the bind carries
    // only --bind, which is what upstream passes.
    MT_EXPECT_TRUE(trace[2] ==
                   "mount -n -o ro,noload /dev/vdc /run/agentenv-drive-vol-7");
    MT_EXPECT_TRUE(trace[3] ==
                   "mount -n --bind /run/agentenv-drive-vol-7/project/src /mnt/data");
    // Detached last: the full filesystem must not stay reachable outside the
    // requested sub-path.
    MT_EXPECT_TRUE(trace[4] == "umount -l /run/agentenv-drive-vol-7");
}

MT_TEST(a_failed_stage_detach_does_not_fail_a_completed_mount) {
    // The bind holds the staged filesystem, so the lazy unmount cannot
    // complete immediately. Treating that as fatal would report a volume that
    // *is* mounted as a launch failure.
    ScriptedGuest guest;
    ExtraDrive drive = Drive("v", "/mnt/data", false);
    drive.sub_path = core::Optional<std::string>(std::string("inner"));
    guest.FailOnce("umount -l /run/agentenv-drive-v", 1, "target is busy");

    MT_EXPECT_TRUE(MountInitialGuestDrives(&guest, One(0, drive)).ok());
}

MT_TEST(a_failed_bind_aborts_before_the_stage_is_detached) {
    // Leaving the stage mounted on failure is the right outcome: the caller
    // tears the sandbox down, and detaching first would discard the only
    // evidence of what was mounted where.
    ScriptedGuest guest;
    ExtraDrive drive = Drive("v", "/mnt/data", false);
    drive.sub_path = core::Optional<std::string>(std::string("inner"));
    guest.FailOnce("--bind", 32, "no such file");
    guest.FailOnce("--bind", 32, "no such file");

    const core::Expected<core::Unit, std::string> r = MountInitialGuestDrives(&guest, One(0, drive));
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(Contains(r.error(), "/run/agentenv-drive-v/inner"));
    MT_EXPECT_TRUE(Contains(r.error(), "at /mnt/data"));
    // Only the retry's own umount of the bind *target* ran; the stage was left
    // in place.
    MT_EXPECT_TRUE(IndexOf(guest.Trace(), "umount -l /run/agentenv-drive-v") ==
                   guest.Trace().size());
}

MT_TEST(a_failed_stage_directory_stops_before_the_device_is_mounted) {
    ScriptedGuest guest;
    ExtraDrive drive = Drive("v", "/mnt/data", false);
    drive.sub_path = core::Optional<std::string>(std::string("inner"));
    guest.FailMkdirFor("/run/agentenv-drive-");

    MT_EXPECT_TRUE(!MountInitialGuestDrives(&guest, One(0, drive)).ok());
    MT_EXPECT_TRUE(IndexOf(guest.Trace(), "mount") == guest.Trace().size());
}

MT_TEST(an_empty_drive_list_issues_no_guest_commands) {
    ScriptedGuest guest;
    MT_EXPECT_TRUE(MountInitialGuestDrives(&guest, std::vector<IndexedGuestDrive>()).ok());
    MT_EXPECT_TRUE(guest.calls().empty());
}

// ---------------------------------------------------------------------------
// sync
// ---------------------------------------------------------------------------

MT_TEST(sync_runs_busybox_sync_from_the_root_directory) {
    ScriptedGuest guest;
    MT_EXPECT_TRUE(SyncWritableVolumeFilesystems(&guest).ok());
    MT_EXPECT_TRUE(guest.calls().size() == 1);
    MT_EXPECT_TRUE(guest.calls()[0].op == std::string(kGuestBusyboxPath));
    MT_EXPECT_TRUE(guest.Trace()[0] == "sync");
    // No timeout: a sync of a large dirty page cache legitimately takes a
    // while, and killing it is exactly the thing that would leave the capture
    // missing writes.
    MT_EXPECT_TRUE(!guest.calls()[0].timeout_ms.has_value());
}

MT_TEST(a_nonzero_sync_exit_fails_the_pause) {
    // Unflushed page cache would be sealed into the volume snapshot as
    // missing writes the guest believes it made, so this cannot be a warning.
    ScriptedGuest guest;
    guest.FailOnce("sync", 1, "\nI/O error\n");
    const core::Expected<core::Unit, std::string> r = SyncWritableVolumeFilesystems(&guest);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(Contains(r.error(), "exit code 1"));
    MT_EXPECT_TRUE(Contains(r.error(), "I/O error"));
    MT_EXPECT_TRUE(!Contains(r.error(), "\n"));
}

MT_TEST(a_transport_failure_during_sync_is_reported_as_itself) {
    ScriptedGuest guest;
    guest.TransportErrorOnce("sync");
    const core::Expected<core::Unit, std::string> r = SyncWritableVolumeFilesystems(&guest);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error() == "transport closed");
}

// ---------------------------------------------------------------------------
// freeze / thaw
// ---------------------------------------------------------------------------

MT_TEST(the_freeze_script_keeps_one_opened_fd_across_check_and_ioctl) {
    // If the script stat'd the path and then froze the path, an unmount
    // between the two would redirect fsfreeze at whatever is now there — in
    // the worst case the root filesystem hosting envd, freezing the daemon
    // that is supposed to thaw it.
    const std::string script = VolumeFreezeScript();
    // Kept in a local for readability only. It used to be required, because
    // `MT_EXPECT_TRUE` concatenated its stringized condition into the printf
    // format and the `%d` was read as a specifier; the macro now passes it as
    // an argument, so an inline literal would be safe too.
    const std::string stat_the_fd = "stat -Lc %d /proc/self/fd/3";
    MT_EXPECT_TRUE(Contains(script, "exec 3< \"$1\""));
    MT_EXPECT_TRUE(Contains(script, stat_the_fd));
    MT_EXPECT_TRUE(Contains(script, "fsfreeze \"$2\" /proc/self/fd/3"));
    // Neither the check nor the ioctl may name the mount path again.
    MT_EXPECT_TRUE(!Contains(script, "fsfreeze \"$2\" \"$1\""));
    // `set -eu`: a failed validation must abort before fsfreeze runs, and an
    // unset parameter must not expand to an empty comparison.
    MT_EXPECT_TRUE(Contains(script, "set -eu"));
}

MT_TEST(the_freeze_script_validates_asymmetrically) {
    // Freeze refuses the root device; thaw requires the recorded one. A thaw
    // aimed at a filesystem nobody froze is a silent no-op today and an
    // unbalanced thaw of someone else's freeze tomorrow.
    const std::string script = VolumeFreezeScript();
    const std::string refuse_root = "test \"$device\" != \"$(/agentenv/bin/busybox stat -c %d /)\"";
    const std::string require_recorded = "test \"$device\" = \"$3\"";
    // The device number is echoed so the caller can record what to thaw.
    const std::string echo_device = "printf '%s\\n' \"$device\"";
    MT_EXPECT_TRUE(Contains(script, refuse_root));
    MT_EXPECT_TRUE(Contains(script, require_recorded));
    MT_EXPECT_TRUE(Contains(script, echo_device));
}

MT_TEST(freeze_passes_an_empty_device_and_the_freeze_operation) {
    ScriptedGuest guest;
    guest.SucceedWithStdout("--freeze", "2049\n");

    const core::Expected<ProcessOutput, std::string> r =
        SetVolumeFreeze(&guest, "/mnt/data", core::Optional<std::string>());
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().stdout_data == "2049\n");

    const std::vector<std::string>& args = guest.calls()[0].args;
    MT_EXPECT_TRUE(args.size() == 7);
    MT_EXPECT_TRUE(args[0] == "sh");
    MT_EXPECT_TRUE(args[1] == "-c");
    MT_EXPECT_TRUE(args[2] == VolumeFreezeScript());
    MT_EXPECT_TRUE(args[3] == "freeze-volume");
    MT_EXPECT_TRUE(args[4] == "/mnt/data");
    MT_EXPECT_TRUE(args[5] == "--freeze");
    // `$3` is supplied even when unused, so `set -u` cannot abort the thaw
    // branch's comparison on an unset parameter.
    MT_EXPECT_TRUE(args[6].empty());
}

MT_TEST(an_absent_device_selects_freeze_and_an_empty_one_selects_thaw) {
    // Presence, not emptiness, picks the operation. The caller records the
    // thaw obligation with an empty device *before* awaiting the freeze, so an
    // empty recorded device means "the freeze's outcome is unknown" — and the
    // script's `test "$device" = ""` then fails rather than thawing whatever
    // is mounted there now. Collapsing absent and empty would turn that
    // deliberate failure into an unguarded unfreeze.
    ScriptedGuest absent;
    SetVolumeFreeze(&absent, "/mnt/data", core::Optional<std::string>());
    MT_EXPECT_TRUE(absent.calls()[0].args[5] == "--freeze");

    ScriptedGuest empty;
    SetVolumeFreeze(&empty, "/mnt/data", core::Optional<std::string>(std::string()));
    MT_EXPECT_TRUE(empty.calls()[0].args[5] == "--unfreeze");
    MT_EXPECT_TRUE(empty.calls()[0].args[6].empty());
}

MT_TEST(thaw_forwards_the_recorded_device) {
    ScriptedGuest guest;
    SetVolumeFreeze(&guest, "/mnt/data", core::Optional<std::string>(std::string("2049")));
    MT_EXPECT_TRUE(guest.calls()[0].args[5] == "--unfreeze");
    MT_EXPECT_TRUE(guest.calls()[0].args[6] == "2049");
}

MT_TEST(freeze_is_bounded_so_a_wedged_guest_cannot_hold_the_capture) {
    // Upstream bounds the call with a 30s task timeout; with no way to abandon
    // a blocking call the bound is pushed into the guest as a process timeout.
    // What must not happen is the bound disappearing: a freeze that never
    // returns blocks the capture holding the sandbox lock.
    ScriptedGuest guest;
    SetVolumeFreeze(&guest, "/mnt/data", core::Optional<std::string>());
    MT_EXPECT_TRUE(guest.calls()[0].timeout_ms.has_value());
    if (guest.calls()[0].timeout_ms.has_value()) {
        MT_EXPECT_TRUE(*guest.calls()[0].timeout_ms == kVolumeFreezeTimeoutMs);
    }
    MT_EXPECT_TRUE(kVolumeFreezeTimeoutMs == 30 * 1000);
}

MT_TEST(a_nonzero_freeze_exit_is_returned_not_turned_into_an_error) {
    // The caller needs the output, not a verdict: stdout carries the device
    // number it must record, and a failed freeze and a failed thaw are
    // classified differently (terminal vs. recoverable) at the call sites.
    ScriptedGuest guest;
    guest.FailOnce("--freeze", 1, "device is the root filesystem");

    const core::Expected<ProcessOutput, std::string> r =
        SetVolumeFreeze(&guest, "/mnt/data", core::Optional<std::string>());
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().exit_code == 1);
    MT_EXPECT_TRUE(Contains(r.value().stderr_data, "root filesystem"));
}

MT_TEST(a_transport_failure_during_freeze_leaves_the_ioctl_outcome_unknown) {
    // Distinct from a non-zero exit on purpose: the caller treats this as
    // terminal because the VM cannot be returned to service without knowing
    // whether the filesystem is frozen.
    ScriptedGuest guest;
    guest.TransportErrorOnce("--freeze");
    const core::Expected<ProcessOutput, std::string> r =
        SetVolumeFreeze(&guest, "/mnt/data", core::Optional<std::string>());
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error() == "transport closed");
}

MT_MAIN
