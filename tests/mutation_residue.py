#!/usr/bin/env python3
"""Detects source files left mutated by a mutation harness that did not finish.

Every `tests/mutate_*.py` harness edits a real source file in place, runs the
suite, and restores it in a `finally`. A `finally` does not run when the
process is killed -- a CI timeout, a cancelled command, SIGKILL -- and the tree
is then left holding a deliberately broken line.

That alone would be recoverable. What made it cost hours is the second step:
the harness reads the file it is about to mutate as its *baseline* and copies
it to `<source>.mutation-backup`. Run it again on a tree that is already
mutated and it adopts the mutation as the baseline, overwrites the one good
backup with a bad one, and from then on nothing on disk remembers the original.

The residue from one such kill was a single line in `src/api/impls.cc`:

    mounts["/mnt/restored"] = child.value().id;   // the mutation
    mounts[mount_path.value()] = child.value().id;  // the original

which restores every captured volume onto one hard-coded mount point. It
survived a full `cmake --build` without a warning -- `mount_path` is still
used, just only for validation -- and the only test that reads the mount map
back was the one suite slow enough that I ran it last. Between the kill and
finding it, every build and test result from that tree was worthless, including
a clean `ctest` run that had just been used as a baseline.

So the leftover backup is treated as the signal it is: its presence means the
tree's state is unknown and no result from it can be trusted.

    python3 tests/mutation_residue.py            # report, exit 1 if residue
    python3 tests/mutation_residue.py --restore  # restore from the backups

Harnesses call `refuse_if_residue()` before taking their own baseline, so a
second run stops instead of laundering the mutation into its backup.
"""

import pathlib
import shutil
import signal
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SUFFIX = ".mutation-backup"

# Where harnesses mutate. `build/` is excluded because a backup copied in there
# is a build artifact, not a source the tree is read from.
SEARCH_ROOTS = ("src", "include", "tests", "storage", "crates")


def backups():
    """Every leftover backup in the tree, sorted for a stable report."""
    found = []
    for name in SEARCH_ROOTS:
        base = ROOT / name
        if base.is_dir():
            found.extend(base.rglob("*" + SUFFIX))
    return sorted(found)


def source_of(backup):
    """The file a backup was taken from: `impls.cc.mutation-backup` -> `impls.cc`."""
    return backup.with_name(backup.name[: -len(SUFFIX)])


def report(found):
    print(f"{len(found)} mutation backup(s) left behind -- a harness was killed "
          f"before it could restore:")
    for backup in found:
        source = source_of(backup)
        rel = source.relative_to(ROOT)
        if not source.exists():
            print(f"  {rel}  (MISSING -- restore it from the backup)")
            continue
        left = backup.read_text().splitlines()
        right = source.read_text().splitlines()
        differing = [
            i for i in range(max(len(left), len(right)))
            if (left[i] if i < len(left) else None)
            != (right[i] if i < len(right) else None)
        ]
        if not differing:
            print(f"  {rel}  (identical to its backup -- safe, just stale)")
            continue
        print(f"  {rel}  differs from its backup on "
              f"{len(differing)} line(s):")
        for i in differing[:5]:
            print(f"    backup:{i + 1}  {left[i] if i < len(left) else '<absent>'}")
            print(f"    source:{i + 1}  {right[i] if i < len(right) else '<absent>'}")
        if len(differing) > 5:
            print(f"    ... and {len(differing) - 5} more")

    print("\nThe backup is NOT automatically the original: a harness run on an\n"
          "already-mutated tree copies the mutation into it. Check the diff\n"
          "above against `git diff` before trusting either side.")


def refuse_if_residue():
    """Called by a harness before it takes its own baseline.

    Raises rather than returning a code, so a harness cannot carry on by
    ignoring the result.
    """
    found = backups()
    if not found:
        return
    report(found)
    raise SystemExit(
        "\nrefusing to run: this tree may still hold a mutation, and taking a "
        "baseline now would copy it into the backup and lose the original. "
        "Resolve the files above (`git checkout --` or "
        "`python3 tests/mutation_residue.py --restore`), then re-run."
    )


def restore_on_signal():
    """Turns SIGTERM/SIGINT into an exception so a harness `finally` still runs.

    `timeout`, a cancelled command and Ctrl-C are how these runs actually end:
    a full mutation pass rebuilds and re-runs the suite once per mutation, so
    it outlives most command timeouts. Python's default SIGTERM handler exits
    without unwinding, which skips the `finally` that restores the source --
    the exact sequence that left a mutated `impls.cc` in the tree. Raising
    instead makes termination take the same path as an error.

    SIGKILL still cannot be caught, which is why the backup check exists as
    well: this reduces how often residue happens, it does not prevent it.
    """
    def handler(signum, _frame):
        raise KeyboardInterrupt(f"terminated by signal {signum}")

    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        try:
            signal.signal(sig, handler)
        except (ValueError, OSError):
            # Not the main thread, or the platform lacks the signal; the
            # backup check still covers this case.
            pass


def main(argv):
    found = backups()
    if not found:
        print("no mutation backups left behind")
        return 0

    report(found)
    if "--restore" not in argv:
        return 1

    print("\n--restore given: copying each backup over its source")
    for backup in found:
        source = source_of(backup)
        shutil.copy(backup, source)
        backup.unlink()
        print(f"  restored {source.relative_to(ROOT)}")
    print("Rebuild before trusting any test result from this tree.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
