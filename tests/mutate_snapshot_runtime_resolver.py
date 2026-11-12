#!/usr/bin/env python3
"""Mutation harness for the snapshot runtime resolver seam.

The resolver used to be `PosixFsRuntimeResolver`, built from a filesystem root
path, so it could only serve a backend whose bytes are reachable by path --
`oss` had no resolver at all and resume answered 501 there. It is now
`RepositoryRuntimeResolver` over the `SnapshotRepository` interface, and the
mutations split along that seam: some break resolving in general, and the last
group breaks it only for a repository this node cannot mount, which is the case
that had no implementation and therefore no test.

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
RESOLVER = ROOT / "src/snapshot/resolver.cc"
SUPPORT = ROOT / "src/snapshot/runtime_support.cc"
# Managed layers are bound in place here, shared with the volume side. The
# resolver had its own posix-only copy of this until it was generalised.
INTERFACES = ROOT / "src/snapshot/repository/interfaces.cc"
TARGET = "test_snapshot_runtime_resolver"

NOT_READY = """    if (!snapshot.committed.has_value()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("snapshot '") + snapshot.id.ToString() + "' is not ready"));
    }
"""

# The `Local` arm of `LocateBlob`: the bytes are addressable at `path`, so a
# miss there means corruption rather than "ask someone else".
VM_STATE_GUARD = """        if (!core::fs::Exists(path)) {
            return core::make_unexpected(
                RepositoryError::ArtifactNotFound(std::string("vm state at ") + path));
        }
"""

# Binding a managed layer to its in-repository file. Dropping it leaves
# `config.file` empty, which is what "read the layers in place" comes down to.
MANAGED_IN_PLACE = (
    "    if (location.value().kind == BlobLocation::Kind::Local)"
    " config.file = location.value().value;\n"
)

# The fallback that downloads a blob this node cannot read in place. Upstream
# calls it at resolver.rs:198; before this seam was generalised, nothing in
# this tree called `FetchBlob` outside of tests.
FETCH_BLOB_FALLBACK = """            const RepositoryResult<uint64_t> copied = repository->FetchBlob(*key, dest);
            if (!copied.ok()) return core::make_unexpected(copied.error().ToString());
            return copied.value();
"""

# The whole `Local` arm. The fallback mutation below replaces it so that a
# missing in-place file drops through to the `Remote` fetch path. An earlier
# version rewrote only the final `return path;`, which is unreachable when the
# file is missing because the guard above has already returned -- an
# equivalent mutant that "survived" without proving anything about the tests.
LOCAL_ARM = """        if (!core::fs::Exists(path)) {
            return core::make_unexpected(
                RepositoryError::ArtifactNotFound(std::string("vm state at ") + path));
        }
        return path;
    }
