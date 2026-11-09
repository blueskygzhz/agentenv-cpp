#!/usr/bin/env python3
"""Mutation harness for the writable-persistent-volume capture guards.

Four doors lead out of a running sandbox that holds an exclusive volume:
`pause`, `fork`, `snapshot_volumes` and `freeze_and_snapshot_volumes`. Upstream
flushes the volume through envd and seals its upper into a new overlaybd commit
layer behind every one of them. This port can do neither, so all four must
refuse -- and `pause` and `fork` did not, which is the whole reason this file
exists.

What makes the `pause` hole worth a harness of its own is that nothing *after*
it is wrong. A paused sandbox's delete skips `freeze_and_snapshot_volumes`
because `previous_state != Running`, exactly as upstream does; publishing then
reads the volume's `image.json` lowers, exactly as upstream does, and that is
correct there only because the seal has already folded the run's upper into
them. Remove the seal and every remaining link faithfully carries the loss
through to `Stop` deleting `extra-drive-runtime-<id>/upper.data`. A mutation
that re-opens any one door therefore produces no error anywhere -- it produces
a 200, a Paused record, and a volume that quietly forgot the session.

Status is decided **by exit code only**: the suite calls `exit(1)` on the first
failing assertion, so its summary line is not printed on failure and scraping
stdout would misreport a caught mutation as a survivor.
"""

import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
BACKEND = ROOT / "src/sandbox/firecracker/sandbox_backend.cc"
TARGET = "test_firecracker_sandbox_backend"

# The predicate every guard selects on. Both halves are load-bearing: a
# read-only volume has nothing to flush, and a writable drive with no output
# dir is an ordinary attached drive whose contents live in the snapshot.
PREDICATE = """    bool HasWritablePersistentVolumes() const {
        const std::vector<ExtraDrive>& drives = ExtraDrives();
        for (std::size_t i = 0; i < drives.size(); ++i) {
            if (!drives[i].read_only && drives[i].snapshot_output_dir.has_value()) return true;
        }
        return false;
    }"""

# Anchored on the comment above each guard, because the four call sites are
# otherwise near-identical `if (impl_->HasWritablePersistentVolumes())` lines
# and an ambiguous anchor would silently mutate the wrong door.
PAUSE_GUARD = """    if (impl_->HasWritablePersistentVolumes()) {
        return core::make_unexpected(SandboxCaptureError::Recoverable(
            "pause: this sandbox has writable persistent volumes, which must be flushed """

FORK_GUARD = """    if (impl_->HasWritablePersistentVolumes()) {
        return core::make_unexpected(SandboxCaptureError::Recoverable(
            "fork: this sandbox has writable persistent volumes, which must be flushed """

MUTATIONS = [
    # ---- the guards themselves ---------------------------------------------
    #
    # Each of these restores the exact code that was there before: a pause or a
    # fork that forwards straight to the VMM. Nothing fails; the writes are
    # just gone a few steps later.
    (
        "pause_no_longer_refuses_writable_volumes",
        BACKEND,
        PAUSE_GUARD,
        """    if (false) {
        return core::make_unexpected(SandboxCaptureError::Recoverable(
            "pause: this sandbox has writable persistent volumes, which must be flushed """,
    ),
    (
        "fork_no_longer_refuses_writable_volumes",
        BACKEND,
        FORK_GUARD,
        """    if (false) {
        return core::make_unexpected(SandboxCaptureError::Recoverable(
            "fork: this sandbox has writable persistent volumes, which must be flushed """,
    ),
    # The guard runs, but only after the VMM has been told to pause. The
    # refusal is still reported, so a status-only assertion would pass -- and
    # the caller would be handed a `Recoverable` error while the guest sits
    # frozen, which is the one thing `Recoverable` promises is not the case.
    (
        "pause_refuses_only_after_freezing_the_guest",
        BACKEND,
        PAUSE_GUARD,
        """    impl_->instance->Pause();
    if (impl_->HasWritablePersistentVolumes()) {
        return core::make_unexpected(SandboxCaptureError::Recoverable(
            "pause: this sandbox has writable persistent volumes, which must be flushed """,
    ),
    # Same for fork: the parent is frozen before the refusal, and the error
    # says it is still serving.
    (
        "fork_refuses_only_after_freezing_the_source",
        BACKEND,
        FORK_GUARD,
        """    impl_->instance->Pause();
    if (impl_->HasWritablePersistentVolumes()) {
        return core::make_unexpected(SandboxCaptureError::Recoverable(
            "fork: this sandbox has writable persistent volumes, which must be flushed """,
    ),
    # Terminal instead of recoverable. Nothing was touched, so this condemns a
    # sandbox that is still serving perfectly well -- the orchestrator stops it
    # and fails its volumes rather than putting it back.
    (
        "pause_refusal_is_reported_as_terminal",
        BACKEND,
        PAUSE_GUARD,
        """    if (impl_->HasWritablePersistentVolumes()) {
        return core::make_unexpected(SandboxCaptureError::Terminal(
            "pause: this sandbox has writable persistent volumes, which must be flushed """,
    ),
    # ---- the predicate the guards share ------------------------------------
    #
    # Every door selects on this, so breaking it re-opens all four at once.
    (
        "every_drive_counts_as_a_writable_volume",
        BACKEND,
        PREDICATE,
        """    bool HasWritablePersistentVolumes() const { return !ExtraDrives().empty(); }""",
    ),
    (
        "no_drive_counts_as_a_writable_volume",
        BACKEND,
        PREDICATE,
        """    bool HasWritablePersistentVolumes() const { return false; }""",
    ),
    # Read-only volumes get pulled in, so a sandbox with nothing to flush is
    # refused a capture upstream completes.
    (
        "read_only_volumes_are_treated_as_writable",
        BACKEND,
        "            if (!drives[i].read_only && drives[i].snapshot_output_dir.has_value()) return true;",
        "            if (drives[i].snapshot_output_dir.has_value()) return true;",
    ),
    # Plain writable attached drives get pulled in. Their contents live in the
    # snapshot, not in a volume, so there is nothing to seal.
    (
        "plain_writable_drives_are_treated_as_volumes",
        BACKEND,
        "            if (!drives[i].read_only && drives[i].snapshot_output_dir.has_value()) return true;",
        "            if (!drives[i].read_only) return true;",
    ),
    # The accessor the predicate reads. A backend built over an already-booted
    # instance -- the form `Pause` hands back -- keeps its drives only in the
    # capture descriptor, so losing that fallback reports "no drives" for a
    # sandbox that is holding writable volumes.
    (
        "handed_in_instances_read_their_drives_from_the_resume_spec",
        BACKEND,
        "        if (spec.drives.has_value()) return spec.drives->extra_drives;\n"
        "        return snapshot_config.common.extra_drives;",
        "        return resume_spec.snapshot_config.common.extra_drives;",
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
    sources = {BACKEND}
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
        for name, path, old, new in MUTATIONS:
            for p in sources:
                p.write_text(originals[p])
            text = path.read_text()
            assert text.count(old) == 1, \
                f"anchor for {name} matched {text.count(old)} times in {path.name}"
            if old == new:
                noop.append(name)
                print(f"  NO-OP    {name} (mutation is identity)")
                continue
            path.write_text(text.replace(old, new, 1))

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
