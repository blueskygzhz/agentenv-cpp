#!/usr/bin/env python3
"""Mutation harness for `restore_snapshot_volume_mounts` and its wiring.

The gap this closes is the kind that does not look like a bug from outside. A
captured snapshot records one `SnapshotVolume` per volume the sandbox was
holding; resuming it is supposed to rebuild each one and mount it back at the
path it was captured on. Before this was ported the resume **succeeded with no
volumes at all** -- a 201, a running sandbox, and every path the guest had
mounted now empty. A client cannot tell that from a capture that never had
volumes, and the guest sees directories rather than an error.

So almost every mutation below produces a *successful* create. That is what
makes the restore worth a harness: there is no status code to assert on, and
the only observable difference is in the volume catalog and the recorded mount
map. The tests read both back rather than the response.

Three groups:

- The restore itself: whether it runs, what it creates, and whether each
  captured volume's own mode/size/path/layers reach the new record. Getting
  any of these wrong still boots a sandbox.
- The unwind. A restored volume was created *by this request* and nothing else
  refers to it, so every failure downstream has to **delete** it -- where the
  caller's own volumes are only **released**. Confusing the two is a leak in
  one direction and data loss in the other, and both are silent.
- `extra_drives_in_snapshot`. It tells the launch the drives were bound before
  the snapshot was loaded, which is true for a restore and false for a
  request-supplied mount. Set wrongly, the launch rebinds drive slots the
  capture was already using.

Status is decided **by exit code only**: the suite calls `exit(1)` on the first
failing assertion, so its summary line is not printed on failure and scraping
stdout would misreport a caught mutation as a survivor.
"""

import pathlib
import shutil
import subprocess
import sys

# `tests/` is not a package and a harness runs as a script from the repo
# root, so the shared guard is imported by path rather than by package.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import mutation_residue


ROOT = pathlib.Path(__file__).resolve().parent.parent
IMPLS = ROOT / "src/api/impls.cc"
TARGET = "test_api_sandbox_volume_mounts"

