#!/usr/bin/env python3
"""Mutation harness for `SnapshotExtraDrives` (Rust `snapshot_extra_drives`).

This loop had no implementation at all, and its absence was not a missing
feature -- it was a wrong manifest. `pause_to_dir` described every extra drive
straight off the launch config, so each entry named the drive's *source image*
and its *requested* size. A restore from such a manifest opens each data disk's
pre-boot stack, silently without a single write the sandbox made to it.

Every mutation below is a plausible-looking variant of the loop that still
returns `Ok` from the capture. That is the shape of defect this file exists to
rule out: nothing fails at pause time, the snapshot publishes, and the loss only
appears when something restores or reads back through the stack -- by which
point the live upper that held the truth has been deleted with the sandbox.

**Out of scope, and still unlocked**: the `PauseToDir` wiring around the loop --
specifically step 8's filtering of volumes out of the manifest. Reaching that
branch needs at least one *sealed* drive, which needs a live ublk device, so no
test can get there today. `volume_drive_slots` and the bookkeeping mismatch are
covered (`test_firecracker_sandbox_backend`); the volume filter is not.

Status is decided **by exit code only**: the suites call `exit(1)` on the first
failing assertion, so the summary line is not printed on failure and scraping
stdout would misreport a caught mutation as a survivor.
"""

import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
BACKEND = ROOT / "src/sandbox/firecracker/sandbox_backend.cc"

# Both suites run for every mutation: the loop's own behaviour is pinned in the
# restack suite, its preconditions (the bookkeeping mismatch) in the backend
# suite. A mutation is caught when *either* turns red.
TARGETS = ["test_firecracker_overlaybd_restack", "test_firecracker_sandbox_backend"]

# Where each drive's layer goes. The ternary is the volume/non-volume split.
OUTPUT_DIR = """        const std::string output_dir =
            persistent.has_value() ? *persistent
                                   : snapshot_dir + "/drives/" + drives[i].drive_id;"""

# What each drive's layer is called.
LAYER_NAME = """        const std::string layer_file_name =
            persistent.has_value()
                ? "snapshot-" + core::Uuid::GenV7().ToSimpleString() + ".commit"
                : std::string("snapshot.commit");"""

# The restack call: which device is sealed, whether it is treated as writable,
# and which config is read as the live stack.
RESTACK_CALL = """            restacker(runtimes[i].device), drives[i].read_only,
            runtimes[i].image_config_path, output_dir, layer_file_name,"""

SIZED = """            drives[i]
                .WithImageConfigPath(staged.value())
                .TryWithVirtualSize(runtimes[i].actual_virtual_size);"""

