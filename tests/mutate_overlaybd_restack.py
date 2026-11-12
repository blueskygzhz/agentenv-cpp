#!/usr/bin/env python3
"""Mutation harness for the overlaybd restack capture path.

Each entry rewrites one load-bearing decision in
`src/sandbox/firecracker/overlaybd_restack.cc` and asserts the test binary
notices. A mutation that survives means the corresponding test does not
actually constrain the behaviour it claims to.

Status is decided **by exit code only**. The suite calls `exit(1)` on the first
failing assertion, so its summary line is not printed on failure and scraping
stdout for the word "failed" misreports a caught mutation as a survivor.
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
SRC = ROOT / "src/sandbox/firecracker/overlaybd_restack.cc"
BIN = ROOT / "build/bin/test_firecracker_overlaybd_restack"
TARGET = "test_firecracker_overlaybd_restack"

# (name, old, new) -- `old` must appear verbatim, which is what keeps a
# mutation from silently becoming a no-op after a refactor.
MUTATIONS = [
    (
        "read_only_does_not_short_circuit",
        "    if (read_only) {\n        // Nothing writable to seal",
        "    if (false) {\n        // Nothing writable to seal",
    ),
    (
        "staging_failure_always_recoverable",
        "        if (restacked) {\n            return core::make_unexpected("
        "SandboxCaptureError::Terminal(staged.error()));\n        }",
        "        if (false) {\n            return core::make_unexpected("
        "SandboxCaptureError::Terminal(staged.error()));\n        }",
    ),
    (
        "staging_failure_always_terminal",
        "        if (restacked) {\n            return core::make_unexpected("
        "SandboxCaptureError::Terminal(staged.error()));\n        }",
        "        if (true) {\n            return core::make_unexpected("
        "SandboxCaptureError::Terminal(staged.error()));\n        }",
    ),
    (
        "size_mismatch_not_checked",
        "        if (copied_size.value() != descriptor.value()->size) {",
        "        if (false) {",
    ),
    (
        "size_mismatch_reported_recoverable",
        "            return core::make_unexpected(SandboxCaptureError::Terminal(\n"
        "                \"restack snapshot descriptor size mismatch for '\"",
        "            return core::make_unexpected(SandboxCaptureError::Recoverable(\n"
        "                \"restack snapshot descriptor size mismatch for '\"",
    ),
    (
        "size_check_runs_without_a_descriptor",
        "    if (descriptor.value()) {\n        const core::Expected<uint64_t, std::string> "
        "copied_size =",
        "    if (true) {\n        const core::Expected<uint64_t, std::string> copied_size =",
    ),
    (
        "live_config_not_rewritten_after_seal",
        "    const core::Expected<core::Unit, std::string> rewritten =\n"
        "        RewriteLiveRuntimeConfigForRestack(live_runtime_image_config_path, "
        "snapshot_layer_path,\n                                           descriptor.value());",
        "    const core::Expected<core::Unit, std::string> rewritten = core::Unit();",
    ),
    (
        "rewrite_drops_the_descriptor",
        "    image_config.lowers.push_back(\n"
        "        LocalLayerConfigWithDescriptor(snapshot_layer_path, descriptor));",
        "    image_config.lowers.push_back(\n"
        "        LocalLayerConfigWithDescriptor(snapshot_layer_path, "
        "core::Optional<RestackedLayer>()));",
    ),
    (
        "rewrite_failure_reported_recoverable",
        "        return core::make_unexpected(SandboxCaptureError::Terminal(\n"
        "            \"rewrite live runtime config after restack snapshot: \"",
        "        return core::make_unexpected(SandboxCaptureError::Recoverable(\n"
        "            \"rewrite live runtime config after restack snapshot: \"",
    ),
    (
        "staged_config_keeps_the_live_upper",
        "    image_config.upper = obd::UpperConfig();",
        "    // mutated: upper kept",
    ),
    (
        "staged_config_leaves_result_file_unset",
        '    image_config.result_file = "./result.txt";',
        "    // mutated: result_file untouched",
    ),
    (
        "sealed_layer_identity_not_verified",
        "        if (actual != expected) {",
        "        if (false) {",
    ),
    (
        "empty_stack_accepted_after_a_seal",
        "        if (image_config.lowers.empty()) {",
        "        if (false) {",
    ),
    (
        "sealed_layer_not_handed_to_the_rewriter",
        "        appended_layer = core::Optional<obd::LayerConfig>(image_config.lowers.back());\n"
        "        image_config.lowers.pop_back();",
        "        image_config.lowers.pop_back();",
    ),
    (
        "stale_layer_file_not_cleared",
        "core::Expected<core::Unit, std::string> PrepareSnapshotLayerPath(const std::string& path) "
        "{\n    const core::Expected<core::Unit, std::string> removed = core::fs::RemoveFile(path);",
        "core::Expected<core::Unit, std::string> PrepareSnapshotLayerPath(const std::string& path) "
        "{\n    const core::Expected<core::Unit, std::string> removed = core::Unit();",
    ),
    (
        "rootfs_subdir_renamed",
        'core::fs::Join(snapshot_root, "rootfs"),',
        'core::fs::Join(snapshot_root, "root"),',
    ),
    (
        "rootfs_layer_file_renamed",
        '                                          "snapshot.commit", runtime_owned_roots, compact,',
        '                                          "snap.commit", runtime_owned_roots, compact,',
    ),
    (
        "descriptor_ignored_on_the_local_layer",
        "    if (descriptor) {\n        layer.digest = descriptor->digest;",
        "    if (false) {\n        layer.digest = descriptor->digest;",
    ),
    (
        "adopting_onto_itself_deletes_the_layer",
        "    if (source == destination) return core::Unit();",
        "    // mutated: no self-adoption short circuit",
    ),
    (
        "empty_upper_data_accepted_as_restackable",
        "    if (loaded.value().upper.data.empty()) {",
        "    if (false) {",
    ),
    (
        "unresolvable_runtime_root_dropped",
        "    return resolved.ok() ? resolved.value() : path;",
        '    return resolved.ok() ? resolved.value() : std::string("");',
    ),
    (
        "stale_destination_not_removed_before_adopting",
        "    const core::Expected<core::Unit, std::string> removed = "
        "core::fs::RemoveFile(destination);",
        "    const core::Expected<core::Unit, std::string> removed = core::Unit();",
    ),
]


def build():
    r = subprocess.run(
        ["cmake", "--build", "build", "-j", "8", "--target", TARGET],
        cwd=ROOT, capture_output=True, text=True,
    )
    return r.returncode == 0, r.stdout + r.stderr


def run():
    r = subprocess.run([str(BIN)], cwd=ROOT, capture_output=True, text=True)
    return r.returncode


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

    # The clean tree must build and pass, or every result below is meaningless.
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

            ok, log = build()
            if not ok:
                # A mutation that cannot compile proves nothing about the tests.
                broken.append(name)
                print(f"  BROKEN   {name} (did not compile)")
                continue

            code = run()
            if code != 0:
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
    if survived:
        print("survivors:")
        for name in survived:
            print(f"  - {name}")
    if broken:
        print("did not compile:")
        for name in broken:
            print(f"  - {name}")
    return 1 if (survived or broken) else 0


if __name__ == "__main__":
    sys.exit(main())
