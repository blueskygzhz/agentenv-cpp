#!/usr/bin/env python3
"""Mutation harness for the overlaybd memory layer-stack bookkeeping.

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
SRC = ROOT / "src/sandbox/firecracker/mem_image_config.cc"
BIN = ROOT / "build/bin/test_firecracker_mem_image_config"
TARGET = "test_firecracker_mem_image_config"

MUTATIONS = [
    (
        "budget_ignores_the_durable_prefix",
        "    const std::size_t shrink = durable_prefix_len / 4;",
        "    const std::size_t shrink = 0;",
    ),
    (
        "budget_shrinks_too_fast",
        "    const std::size_t shrink = durable_prefix_len / 4;",
        "    const std::size_t shrink = durable_prefix_len / 2;",
    ),
    (
        "budget_not_clamped_to_one",
        "    return budget > 1 ? budget : 1;",
        "    return budget;",
    ),
    (
        "appended_layer_not_counted_against_budget",
        "    const std::size_t compactable =\n"
        "        runtime_owned.size() + (appended_layer.has_value() ? 1u : 0u);",
        "    const std::size_t compactable = runtime_owned.size();",
    ),
    (
        "compaction_threshold_off_by_one",
        "    if (compactable > max_runtime_owned) {",
        "    if (compactable > max_runtime_owned + 1) {",
    ),
    (
        "never_compacts",
        "    if (compactable > max_runtime_owned) {",
        "    if (false) {",
    ),
    (
        "always_compacts",
        "    if (compactable > max_runtime_owned) {",
        "    if (true) {",
    ),
    (
        "remote_lower_check_skipped",
        "            if (suffix[i].file.empty()) {",
        "            if (false) {",
    ),
    (
        "appended_layer_dropped_from_compaction_input",
        "        if (appended_layer.has_value()) suffix.push_back(*appended_layer);",
        "        // mutated: appended layer not compacted",
    ),
    (
        "empty_compaction_result_still_pushed",
        "        if (compacted.value().has_value()) {\n"
        "            lowers.push_back(LocalLayerConfig(*compacted.value()));\n        }",
        "        lowers.push_back(LocalLayerConfig(compacted.value().has_value() "
        "? *compacted.value() : std::string()));",
    ),
    (
        "adopted_layers_lose_their_stack_index",
        '        std::snprintf(idx, sizeof(idx), "%04zu", i);',
        '        std::snprintf(idx, sizeof(idx), "%s", "");',
    ),
    (
        "adoption_result_not_written_back",
        "        runtime_owned[i].file = destination;",
        "        // mutated: lower still points at the runtime path",
    ),
    (
        "appended_layer_not_added_on_the_adopt_path",
        "    if (appended_layer.has_value()) lowers.push_back(*appended_layer);\n    return lowers;",
        "    return lowers;",
    ),
    (
        "appended_layer_added_below_the_suffix",
        "    for (std::size_t i = 0; i < runtime_owned.size(); ++i) {\n"
        "        lowers.push_back(runtime_owned[i]);\n    }\n"
        "    if (appended_layer.has_value()) lowers.push_back(*appended_layer);",
        "    if (appended_layer.has_value()) lowers.push_back(*appended_layer);\n"
        "    for (std::size_t i = 0; i < runtime_owned.size(); ++i) {\n"
        "        lowers.push_back(runtime_owned[i]);\n    }",
    ),
    (
        "missing_adopter_silently_skips",
        "    if (!runtime_owned.empty() && !adopt) {",
        "    if (false) {",
    ),
    (
        "missing_compactor_silently_skips",
        "        if (!compact) {",
        "        if (false) {",
    ),
    (
        "build_drops_the_repo_blob_url",
        "    out.repo_blob_url = base.repo_blob_url;",
        "    // mutated: repo_blob_url dropped",
    ),
    (
        "build_carries_the_whole_inherited_config",
        "    obd::ImageConfig out;\n    // Only these two survive",
        "    obd::ImageConfig out = base;\n    // Only these two survive",
    ),
    (
        "build_does_not_append_the_new_layer",
        "            core::Optional<obd::LayerConfig>(LocalLayerConfig(new_layer_path)),",
        "            core::Optional<obd::LayerConfig>(),",
    ),
    (
        "build_uses_the_rootfs_compaction_name",
        '            "mem_compacted.commit", runtime_owned_roots, compact, adopt);',
        '            "managed-base.commit", runtime_owned_roots, compact, adopt);',
    ),
    (
        "local_layer_invents_a_digest",
        "obd::LayerConfig LocalLayerConfig(const std::string& path) {\n"
        "    obd::LayerConfig layer;\n    layer.file = path;",
        "obd::LayerConfig LocalLayerConfig(const std::string& path) {\n"
        "    obd::LayerConfig layer;\n    layer.file = path;\n"
        '    layer.digest = "sha256:0";\n    layer.size = 1;',
    ),
    (
        "durable_prefix_also_adopted",
        "    std::vector<obd::LayerConfig> runtime_owned(\n"
        "        existing_lowers.begin() + static_cast<std::ptrdiff_t>(prefix_len),\n"
        "        existing_lowers.end());",
        "    std::vector<obd::LayerConfig> runtime_owned(existing_lowers.begin(),\n"
        "                                                existing_lowers.end());",
    ),
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