MUTATIONS = [
    # ---- whether the restore runs at all -----------------------------------
    #
    # The original bug, restored exactly: the branch is skipped, the mount map
    # stays empty, and the sandbox launches without its filesystems.
    (
        "the_restore_never_runs",
        """    if (!replaces_mounts) {
        const core::Expected<
            std::pair<std::map<std::string, std::string>, std::vector<std::string> >, ApiError>
            restored = RestoreSnapshotVolumeMounts(volumes_.get(), committed);""",
        """    if (false) {
        const core::Expected<
            std::pair<std::map<std::string, std::string>, std::vector<std::string> >, ApiError>
            restored = RestoreSnapshotVolumeMounts(volumes_.get(), committed);""",
    ),
    # It runs but its result is dropped, which is the same empty launch by a
    # different route -- and this one still creates the volumes, so it leaks
    # them on every single resume.
    (
        "the_restored_mounts_are_discarded",
        """        effective_mounts   = restored.value().first;""",
        """        effective_mounts   = std::map<std::string, std::string>();""",
    ),
    # Keyed on emptiness instead of presence. A present-but-empty
    # `volumeMounts` is an explicit "no volumes", so this would restore them
    # for a request that said not to -- except such a request is already
    # refused, which is what makes the distinction observable only through the
    # 400.
    (
        "the_branch_keys_on_emptiness_rather_than_presence",
        """    const bool replaces_mounts = req.volume_mounts.has_value();""",
        """    const bool replaces_mounts =
        req.volume_mounts.has_value() && !req.volume_mounts->empty();""",
    ),
    # The restore is reached even when the request supplied its own mounts, so
    # the caller's volumes are replaced by the capture's.
    (
        "an_explicit_mount_list_is_overridden_by_the_restore",
        """    if (!replaces_mounts) {
        const core::Expected<""",
        """    if (true) {
        const core::Expected<""",
    ),
    # A capture with no volumes is refused for want of a manager. Upstream
    # returns early on the empty list, so a node with no volume manager can
    # still resume a snapshot that never had any.
    (
        "a_volumeless_capture_needs_a_volume_manager",
        """    if (committed.volume_snapshots.empty()) {
        return std::make_pair(mounts, volume_ids);
    }""",
        """    // empty check removed""",
    ),
    # No manager is treated as "nothing to restore" instead of refused, which
    # is the 201-with-no-disks answer again.
    (
        "a_missing_volume_manager_silently_restores_nothing",
        """    if (manager == NULL) {
        return core::make_unexpected(ApiError::Make(
            501, "restoring a snapshot's volume mounts requires a volume manager"));
    }""",
        """    if (manager == NULL) return std::make_pair(mounts, volume_ids);""",
    ),

    # ---- what the restore creates ------------------------------------------
    #
    # Each captured field is swapped for a plausible-looking wrong one. None of
    # these fails the launch.
    #
    # Mode: a read-only volume restored as exclusive takes a write lease, which
    # refuses the next concurrent resume of the same snapshot.
    (
        "every_restored_volume_is_exclusive",
        """            name, captured.mode, captured.size_mb, captured.layers);""",
        """            name, volume::VolumeMode::Exclusive, captured.size_mb, captured.layers);""",
    ),
    # Size: the record's `size_mb` is what the guest's filesystem is grown to.
    (
        "the_restored_size_comes_from_a_default",
        """            name, captured.mode, captured.size_mb, captured.layers);""",
        """            name, captured.mode, volume::kDefaultVolumeSizeMb, captured.layers);""",
    ),
    # Layers: an empty backing is refused by `CreateFromSnapshot`, so this one
    # *is* visible -- but as a failure on a request that should have worked.
    (
        "the_captured_layers_are_not_carried_over",
        """            name, captured.mode, captured.size_mb, captured.layers);""",
        """            name, captured.mode, captured.size_mb,
            std::vector<snapshot::OverlaybdLayerRef>());""",
    ),
    # Every captured volume lands on one mount path, so a capture with two
    # volumes comes back with one and the second silently replaces the first.
    (
        "all_restored_volumes_share_one_mount_path",
        """        mounts[mount_path.value()] = child.value().id;""",
        """        mounts["/mnt/restored"] = child.value().id;""",
    ),
    # Only the first captured volume is restored.
    (
        "the_restore_stops_after_the_first_volume",
        """        volume_ids.push_back(child.value().id);
        mounts[mount_path.value()] = child.value().id;
    }
    return std::make_pair(mounts, volume_ids);""",
        """        volume_ids.push_back(child.value().id);
        mounts[mount_path.value()] = child.value().id;
        break;
    }
    return std::make_pair(mounts, volume_ids);""",
    ),
    # A fixed name, so the second volume of a capture -- and the second resume
    # of one snapshot -- collides on `NameConflict`.
    (
        "restored_volumes_share_a_fixed_name",
        """        const std::string name = "volume-restore-" + core::Uuid::GenV7().ToSimpleString();""",
        """        const std::string name = "volume-restore";""",
    ),
    # The captured mount path is trusted rather than renormalised. The record
    # is durable and may come from another node or an older build, and this
    # path becomes a guest mount point.
    (
        "the_captured_mount_path_is_trusted",
        """        const core::Expected<std::string, std::string> mount_path =
            sandbox::NormalizeMountPath(captured.mount_path);
        if (!mount_path.ok()) {
            CleanupVolumeIds(manager, volume_ids);
            return core::make_unexpected(ApiError::Make(400, mount_path.error()));
        }""",
        """        const core::Expected<std::string, std::string> mount_path = captured.mount_path;""",
    ),

    # ---- the unwind --------------------------------------------------------
    #
    # A restored volume is this request's own. Every failure below has to
    # delete it, and every one of these leaks a full copy of the capture's
    # volumes per attempt.
    (
        "a_partly_failed_restore_keeps_what_it_made",
        """        if (!child.ok()) {
            // Unwound before returning, so a snapshot whose third volume
            // cannot be restored does not leave the first two behind.
            CleanupVolumeIds(manager, volume_ids);""",
        """        if (!child.ok()) {""",
    ),
    (
        "a_bad_mount_path_keeps_the_volumes_already_restored",
        """            CleanupVolumeIds(manager, volume_ids);
            return core::make_unexpected(ApiError::Make(400, mount_path.error()));""",
        """            return core::make_unexpected(ApiError::Make(400, mount_path.error()));""",
    ),
    (
        "a_failed_lease_keeps_the_restored_volumes",
        """        CleanupVolumeIds(volumes_.get(), restored_volume_ids);
        return core::make_unexpected(prepared.error());""",
        """        return core::make_unexpected(prepared.error());""",
    ),
    (
        "a_failed_launch_keeps_the_restored_volumes",
        """        // The restored ones are the other case: this request made them, so a
        // launch that never happened leaves nothing that should keep them.
        CleanupVolumeIds(volumes, restored_volume_ids);
        return core::make_unexpected(ToApiError(r.error()));""",
        """        return core::make_unexpected(ToApiError(r.error()));""",
    ),
    (
        "a_failed_handover_keeps_the_restored_volumes",
        """        // After the teardown, not before: until the sandbox is gone it is
        // still mounting these, and deleting a volume out from under a running
        // guest is the failure this ordering avoids.
        CleanupVolumeIds(volumes, restored_volume_ids);
        return core::make_unexpected(ApiError::Make(500, message));""",
        """        return core::make_unexpected(ApiError::Make(500, message));""",
    ),
    # The ids never reach `FinishCreate`, so both of its cleanups become no-ops
    # while still looking present at the call site.
    (
        "the_restored_ids_are_not_threaded_into_the_launch",
        """    return FinishCreate(svc_.get(), volumes_.get(), request, prepared.value(),
                        restored_volume_ids);""",
        """    return FinishCreate(svc_.get(), volumes_.get(), request, prepared.value());""",
    ),
    # The cleanup releases instead of deleting -- the exact confusion between
    # the two volume kinds that this code exists to keep straight. The records
    # survive, unleased, forever.
    (
        "the_cleanup_releases_instead_of_deleting",
        """        const core::Expected<core::Unit, volume::VolumeError> removed =
            manager->Remove(volume_ids[i]);""",
        """        const core::Expected<core::Unit, volume::VolumeError> removed =
            manager->ReplaceOwnerFor(std::string(), core::Optional<std::string>(),
                                     std::vector<std::string>(1, volume_ids[i]));""",
    ),
    (
        "the_cleanup_does_nothing",
        """    for (std::size_t i = 0; i < volume_ids.size(); ++i) {
        const core::Expected<core::Unit, volume::VolumeError> removed =
            manager->Remove(volume_ids[i]);""",
        """    for (std::size_t i = 0; i < 0; ++i) {
        const core::Expected<core::Unit, volume::VolumeError> removed =
            manager->Remove(volume_ids[i]);""",
    ),

    # ---- extra_drives_in_snapshot ------------------------------------------
    #
    # Describes how the launch binds `extra_drives`. Set wrongly in either
    # direction the sandbox still starts, and the drives land on the wrong
    # slots.
    (
        "the_snapshot_drive_flag_is_never_set",
        """    const bool extra_drives_in_snapshot =
        !replaces_mounts && !committed.volume_snapshots.empty();""",
        """    const bool extra_drives_in_snapshot = false;""",
    ),
    (
        "the_snapshot_drive_flag_is_always_set",
        """    const bool extra_drives_in_snapshot =
        !replaces_mounts && !committed.volume_snapshots.empty();""",
        """    const bool extra_drives_in_snapshot = true;""",
    ),
    # Set for every resume, including one whose capture had no volumes -- so an
    # ordinary resume stops binding the drives it was given.
    (
        "the_snapshot_drive_flag_ignores_whether_the_capture_had_volumes",
        """        !replaces_mounts && !committed.volume_snapshots.empty();""",
        """        !replaces_mounts;""",
    ),
    # Dropping the `!replaces_mounts` half -- leaving `!volume_snapshots
    # .empty()` -- is **not** listed, and deliberately so. The two expressions
    # can only disagree when the request supplies its own mounts *and* the
    # capture recorded some, and that combination is already a 400 from
    # `check_snapshot_volume_replacement` thirty lines earlier, so the flag is
    # never read for it. The mutation is unobservable rather than untested, and
    # it survived the first run of this harness for that reason.
    #
    # The conjunct stays in the source anyway: it is upstream's expression
    # (`body.volume_mounts.is_none() && !volume_snapshots.is_empty()`), and it
    # is what keeps the flag correct on its own terms rather than by relying on
    # a check somewhere else to have run first.
    (
        "the_snapshot_drive_flag_is_not_passed_to_the_launch",
        """    request.extra_drives_in_snapshot = extra_drives_in_snapshot;""",
        """    // flag not forwarded""",
    ),

    # ---- the check that makes the branch unambiguous -----------------------
    #
    # Without it, a request naming mounts for a capture that recorded some
    # would put the guest's own filesystems and the requested ones on the same
    # slots -- and the restore branch would need a guard it does not have.
    (
        "a_capture_with_volumes_accepts_an_explicit_mount_list",
        """    if (replaces_mounts && !committed.volume_snapshots.empty()) {""",
        """    if (false) {""",
    ),
]


