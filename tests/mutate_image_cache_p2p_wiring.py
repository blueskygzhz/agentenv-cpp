#!/usr/bin/env python3
"""Mutation harness for `InitializeImageCacheP2pTransport`.

Status is decided **by exit code only**: the suite calls `exit(1)` on the
first failing assertion, so its summary line is not printed on failure and
scraping stdout for "failed" would misreport a caught mutation as a survivor.
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
SRC = ROOT / "src/image/cache_service.cc"
TARGET = "test_image_cache_service"

MUTATIONS = [
    # The whole bug this hook fixes: the transport is never handed over.
    (
        "transport_never_handed_over",
        "    cache::ImageCacheService::SharedFromAppConfig(config)->InitializeP2pTransport(\n"
        "        std::move(transport));",
        "    (void)config;",
    ),
    # Initialises a *fresh* service instead of the shared one, so the
    # production lookups still see no transport. This is the subtle failure
    # the test exists for -- the call happens, it just lands nowhere.
    (
        "initialises_an_unshared_service",
        "    cache::ImageCacheService::SharedFromAppConfig(config)->InitializeP2pTransport(\n"
        "        std::move(transport));",
        "    cache::ImageCacheService::FromResolvedConfig(config.ImageCacheLayout())\n"
        "        ->InitializeP2pTransport(std::move(transport));",
    ),
    # No mutation for a null-transport guard: there is none, deliberately.
    # One was written, and the mutation that deleted it SURVIVED -- correctly,
    # because `InitializeP2pTransport` tests the stored pointer's truthiness
    # rather than whether it was ever assigned, so storing a null is
    # indistinguishable from storing nothing. The guard changed no observable
    # behaviour and the test written for it asserted a difference that did not
    # exist, so both were removed instead of being kept with a mutation that
    # proves nothing. `p2p::TransportFromConfig` never yields a null anyway.
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

    original = SRC.read_text()
    backup = SRC.with_suffix(".cc.mutation-backup")
    shutil.copy(SRC, backup)

    if not build():
        print("BASELINE BUILD FAILED")
        return 2
    if run() != 0:
        print("BASELINE TESTS FAILED")
        return 2
    print("baseline: build ok, tests pass\n")

    caught, survived, broken = [], [], []
    try:
        for name, old, new in MUTATIONS:
            assert original.count(old) == 1, \
                f"anchor for {name} matched {original.count(old)} times"
            SRC.write_text(original.replace(old, new, 1))

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
        shutil.copy(backup, SRC)
        backup.unlink()
        build()

    print(f"\n{len(caught)}/{len(MUTATIONS)} caught, "
          f"{len(survived)} survived, {len(broken)} did not compile")
    for label, group in (("survivors", survived), ("did not compile", broken)):
        if group:
            print(f"{label}:")
            for name in group:
                print(f"  - {name}")
    return 1 if (survived or broken) else 0


if __name__ == "__main__":
    raise SystemExit(main())