MUTATIONS = [
    # ---- the preconditions, in the order they are checked -------------------
    #
    # A sandbox with no extra drives is the common case and must capture on a
    # node whose daemon is unreachable. Without the short-circuit the sealer
    # requirement turns a working snapshot into a refusal.
    (
        "an_empty_drive_list_still_demands_a_sealer",
        "    if (drives.empty()) return std::vector<ExtraDrive>();",
        "    if (false) return std::vector<ExtraDrive>();",
    ),
    # The pairing upstream asserts with `bail!("extra drive bookkeeping
    # mismatch")`. Without it a backend built over a handed-in instance -- the
    # form `Pause` produces -- indexes `runtimes` out of bounds, and in the
    # cases that stay in range it seals one drive's device under another
    # drive's name.
    (
        "configured_drives_need_not_match_prepared_devices",
        "    if (drives.size() != runtimes.size()) {",
        "    if (false) {",
    ),
    # No sealer and drives to seal. Proceeding describes each drive by its live
    # config, whose writable upper the runtime deletes when the sandbox stops.
    (
        "drives_can_be_captured_without_a_sealer",
        "    if (!restacker) {",
        "    if (false) {",
    ),
    # ---- where the layer lands ---------------------------------------------
    #
    # A volume outlives the capture and is published from its own directory.
    # Sealed into the snapshot dir instead, its layer is inside a tree that a
    # superseded or failed capture is free to delete -- taking with it a lower
    # the volume's live `image.json` still references.
    (
        "a_volume_seals_into_the_capture_like_any_other_drive",
        OUTPUT_DIR,
        """        const std::string output_dir =
            snapshot_dir + "/drives/" + drives[i].drive_id;""",
    ),
    # ---- what the layer is called ------------------------------------------
    #
    # A volume reuses one directory across pauses. A fixed name makes the
    # second pause overwrite the layer the first sealed -- a layer the live
    # `image.json` still lists as a lower -- so the stack is not extended but
    # corrupted, and only a later read notices.
    (
        "a_volume_layer_reuses_one_name_across_pauses",
        LAYER_NAME,
        """        const std::string layer_file_name = std::string("snapshot.commit");""",
    ),
    # And the reverse: every layer gets a fresh name, so a non-volume's place
    # in the published snapshot layout stops being predictable.
    (
        "every_layer_gets_a_per_pause_name",
        LAYER_NAME,
        """        const std::string layer_file_name =
            "snapshot-" + core::Uuid::GenV7().ToSimpleString() + ".commit";""",
    ),
    # ---- what is sealed ----------------------------------------------------
    #
    # One device for every drive. The capture succeeds and each entry names its
    # own stack, so the manifest looks right -- while drive B's name and mount
    # path describe drive A's disk.
    (
        "every_drive_is_sealed_against_the_first_device",
        RESTACK_CALL,
        """            restacker(runtimes[0].device), drives[i].read_only,
            runtimes[i].image_config_path, output_dir, layer_file_name,""",
    ),
    # The *source image* config instead of the live runtime one. This is
    # precisely the defect the missing loop used to cause, reintroduced one
    # level down: the capture seals and describes the stack the sandbox booted
    # from, holding none of its writes.
    (
        "the_source_image_config_is_sealed_instead_of_the_live_one",
        RESTACK_CALL,
        """            restacker(runtimes[i].device), drives[i].read_only,
            drives[i].image_config_path, output_dir, layer_file_name,""",
    ),
    # A read-only drive has no upper to seal, and several sandboxes may be
    # sharing it. Asking the daemon to seal it is the one call here that can
    # damage a disk this operation must leave untouched.
    (
        "read_only_drives_are_sealed_too",
        RESTACK_CALL,
        """            restacker(runtimes[i].device), false,
            runtimes[i].image_config_path, output_dir, layer_file_name,""",
    ),
    # ---- what the result claims --------------------------------------------
    #
    # Rust `try_with_virtual_size(runtime.actual_virtual_size)`. A restore
    # sizes the ublk device from this before opening the stack, so preferring
    # the requested value opens a correctly captured stack behind a device of
    # the wrong length.
    (
        "the_requested_size_is_recorded_instead_of_the_actual_one",
        SIZED,
        """            drives[i]
                .WithImageConfigPath(staged.value())
                .TryWithVirtualSize(drives[i].virtual_size.has_value()
                                        ? *drives[i].virtual_size
                                        : runtimes[i].actual_virtual_size);""",
    ),
    # The drive is returned unrewritten, still naming its live runtime config.
    # The sandbox's own `upper.data` is then what the manifest points at, and
    # `Stop` deletes it.
    (
        "the_returned_drive_still_names_the_live_config",
        SIZED,
        """            drives[i].TryWithVirtualSize(runtimes[i].actual_virtual_size);""",
    ),
    # A zero-length device reaching the daemon as a request for an empty one.
    # By here the upper is sealed, so "recoverable" tells the caller to resume
    # a sandbox whose drive describes a stack it no longer has.
    (
        "a_sealed_drive_of_no_size_is_reported_as_recoverable",
        """            return core::make_unexpected(SandboxCaptureError::Terminal(
                "snapshot extra drive '" + drives[i].drive_id + "': " + sized.error()));""",
        """            return core::make_unexpected(SandboxCaptureError::Recoverable(
                "snapshot extra drive '" + drives[i].drive_id + "': " + sized.error()));""",
    ),
    # The restack is the only step that knows whether the seal happened.
    # Flattening its answer to recoverable hands back a sandbox that cannot be
    # resumed under an error that says it can.
    (
        "the_restacks_classification_is_flattened_to_recoverable",
        # Anchored on the comment: the identical line also appears on the
        # rootfs path, and an ambiguous anchor would mutate whichever came
        # first rather than the one under test.
        "        // sandbox unresumable.\n"
        "        if (!staged.ok()) return core::make_unexpected(staged.error());",
        "        // sandbox unresumable.\n"
        "        if (!staged.ok())\n"
        "            return core::make_unexpected(\n"
        "                SandboxCaptureError::Recoverable(staged.error().Message()));",
    ),
]


def build(target):
    return subprocess.run(
        ["cmake", "--build", "build", "-j", "8", "--target", target],
        cwd=ROOT, capture_output=True, text=True,
    ).returncode == 0


def run(target):
    return subprocess.run([str(ROOT / "build/bin" / target)],
                          cwd=ROOT, capture_output=True, text=True).returncode


def build_all():
    return all(build(t) for t in TARGETS)


def failing_targets():
    """Targets whose suite reported a failure, i.e. noticed the mutation."""
    return [t for t in TARGETS if run(t) != 0]


def main():
    original = BACKEND.read_text()
    backup = BACKEND.with_suffix(BACKEND.suffix + ".mutation-backup")
    shutil.copy(BACKEND, backup)

    if not build_all():
        print("BASELINE BUILD FAILED")
        return 2
    red = failing_targets()
    if red:
        print(f"BASELINE TESTS FAILED: {', '.join(red)}")
        return 2
    print("baseline: build ok, tests pass\n")

    caught, survived, broken, noop = [], [], [], []
    try:
        for name, old, new in MUTATIONS:
            BACKEND.write_text(original)
            assert original.count(old) == 1, \
                f"anchor for {name} matched {original.count(old)} times"
            if old == new:
                noop.append(name)
                print(f"  NO-OP    {name} (mutation is identity)")
                continue
            BACKEND.write_text(original.replace(old, new, 1))

            if not build_all():
                broken.append(name)
                print(f"  BROKEN   {name} (did not compile)")
                continue
            red = failing_targets()
            if red:
                caught.append(name)
                print(f"  caught   {name} ({', '.join(red)})")
            else:
                survived.append(name)
                print(f"  SURVIVED {name}")
    finally:
        shutil.copy(backup, BACKEND)
        backup.unlink()
        build_all()

    print(f"\n{len(caught)}/{len(MUTATIONS)} caught, "
          f"{len(survived)} survived, {len(broken)} did not compile, "
          f"{len(noop)} were no-ops")
    for label, group in (("survivors", survived),
                         ("did not compile", broken),
                         ("no-ops", noop)):
        if group:
            print(f"{label}:")
            for name in group:
                print(f"  - {name}")
    return 1 if (survived or broken or noop) else 0


if __name__ == "__main__":
    raise SystemExit(main())
