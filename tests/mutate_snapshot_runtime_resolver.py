#!/usr/bin/env python3
"""Mutation harness for the snapshot runtime resolver seam.

Status is decided **by exit code only**: the suite calls `exit(1)` on the
first failing assertion, so its summary line is not printed on failure and
scraping stdout for "failed" would misreport a caught mutation as a survivor.
"""

import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
RUNTIME = ROOT / "src/snapshot/repository/posixfs/runtime.cc"
SUPPORT = ROOT / "src/snapshot/runtime_support.cc"
TARGET = "test_snapshot_runtime_resolver"

NOT_READY = """    if (!snapshot.committed.has_value()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("snapshot '") + snapshot.id.ToString() + "' is not ready"));
    }
"""

VM_STATE_GUARD = """    if (!core::fs::Exists(vm_state_path)) {
        return core::make_unexpected(
            RepositoryError::ArtifactNotFound(std::string("vm state at ") + vm_state_path));
    }
"""

MANAGED_IN_PLACE = """    storage::overlaybd::LayerConfig config;
    config.file   = path;
"""

MUTATIONS = [
    # An uncommitted record resolves, producing a manifest that names
    # artifacts no build has written.
    ("uncommitted_record_resolves", RUNTIME, NOT_READY, ""),
    # A missing capture is no longer distinguished from a healthy one, so the
    # launch fails later with no indication of which node is short of what.
    ("missing_vm_state_not_detected", RUNTIME, VM_STATE_GUARD, ""),
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
        RUNTIME,
        "    runnable.lease    = lease;\n",
        "",
    ),
    # Managed layers stop being bound in place, so the produced `image.json`
    # no longer points into the repository -- this is what "files are read in
    # place" means on a POSIX backend.
    (
        "managed_layer_not_bound_in_place",
        RUNTIME,
        MANAGED_IN_PLACE,
        "    storage::overlaybd::LayerConfig config;\n",
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
    sources = {RUNTIME, SUPPORT}
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
