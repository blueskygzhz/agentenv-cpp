#!/usr/bin/env python3
"""Mutation harness for the capture steps added to `PauseToDir` / `CaptureToDir`.

Status is decided **by exit code only**: the suite calls `exit(1)` on the first
failing assertion, so its summary line is not printed on failure and scraping
stdout for "failed" would misreport a caught mutation as a survivor.
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
SRC = ROOT / "src/sandbox/firecracker/sandbox_backend.cc"
BIN = ROOT / "build/bin/test_firecracker_sandbox_backend"
TARGET = "test_firecracker_sandbox_backend"

MUTATIONS = [
    (
        "memory_stack_not_written",
        '        result.memory_image_config_path = spec.snapshot_dir + "/" '
        "+ kMemImageConfigFileName;\n        return core::fs::WriteAtomically(",
        '        result.memory_image_config_path = spec.snapshot_dir + "/" '
        "+ kMemImageConfigFileName;\n        return core::Unit();\n        "
        "return core::fs::WriteAtomically(",
    ),
    (
        "memory_stack_path_renamed",
        'const char* const kMemImageConfigFileName = "mem_image.json";',
        'const char* const kMemImageConfigFileName = "memory.json";',
    ),
    (
        "memory_stack_drops_the_new_layer",
        "            BuildMemSnapshotImageConfig(impl_->InheritedMemImageConfig(),\n"
        "                                        result.memory_layer_path, mem_dir,",
        "            BuildMemSnapshotImageConfig(impl_->InheritedMemImageConfig(),\n"
        '                                        std::string("/dev/null"), mem_dir,',
    ),
    (
        "rootfs_captured_unconditionally",
        "    if (impl_->CapturesRootfs(spec)) {",
        "    if (true) {",
    ),
    (
        "rootfs_never_captured",
        "    if (impl_->CapturesRootfs(spec)) {",
        "    if (false) {",
    ),
    # No mutation for "the seal runs after the memory layer": inverting that
    # means moving a block, which a textual substitution cannot express, and a
    # comment-only edit would survive while proving nothing. The ordering is
    # asserted directly by
    # `capture_seals_the_rootfs_only_after_the_memory_layer` instead.
    (
        "seal_error_reclassified_by_string",
        "            return core::make_unexpected(\n"
        '                impl_->RecoverCaptureFailure("rootfs restack", rootfs.error()));',
        "            return core::make_unexpected(impl_->RecoverCaptureFailure(\n"
        '                "rootfs restack", ClassifyCaptureError(rootfs.error().message)));',
    ),
    (
        "failed_rootfs_keeps_the_partial_directory",
        "        if (!rootfs.ok()) {\n            CleanupFailedSnapshotDir(spec.snapshot_dir);",
        "        if (!rootfs.ok()) {",
    ),
    (
        "manifest_names_the_bare_memory_layer",
        "                                         result.memory_image_config_path, "
        "result.memory_size,",
        "                                         result.memory_layer_path, result.memory_size,",
    ),
    (
        "manifest_loses_the_rootfs_size",
        "                                         result.rootfs_virtual_size, "
        "impl_->ExtraDrives());",
        "                                         0, impl_->ExtraDrives());",
    ),
    (
        "rootfs_outcome_not_recorded",
        "        result.rootfs_image_config_path = rootfs.value().image_config_path;\n"
        "        result.rootfs_virtual_size = rootfs.value().virtual_size;",
        "        result.rootfs_virtual_size = rootfs.value().virtual_size;",
    ),
    (
        "manifest_always_absent",
        "        result.manifest = core::Optional<SandboxSnapshotManifest>(manifest.value());",
        "        // mutated: manifest dropped",
    ),
    (
        "capture_to_dir_resumes_the_vm",
        "    // Deliberately **not** resumed: this is what separates "
        "`CaptureToDir` from",
        "    (void)impl_->instance->Resume();\n"
        "    // Deliberately **not** resumed: this is what separates "
        "`CaptureToDir` from",
    ),
    (
        "capture_to_dir_ignores_a_missing_manifest",
        "    if (!captured.value().manifest) {\n        CleanupFailedSnapshotDir(at);",
        "    if (false) {\n        CleanupFailedSnapshotDir(at);",
    ),
    (
        "capture_to_dir_missing_manifest_not_resumed",
        "        return core::make_unexpected(impl_->RecoverCaptureFailure(\n"
        '            "capture_to_dir",\n'
        "            SandboxCaptureError::Recoverable(\n"
        '                "this sandbox has no overlaybd ublk rootfs to capture, so no snapshot "\n'
        '                "manifest could be produced")));',
        "        return core::make_unexpected(SandboxCaptureError::Recoverable(\n"
        '            "this sandbox has no overlaybd ublk rootfs to capture, so no snapshot "\n'
        '            "manifest could be produced"));',
    ),
    (
        "capture_to_dir_without_a_vm_is_accepted",
        '            SandboxCaptureError::Terminal("capture_to_dir: no live microVM"));',
        '            SandboxCaptureError::Recoverable("capture_to_dir: no live microVM"));',
    ),
    (
        "rootfs_inputs_mixed_between_override_and_device",
        "            live_image_config_path = pause_spec.rootfs_override->live_image_config_path;\n"
        "            virtual_size = pause_spec.rootfs_override->virtual_size;",
        "            live_image_config_path = pause_spec.rootfs_override->live_image_config_path;\n"
        "            virtual_size = 0;",
    ),
    (
        "rootfs_staged_read_only",
        "            restack, false, live_image_config_path, snapshot_dir,",
        "            restack, true, live_image_config_path, snapshot_dir,",
    ),
    # No mutation for "DefaultLayerCompactor compresses instead of staying
    # raw". Reaching it needs compaction to actually trigger, which needs a
    # runtime-owned suffix over budget made of *real* overlaybd layers --
    # `CompactLayers` reads them. The fake live config in this suite holds
    # placeholder text, so the compactor is never called and the mutation
    # survives while proving nothing. Recorded as a coverage gap in the README
    # rather than papered over with an assertion that cannot fail.
]


def build():
    r = subprocess.run(
        ["cmake", "--build", "build", "-j", "8", "--target", TARGET],
        cwd=ROOT, capture_output=True, text=True,
    )
    return r.returncode == 0, r.stdout + r.stderr


def run():
    return subprocess.run([str(BIN)], cwd=ROOT, capture_output=True, text=True).returncode


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

    ok, log = build()
    if not ok:
        print("BASELINE BUILD FAILED")
        print(log[-3000:])
        return 2
    if run() != 0:
        print("BASELINE TESTS FAILED")
        return 2
    print("baseline: build ok, tests pass\n")

    caught, survived, broken = [], [], []
    try:
        for name, old, new in MUTATIONS:
            assert old in original, f"anchor missing for {name}"
            SRC.write_text(original.replace(old, new, 1))

            ok, _ = build()
            if not ok:
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
    sys.exit(main())