"""

# Carries the typed cause out of the cache's string-only fetch contract so
# `MaterializeImageConfigError` can still lift an `ArtifactNotFound`.
TYPED_CAUSE = """                *cause = core::Optional<RepositoryError>(written.error());
"""

MUTATIONS = [
    # An uncommitted record resolves, producing a manifest that names
    # artifacts no build has written.
    ("uncommitted_record_resolves", RESOLVER, NOT_READY, ""),
    # A missing capture is no longer distinguished from a healthy one, so the
    # launch fails later with no indication of which node is short of what.
    ("missing_vm_state_not_detected", RESOLVER, VM_STATE_GUARD, ""),
    # The hydrated manifest keeps the *committed* paths instead of the
    # node-local materialised ones, which is the whole job of resolving.
    (
        "manifest_not_hydrated_with_runtime_paths",
        SUPPORT,
        "    manifest.vm_state.path          = vm_state_path;\n"
        "    manifest.memory.image_config_path = memory_image_config_path;\n"
        "    manifest.rootfs.image_config_path = rootfs_image_config_path;\n",
        "",
    ),
    # The lease is dropped, so the runtime cache may reclaim the configs the
    # returned manifest points at while a launch is still starting.
    (
        "lease_not_carried",
        RESOLVER,
        "    runnable.lease    = lease;\n",
        "",
    ),
    # The runtime image path stops depending on the snapshot id, so two
    # snapshots would share one materialised config and the second resolve
    # would overwrite the first.
    (
        "runtime_config_path_ignores_the_snapshot_id",
        SUPPORT,
        "std::string RuntimeImageMaterializer::SnapshotDir(const core::SnapshotId& id) const {\n"
        "    return runtime_root_ + \"/\" + id.ToString();\n",
        "std::string RuntimeImageMaterializer::SnapshotDir(const core::SnapshotId& id) const {\n"
        "    (void)id;\n    return runtime_root_ + \"/shared\";\n",
    ),

    # ---- a repository this node cannot mount -------------------------------
    #
    # These four are the half that had no implementation to mutate while the
    # resolver was posix-only. Each leaves a shared-filesystem node working
    # perfectly, which is why none of them would have been noticed.
    #
    # The download is dropped, so a `Remote` blob resolves to a cache entry
    # nothing ever wrote. On a shared filesystem every blob is `Local` and
    # this branch is dead, so the posix tests stay green.
    ("remote_vm_state_is_never_fetched", RESOLVER, FETCH_BLOB_FALLBACK,
     "            (void)key;\n            (void)dest;\n            return 0;\n"),
    # `Local` stops being read in place and falls through to the fetch path,
    # which is the behaviour the comment on that branch exists to forbid: a
    # misconfigured mount would be masked by any peer holding a copy.
    ("a_missing_local_blob_falls_back_to_fetching", RESOLVER, LOCAL_ARM,
     "        if (core::fs::Exists(path)) return path;\n    }\n"),
    # The typed cause is dropped on the way out of the artifact cache, so a
    # layer the repository lacks is reported as a generic backend error and
    # `MaterializeImageConfigError` has nothing to lift.
    ("artifact_not_found_is_flattened_by_the_cache", RESOLVER, TYPED_CAUSE, ""),
    # Managed layers stop being bound to their in-repository file, so the
    # generated `image.json` addresses every layer by URL even on a backend
    # whose bytes are right there on disk.
    ("managed_layer_not_bound_in_place", INTERFACES, MANAGED_IN_PLACE, ""),
]


def build():
    return subprocess.run(
        ["cmake", "--build", "build", "-j", "8", "--target", TARGET],
        cwd=ROOT, capture_output=True, text=True,
    ).returncode == 0


def run():
    # A mutation can turn a failure into a hang -- `remote_vm_state_is_never_fetched`
    # did, through a retry loop in the artifact cache that has since been
    # fixed. A hang is a detected failure, so it counts as caught rather than
    # stalling every mutation after it. The suite itself takes seconds.
    try:
        return subprocess.run([str(ROOT / "build/bin" / TARGET)],
                              cwd=ROOT, capture_output=True, text=True,
                              timeout=120).returncode
    except subprocess.TimeoutExpired:
        return 124


def main():
    # Refuses if a previous run was killed mid-mutation: taking a baseline
    # from a tree that still holds one would copy the mutation into the
    # backup and lose the original. See tests/mutation_residue.py.
    mutation_residue.refuse_if_residue()
    # A full pass rebuilds once per mutation and routinely outlives a
    # command timeout; without this, SIGTERM skips the restore below.
    mutation_residue.restore_on_signal()

    sources = {RESOLVER, SUPPORT, INTERFACES}
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

    caught, survived, broken = [], [], []
    try:
        for name, path, old, new in MUTATIONS:
            for p in sources:
                p.write_text(originals[p])
            text = path.read_text()
            assert text.count(old) == 1, \
                f"anchor for {name} matched {text.count(old)} times in {path.name}"
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
          f"{len(survived)} survived, {len(broken)} did not compile")
    for label, group in (("survivors", survived), ("did not compile", broken)):
        if group:
            print(f"{label}:")
            for name in group:
                print(f"  - {name}")
    return 1 if (survived or broken) else 0


if __name__ == "__main__":
    raise SystemExit(main())