def build():
    return subprocess.run(
        ["cmake", "--build", "build", "-j", "8", "--target", TARGET],
        cwd=ROOT, capture_output=True, text=True,
    ).returncode == 0


def run():
    return subprocess.run([str(ROOT / "build/bin" / TARGET)],
                          cwd=ROOT, capture_output=True, text=True).returncode


def main():
    # Refuses if a previous run was killed mid-mutation: taking a baseline
    # from a tree that still holds one would copy the mutation into the
    # backup and lose the original. See tests/mutation_residue.py.
    mutation_residue.refuse_if_residue()
    # A full pass rebuilds once per mutation and routinely outlives a
    # command timeout; without this, SIGTERM skips the restore below.
    mutation_residue.restore_on_signal()

    sources = {IMPLS}
    originals = {p: p.read_text() for p in sources}
    backups = {p: p.with_suffix(p.suffix + ".mutation-backup") for p in sources}
    for p, b in backups.items():
        shutil.copy(p, b)

    if not build():
        print("BASELINE BUILD FAILED")
        return 2
    if run() != 0:
        print("BASELINE TESTS FAILED")
        return 2
    print("baseline: build ok, tests pass\n")

    caught, survived, broken, noop = [], [], [], []
    try:
        for name, old, new in MUTATIONS:
            for p in sources:
                p.write_text(originals[p])
            text = IMPLS.read_text()
            assert text.count(old) == 1, \
                f"anchor for {name} matched {text.count(old)} times"
            if old == new:
                noop.append(name)
                print(f"  NO-OP    {name} (mutation is identity)")
                continue
            IMPLS.write_text(text.replace(old, new, 1))

            if not build():
                broken.append(name)
                print(f"  BROKEN   {name} (did not compile)")
                continue
            if run() != 0:
                caught.append(name)
                print(f"  caught   {name}")
            else:
                survived.append(name)
                print(f"  SURVIVED {name}")
    finally:
        for p, b in backups.items():
            shutil.copy(b, p)
            b.unlink()
        build()

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
