#!/usr/bin/env python3
"""Mutation harness for the `templateID` -> runnable-snapshot launch chain.

The chain is: `ApiImpl::CreateSandbox` -> repository `Get` -> resolver
`Resolve` -> `SandboxLaunchSource::FromRunnableSnapshot` -> the orchestrator's
`Kind::Snapshot` branch. Each mutation below breaks one link in a way that
leaves the handler returning 201, which is exactly how the chain managed to be
broken end to end while every test passed.

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
IMPLS = ROOT / "src/api/impls.cc"
TYPES = ROOT / "src/orchestrator/types.cc"
MANIFEST = ROOT / "src/sandbox/manifest.cc"
# `CheckSnapshotAttachedDrives` and `CheckAttachedDriveOverlap`, which the
# handler calls but which are neither of them in `impls.cc`.
PARAMS = ROOT / "src/api/sandbox_params.cc"
TARGET = "test_api_create_from_snapshot"

# The record lookup, whose result feeds the resolve.
MISSING_RECORD = """    if (!record.value().has_value()) {
        // Upstream's `Ok(None)` arm, verbatim down to the wording.
        return core::make_unexpected(
            ApiError::Make(400, "template " + id_or_alias + " not found"));
    }
"""

NO_RESOLVER_GUARD = """    if (!runtime_resolver_) {"""

# The overlap check, lifted so it can be *moved* rather than deleted.
#
# Deleting it is a separate mutation. This one keeps the same 400 and changes
# only *when* it is raised, so the sole observable difference is the `pending-`
# lease left on a volume for a sandbox that was never going to exist. Two
# edits, because a move is a deletion plus an insertion and no single anchor
# spans both without swallowing the comment block between them.
OVERLAP_CHECK = """    if (requested.value().has_value()) {
        const core::Expected<core::Unit, std::string> overlap =
            CheckAttachedDriveOverlap(attached_drives, *requested.value());
        if (!overlap.ok()) {
            return core::make_unexpected(ApiError::Make(400, overlap.error()));
        }
    }
"""

OVERLAP_COMMENT = """    // Rust `check_attached_drive_overlap`, which was absent for the same
    // reason the field was. Compared against the *requested* mounts, before
    // any volume is leased: a 400 after `PrepareVolumeMounts` would leave
    // placeholder leases behind for a request that was never going to launch.
"""

# Unique to this handler: the cold one spells its own concatenation
# differently, so this does not need a disambiguating anchor.
AFTER_PREPARE = (
    "    // Rust: `let mut extra_drives = attached_drives; extra_drives.extend("
)

MOVE_OVERLAP_BELOW_PREPARE = [
    (OVERLAP_COMMENT + OVERLAP_CHECK + "\n", ""),
    (AFTER_PREPARE, OVERLAP_CHECK + "\n" + AFTER_PREPARE),
]

MUTATIONS = [
    # The launch source stops naming the snapshot kind, so the orchestrator
    # takes the fresh branch with an empty `image_config_path` -- the exact
    # pre-change behaviour.
    (
        "launch_source_keeps_the_image_kind",
        TYPES,
        "    s.kind = Kind::Snapshot;\n    s.snapshot_id = record.id.ToString();\n",
        "    s.snapshot_id = record.id.ToString();\n",
    ),
    # The hydrated manifest is dropped, so the backend is handed a resume with
    # no vm_state or image configs to resume from.
    (
        "manifest_paths_not_projected",
        TYPES,
        "    s.snapshot_manifest.vm_state_path            = manifest.vm_state.path;\n",
        "",
    ),
    # Resources come from the node's defaults instead of the record, so a
    # restored sandbox silently gets different CPU/memory than it was captured
    # with.
    (
        "resources_not_inherited_from_the_record",
        TYPES,
        "    s.snapshot_resources           = record.resources;\n",
        "",
    ),
    # Runtime versions are dropped, so a resume would run this node's envd
    # against a guest captured with another.
    (
        "runtime_versions_not_inherited",
        TYPES,
        "    s.snapshot_runtime_versions    = committed.runtime_versions;\n",
        "",
    ),
    # A missing record becomes a successful create: upstream's `Ok(None)` arm
    # removed, so the resolver is handed an absent optional.
    ("missing_record_not_reported", IMPLS, MISSING_RECORD, ""),
    # A node with no resolver stops saying so and falls through.
    (
        "absent_resolver_not_reported",
        IMPLS,
        NO_RESOLVER_GUARD,
        "    if (false) {",
    ),
    # The free-slot check stops firing, so a request for more drives than the
    # capture reserved boots a guest missing them.
    (
        "free_slot_check_disabled",
        IMPLS,
        "    if (requested_drive_count > free_slots) {",
        "    if (false) {",
    ),
    # The five request fields the handler never used to set. Each was plumbed
    # all the way downstream, so breaking one again is invisible except here.
    (
        "network_policy_not_built_from_the_request",
        IMPLS,
        "    request.network_policy = policy.value();\n"
        "\n    // Rust `params_model_to_map`",
        "\n    // Rust `params_model_to_map`",
    ),
    (
        "network_config_half_of_the_policy_ignored",
        IMPLS,
        "        NetworkPolicyFromCreate(req.allow_internet_access,\n"
        "                                req.network.has_value() ? &*req.network : NULL);\n"
        "    if (!policy.ok()) {\n"
        "        return core::make_unexpected(ApiError::Make(400, policy.error()));\n"
        "    }\n"
        "    request.network_policy = policy.value();",
        "        NetworkPolicyFromCreate(req.allow_internet_access, NULL);\n"
        "    if (!policy.ok()) {\n"
        "        return core::make_unexpected(ApiError::Make(400, policy.error()));\n"
        "    }\n"
        "    request.network_policy = policy.value();",
    ),
    (
        "invalid_network_reported_as_a_server_fault",
        IMPLS,
        "    if (!policy.ok()) {\n"
        "        return core::make_unexpected(ApiError::Make(400, policy.error()));\n"
        "    }\n"
        "    request.network_policy = policy.value();",
        "    if (!policy.ok()) {\n"
        "        return core::make_unexpected(InternalError(policy.error()));\n"
        "    }\n"
        "    request.network_policy = policy.value();",
    ),
    (
        "auto_pause_default_flips_to_delete",
        IMPLS,
        "    request.timeout_action = req.auto_pause.has_value() && !*req.auto_pause\n"
        "                                 ? orchestrator::SandboxTimeoutAction::Delete\n"
        "                                 : orchestrator::SandboxTimeoutAction::Pause;\n"
        "    // Rust `body.auto_resume.as_ref().is_some_and(|cfg| cfg.enabled)`. The",
        "    request.timeout_action = orchestrator::SandboxTimeoutAction::Pause;\n"
        "    // Rust `body.auto_resume.as_ref().is_some_and(|cfg| cfg.enabled)`. The",
    ),
    (
        "auto_resume_never_requested",
        IMPLS,
        "    request.auto_resume = req.auto_resume.has_value() && *req.auto_resume;\n"
        "    // Rust `user_metadata: body.metadata.clone()`.",
        "    // Rust `user_metadata: body.metadata.clone()`.",
    ),
    # Anchored on the preceding comment: the identical loop appears in
    # `CreateColdSandbox` too, and an ambiguous anchor would mutate whichever
    # handler came first.
    (
        "request_metadata_dropped",
        IMPLS,
        "    // Rust `user_metadata: body.metadata.clone()`.\n"
        "    for (std::map<std::string, std::string>::const_iterator it = req.metadata.begin();\n"
        "         it != req.metadata.end(); ++it) {\n"
        "        request.user_metadata[it->first] = it->second;\n"
        "    }",
        "",
    ),
    (
        "custom_extension_params_validation_skipped",
        IMPLS,
        "    if (!params_ok.ok()) {\n"
        "        return core::make_unexpected(ApiError::Make(400, params_ok.error()));\n"
        "    }\n"
        "    // Rust's snapshot path passes",
        "    // Rust's snapshot path passes",
    ),
    (
        "timeout_read_as_milliseconds",
        IMPLS,
        "        request.timeout_ms = core::Optional<int64_t>(*req.timeout_secs * 1000);\n"
        "    }\n"
        "    for (std::size_t i = 0; i < req.env_vars.size(); ++i) {\n"
        "        // `KEY=VALUE`, as the DTO carries them.",
        "        request.timeout_ms = core::Optional<int64_t>(*req.timeout_secs);\n"
        "    }\n"
        "    for (std::size_t i = 0; i < req.env_vars.size(); ++i) {\n"
        "        // `KEY=VALUE`, as the DTO carries them.",
    ),
    # `check_snapshot_volume_replacement` stops firing.
    (
        "volume_replacement_check_disabled",
        IMPLS,
        "    if (replaces_mounts && !committed.volume_snapshots.empty()) {",
        "    if (false) {",
    ),
    # The absent-vs-empty distinction collapses, so a present-but-empty
    # `volumeMounts` reads as "restore the captured volumes" instead of as the
    # explicit "no volumes" it is.
    (
        "present_empty_mounts_read_as_absent",
        IMPLS,
        "    const bool replaces_mounts = req.volume_mounts.has_value();",
        "    const bool replaces_mounts = req.volume_mounts.has_value() &&\n"
        "                                 !req.volume_mounts->empty();",
    ),
    # `free_drive_slots` wraps instead of saturating, reporting SIZE_MAX free
    # slots for a manifest whose counts disagree.
    (
        "free_drive_slots_wraps_on_underflow",
        MANIFEST,
        "    return volume_drive_slots > slot_drives ? volume_drive_slots - slot_drives : 0;",
        "    return volume_drive_slots - slot_drives;",
    ),
    # A pre-slots capture reports the stored physical count, which was never
    # written for those records.
    (
        "effective_physical_count_ignores_the_legacy_shape",
        MANIFEST,
        "    if (volume_drive_slots == 0) return attached_drives.size();\n",
        "",
    ),
    # ---- attachedDrives ----------------------------------------------------
    #
    # `ResolveAttachedDrives` was ported, unit-tested, and had no production
    # caller; `check_snapshot_attached_drives` and `check_attached_drive_overlap`
    # were not ported at all. Every mutation below leaves the handler at 201.
    (
        "snapshot_attached_drive_check_disabled",
        IMPLS,
        "        CheckSnapshotAttachedDrives(committed, req.attached_drives.has_value());",
        "        CheckSnapshotAttachedDrives(committed, false);",
    ),
    # The absent-vs-empty distinction collapses, so `[]` reads as "restore the
    # capture's drives" instead of the explicit refusal it is.
    (
        "present_empty_attached_drives_read_as_absent",
        IMPLS,
        "        CheckSnapshotAttachedDrives(committed, req.attached_drives.has_value());",
        "        CheckSnapshotAttachedDrives(\n"
        "            committed,\n"
        "            req.attached_drives.has_value() && !req.attached_drives->empty());",
    ),
    # Only the captured volumes guard the field, so a capture holding attached
    # drives and no volumes accepts a replacement list.
    (
        "snapshot_attached_drive_check_ignores_captured_drives",
        PARAMS,
        "    if (explicit_drives &&\n"
        "        (!committed.volume_snapshots.empty() || !committed.attached_drives.empty())) {",
        "    if (explicit_drives && !committed.volume_snapshots.empty()) {",
    ),
    # Back to counting only the volumes, which is what this port did while the
    # request could not carry drives.
    (
        "attached_drives_not_counted_against_the_free_slots",
        IMPLS,
        "        attached_count + (requested.value().has_value() ? requested.value()->size() : 0);",
        "        (requested.value().has_value() ? requested.value()->size() : 0);",
    ),
    # A node with no resolver stops saying so; `ResolveAttachedDrives` then
    # reports its own 500 instead of this 501.
    (
        "absent_resolver_not_reported_for_attached_drives",
        IMPLS,
        "        if (!image_resolver_) {\n"
        "            return core::make_unexpected(ApiError::Make(\n"
        "                501, \"attaching drives from images requires an image resolver\"));\n"
        "        }\n",
        "",
    ),
    # The resolved drives never reach the launch: the handler still answers
    # 201 and the guest boots without the disk that was asked for.
    (
        "attached_drives_dropped_from_the_launch",
        IMPLS,
        "    request.extra_drives = attached_drives;\n",
        "    request.extra_drives.clear();\n",
    ),
    # Volumes take the first free slots instead of the attached drives, so the
    # guest's mounts bind to the wrong backing devices.
    (
        "volumes_lead_the_attached_drives",
        IMPLS,
        "    request.extra_drives = attached_drives;\n"
        "    for (std::size_t i = 0; i < prepared.value().drives.size(); ++i) {\n"
        "        request.extra_drives.push_back(prepared.value().drives[i]);\n"
        "    }\n",
        "    request.extra_drives = prepared.value().drives;\n"
        "    for (std::size_t i = 0; i < attached_drives.size(); ++i) {\n"
        "        request.extra_drives.push_back(attached_drives[i]);\n"
        "    }\n",
    ),
    (
        "attached_drive_overlap_check_disabled",
        IMPLS,
        "            CheckAttachedDriveOverlap(attached_drives, *requested.value());",
        "            CheckAttachedDriveOverlap(std::vector<sandbox::ExtraDrive>(),\n"
        "                                      *requested.value());",
    ),
    # Only one nesting direction is refused, so a volume mounted *under* an
    # attached drive gets through.
    (
        "overlap_checked_in_one_direction_only",
        PARAMS,
        "                sandbox::MountPathStartsWith(path.value(), drive_path) ||\n"
        "                sandbox::MountPathStartsWith(drive_path, path.value());",
        "                sandbox::MountPathStartsWith(drive_path, path.value());",
    ),
    # The overlap check moves below `PrepareVolumeMounts`. The request is still
    # refused with the same 400, but the volume has been leased to a `pending-`
    # placeholder for a sandbox that will never exist, and nothing releases it.
    (
        "overlap_checked_after_the_volumes_are_leased",
        IMPLS,
        MOVE_OVERLAP_BELOW_PREPARE,
        None,
    ),
    # The resolver is handed a record other than the one that was looked up.
    # Caught only because the fake echoes back the id it was given and the
    # tests assert on it; without that assertion a handler could resolve the
    # wrong snapshot and still answer 201.
    (
        "resolver_called_with_the_wrong_record",
        IMPLS,
        "        record = repository.value()->Get(id_or_alias);",
        "        record = repository.value()->Get(id_or_alias + std::string(\"x\"));",
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

    sources = {IMPLS, TYPES, MANIFEST, PARAMS}
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
            # A mutation is normally one `old -> new` pair; a *move* needs two
            # (the deletion and the insertion), so `old` may instead be a list
            # of pairs. Anchors are counted against the unmutated text, before
            # any of them is applied, so an edit cannot create or destroy a
            # later edit's anchor unnoticed.
            edits = old if isinstance(old, list) else [(old, new)]
            text = path.read_text()
            for before, _ in edits:
                assert text.count(before) == 1, \
                    f"anchor for {name} matched {text.count(before)} times in {path.name}"
            if all(before == after for before, after in edits):
                noop.append(name)
                print(f"  NO-OP    {name} (mutation is identity)")
                continue
            for before, after in edits:
                text = text.replace(before, after, 1)
            path.write_text(text)

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
